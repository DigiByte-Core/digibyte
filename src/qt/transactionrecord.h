// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_QT_TRANSACTIONRECORD_H
#define DIGIBYTE_QT_TRANSACTIONRECORD_H

#include <consensus/amount.h>
#include <consensus/digidollar.h>
#include <uint256.h>

#include <map>

#include <QList>
#include <QString>

class CTransaction;
class CTxOut;

namespace interfaces {
class Node;
class Wallet;
struct WalletTx;
struct WalletTxStatus;
}

/** Reading the DigiDollar facts out of a transaction.
 *
 * A DigiDollar is held on a taproot output worth no DGB at all. How many
 * DigiDollars that output holds is written in a data output of the same
 * transaction, so both the transaction list and the details window have to
 * read it from there. These helpers are that reader.
 */
namespace DigiDollarTxFacts {

/** True for the zero value taproot output that holds a DigiDollar. */
bool IsTokenOutput(const CTxOut& txout);

/** The DigiDollar amount in cents held by each DigiDollar output of this
 *  transaction, keyed by output number. Outputs whose amount the data output
 *  does not cover are left out rather than reported as zero. */
std::map<unsigned int, CAmount> TokenAmountsByOutput(const CTransaction& tx, DigiDollar::DigiDollarTxType type);

/** What the data output of a mint says. Each field has its own flag, because
 *  a field the transaction does not carry must be reported as missing and
 *  never as zero. */
struct MintFacts {
    CAmount dd_cents{0};
    bool have_dd_cents{false};
    int64_t unlock_height{0};
    bool have_unlock_height{false};
    int lock_tier{0};
    bool have_lock_tier{false};
};
MintFacts ReadMintFacts(const CTransaction& tx);

} // namespace DigiDollarTxFacts

/** UI model for transaction status. The transaction status is the part of a transaction that will change over time.
 */
struct TransactionStatus {
    enum Status {
        Confirmed,          /**< Have 6 or more confirmations (normal tx) or fully mature (mined tx) **/
        /// Normal (sent/received) transactions
        Unconfirmed,        /**< Not yet mined into a block **/
        Confirming,         /**< Confirmed, but waiting for the recommended number of confirmations **/
        Conflicted,         /**< Conflicts with other transaction or mempool **/
        Abandoned,          /**< Abandoned from the wallet **/
        /// Generated (mined) transactions
        Immature,           /**< Mined but waiting for maturity */
        NotAccepted         /**< Mined but not accepted */
    };

    /// Transaction counts towards available balance
    bool countsForBalance{false};
    /// Sorting key based on status
    std::string sortKey;

    /** @name Generated (mined) transactions
       @{*/
    int matures_in{0};
    /**@}*/

    /** @name Reported status
       @{*/
    Status status{Unconfirmed};
    qint64 depth{0};
    /**@}*/

    /** Current block hash (to know whether cached status is still valid) */
    uint256 m_cur_block_hash{};

    bool needsUpdate{false};
};

/** UI model for a transaction. A core transaction can be represented by multiple UI transactions if it has
    multiple outputs.
 */
class TransactionRecord
{
public:
    enum Type
    {
        Other,
        Generated,
        SendToAddress,
        SendToOther,
        RecvWithAddress,
        RecvFromOther,
        DDTimeLockCollateral,   // DigiDollar collateral locked in timelock
        DDCollateralReturn,     // DigiDollar collateral returned from redemption
        DDSend,                 // DigiDollar sent (0-value P2TR output)
        DDRecv,                 // DigiDollar received (0-value P2TR output)
        DDSendFee,              // DGB fee paid by a DigiDollar transaction
        // DigiDollars created by a mint. The row carries the new DigiDollar
        // amount only. The DGB that paid for it is shown by the collateral
        // lock row and the fee row of the same transaction, so this row keeps
        // debit and credit at zero and never repeats it.
        DDMint,
        // DigiDollars handed back when a redemption burned less than the
        // DigiDollar inputs it spent. The row carries a positive DigiDollar
        // amount and no DGB.
        DDChangeReturned,
    };

    /** Number of confirmation recommended for accepting a transaction */
    static const int RecommendedNumConfirmations = 6;

    TransactionRecord():
            hash(), time(0), type(Other), debit(0), credit(0), ddAmount(0), idx(0)
    {
    }

    TransactionRecord(uint256 _hash, qint64 _time):
            hash(_hash), time(_time), type(Other), debit(0),
            credit(0), ddAmount(0), idx(0)
    {
    }

    TransactionRecord(uint256 _hash, qint64 _time,
                Type _type, const std::string &_address,
                const CAmount& _debit, const CAmount& _credit):
            hash(_hash), time(_time), type(_type), address(_address), debit(_debit), credit(_credit),
            ddAmount(0), idx(0)
    {
    }

    /** Decompose CWallet transaction to model transaction records.
     */
    static bool showTransaction();
    static QList<TransactionRecord> decomposeTransaction(const interfaces::WalletTx& wtx);

    /** @name Immutable transaction attributes
      @{*/
    uint256 hash;
    qint64 time;
    Type type;
    std::string address;
    CAmount debit;
    CAmount credit;
    //! DigiDollar amount of this row, in cents. Negative when DigiDollars
    //! leave the wallet, positive when they arrive or are created. Zero on
    //! rows that only move DGB.
    CAmount ddAmount;
    /**@}*/

    /** Subtransaction index, for sort key */
    int idx;

    /** Status: can change with block chain update */
    TransactionStatus status;

    /** Whether the transaction was sent/received with a watch-only address */
    bool involvesWatchAddress;

    /** Return the unique identifier for this transaction (part) */
    QString getTxHash() const;

    /** Return the output index of the subtransaction  */
    int getOutputIndex() const;

    /** Update status from core wallet tx.
     */
    void updateStatus(const interfaces::WalletTxStatus& wtx, const uint256& block_hash, int numBlocks, int64_t block_time);

    /** Return whether a status update is needed.
     */
    bool statusUpdateNeeded(const uint256& block_hash) const;
};

#endif // DIGIBYTE_QT_TRANSACTIONRECORD_H
