"""Helpers for the isolated Thaw Day rehearsal; they never launch or stop nodes.

Put this checkout's test/functional directory on sys.path before importing.
RPC callbacks have the form rpc(method_name, *positional_parameters).
Only call these helpers against the isolated rehearsal wallets and chain.
"""

from decimal import Decimal

from digidollar_thawday_extra_burn import metadata, script_number, token_outputs
from test_framework.key import compute_xonly_pubkey, sign_schnorr
from test_framework.messages import COIN, CTxInWitness, CTxOut, tx_from_hex
from test_framework.script import CScript, CScriptNum, OP_RETURN, TaprootSignatureHash


def require(condition, message):
    if not condition:
        raise ValueError(message)


def satoshis(value):
    value = Decimal(str(value)) * COIN
    require(value == value.to_integral_value(), "DGB amount has sub-satoshi precision")
    return int(value)


def extra_burn_redemption(rpc, original_hex, principal_cents, extra_cents,
                          source_transactions, owner_key_for,
                          collateral_outpoint, collateral_input=0):
    """Return a valid re-signed normal redemption and its exact burn evidence.

    original_hex must be a wallet-built, matured NORMAL redemption kept out of
    the mempool with walletbroadcast=0. All source inputs must be confirmed.
    source_transactions maps each known DD txid to CTransaction or raw hex.
    owner_key_for maps an x-only public key (bytes) to its legitimate secret
    (32 bytes), or is a callable accepting that public key. known_owner_keys()
    supplies the existing public descriptor fixture's map, if imported BEFORE
    this vault was minted. collateral_outpoint is (mint_txid, collateral_vout).

    No existing signature survives an output change. This keeps the same
    collateral leaf/control proof and signs its normal owner path again.
    Wallet signing supplies token and fee signatures. This function neither
    broadcasts nor mines. A partial wallet signing result is expected ONLY
    for the collateral input; final testmempoolaccept must allow the result.
    """
    require(isinstance(principal_cents, int) and principal_cents > 0, "Use positive integer cents")
    require(isinstance(extra_cents, int) and extra_cents > 0, "Use positive integer extra cents")
    original = tx_from_hex(original_hex)
    original.rehash()
    require(original.hash not in rpc("getrawmempool"), "Original redemption is already in the mempool")
    original_acceptance = rpc("testmempoolaccept", [original_hex], 0)[0]
    require(original_acceptance.get("allowed") is True,
            "Original redemption must independently pass: " + str(original_acceptance))
    tx = tx_from_hex(original_hex)
    require(0 <= collateral_input < len(tx.vin), "Collateral input index is out of range")
    source = tx.vin[collateral_input].prevout
    require((f"{source.hash:064x}", source.n) == tuple(collateral_outpoint), "Collateral input names another vault")
    require(len(tx.wit.vtxinwit) > collateral_input, "Missing collateral witness")
    stack = tx.wit.vtxinwit[collateral_input].scriptWitness.stack
    require(len(stack) == 3, "Expected a normal owner signature, leaf, and control proof")
    _, leaf, control = stack
    fields = list(CScript(leaf))
    require(len(fields) >= 2 and isinstance(fields[-2], bytes) and len(fields[-2]) == 32,
            "Normal leaf does not expose the expected owner public key")
    owner_public = fields[-2]
    secret = owner_key_for(owner_public) if callable(owner_key_for) else owner_key_for.get(owner_public)
    require(secret is not None, "The test has no legitimate private key for this collateral owner")
    require(compute_xonly_pubkey(secret)[0] == owner_public, "Owner secret does not match the leaf")
    index, dd_fields = metadata(tx)
    require(script_number(dd_fields[2]) == 3 and len(dd_fields) == 4, "Expected standard redemption metadata")
    change_before = script_number(dd_fields[3])
    change_after = change_before - extra_cents
    require(change_after >= 100, "Consolidate token inputs first; extra burn must leave at least 100 cents change")
    outputs_before = [output.serialize() for output in tx.vout]
    tx.vout[index].scriptPubKey = CScript([OP_RETURN, b"DD", dd_fields[2], CScriptNum(change_after)])
    require(all(output.serialize() == outputs_before[i] for i, output in enumerate(tx.vout) if i != index),
            "An output other than the DD metadata changed")
    spent = []
    token_input = 0
    for txin in tx.vin:
        txid = f"{txin.prevout.hash:064x}"
        coin = rpc("gettxout", txid, txin.prevout.n, False)
        require(coin is not None, "Source input is absent from the confirmed UTXO set")
        script = bytes.fromhex(coin["scriptPubKey"]["hex"])
        amount = satoshis(coin["value"])
        spent.append(CTxOut(amount, script))
        source_tx = source_transactions.get(txid)
        if source_tx is not None:
            if isinstance(source_tx, str):
                source_tx = tx_from_hex(source_tx)
            source_tx.rehash()
            require(source_tx.hash == txid, "Source transaction registry has the wrong bytes")
            token_input += token_outputs(source_tx).get(txin.prevout.n, 0)
        elif amount == 0 and len(script) == 34 and script[:2] == b"\x51\x20":
            raise ValueError("DD source transaction missing from the independent amount registry")
    require(token_input - change_before == principal_cents, "Original is not the expected normal full-principal burn")
    require(token_input - sum(token_outputs(tx).values()) == principal_cents + extra_cents, "Wrong extra burn")
    tx.wit.vtxinwit = [CTxInWitness() for _ in tx.vin]
    for txin in tx.vin:
        txin.scriptSig = b""
    signed = rpc("signrawtransactionwithwallet", tx.serialize().hex(), [], "DEFAULT")
    tx = tx_from_hex(signed["hex"])
    while len(tx.wit.vtxinwit) < len(tx.vin):
        tx.wit.vtxinwit.append(CTxInWitness())
    require(all(tx.wit.vtxinwit[i].scriptWitness.stack or tx.vin[i].scriptSig
                for i in range(len(tx.vin)) if i != collateral_input),
            "Wallet did not sign every token and fee input")
    digest = TaprootSignatureHash(tx, spent, 0, input_index=collateral_input,
                                  scriptpath=True, script=CScript(leaf))
    tx.wit.vtxinwit[collateral_input].scriptWitness.stack = [sign_schnorr(secret, digest), leaf, control]
    tx.rehash()
    raw = tx.serialize().hex()
    acceptance = rpc("testmempoolaccept", [raw], 0)[0]
    require(acceptance.get("allowed") is True, "Re-signed extra burn rejected: " + str(acceptance))
    return {"tx": tx, "hex": raw, "txid": tx.hash,
            "token_input_cents": token_input, "change_before_cents": change_before,
            "change_after_cents": change_after, "principal_closed_cents": principal_cents,
            "actual_burn_cents": principal_cents + extra_cents,
            "original_acceptance": original_acceptance, "acceptance": acceptance}

