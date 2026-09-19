// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_QT_WALLETMODEL_H
#define DIGIBYTE_QT_WALLETMODEL_H

#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif

#include <key.h>

#include <qt/walletmodeltransaction.h>

#include <interfaces/wallet.h>
#include <support/allocators/secure.h>
#include <univalue.h>

#include <functional>
#include <memory>
#include <vector>

#include <QObject>

enum class OutputType;

class AddressTableModel;
class ClientModel;
class OptionsModel;
class PlatformStyle;
class RecentRequestsTableModel;
class SendCoinsRecipient;
class TransactionTableModel;
class WalletModelTransaction;

class CKeyID;
class COutPoint;
class CPubKey;
class uint256;

namespace interfaces {
class Node;
} // namespace interfaces
namespace wallet {
class CCoinControl;
} // namespace wallet

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

/** Interface to DigiByte wallet from Qt view code. */
class WalletModel : public QObject
{
    Q_OBJECT

public:
    explicit WalletModel(std::unique_ptr<interfaces::Wallet> wallet, ClientModel& client_model, const PlatformStyle *platformStyle, QObject *parent = nullptr);
    ~WalletModel();

    enum StatusCode // Returned by sendCoins
    {
        OK,
        InvalidAmount,
        InvalidAddress,
        AmountExceedsBalance,
        AmountWithFeeExceedsBalance,
        DuplicateAddress,
        TransactionCreationFailed, // Error returned when wallet is still locked
        AbsurdFee
    };

    enum EncryptionStatus
    {
        NoKeys,       // wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)
        Unencrypted,  // !wallet->IsCrypted()
        Locked,       // wallet->IsCrypted() && wallet->IsLocked()
        Unlocked      // wallet->IsCrypted() && !wallet->IsLocked()
    };

    OptionsModel* getOptionsModel() const;
    AddressTableModel* getAddressTableModel() const;
    TransactionTableModel* getTransactionTableModel() const;
    RecentRequestsTableModel* getRecentRequestsTableModel() const;

    EncryptionStatus getEncryptionStatus() const;

    // Check address for validity
    bool validateAddress(const QString& address) const;

    // Return status record for SendCoins, contains error id + information
    struct SendCoinsReturn
    {
        SendCoinsReturn(StatusCode _status = OK, QString _reasonCommitFailed = "")
            : status(_status),
              reasonCommitFailed(_reasonCommitFailed)
        {
        }
        StatusCode status;
        QString reasonCommitFailed;
    };

    // prepare transaction for getting txfee before sending coins
    SendCoinsReturn prepareTransaction(WalletModelTransaction &transaction, const wallet::CCoinControl& coinControl);

    // Send coins to a list of recipients
    void sendCoins(WalletModelTransaction& transaction);

    // Wallet encryption
    bool setWalletEncrypted(const SecureString& passphrase);
    // Passphrase only needed when unlocking
    bool setWalletLocked(bool locked, const SecureString &passPhrase=SecureString());
    bool changePassphrase(const SecureString &oldPass, const SecureString &newPass);

    // RAII object for unlocking wallet, returned by requestUnlock()
    class UnlockContext
    {
    public:
        UnlockContext(std::shared_ptr<interfaces::Wallet> wallet, bool valid, bool relock);
        ~UnlockContext();

        bool isValid() const { return valid; }

        // Disable unused copy/move constructors/assignments explicitly.
        UnlockContext(const UnlockContext&) = delete;
        UnlockContext(UnlockContext&&) = delete;
        UnlockContext& operator=(const UnlockContext&) = delete;
        UnlockContext& operator=(UnlockContext&&) = delete;

    private:
        /** Keep the wallet interface alive until an asynchronous operation has
         * released its unlock lease. The GUI model may be destroyed first when
         * its wallet is closed. */
        std::shared_ptr<interfaces::Wallet> wallet;
        const bool valid;
        const bool relock;
    };

    UnlockContext requestUnlock();
    /** Keep an unlock alive only for one asynchronous signing operation. */
    std::shared_ptr<UnlockContext> requestUnlockForAsync();

    bool bumpFee(uint256 hash, uint256& new_hash);
    bool displayAddress(std::string sAddress) const;

    static bool isWalletEnabled();

    interfaces::Node& node() const { return m_node; }
    interfaces::Wallet& wallet() const { return *m_wallet; }
    ClientModel& clientModel() const { return *m_client_model; }
    void setClientModel(ClientModel* client_model);

    QString getWalletName() const;
    QString getDisplayName() const;

    bool isMultiwallet() const;

    void refresh(bool pk_hash_only = false);

    uint256 getLastBlockProcessed() const;

    // Retrieve the cached wallet balance
    interfaces::WalletBalances getCachedBalance() const;

    // If coin control has selected outputs, searches the total amount inside the wallet.
    // Otherwise, uses the wallet's cached available balance.
    CAmount getAvailableBalance(const wallet::CCoinControl* control);

    // DigiDollar specific methods
    struct DigiDollarSendResult
    {
        DigiDollarSendResult(StatusCode _status = OK, QString _txid = "", QString _reasonFailed = "")
            : status(_status), txid(_txid), reasonFailed(_reasonFailed) {}
        StatusCode status;
        QString txid;
        QString reasonFailed;
    };

    struct DigiDollarMintResult
    {
        DigiDollarMintResult(StatusCode _status = OK, QString _txid = "", QString _positionId = "", QString _reasonFailed = "", CAmount _collateralLocked = 0)
            : status(_status), txid(_txid), positionId(_positionId), reasonFailed(_reasonFailed), collateralLocked(_collateralLocked) {}
        StatusCode status;
        QString txid;
        QString positionId;
        QString reasonFailed;
        CAmount collateralLocked;  // Amount of DGB locked as collateral (in satoshis)
    };

    struct DigiDollarRedeemResult
    {
        DigiDollarRedeemResult(StatusCode _status = OK, QString _txid = "", QString _reasonFailed = "")
            : status(_status), txid(_txid), reasonFailed(_reasonFailed) {}
        StatusCode status;
        QString txid;
        QString reasonFailed;
    };

    // Send DigiDollar to an address
    DigiDollarSendResult sendDigiDollar(const QString& address, CAmount amount, const QString& comment = "",
                                        const std::vector<COutPoint>* preset_dd_inputs = nullptr);

    // Mint DigiDollar with collateral
    DigiDollarMintResult mintDigiDollar(CAmount ddAmount, int lockTier);

    //! Empty when this wallet supports minting, including while it is locked.
    QString getDigiDollarMintWalletError() const;

    // Redeem DigiDollar position
    DigiDollarRedeemResult redeemDigiDollar(const QString& positionId, CAmount amount, const QString& redeemAddress = "");

    struct DigiDollarBalanceSummary
    {
        CAmount available{0};
        CAmount confirmed_total{0};
        CAmount paymaster_reserved{0};
        CAmount pending{0};
    };

    // Get confirmed DigiDollar currently spendable by ordinary wallet actions.
    CAmount getDigiDollarBalance() const;

    // Get all confirmed DigiDollar owned by the wallet, including Paymaster reservations.
    CAmount getTotalDigiDollarBalance() const;

    // Get confirmed DigiDollar protected by Paymaster sessions or provider pools.
    CAmount getPaymasterReservedDigiDollarBalance() const;

    // Get a consistent balance breakdown for overview display.
    DigiDollarBalanceSummary getDigiDollarBalanceSummary() const;

    // Get pending (unconfirmed but trusted) DigiDollar balance
    CAmount getPendingDigiDollarBalance() const;

    // Get locked collateral amount
    CAmount getLockedCollateral() const;

    // Get available DGB balance for collateral
    CAmount getAvailableDGBBalance() const;

    // Validate DigiDollar address
    bool validateDigiDollarAddress(const QString& address) const;

    // Calculate required collateral for minting
    CAmount calculateRequiredCollateral(CAmount ddAmount, int lockTier) const;

    // Execute RPC command (for DigiDollar widgets)
    UniValue executeRpc(const std::string& command, const UniValue& params) const;

    /**
     * Execute a wallet-scoped RPC away from the GUI thread and deliver its
     * result back on this object's thread. Paymaster workflows use this
     * bridge so network waits and wallet synchronization never block Qt.
     */
    using RpcCallback = std::function<void(UniValue result, QString error)>;
    void executeRpcAsync(std::string command, UniValue params, RpcCallback callback);

    /**
     * Build the DigiDollar transaction-history view away from Qt's event
     * thread. Unlike executeRpcAsync(), this local wallet query does not
     * depend on the RPC server being started. Requests from multiple DD views
     * are coalesced into one wallet scan so tab changes do not multiply the
     * expensive ownership and confirmation reconstruction.
     */
    void getDigiDollarTransactionHistoryAsync(int count, int skip, RpcCallback callback);

    /**
     * Return the latest model-owned history snapshot without waiting for
     * cs_wallet. At startup this contains up to 50 persisted rows; a completed
     * asynchronous refresh replaces it with the authoritative history.
     */
    UniValue getCachedDigiDollarTransactionHistory(int count, int skip);

    // Generate new DigiDollar receiving address
    QString getNewDigiDollarAddress(const QString& label = "");

    // Get DigiDollarWallet for coin control
    DigiDollarWallet* getDigiDollarWallet() const;

private:
    // Background Qt jobs keep a shared interface reference while reading a
    // wallet snapshot. The public constructor still accepts unique ownership;
    // no wallet is shared outside WalletModel and its bounded worker jobs.
    std::shared_ptr<interfaces::Wallet> m_wallet;
    std::unique_ptr<interfaces::Handler> m_handler_unload;
    std::unique_ptr<interfaces::Handler> m_handler_status_changed;
    std::unique_ptr<interfaces::Handler> m_handler_address_book_changed;
    std::unique_ptr<interfaces::Handler> m_handler_transaction_changed;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_watch_only_changed;
    std::unique_ptr<interfaces::Handler> m_handler_can_get_addrs_changed;
    ClientModel* m_client_model;
    interfaces::Node& m_node;

    bool fHaveWatchOnly;
    bool fForceCheckBalanceChanged{false};

    // Wallet has an options model for wallet-specific options
    // (transaction fee, for example)
    OptionsModel *optionsModel;

    AddressTableModel* addressTableModel{nullptr};
    TransactionTableModel* transactionTableModel{nullptr};
    RecentRequestsTableModel* recentRequestsTableModel{nullptr};

    // Cache some values to be able to detect changes
    interfaces::WalletBalances m_cached_balances;
    EncryptionStatus cachedEncryptionStatus{Unencrypted};
    QTimer* timer;

    struct DigiDollarHistoryRequest {
        int count{0};
        int skip{0};
        RpcCallback callback;
    };

    // A WalletModel belongs to exactly one wallet, making it the natural
    // boundary for both the immediate persisted seed and the single-flight
    // canonical refresh. Widgets never share history across wallet models.
    UniValue m_digi_dollar_history_cache{UniValue::VARR};
    std::vector<DigiDollarHistoryRequest> m_digi_dollar_history_requests;
    bool m_digi_dollar_history_refresh_in_flight{false};

    // Block hash denoting when the last balance update was done.
    uint256 m_cached_last_update_tip{};

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();
    void checkBalanceChanged(const interfaces::WalletBalances& new_balances);

Q_SIGNALS:
    // Signal that balance in wallet changed
    void balanceChanged(const interfaces::WalletBalances& balances);

    // Signal that DigiDollar wallet state changed and DD widgets should refresh.
    void digiDollarChanged();

    // Encryption status of wallet changed
    void encryptionStatusChanged();

    // Signal emitted when wallet needs to be unlocked
    // It is valid behaviour for listeners to keep the wallet locked after this signal;
    // this means that the unlocking failed or was cancelled.
    void requireUnlock();

    // Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);

    // Coins sent: from wallet, to recipient, in (serialized) transaction:
    void coinsSent(WalletModel* wallet, SendCoinsRecipient recipient, QByteArray transaction);

    // Show progress dialog e.g. for rescan
    void showProgress(const QString &title, int nProgress);

    // Watch-only address added
    void notifyWatchonlyChanged(bool fHaveWatchonly);

    // Signal that wallet is about to be removed
    void unload();

    // Notify that there are now keys in the keypool
    void canGetAddressesChanged();

    void timerTimeout();

public Q_SLOTS:
    /* Starts a timer to periodically update the balance */
    void startPollBalance();

    /* Wallet status might have changed */
    void updateStatus();
    /* New transaction, or transaction changed status */
    void updateTransaction();
    /* New, updated or removed address book entry */
    void updateAddressBook(const QString &address, const QString &label, bool isMine, wallet::AddressPurpose purpose, int status);
    /* Watch-only added */
    void updateWatchOnlyFlag(bool fHaveWatchonly);
    /* Current, immature or unconfirmed balance might have changed - emit 'balanceChanged' if so */
    void pollBalanceChanged();
};

#endif // DIGIBYTE_QT_WALLETMODEL_H
