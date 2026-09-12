// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <qt/transactionrecord.h>

#include <chain.h>
#include <consensus/digidollar.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <wallet/types.h>

#include <stdint.h>

#include <map>
#include <vector>

#include <QDateTime>

using wallet::ISMINE_NO;
using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

namespace {

//! True for the output that holds a mint's locked collateral: the first
//! output of the mint, a taproot output carrying real DGB.
bool IsMintCollateralOutput(const CTxOut& txout)
{
    return txout.nValue > 0 && txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1;
}

} // namespace

bool DigiDollarTxFacts::IsTokenOutput(const CTxOut& txout)
{
    return txout.nValue == 0 && txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1;
}

DigiDollarTxFacts::MintFacts DigiDollarTxFacts::ReadMintFacts(const CTransaction& tx)
{
    // A mint writes its facts in one data output, in this order: the two
    // letters DD, the transaction type, the DigiDollar amount in cents, the
    // block height the collateral unlocks at, the lock tier the wallet chose,
    // and the owner's key. A tier of zero is written as an empty push, so the
    // reader has to tell "written as zero" apart from "not written at all".
    MintFacts facts;

    for (const CTxOut& txout : tx.vout) {
        const CScript& script = txout.scriptPubKey;
        if (script.empty() || script[0] != OP_RETURN) continue;

        auto pc = script.begin();
        opcodetype opcode;
        std::vector<unsigned char> data;

        if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) continue;
        if (!script.GetOp(pc, opcode, data)) continue;
        if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;
        if (!script.GetOp(pc, opcode, data)) return facts;

        try {
            if (CScriptNum(data, false).getint() != static_cast<int>(DigiDollar::DD_TX_MINT)) return facts;

            if (!script.GetOp(pc, opcode, data)) return facts;
            facts.dd_cents = CScriptNum(data, false).GetInt64();
            facts.have_dd_cents = facts.dd_cents > 0;

            if (!script.GetOp(pc, opcode, data)) return facts;
            facts.unlock_height = CScriptNum(data, false).GetInt64();
            facts.have_unlock_height = facts.unlock_height > 0;

            if (script.GetOp(pc, opcode, data) && data.size() <= 4) {
                facts.lock_tier = CScriptNum(data, false).getint();
                facts.have_lock_tier = facts.lock_tier >= 0 && facts.lock_tier <= 9;
            }
        } catch (const scriptnum_error&) {
            return facts;
        }
        return facts;
    }
    return facts;
}

std::map<unsigned int, CAmount> DigiDollarTxFacts::TokenAmountsByOutput(const CTransaction& tx, DigiDollar::DigiDollarTxType ddTxType)
{
    std::vector<CAmount> ddAmounts;

    for (const CTxOut& txout : tx.vout) {
        const CScript& script = txout.scriptPubKey;
        if (script.empty() || script[0] != OP_RETURN) continue;

        auto pc = script.begin();
        opcodetype opcode;
        std::vector<unsigned char> data;

        if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) continue;
        if (!script.GetOp(pc, opcode, data)) continue;
        if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;
        if (!script.GetOp(pc, opcode, data)) continue;

        int txType = 0;
        try {
            CScriptNum txTypeNum(data, false);
            txType = txTypeNum.getint();
        } catch (const scriptnum_error&) {
            break;
        }
        if (txType != static_cast<int>(ddTxType)) break;

        while (script.GetOp(pc, opcode, data) && !data.empty()) {
            try {
                CScriptNum amountNum(data, false);
                const CAmount amount = amountNum.GetInt64();
                if (amount > 0) ddAmounts.push_back(amount);
            } catch (const scriptnum_error&) {
                break;
            }
        }
        break;
    }

    std::map<unsigned int, CAmount> amountsByOutput;
    size_t ddOutputIndex = 0;
    for (unsigned int i = 0; i < tx.vout.size(); ++i) {
        const CTxOut& txout = tx.vout[i];
        if (!IsTokenOutput(txout)) continue;

        if (ddTxType == DigiDollar::DD_TX_MINT && i == 0) {
            continue; // vault output, not the DD token output
        }

        const size_t amountIndex = (ddTxType == DigiDollar::DD_TX_MINT) ? 0 : ddOutputIndex;
        if (amountIndex < ddAmounts.size()) {
            amountsByOutput[i] = ddAmounts[amountIndex];
        }
        ++ddOutputIndex;
    }
    return amountsByOutput;
}

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction()
{
    // There are currently no cases where we hide transactions, but
    // we may want to use this in the future for things like RBF.
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const interfaces::WalletTx& wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.time;
    CAmount nCredit = wtx.credit;
    CAmount nDebit = wtx.debit;
    CAmount nNet = nCredit - nDebit;
    uint256 hash = wtx.tx->GetHash();
    std::map<std::string, std::string> mapValue = wtx.value_map;

    bool involvesWatchAddress = false;
    isminetype fAllFromMe = ISMINE_SPENDABLE;
    bool any_from_me = false;
    if (wtx.is_coinbase) {
        fAllFromMe = ISMINE_NO;
    } else {
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllFromMe > mine) fAllFromMe = mine;
            if (mine) any_from_me = true;
        }
    }

    // Check if this is a DigiDollar transaction and get its type (needed for special handling)
    bool isDDTransaction = DigiDollar::HasDigiDollarMarker(*wtx.tx);
    DigiDollar::DigiDollarTxType ddTxType = DigiDollar::GetDigiDollarTxType(*wtx.tx);
    const std::map<unsigned int, CAmount> ddAmountsByOutput = isDDTransaction ? DigiDollarTxFacts::TokenAmountsByOutput(*wtx.tx, ddTxType) : std::map<unsigned int, CAmount>{};

    // Special handling for DigiDollar REDEEM transactions
    // These have locked collateral inputs that aren't recognized as "mine" by standard wallet,
    // so they would otherwise fall through to "mixed transaction" handling
    if (isDDTransaction && ddTxType == DigiDollar::DD_TX_REDEEM) {
        for (const isminetype mine : wtx.txout_is_mine) {
            if (mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
        }

        // For REDEEM transactions, show collateral return and fee change as SEPARATE entries.
        // vout[0] is always the collateral return (the locked DGB coming back).
        // Any additional DGB outputs that belong to us are fee change.
        // Previously these were summed into one total, making it look like the user
        // got more DGB back than they locked (the "extra" was just fee change).
        if (wtx.tx->vout.size() > 0) {
            const CTxOut& txout = wtx.tx->vout[0];
            isminetype mine = wtx.txout_is_mine[0];

            // Record 1: Collateral return (vout[0] only — exact locked amount)
            if (mine && txout.nValue > 0) {
                TransactionRecord sub(hash, nTime);
                sub.idx = 0;
                sub.credit = txout.nValue;
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                sub.type = TransactionRecord::DDCollateralReturn;
                sub.address = EncodeDestination(wtx.txout_address[0]);
                parts.append(sub);
            }

            // Record 2: Fee change (any other DGB outputs belonging to us)
            // These are leftover DGB from the input used to pay the transaction fee.
            CAmount feeChange = 0;
            int feeChangeIdx = -1;
            for (unsigned int i = 1; i < wtx.tx->vout.size(); i++) {
                if (wtx.txout_is_mine[i] && wtx.tx->vout[i].nValue > 0) {
                    feeChange += wtx.tx->vout[i].nValue;
                    if (feeChangeIdx < 0) feeChangeIdx = i;
                }
            }
            if (feeChange > 0 && feeChangeIdx >= 0) {
                TransactionRecord changeSub(hash, nTime);
                changeSub.idx = feeChangeIdx;
                changeSub.credit = feeChange;
                changeSub.involvesWatchAddress = involvesWatchAddress;
                changeSub.type = TransactionRecord::RecvWithAddress;
                changeSub.address = EncodeDestination(wtx.txout_address[feeChangeIdx]);
                parts.append(changeSub);
            }

            // Record 3: DigiDollars handed back. A redemption has to burn the
            // whole vault, but the wallet may have had to spend more
            // DigiDollars than that to reach the amount, and the remainder
            // comes back on a DigiDollar output. It carries no DGB, so it
            // cannot double count the collateral above. Without this row the
            // wallet shows those DigiDollars nowhere at all.
            for (unsigned int i = 1; i < wtx.tx->vout.size(); i++) {
                if (!wtx.txout_is_mine[i]) continue;
                if (!DigiDollarTxFacts::IsTokenOutput(wtx.tx->vout[i])) continue;

                auto amount_it = ddAmountsByOutput.find(i);
                if (amount_it == ddAmountsByOutput.end()) continue;

                TransactionRecord ddChangeSub(hash, nTime);
                ddChangeSub.idx = i;
                ddChangeSub.involvesWatchAddress = wtx.txout_is_mine[i] & ISMINE_WATCH_ONLY;
                ddChangeSub.type = TransactionRecord::DDChangeReturned;
                ddChangeSub.address = EncodeDestination(wtx.txout_address[i]);
                ddChangeSub.ddAmount = amount_it->second;
                parts.append(ddChangeSub);
            }
        }
        return parts;
    }

    // A DigiDollar mint locks DGB in a vault and creates DigiDollars. It gets
    // its own rows, because the general code further down sees the mint's own
    // DigiDollar output as both a payment out and a payment in, and shows the
    // same new money twice: once as sent and once as received.
    //
    // A mint that the wallet paid for produces up to three rows:
    //   - the DGB locked in the vault, which is the collateral on its own,
    //   - the network fee, on its own row,
    //   - the DigiDollars created, with no DGB on that row.
    // The fee gets its own row so that the locked amount shown is exactly what
    // the vault holds. Adding the fee to it, as the general code does, tells
    // the owner more DGB is locked up than really is.
    if (isDDTransaction && ddTxType == DigiDollar::DD_TX_MINT && any_from_me) {
        for (const isminetype mine : wtx.txout_is_mine) {
            if (mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
        }

        // The fee is inputs minus outputs, so it is only known when every
        // input was ours. On the rare mint that also spends someone else's
        // coins we leave the fee out rather than guess at it.
        const bool paid_every_input = fAllFromMe != ISMINE_NO;
        const CAmount nTxFee = paid_every_input ? nDebit - wtx.tx->GetValueOut() : 0;

        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            const CTxOut& txout = wtx.tx->vout[i];

            // The data output carries the mint's numbers, not money.
            if (!txout.scriptPubKey.empty() && txout.scriptPubKey[0] == OP_RETURN) continue;
            if (wtx.txout_is_change[i]) continue;

            if (i == 0 && IsMintCollateralOutput(txout)) {
                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.involvesWatchAddress = involvesWatchAddress;
                sub.type = TransactionRecord::DDTimeLockCollateral;
                sub.address = EncodeDestination(wtx.txout_address[i]);
                sub.debit = -txout.nValue;
                parts.append(sub);
                continue;
            }

            if (DigiDollarTxFacts::IsTokenOutput(txout)) {
                auto amount_it = ddAmountsByOutput.find(i);
                // Without the amount from the data output there is nothing
                // true to put on the row, and a row reading zero dollars would
                // be worse than no row at all.
                if (amount_it == ddAmountsByOutput.end()) continue;

                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.involvesWatchAddress = wtx.txout_is_mine[i] & ISMINE_WATCH_ONLY;
                sub.type = TransactionRecord::DDMint;
                sub.address = EncodeDestination(wtx.txout_address[i]);
                sub.ddAmount = amount_it->second;
                parts.append(sub);
                continue;
            }

            // Any other DGB output that is not change really did leave the
            // wallet, so it is shown as an ordinary payment.
            if (txout.nValue > 0) {
                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.involvesWatchAddress = involvesWatchAddress;
                if (!std::get_if<CNoDestination>(&wtx.txout_address[i])) {
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                } else {
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }
                sub.debit = -txout.nValue;
                parts.append(sub);
            }
        }

        if (nTxFee > 0) {
            TransactionRecord sub(hash, nTime);
            // There is no output for a fee, so this row sorts after the last
            // real one.
            sub.idx = static_cast<int>(wtx.tx->vout.size());
            sub.involvesWatchAddress = involvesWatchAddress;
            sub.type = TransactionRecord::DDSendFee;
            sub.debit = -nTxFee;
            parts.append(sub);
        }

        return parts;
    }

    // Special handling for DigiDollar TRANSFER transactions
    // DD token inputs (0-value P2TR) may not be recognized as ISMINE_SPENDABLE,
    // causing fAllFromMe to be false while any_from_me is true (from the DGB fee inputs).
    // Without this, the TX falls to the "mixed debit" path and shows as "(n/a)".
    if (isDDTransaction && ddTxType == DigiDollar::DD_TX_TRANSFER && any_from_me && !fAllFromMe) {
        for (const isminetype mine : wtx.txout_is_mine) {
            if (mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
        }

        CAmount nTxFee = nDebit - wtx.tx->GetValueOut();

        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            const CTxOut& txout = wtx.tx->vout[i];

            // Skip OP_RETURN outputs
            if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN)
                continue;

            // Skip change outputs
            if (wtx.txout_is_change[i])
                continue;

            const bool isDDTokenOutput = DigiDollarTxFacts::IsTokenOutput(txout);

            if (isDDTokenOutput) {
                // DD send record
                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.involvesWatchAddress = involvesWatchAddress;
                sub.type = TransactionRecord::DDSend;
                sub.address = EncodeDestination(wtx.txout_address[i]);
                auto amount_it = ddAmountsByOutput.find(i);
                if (amount_it != ddAmountsByOutput.end()) {
                    sub.ddAmount = -amount_it->second;
                }
                sub.debit = 0;
                parts.append(sub);
            }
        }

        // Create a single DDSendFee record for the DGB fee portion
        if (nTxFee > 0 || nDebit > 0) {
            TransactionRecord sub(hash, nTime);
            sub.idx = parts.size();
            sub.involvesWatchAddress = involvesWatchAddress;
            sub.type = TransactionRecord::DDSendFee;
            sub.debit = -nDebit; // Total DGB spent (fee + any non-change DGB outputs)
            sub.credit = nCredit; // DGB change returned
            parts.append(sub);
        }

        // Also add credit records for received DD tokens in this TX
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            const CTxOut& txout = wtx.tx->vout[i];
            isminetype mine = wtx.txout_is_mine[i];
            if (!mine) continue;

            const bool isDDTokenOutput = DigiDollarTxFacts::IsTokenOutput(txout);

            if (isDDTokenOutput) {
                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.credit = 0;
                auto amount_it = ddAmountsByOutput.find(i);
                if (amount_it != ddAmountsByOutput.end()) {
                    sub.ddAmount = amount_it->second;
                }
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                sub.type = TransactionRecord::DDRecv;
                sub.address = EncodeDestination(wtx.txout_address[i]);
                parts.append(sub);
            }
        }

        return parts;
    }

    if (fAllFromMe || !any_from_me) {
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
        }

        CAmount nTxFee = nDebit - wtx.tx->GetValueOut();

        for(unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            const CTxOut& txout = wtx.tx->vout[i];

            // Skip OP_RETURN outputs entirely (they have no value and are just data)
            if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) {
                continue;
            }

            // Check if this is a DD token output (0-value P2TR)
            // P2TR outputs start with OP_1 (0x51) and are 34 bytes
            bool isDDTokenOutput = isDDTransaction && DigiDollarTxFacts::IsTokenOutput(txout);

            if (fAllFromMe) {
                // Change is only really possible if we're the sender
                // Otherwise, someone just sent digibytes to a change address, which should be shown
                if (wtx.txout_is_change[i]) {
                    continue;
                }

                //
                // Debit
                //

                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.involvesWatchAddress = involvesWatchAddress;

                // Check if this is a DigiDollar output (0-value P2TR) - DD Send
                if (isDDTokenOutput) {
                    sub.type = TransactionRecord::DDSend;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                    auto amount_it = ddAmountsByOutput.find(i);
                    if (amount_it != ddAmountsByOutput.end()) {
                        sub.ddAmount = -amount_it->second;
                    }
                }
                // Check if this is a DigiDollar collateral output (MINT transaction, vout 0)
                // Collateral is the first output (index 0) in a mint tx, has value > 0, P2TR
                else if (isDDTransaction && ddTxType == DigiDollar::DD_TX_MINT &&
                    i == 0 && txout.nValue > 0 &&
                    txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == 0x51) {
                    sub.type = TransactionRecord::DDTimeLockCollateral;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else if (!std::get_if<CNoDestination>(&wtx.txout_address[i]))
                {
                    // Sent to DigiByte Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }

                CAmount nValue = txout.nValue;
                /* Add fee to first output */
                if (nTxFee > 0)
                {
                    nValue += nTxFee;
                    nTxFee = 0;
                }
                sub.debit = -nValue;

                parts.append(sub);
            }

            isminetype mine = wtx.txout_is_mine[i];
            if(mine)
            {
                //
                // Credit
                //

                TransactionRecord sub(hash, nTime);
                sub.idx = i; // vout index
                sub.credit = txout.nValue;
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;

                // Check for DigiDollar-related received outputs
                if (isDDTokenOutput) {
                    // Received DigiDollar (0-value P2TR in DD transaction)
                    sub.type = TransactionRecord::DDRecv;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                    auto amount_it = ddAmountsByOutput.find(i);
                    if (amount_it != ddAmountsByOutput.end()) {
                        sub.ddAmount = amount_it->second;
                    }
                }
                else if (isDDTransaction && ddTxType == DigiDollar::DD_TX_REDEEM &&
                         txout.nValue > 0 && i == 0) {
                    // Received collateral back (redemption) - first output with DGB value in REDEEM tx
                    sub.type = TransactionRecord::DDCollateralReturn;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else if (wtx.txout_address_is_mine[i])
                {
                    // Received by DigiByte Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                }
                if (wtx.is_coinbase)
                {
                    // Generated
                    sub.type = TransactionRecord::Generated;
                }

                parts.append(sub);
            }
        }
    } else {
        //
        // Mixed debit transaction, can't break down payees
        //
        parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, 0));
        parts.last().involvesWatchAddress = involvesWatchAddress;
    }

    return parts;
}

void TransactionRecord::updateStatus(const interfaces::WalletTxStatus& wtx, const uint256& block_hash, int numBlocks, int64_t block_time)
{
    // Determine transaction status

    // Sort order, unrecorded transactions sort to the top
    int typesort;
    switch (type) {
    case SendToAddress: case SendToOther:
        typesort = 2; break;
    case RecvWithAddress: case RecvFromOther:
        typesort = 3; break;
    default:
        typesort = 9;
    }
    status.sortKey = strprintf("%010d-%01d-%010u-%03d-%d",
        wtx.block_height,
        wtx.is_coinbase ? 1 : 0,
        wtx.time_received,
        idx,
        typesort);
    status.countsForBalance = wtx.is_trusted && !(wtx.blocks_to_maturity > 0);
    status.depth = wtx.depth_in_main_chain;
    status.m_cur_block_hash = block_hash;

    // For generated transactions, determine maturity
    if (type == TransactionRecord::Generated) {
        if (wtx.blocks_to_maturity > 0)
        {
            status.status = TransactionStatus::Immature;

            if (wtx.is_in_main_chain)
            {
                status.matures_in = wtx.blocks_to_maturity;
            }
            else
            {
                status.status = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    else
    {
        if (status.depth < 0)
        {
            status.status = TransactionStatus::Conflicted;
        }
        else if (status.depth == 0)
        {
            status.status = TransactionStatus::Unconfirmed;
            if (wtx.is_abandoned)
                status.status = TransactionStatus::Abandoned;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Confirming;
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    status.needsUpdate = false;
}

bool TransactionRecord::statusUpdateNeeded(const uint256& block_hash) const
{
    assert(!block_hash.IsNull());
    return status.m_cur_block_hash != block_hash || status.needsUpdate;
}

QString TransactionRecord::getTxHash() const
{
    return QString::fromStdString(hash.ToString());
}

int TransactionRecord::getOutputIndex() const
{
    return idx;
}
