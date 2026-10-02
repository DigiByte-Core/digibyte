// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifdef HAVE_CONFIG_H
#include <config/digibyte-config.h>
#endif

#include <qt/transactiondesc.h>

#include <qt/digibyteunits.h>
#include <qt/guiutil.h>
#include <qt/paymentserver.h>
#include <qt/transactionrecord.h>

#include <common/system.h>
#include <consensus/consensus.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <logging.h>
#include <policy/policy.h>
#include <script/standard.h>
#include <validation.h>
#include <wallet/types.h>

#include <stdint.h>
#include <string>
#include <variant>

#include <QLatin1String>

using wallet::ISMINE_ALL;
using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

namespace {

//! DigiDollar amounts are held in cents. Show them the way money is written.
QString FormatDigiDollar(CAmount cents)
{
    return QStringLiteral("$") + QString::number(cents / 100.0, 'f', 2);
}

//! How long the collateral of a mint is locked for. The same ten names are
//! shown by the mint panel, the redeem panel and the positions table; if one
//! of them changes, change them all together.
QString LockPeriodName(int tier)
{
    switch (tier) {
    case 0: return QObject::tr("1 hour");
    case 1: return QObject::tr("30 days");
    case 2: return QObject::tr("3 months");
    case 3: return QObject::tr("6 months");
    case 4: return QObject::tr("1 year");
    case 5: return QObject::tr("2 years");
    case 6: return QObject::tr("3 years");
    case 7: return QObject::tr("5 years");
    case 8: return QObject::tr("7 years");
    case 9: return QObject::tr("10 years");
    default: return QString();
    }
}

//! The line to print when the transaction simply does not carry a number. It
//! must never be shown as a zero, because a zero looks like a real amount.
QString NotInThisTransaction()
{
    return QObject::tr("not recorded in this transaction");
}

} // namespace

QString TransactionDesc::FormatTxStatus(const interfaces::WalletTxStatus& status, bool inMempool)
{
    int depth = status.depth_in_main_chain;
    if (depth < 0) {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This status
            represents an unconfirmed transaction that conflicts with a confirmed
            transaction. */
        return tr("conflicted with a transaction with %1 confirmations").arg(-depth);
    } else if (depth == 0) {
        QString s;
        if (inMempool) {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This status
                represents an unconfirmed transaction that is in the memory pool. */
            s = tr("0/unconfirmed, in memory pool");
        } else {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This status
                represents an unconfirmed transaction that is not in the memory pool. */
            s = tr("0/unconfirmed, not in memory pool");
        }
        if (status.is_abandoned) {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This
                status represents an abandoned transaction. */
            s += QLatin1String(", ") + tr("abandoned");
        }
        return s;
    } else if (depth < 6) {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This
            status represents a transaction confirmed in at least one block,
            but less than 6 blocks. */
        return tr("%1/unconfirmed").arg(depth);
    } else {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This status
            represents a transaction confirmed in 6 or more blocks. */
        return tr("%1 confirmations").arg(depth);
    }
}

// Takes an encoded PaymentRequest as a string and tries to find the Common Name of the X.509 certificate
// used to sign the PaymentRequest.
bool GetPaymentRequestMerchant(const std::string& pr, QString& merchant)
{
    // Search for the supported pki type strings
    if (pr.find(std::string({0x12, 0x0b}) + "x509+sha256") != std::string::npos || pr.find(std::string({0x12, 0x09}) + "x509+sha1") != std::string::npos) {
        // We want the common name of the Subject of the cert. This should be the second occurrence
        // of the bytes 0x0603550403. The first occurrence of those is the common name of the issuer.
        // After those bytes will be either 0x13 or 0x0C, then length, then either the ascii or utf8
        // string with the common name which is the merchant name
        size_t cn_pos = pr.find({0x06, 0x03, 0x55, 0x04, 0x03});
        if (cn_pos != std::string::npos) {
            cn_pos = pr.find({0x06, 0x03, 0x55, 0x04, 0x03}, cn_pos + 5);
            if (cn_pos != std::string::npos) {
                cn_pos += 5;
                if (pr[cn_pos] == 0x13 || pr[cn_pos] == 0x0c) {
                    cn_pos++; // Consume the type
                    int str_len = pr[cn_pos];
                    cn_pos++; // Consume the string length
                    merchant = QString::fromUtf8(pr.data() + cn_pos, str_len);
                    return true;
                }
            }
        }
    }
    return false;
}

QString TransactionDesc::toHTML(interfaces::Node& node, interfaces::Wallet& wallet, TransactionRecord* rec, DigiByteUnit unit)
{
    int numBlocks;
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm orderForm;
    bool inMempool;
    interfaces::WalletTx wtx = wallet.getWalletTxDetails(rec->hash, status, orderForm, inMempool, numBlocks);

    QString strHTML;

    strHTML.reserve(4000);
    strHTML += "<html><font face='verdana, arial, helvetica, sans-serif'>";

    int64_t nTime = wtx.time;
    CAmount nCredit = wtx.credit;
    CAmount nDebit = wtx.debit;
    CAmount nNet = nCredit - nDebit;

    strHTML += "<b>" + tr("Status") + ":</b> " + FormatTxStatus(status, inMempool);
    strHTML += "<br>";

    strHTML += "<b>" + tr("Date") + ":</b> " + (nTime ? GUIUtil::dateTimeStr(nTime) : "") + "<br>";

    //
    // From
    //
    if (wtx.is_coinbase)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + tr("Generated") + "<br>";
    }
    else if (wtx.value_map.count("from") && !wtx.value_map["from"].empty())
    {
        // Online transaction
        strHTML += "<b>" + tr("From") + ":</b> " + GUIUtil::HtmlEscape(wtx.value_map["from"]) + "<br>";
    }
    else
    {
        // Offline transaction
        if (nNet > 0)
        {
            // Credit
            CTxDestination address = DecodeDestination(rec->address);
            if (IsValidDestination(address)) {
                std::string name;
                isminetype ismine;
                if (wallet.getAddress(address, &name, &ismine, /* purpose= */ nullptr))
                {
                    strHTML += "<b>" + tr("From") + ":</b> " + tr("unknown") + "<br>";
                    strHTML += "<b>" + tr("To") + ":</b> ";
                    strHTML += GUIUtil::HtmlEscape(rec->address);
                    QString addressOwned = ismine == ISMINE_SPENDABLE ? tr("own address") : tr("watch-only");
                    if (!name.empty())
                        strHTML += " (" + addressOwned + ", " + tr("label") + ": " + GUIUtil::HtmlEscape(name) + ")";
                    else
                        strHTML += " (" + addressOwned + ")";
                    strHTML += "<br>";
                }
            }
        }
    }

    //
    // To
    //
    if (wtx.value_map.count("to") && !wtx.value_map["to"].empty())
    {
        // Online transaction
        std::string strAddress = wtx.value_map["to"];
        strHTML += "<b>" + tr("To") + ":</b> ";
        CTxDestination dest = DecodeDestination(strAddress);
        std::string name;
        if (wallet.getAddress(
                dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
            strHTML += GUIUtil::HtmlEscape(name) + " ";
        strHTML += GUIUtil::HtmlEscape(strAddress) + "<br>";
    }

    //
    // DigiDollar
    //
    // A DigiDollar lives on a taproot output that holds no DGB at all, and the
    // number of DigiDollars it holds is written in a data output of the same
    // transaction. Without the lines below the window shows those outputs as
    // zero DGB and never names the DigiDollars, which is why a mint looked like
    // it moved nothing.
    const bool is_dd_tx = DigiDollar::HasDigiDollarMarker(*wtx.tx);
    const DigiDollar::DigiDollarTxType dd_type = is_dd_tx
        ? DigiDollar::GetDigiDollarTxType(*wtx.tx)
        : DigiDollar::DD_TX_NONE;
    const std::map<unsigned int, CAmount> dd_amounts = is_dd_tx
        ? DigiDollarTxFacts::TokenAmountsByOutput(*wtx.tx, dd_type)
        : std::map<unsigned int, CAmount>{};

    if (dd_type == DigiDollar::DD_TX_MINT) {
        const DigiDollarTxFacts::MintFacts facts = DigiDollarTxFacts::ReadMintFacts(*wtx.tx);

        strHTML += "<b>" + tr("DigiDollar minted") + ":</b> ";
        strHTML += facts.have_dd_cents ? FormatDigiDollar(facts.dd_cents) : NotInThisTransaction();
        strHTML += "<br>";

        // A valid mint has one positive Taproot output holding its collateral.
        for (const CTxOut& output : wtx.tx->vout) {
            CTxDestination destination;
            if (output.nValue <= 0 || !ExtractDestination(output.scriptPubKey, destination) ||
                !std::holds_alternative<WitnessV1Taproot>(destination)) continue;
            strHTML += "<b>" + tr("Collateral locked") + ":</b> " +
                       DigiByteUnits::formatHtmlWithUnit(unit, output.nValue) + "<br>";
            break;
        }

        strHTML += "<b>" + tr("Lock period") + ":</b> ";
        const QString period = facts.have_lock_tier ? LockPeriodName(facts.lock_tier) : QString();
        strHTML += period.isEmpty() ? NotInThisTransaction() : period;
        strHTML += "<br>";

        strHTML += "<b>" + tr("Collateral unlocks at block") + ":</b> ";
        strHTML += facts.have_unlock_height ? QString::number(facts.unlock_height) : NotInThisTransaction();
        strHTML += "<br>";

        // A vault is known by the transaction that created it.
        strHTML += "<b>" + tr("Vault") + ":</b> " + rec->getTxHash() + "<br>";
    } else if (dd_type == DigiDollar::DD_TX_TRANSFER) {
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            if (!DigiDollarTxFacts::IsTokenOutput(wtx.tx->vout[i])) continue;

            const bool to_us = wtx.txout_is_mine[i];
            strHTML += "<b>" + (to_us ? tr("DigiDollar received") : tr("DigiDollar sent")) + ":</b> ";
            const auto amount_it = dd_amounts.find(i);
            strHTML += amount_it != dd_amounts.end() ? FormatDigiDollar(amount_it->second) : NotInThisTransaction();
            if (IsValidDestination(wtx.txout_address[i])) {
                strHTML += " (" + GUIUtil::HtmlEscape(EncodeDestination(wtx.txout_address[i])) + ")";
            }
            strHTML += "<br>";
        }
    } else if (dd_type == DigiDollar::DD_TX_REDEEM) {
        if (!wtx.tx->vout.empty() && wtx.tx->vout[0].nValue > 0) {
            strHTML += "<b>" + tr("Collateral returned") + ":</b> " +
                       DigiByteUnits::formatHtmlWithUnit(unit, wtx.tx->vout[0].nValue) + "<br>";
        }

        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            if (!DigiDollarTxFacts::IsTokenOutput(wtx.tx->vout[i])) continue;
            const auto amount_it = dd_amounts.find(i);
            if (amount_it == dd_amounts.end()) continue;
            strHTML += "<b>" + tr("DigiDollar change returned") + ":</b> " +
                       FormatDigiDollar(amount_it->second) + "<br>";
        }

        // The amount burned is carried by the DigiDollar inputs this
        // transaction spends, which live in earlier transactions, so it cannot
        // be read here. Say so rather than print a zero.
        strHTML += "<b>" + tr("DigiDollar burned") + ":</b> " +
                   tr("held by the inputs this transaction spent, so it is not shown here") + "<br>";

        // The first input of a redemption is always the locked vault, and a
        // vault is known by the transaction that created it.
        if (!wtx.tx->vin.empty()) {
            strHTML += "<b>" + tr("Vault closed") + ":</b> " +
                       QString::fromStdString(wtx.tx->vin[0].prevout.hash.ToString()) + "<br>";
        }
    }

    //
    // Amount
    //
    if (wtx.is_coinbase && nCredit == 0)
    {
        //
        // Coinbase
        //
        CAmount nUnmatured = 0;
        for (const CTxOut& txout : wtx.tx->vout)
            nUnmatured += wallet.getCredit(txout, ISMINE_ALL);
        strHTML += "<b>" + tr("Credit") + ":</b> ";
        if (status.is_in_main_chain)
            strHTML += DigiByteUnits::formatHtmlWithUnit(unit, nUnmatured)+ " (" + tr("matures in %n more block(s)", "", status.blocks_to_maturity) + ")";
        else
            strHTML += "(" + tr("not accepted") + ")";
        strHTML += "<br>";
    }
    else if (nNet > 0)
    {
        //
        // Credit
        //
        strHTML += "<b>" + tr("Credit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, nNet) + "<br>";
    }
    else
    {
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe)
        {
            if(fAllFromMe & ISMINE_WATCH_ONLY)
                strHTML += "<b>" + tr("From") + ":</b> " + tr("watch-only") + "<br>";

            //
            // Debit
            //
            for (unsigned int i = 0; i < wtx.tx->vout.size(); i++)
            {
                const CTxOut& txout = wtx.tx->vout[i];
                // Ignore change
                isminetype toSelf = wtx.txout_is_mine[i];
                if ((toSelf == ISMINE_SPENDABLE) && (fAllFromMe == ISMINE_SPENDABLE))
                    continue;

                // In a DigiDollar transaction the data output and the outputs
                // that hold DigiDollars are worth no DGB. Listing them here as
                // a payment of zero DGB is what produced the empty amounts
                // people reported. The DigiDollar lines above already say what
                // they carry.
                if (is_dd_tx && txout.nValue == 0) continue;
                // The collateral of a mint has its own line above as well.
                if (dd_type == DigiDollar::DD_TX_MINT && i == 0) continue;

                if (!wtx.value_map.count("to") || wtx.value_map["to"].empty())
                {
                    // Offline transaction
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address))
                    {
                        strHTML += "<b>" + tr("To") + ":</b> ";
                        std::string name;
                        if (wallet.getAddress(
                                address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += GUIUtil::HtmlEscape(EncodeDestination(address));
                        if(toSelf == ISMINE_SPENDABLE)
                            strHTML += " (" + tr("own address") + ")";
                        else if(toSelf & ISMINE_WATCH_ONLY)
                            strHTML += " (" + tr("watch-only") + ")";
                        strHTML += "<br>";
                    }
                }

                strHTML += "<b>" + tr("Debit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, -txout.nValue) + "<br>";
                if(toSelf)
                    strHTML += "<b>" + tr("Credit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, txout.nValue) + "<br>";
            }

            if (fAllToMe)
            {
                // Payment to self
                CAmount nChange = wtx.change;
                CAmount nValue = nCredit - nChange;
                strHTML += "<b>" + tr("Total debit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, -nValue) + "<br>";
                strHTML += "<b>" + tr("Total credit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, nValue) + "<br>";
            }

            CAmount nTxFee = nDebit - wtx.tx->GetValueOut();
            if (nTxFee > 0)
                strHTML += "<b>" + tr("Transaction fee") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, -nTxFee) + "<br>";
        }
        else
        {
            //
            // Mixed debit transaction
            //
            auto mine = wtx.txin_is_mine.begin();
            for (const CTxIn& txin : wtx.tx->vin) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Debit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, -wallet.getDebit(txin, ISMINE_ALL)) + "<br>";
                }
            }
            mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Credit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, wallet.getCredit(txout, ISMINE_ALL)) + "<br>";
                }
            }
        }
    }

    strHTML += "<b>" + tr("Net amount") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, nNet, true) + "<br>";

    //
    // Message
    //
    if (wtx.value_map.count("message") && !wtx.value_map["message"].empty())
        strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["message"], true) + "<br>";
    if (wtx.value_map.count("comment") && !wtx.value_map["comment"].empty())
        strHTML += "<br><b>" + tr("Comment") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["comment"], true) + "<br>";

    strHTML += "<b>" + tr("Transaction ID") + ":</b> " + rec->getTxHash() + "<br>";
    strHTML += "<b>" + tr("Transaction total size") + ":</b> " + QString::number(wtx.tx->GetTotalSize()) + " bytes<br>";
    strHTML += "<b>" + tr("Transaction virtual size") + ":</b> " + QString::number(GetVirtualTransactionSize(*wtx.tx)) + " bytes<br>";
    strHTML += "<b>" + tr("Output index") + ":</b> " + QString::number(rec->getOutputIndex()) + "<br>";

    // Message from normal digibyte:URI (digibyte:123...?message=example)
    for (const std::pair<std::string, std::string>& r : orderForm) {
        if (r.first == "Message")
            strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(r.second, true) + "<br>";

        //
        // PaymentRequest info:
        //
        if (r.first == "PaymentRequest")
        {
            QString merchant;
            if (!GetPaymentRequestMerchant(r.second, merchant)) {
                merchant.clear();
            } else {
                merchant += tr(" (Certificate was not verified)");
            }
            if (!merchant.isNull()) {
                strHTML += "<b>" + tr("Merchant") + ":</b> " + GUIUtil::HtmlEscape(merchant) + "<br>";
            }
        }
    }

    if (wtx.is_coinbase)
    {
        quint32 numBlocksToMaturity = COINBASE_MATURITY_2 +  1;
        strHTML += "<br>" + tr("Generated coins must mature %1 blocks before they can be spent. When you generated this block, it was broadcast to the network to be added to the block chain. If it fails to get into the chain, its state will change to \"not accepted\" and it won't be spendable. This may occasionally happen if another node generates a block within a few seconds of yours.").arg(QString::number(numBlocksToMaturity)) + "<br>";
    }

    //
    // Debug view
    //
    if (node.getLogCategories() != BCLog::NONE)
    {
        strHTML += "<hr><br>" + tr("Debug information") + "<br><br>";
        for (const CTxIn& txin : wtx.tx->vin)
            if(wallet.txinIsMine(txin))
                strHTML += "<b>" + tr("Debit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, -wallet.getDebit(txin, ISMINE_ALL)) + "<br>";
        for (const CTxOut& txout : wtx.tx->vout)
            if(wallet.txoutIsMine(txout))
                strHTML += "<b>" + tr("Credit") + ":</b> " + DigiByteUnits::formatHtmlWithUnit(unit, wallet.getCredit(txout, ISMINE_ALL)) + "<br>";

        strHTML += "<br><b>" + tr("Transaction") + ":</b><br>";
        strHTML += GUIUtil::HtmlEscape(wtx.tx->ToString(), true);

        strHTML += "<br><b>" + tr("Inputs") + ":</b>";
        strHTML += "<ul>";

        for (const CTxIn& txin : wtx.tx->vin)
        {
            COutPoint prevout = txin.prevout;

            Coin prev;
            if(node.getUnspentOutput(prevout, prev))
            {
                {
                    strHTML += "<li>";
                    const CTxOut &vout = prev.out;
                    CTxDestination address;
                    if (ExtractDestination(vout.scriptPubKey, address))
                    {
                        std::string name;
                        if (wallet.getAddress(address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += QString::fromStdString(EncodeDestination(address));
                    }
                    strHTML = strHTML + " " + tr("Amount") + "=" + DigiByteUnits::formatHtmlWithUnit(unit, vout.nValue);
                    strHTML = strHTML + " IsMine=" + (wallet.txoutIsMine(vout) & ISMINE_SPENDABLE ? tr("true") : tr("false")) + "</li>";
                    strHTML = strHTML + " IsWatchOnly=" + (wallet.txoutIsMine(vout) & ISMINE_WATCH_ONLY ? tr("true") : tr("false")) + "</li>";
                }
            }
        }

        strHTML += "</ul>";
    }

    strHTML += "</font></html>";
    return strHTML;
}
