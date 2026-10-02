"""Confirm an ordinary extra burn using only the private rehearsal wallets."""

def apply_extra_burn_case(lab, phase, price, active):
    from test_framework.messages import tx_from_hex
    from digidollar_thawday_extra_burn import known_owner_keys, token_outputs
    from burn_transaction import extra_burn_redemption, require

    bob = lab.nodes[0]
    ceiling = None if active else lab.state["height"] - 1000
    position_id = lab.state[phase + "_extra_burn_position"]
    position = lab.open_positions[position_id]
    principal = int(position["dd_minted"])
    extra = 2500
    actual_burn = principal + extra
    vaults = [entry for entry in lab.vaults if entry[0] == position_id]
    require(len(vaults) == 1 and vaults[0][2] == principal, "Extra-burn vault is missing or ambiguous")
    collateral_outpoint = tuple(vaults[0][:2])
    require(bob.rpc("getblockcount") > position["unlock_height"], "Extra-burn vault has not matured")
    require(active or bob.rpc("getblockcount") < ceiling, "No pre-Thaw extra-burn block budget remains")
    lab.await_quote(price, ceiling)
    lab.sync()
    balances_before = [node.balance() for node in lab.nodes]
    tokens_before, principal_before = lab.expected_supply, lab.expected_principal
    vaults_before = len(lab.open_positions)
    require(bob.rpc("getdigidollarbalance", wallet=True)["unconfirmed"] == 0,
            "Consolidation requires no unconfirmed Bob tokens")
    require(balances_before[0] >= actual_burn + 100, "Bob needs enough tokens for extra burn and one dollar change")
    inputs = bob.rpc("listdigidollarunspent", wallet=True)
    require(inputs and all(row["confirmations"] >= 1 and row["spendable"] and row["safe"] for row in inputs),
            "Bob has token inputs that are not confirmed, safe, and spendable")
    require(sum(row["amount"] for row in inputs) == balances_before[0], "Listed token inputs do not equal Bob's confirmed balance")
    selected = [{"txid": row["txid"], "vout": row["vout"]} for row in inputs]
    destination = bob.rpc("getdigidollaraddress", wallet=True)
    consolidation = bob.rpc("senddigidollar", destination, balances_before[0],
                            "Thaw Day extra-burn input consolidation", 0, selected, "cents", wallet=True)
    consolidation_id = lab.confirm(bob, consolidation, ceiling)
    consolidation_tx = tx_from_hex(lab.dd_transactions[consolidation_id])
    outputs = token_outputs(consolidation_tx)
    require(len(outputs) == 1 and sum(outputs.values()) == balances_before[0], "Consolidation did not create exactly one whole-balance DD output")
    token_outpoint = (consolidation_id, next(iter(outputs)))
    require([node.balance() for node in lab.nodes] == balances_before, "Self-consolidation changed a DD balance")
    lab.note(phase + ": extra-burn token inputs consolidated", {
        "txid": consolidation_id, "selected_inputs": selected,
        "token_outpoint": token_outpoint, "amount_cents": balances_before[0]})

    # Keep the original private even if any later assertion fails. Broadcasting
    # is restored only after a confirmed replacement conflicts the original.
    previous_args = list(bob.record.get("extra_args", []))
    base_args = [arg for arg in previous_args if not arg.startswith("-walletbroadcast")
                 and not arg.startswith("-nowalletbroadcast")]
    bob.record["extra_args"] = base_args + ["-walletbroadcast=0"]
    lab.save_state()
    bob.stop()
    bob.start()
    bob.start_oracles()
    lab.sync()
    lab.await_quote(price, ceiling)
    original_id = None
    try:
        mempool_before = sorted(bob.rpc("getrawmempool"))
        ordinary = bob.rpc("redeemdigidollar", position_id, principal, wallet=True)
        original_id = ordinary["txid"]
        require(ordinary["err_active"] is False and ordinary["required_dd_burn"] == principal,
                "Extra burn needs a healthy normal full-principal redemption")
        require(sorted(bob.rpc("getrawmempool")) == mempool_before,
                "Private original changed the mempool")
        raw_original = bob.rpc("gettransaction", original_id, wallet=True)["hex"]
        original = tx_from_hex(raw_original)
        require(any((f"{txin.prevout.hash:064x}", txin.prevout.n) == token_outpoint for txin in original.vin),
                "Normal redemption did not spend the consolidated token output")
        original_path = lab.run / (phase + "-extra-burn-original.hex")
        original_path.write_text(raw_original + "\n")
        lab.state[phase + "_extra_burn"] = {"status": "PRIVATE_ORIGINAL", "position_id": position_id,
                                            "original_txid": original_id}
        lab.save_state()
        wallet_methods = {"signrawtransactionwithwallet"}
        rpc = lambda method, *params: bob.rpc(method, *params, wallet=method in wallet_methods)
        replacement = extra_burn_redemption(rpc, raw_original, principal, extra,
                                           lab.dd_transactions, known_owner_keys(), collateral_outpoint)
        require(replacement["txid"] != original_id, "Extra burn did not change the transaction id")
        require(replacement["token_input_cents"] == balances_before[0], "Redemption selected an unexpected token amount")
        replacement_path = lab.run / (phase + "-extra-burn-replacement.hex")
        replacement_path.write_text(replacement["hex"] + "\n")
        lab.note(phase + ": private normal and re-signed extra burn both admitted", {
            "original_txid": original_id, "replacement_txid": replacement["txid"],
            "original_absent_from_mempool": original_id not in bob.rpc("getrawmempool"),
            "original_acceptance": replacement["original_acceptance"], "replacement_acceptance": replacement["acceptance"],
            "original_change_cents": replacement["change_before_cents"],
            "replacement_change_cents": replacement["change_after_cents"],
            "required_normal_burn_cents": principal, "actual_burn_cents": actual_burn})
        # Control: an ordinary DGB payment confirms under the same signed price.
        # Bob's wallet is not broadcasting here, so Alice sends it to Bob.
        alice = lab.nodes[1]
        control = alice.rpc("sendtoaddress", bob.rpc("getnewaddress", "extra-burn control", "bech32", wallet=True), 1, wallet=True)
        control_id = lab.confirm(alice, control, ceiling, record=False)
        lab.await_quote(price, ceiling)
        sent = bob.rpc("sendrawtransaction", replacement["hex"], 0)
        require(sent == replacement["txid"] and original_id not in bob.rpc("getrawmempool"),
                "Broadcast did not submit only the replacement")
        # Diagnostic only, non-fatal. getblocktemplate includes oracle-priced DD
        # txs only when the caller passes the "digidollar-oracle" rule; the lab
        # miner (generatetoaddress) opts into that by default, so this mirrors
        # what it will actually build.
        try:
            template = bob.rpc("getblocktemplate", {"rules": ["segwit", "digidollar-oracle"]})
            lab.note(phase + ": miner template right after submitting the replacement", {
                "control_txid": control_id, "template_height": template["height"],
                "replacement_in_template": sent in [entry["txid"] for entry in template["transactions"]]})
        except Exception as diagnostic_error:
            lab.note(phase + ": template diagnostic unavailable", {"error": str(diagnostic_error)})
        # Bob restarted with broadcasting off, so its MuSig2 signing session began
        # cold. await_quote above already waited for a signed price mined after the
        # restart, so the session is warm and the miner (which opts into
        # oracle-priced DD txs by default) confirms the redemption on the next
        # block. Keep a small extra budget in case the epoch is mid-settle.
        confirmed = lab.confirm(bob, sent, ceiling, attempts=24)
        require(confirmed == sent, "Confirmed txid does not match the replacement")
        require(bob.rpc("gettxout", *collateral_outpoint, False) is None, "Extra burn left the vault unspent")
        expected_balances = list(balances_before)
        expected_balances[0] -= actual_burn
        require([node.balance() for node in lab.nodes] == expected_balances,
                "Extra burn changed wallet DD balances by the wrong amount")
        original_state = bob.rpc("gettransaction", original_id, wallet=True)
        require(original_state["confirmations"] < 0, "Original is not conflicted after replacement confirmation")
        require(sent in original_state.get("walletconflicts", []), "Original does not identify the confirmed replacement conflict")
        require(original_id not in bob.rpc("getrawmempool"), "Conflicted original appeared in mempool")
        lab.expected_supply -= actual_burn
        lab.expected_principal -= principal
        del lab.open_positions[position_id]
        lab.state[phase + "_extra_burn"] = {"status": "CONFIRMED", "position_id": position_id,
                                            "original_txid": original_id, "replacement_txid": sent,
                                            "actual_burn_cents": actual_burn, "principal_closed_cents": principal}
        lab.save_state()
        lab.note(phase + ": normal redemption burned extra DD without reducing extra principal", {
            "replacement_txid": sent, "original_confirmations": original_state["confirmations"],
            "walletconflicts": original_state.get("walletconflicts", []),
            "supply_before_cents": tokens_before, "supply_after_cents": lab.expected_supply,
            "principal_before_cents": principal_before, "principal_after_cents": lab.expected_principal,
            "vaults_before": vaults_before, "vaults_after": len(lab.open_positions),
            "wallet_balances_before": balances_before, "wallet_balances_after": expected_balances})
    except BaseException as error:
        lab.note(phase + ": extra-burn case stopped with original broadcasting still disabled", {
            "original_txid": original_id, "error": str(error), "walletbroadcast": 0})
        raise

    bob.stop()
    bob.record["extra_args"] = base_args + ["-walletbroadcast=1"]
    lab.save_state()
    bob.start()
    bob.start_oracles()
    lab.sync()
    lab.await_quote(price, ceiling)
    require(bob.rpc("gettransaction", original_id, wallet=True)["confirmations"] < 0,
            "Original lost its conflict after broadcast was restored")
    require(original_id not in bob.rpc("getrawmempool"), "Original rebroadcast after restart")
    require([node.balance() for node in lab.nodes] == expected_balances,
            "Extra-burn balances changed after broadcasting was restored")
    lab.note(phase + ": extra-burn restart preserved conflict and balances", {
        "original_txid": original_id, "replacement_txid": sent, "walletbroadcast": 1,
        "wallet_balances": expected_balances})
    return lab.state[phase + "_extra_burn"]
