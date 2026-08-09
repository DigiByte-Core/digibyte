#!/usr/bin/env python3
"""Test DigiDollar redemption on a wallet with fragmented DGB.

Regression test for redeemdigidollar failing with
"Insufficient fee inputs for DD redemption fee" on a wallet whose spendable DGB
is fragmented into small UTXOs, even when the wallet holds plenty of DGB.

Spending a fee input is not free: it enlarges the transaction the DigiDollar fee
is computed from, so at the DigiDollar fee rate (35,000,000 sat/kvB) each extra
fee input costs ~3,220,000 sat (~0.0322 DGB) to spend. Fee coin selection used to
sort smallest-first and stop once the RAW sum of the picked UTXOs reached the
target, so on a fragmented wallet it handed back a pile of small UTXOs whose net
contribution was far below the fee, and the redemption builder - which recomputes
the fee from the transaction it actually built - rejected it outright. There was
no re-selection.

Here the wallet is deliberately stocked with 0.0525 DGB UTXOs, just above that
per-input cost: four of them raise exactly the 21,000,000 sat the old fixed fee
estimate asked for, while contributing only 8,120,000 sat once the cost of
spending them is paid. The redemption must still succeed, and must not drag the
fragmented UTXOs in.
"""

from decimal import Decimal

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

# Just above the ~0.0322 DGB it costs to spend one fee input at the DigiDollar
# fee rate: individually economic, collectively unable to pay the fee.
FRAGMENT_AMOUNT = Decimal("0.0525")
NUM_FRAGMENTS = 12
DD_MINT_CENTS = 100000  # $1000.00
LOCK_TIER = 0           # 1 hour = 240 blocks


class DigiDollarRedeemFragmentedFeesTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            ["-digidollar=1", "-mocktime=0", "-dandelion=0", "-txindex=1"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Generating blocks past the DigiDollar activation height")
        self.generate(node, 655)
        node.setmockoracleprice(10000)  # $0.01/DGB in micro-USD

        self.log.info("Minting $1000 DD with a 1-hour timelock")
        mint_result = node.mintdigidollar(DD_MINT_CENTS, LOCK_TIER)
        position_id = mint_result['position_id']
        unlock_height = mint_result['unlock_height']
        assert_equal(mint_result['dd_minted'], DD_MINT_CENTS)
        self.generate(node, 10)
        assert_equal(node.getdigidollarbalance()['total'], DD_MINT_CENTS)

        self.log.info(f"Fragmenting the wallet into {NUM_FRAGMENTS} x {FRAGMENT_AMOUNT} DGB UTXOs")
        for _ in range(NUM_FRAGMENTS):
            node.sendtoaddress(node.getnewaddress(), FRAGMENT_AMOUNT)
        self.generate(node, 2)

        fragments = [
            (utxo['txid'], utxo['vout'])
            for utxo in node.listunspent()
            if utxo['amount'] == FRAGMENT_AMOUNT
        ]
        self.log.info(f"Wallet now holds {len(fragments)} fragmented fee-sized UTXOs")
        assert_greater_than(len(fragments), 4)  # enough to reproduce the old selection

        self.log.info("Advancing past the unlock height")
        self.generate(node, unlock_height - node.getblockcount() + 10)
        assert_greater_than(node.getblockcount(), unlock_height)

        # Before the effective-value fix this call raised
        # "Insufficient fee inputs for DD redemption fee": fee selection returned
        # four fragmented UTXOs totalling exactly the estimated fee, which could
        # not cover the fee of the transaction they were part of.
        self.log.info("Redeeming the position from the fragmented wallet")
        redeem_result = node.redeemdigidollar(position_id, DD_MINT_CENTS)
        redeem_txid = redeem_result['txid']
        assert_equal(redeem_result['dd_redeemed'], DD_MINT_CENTS)
        assert_equal(redeem_result['position_closed'], True)

        self.log.info("Checking the redemption did not spend the uneconomic UTXOs")
        redeem_tx = node.getrawtransaction(redeem_txid, True)
        spent = {(vin['txid'], vin['vout']) for vin in redeem_tx['vin'] if 'txid' in vin}
        used_fragments = spent.intersection(fragments)
        assert_equal(used_fragments, set())

        self.log.info("Confirming the redemption")
        self.generate(node, 5)
        assert_equal(node.getrawtransaction(redeem_txid, True).get('confirmations', 0) > 0, True)
        assert_equal(node.getdigidollarbalance()['total'], 0)

        active = [p for p in node.listdigidollarpositions() if p.get('is_active', False)]
        for position in active:
            assert position['position_id'] != position_id, "Redeemed position should be inactive"

        self.log.info("Redemption from a fragmented wallet succeeded")


if __name__ == '__main__':
    DigiDollarRedeemFragmentedFeesTest().main()
