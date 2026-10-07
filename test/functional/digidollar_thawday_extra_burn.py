#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Keep actual token supply separate from vault principal through Thaw Day recovery."""

from decimal import Decimal
import hashlib
import hmac
import json
from pathlib import Path

from digidollar_thawday_integration import DigiDollarThawDayIntegrationTest
from wallet_taproot import KEYS
from test_framework.address import base58_to_byte
from test_framework.descriptors import descsum_create
from test_framework.key import ECKey, ORDER, compute_xonly_pubkey, sign_schnorr
from test_framework.messages import COIN, CTxInWitness, CTxOut, tx_from_hex
from test_framework.script import CScript, CScriptNum, OP_RETURN, TaprootSignatureHash
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


def script_number(value):
    return value if isinstance(value, int) else CScriptNum.decode(bytes([len(value)]) + value)


def metadata(tx):
    records = []
    for index, output in enumerate(tx.vout):
        if output.scriptPubKey[:1] == bytes([OP_RETURN]):
            fields = list(CScript(output.scriptPubKey))
            if len(fields) >= 2 and fields[1] == b"DD":
                records.append((index, fields))
    assert_equal(len(records), 1)
    return records[0]


def token_outputs(tx):
    """Read the standard wallet metadata, independently of any node index."""
    _, fields = metadata(tx)
    kind = script_number(fields[2])
    indices = [i for i, output in enumerate(tx.vout)
               if output.nValue == 0 and len(output.scriptPubKey) == 34
               and output.scriptPubKey[:2] == b"\x51\x20"]
    if kind in (1, 3):
        assert_equal(len(indices), 1)
        amounts = [script_number(fields[3])]
    else:
        assert_equal(kind, 2)
        # Transfer metadata lists each amount directly after the type.
        amounts = [script_number(value) for value in fields[3:]]
        assert_equal(len(amounts), len(indices))
    assert all(amount > 0 for amount in amounts)
    return dict(zip(indices, amounts))


def known_owner_keys():
    """Derive m/0..63 from the existing public wallet_taproot test fixture."""
    body, version = base58_to_byte(KEYS[0]["xprv"])
    extended = bytes([version]) + body
    assert_equal(len(extended), 78)
    assert_equal(extended[45], 0)
    secret = extended[46:78]
    chain_code = extended[13:45]
    key = ECKey()
    key.set(secret, compressed=True)
    public = key.get_pubkey().get_bytes()
    result = {}
    for index in range(64):
        digest = hmac.new(chain_code, public + index.to_bytes(4, "big"), hashlib.sha512).digest()
        tweak = int.from_bytes(digest[:32], "big")
        assert tweak < ORDER
        child = (tweak + int.from_bytes(secret, "big")) % ORDER
        assert child != 0
        child_secret = child.to_bytes(32, "big")
        child_public = compute_xonly_pubkey(child_secret)[0]
        if index < len(KEYS[0]["pubs"]):
            # These public keys were made by an independent implementation.
            assert_equal(child_public.hex(), KEYS[0]["pubs"][index])
        result[child_public] = child_secret
    return result


class DigiDollarThawDayExtraBurnTest(DigiByteTestFramework):
    THAW_HEIGHT = 600
    PRICE = 500000
    PRINCIPAL = 100000
    EXTRA_BURN = 25000
    MAX_GENERATED = 620
    canonical = DigiDollarThawDayIntegrationTest.canonical

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.wallet_names = [self.default_wallet_name]
        common = ["-digidollaractivationheight=1", f"-ddthawdayheight={self.THAW_HEIGHT}",
                  "-dandelion=0", "-txindex=1"]
        self.extra_args = [common + ["-digidollarstatsindex=1"],
                           common + ["-digidollarstatsindex=0"]]

    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(1, 0)

    def quote(self):
        for node in self.nodes:
            node.setmockoracleprice(self.PRICE)

    def mine(self, count, connected=True, alternate=False):
        assert count >= 0
        self.generated += count
        assert self.generated <= self.MAX_GENERATED, "Test block budget exceeded"
        if not count:
            return []
        kwargs = {} if connected else {"sync_fun": self.no_op}
        if alternate:
            return self.generatetoaddress(self.nodes[0], count,
                                          self.nodes[0].getnewaddress("alternate", "bech32"), **kwargs)
        return self.generate(self.nodes[0], count, **kwargs)

    def mine_to(self, height, **kwargs):
        return self.mine(height - self.nodes[0].getblockcount(), **kwargs)

    def save(self, label, **details):
        self.evidence.append({"label": label, **details})
        (Path(self.options.tmpdir) / "prethaw-overburn-observed.json").write_text(
            json.dumps({"generated_blocks": self.generated, "checkpoints": self.evidence},
                       indent=2, default=str) + "\n")

    def live_tokens(self, node):
        total = 0
        for txid, tx in self.dd_transactions.items():
            for index, amount in token_outputs(tx).items():
                if node.gettxout(txid, index, False) is not None:
                    total += amount
        return total

    def live_principal(self, node):
        total = 0
        for position in self.positions:
            tx = self.dd_transactions[position["txid"]]
            _, fields = metadata(tx)
            assert_equal(script_number(fields[2]), 1)
            original = script_number(fields[3])
            assert_equal(original, self.PRINCIPAL)
            # The reused wallet mint builder places collateral at output zero.
            assert_equal(tx.vout[0].nValue, int(Decimal(position["dgb_collateral"]) * COIN))
            assert_equal(tx.vout[0].scriptPubKey[:2], b"\x51\x20")
            if node.gettxout(position["txid"], 0, False) is not None:
                total += original
        return total

    def check_live(self, node, principal, circulating):
        assert_equal(self.live_principal(node), principal)
        assert_equal(self.live_tokens(node), circulating)

    def next_health(self, node, principal, collateral, vaults):
        view = node.getprotectionstatus()["next_block_health"]
        assert_equal(view["candidate_height"], node.getblockcount() + 1)
        assert_equal(view["rule_version"], 1)
        assert_equal(view["ready"], True)
        assert_equal(view["selected_health_denominator"], "open_vault_principal")
        assert_equal(view["health_denominator_cents"], principal)
        assert_equal(view["oracle_price_micro_usd"], self.PRICE)
        record = view["canonical_health"]
        assert_equal(record["ready"], True)
        assert_equal(record["block_hash"], node.getbestblockhash())
        assert_equal(record["open_vault_principal"], principal)
        assert_equal(record["collateral"], collateral)
        assert_equal(record["active_vaults"], vaults)
        expected_health = min(30000, collateral * (self.PRICE // 10) * 100 //
                              (COIN * 1000 * principal))
        assert_equal(view["health_percentage"], expected_health)
        return view

    def check_active(self, label, nodes=None):
        records = []
        for node in (nodes or self.nodes):
            self.check_live(node, 200000, 175000)
            record = self.canonical(node, 200000, 175000, self.remaining_collateral, 2)
            view = self.next_health(node, 200000, self.remaining_collateral, 2)
            records.append({"node": node.index, "canonical": record, "next_block_health": view})
        assert all(row["canonical"] == records[0]["canonical"] for row in records)
        self.save(label, nodes=records)

    def resign_extra_burn(self, original):
        node = self.nodes[0]
        tx = tx_from_hex(original)
        assert_equal(len(tx.wit.vtxinwit[0].scriptWitness.stack), 3)
        _, leaf, control = tx.wit.vtxinwit[0].scriptWitness.stack
        leaf_fields = list(CScript(leaf))
        owner_public = leaf_fields[-2]
        owner_secret = self.owner_keys.get(owner_public)
        assert owner_secret is not None, "Collateral key is outside the imported public test descriptor range"
        index, fields = metadata(tx)
        assert_equal(script_number(fields[2]), 3)
        assert_equal(len(fields), 4)
        original_change = script_number(fields[3])
        assert original_change > self.EXTRA_BURN
        # Keep the original type encoding and change only the amount.
        changed = CScript([OP_RETURN, b"DD", fields[2], CScriptNum(original_change - self.EXTRA_BURN)])
        before_outputs = [output.serialize() for output in tx.vout]
        tx.vout[index].scriptPubKey = changed
        assert all(output.serialize() == before_outputs[i] for i, output in enumerate(tx.vout) if i != index)
        spent = []
        total_token_input = 0
        for txin in tx.vin:
            txid = f"{txin.prevout.hash:064x}"
            coin = node.gettxout(txid, txin.prevout.n, False)
            assert coin is not None
            spent.append(CTxOut(int(Decimal(coin["value"]) * COIN),
                               bytes.fromhex(coin["scriptPubKey"]["hex"])))
            if txid in self.dd_transactions:
                total_token_input += token_outputs(self.dd_transactions[txid]).get(txin.prevout.n, 0)
        assert_equal(total_token_input - original_change, self.PRINCIPAL)
        assert_equal(total_token_input - sum(token_outputs(tx).values()), self.PRINCIPAL + self.EXTRA_BURN)
        tx.wit.vtxinwit = [CTxInWitness() for _ in tx.vin]
        for txin in tx.vin:
            txin.scriptSig = b""
        # The wallet owns the ordinary P2TR token keys and the DGB fee keys.
        # It cannot sign the NUMS collateral output by key path; supply its
        # legitimate normal-path signature below using the existing leaf/proof.
        signed = node.signrawtransactionwithwallet(tx.serialize().hex(), [], "DEFAULT")
        tx = tx_from_hex(signed["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        assert all(tx.wit.vtxinwit[i].scriptWitness.stack or tx.vin[i].scriptSig
                   for i in range(1, len(tx.vin))), "Wallet could not sign a token or fee input"
        digest = TaprootSignatureHash(tx, spent, 0, input_index=0, scriptpath=True, script=CScript(leaf))
        signature = sign_schnorr(owner_secret, digest)
        tx.wit.vtxinwit[0].scriptWitness.stack = [signature, leaf, control]
        tx.rehash()
        self.save("resigned_normal_overburn", original_change=original_change,
                  changed_change=original_change - self.EXTRA_BURN,
                  required_normal_burn=self.PRINCIPAL, actual_burn=self.PRINCIPAL + self.EXTRA_BURN,
                  txid=tx.hash)
        return tx

    def run_test(self):
        owner, observer = self.nodes
        self.generated = 0
        self.evidence = []
        self.positions = []
        self.dd_transactions = {}
        self.owner_keys = known_owner_keys()
        self.mine(175)
        imported = owner.importdescriptors([{
            "desc": descsum_create(f"tr({KEYS[0]['xprv']}/*)"), "timestamp": "now",
            "active": True, "internal": False, "range": [0, 63], "next_index": 0,
        }])
        assert_equal(imported[0]["success"], True)
        for _ in range(3):
            self.quote()
            position = owner.mintdigidollar(self.PRINCIPAL, 0)
            block = self.mine(1)[0]
            assert position["txid"] in owner.getblock(block)["tx"]
            self.positions.append(position)
            self.dd_transactions[position["txid"]] = tx_from_hex(owner.getrawtransaction(position["txid"]))
        self.total_collateral = sum(int(Decimal(pos["dgb_collateral"]) * COIN) for pos in self.positions)
        self.remaining_collateral = self.total_collateral - int(Decimal(self.positions[0]["dgb_collateral"]) * COIN)

        self.quote()
        transfer = owner.senddigidollar(owner.getdigidollaraddress(), 250000)
        transfer_block = self.mine(1)[0]
        assert transfer["txid"] in owner.getblock(transfer_block)["tx"]
        self.dd_transactions[transfer["txid"]] = tx_from_hex(owner.getrawtransaction(transfer["txid"]))
        assert_equal(sorted(token_outputs(self.dd_transactions[transfer["txid"]]).values()), [50000, 250000])
        unlock = max(position["unlock_height"] for position in self.positions) + 1
        assert unlock + 2 < self.THAW_HEIGHT - 1, "Existing lock fixture does not fit below the scheduled H"
        self.mine_to(unlock)

        # Keep the ordinary wallet-built transaction out of the mempool while
        # constructing its valid overburn replacement. All source coins are real.
        self.extra_args[0].append("-walletbroadcast=0")
        self.restart_node(0, self.extra_args[0])
        self.connect_nodes(1, 0)
        self.sync_all()
        self.quote()
        ordinary = owner.redeemdigidollar(self.positions[0]["position_id"], self.PRINCIPAL)
        assert_equal(ordinary["err_active"], False)
        assert_equal(ordinary["required_dd_burn"], self.PRINCIPAL)
        assert ordinary["txid"] not in owner.getrawmempool()
        original = owner.gettransaction(ordinary["txid"])["hex"]
        assert_equal(owner.testmempoolaccept([original], maxfeerate=0)[0]["allowed"], True)
        tx = self.resign_extra_burn(original)
        raw = tx.serialize().hex()
        acceptance = owner.testmempoolaccept([raw], maxfeerate=0)[0]
        self.save("overburn_mempool_validation", result=acceptance)
        assert_equal(acceptance["allowed"], True)
        assert_equal(owner.sendrawtransaction(raw, maxfeerate=0), tx.hash)
        burn_block = self.mine(1)[0]
        burn_height = owner.getblockcount()
        assert burn_height < self.THAW_HEIGHT - 1
        self.dd_transactions[tx.hash] = tx
        for node in self.nodes:
            assert tx.hash in node.getblock(burn_block)["tx"]
            self.check_live(node, 200000, 175000)
        assert_equal(owner.getdigidollarstats()["total_dd_supply"], 175000)
        self.save("accepted_preH_normal_overburn", height=burn_height, block=burn_block, txid=tx.hash,
                  original_open_principal=200000, live_tokens=175000, oracle_price=self.PRICE)

        self.mine_to(self.THAW_HEIGHT - 1)
        self.quote()
        for node in self.nodes:
            self.check_live(node, 200000, 175000)
            self.next_health(node, 200000, self.remaining_collateral, 2)
        self.save("H_minus_1", block=owner.getbestblockhash(), original_open_principal=200000, live_tokens=175000)
        activation = self.mine(1)[0]
        self.quote()
        self.check_active("H")
        self.mine(2)
        original_tip = owner.getbestblockhash()
        self.quote()
        self.check_active("H_plus_2")

        self.disconnect_nodes(1, 0)
        self.restart_node(0, self.extra_args[0])
        self.quote()
        assert_equal(owner.getbestblockhash(), original_tip)
        self.check_active("postH_restart", [owner])
        owner.invalidateblock(activation)
        assert_equal(owner.getblockcount(), self.THAW_HEIGHT - 1)
        self.restart_node(0, self.extra_args[0])
        self.quote()
        self.check_live(owner, 200000, 175000)
        self.next_health(owner, 200000, self.remaining_collateral, 2)
        self.save("cold_reorg_to_H_minus_1", block=owner.getbestblockhash(),
                  original_open_principal=200000, live_tokens=175000)

        replacement = self.mine(4, connected=False, alternate=True)
        assert replacement[0] != activation
        replacement_tip = owner.getbestblockhash()
        owner.reconsiderblock(activation)
        assert_equal(owner.getbestblockhash(), replacement_tip)
        self.quote()
        self.check_active("replacement_branch_across_H", [owner])
        self.connect_nodes(1, 0)
        self.sync_all()
        self.quote()
        self.check_active("continuous_observer_reorg_across_H")

        # Also undo the actual pre-H overburn, then replay it through H. This
        # checks the spent vault is restored at its full original principal.
        self.disconnect_nodes(1, 0)
        owner.invalidateblock(burn_block)
        assert_equal(owner.getblockcount(), burn_height - 1)
        self.check_live(owner, 300000, 300000)
        self.restart_node(0, self.extra_args[0])
        self.quote()
        self.check_live(owner, 300000, 300000)
        assert_equal(owner.getdigidollarstats()["total_dd_supply"], 300000)
        self.save("preH_overburn_undone_and_restarted", block=owner.getbestblockhash(),
                  original_open_principal=300000, live_tokens=300000)
        owner.reconsiderblock(burn_block)
        assert_equal(owner.getbestblockhash(), replacement_tip)
        self.quote()
        self.check_active("preH_overburn_replayed_through_H", [owner])
        self.connect_nodes(1, 0)
        self.sync_all()
        self.quote()
        self.check_active("final_index_on_off_agreement")
        assert_equal(owner.gettxoutsetinfo("muhash")["muhash"], observer.gettxoutsetinfo("muhash")["muhash"])
        self.save("complete", block=replacement_tip, actual_burn=125000,
                  original_open_principal=200000, live_tokens=175000)


if __name__ == "__main__":
    DigiDollarThawDayExtraBurnTest().main()
