// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif

#include <qt/walletmodel.h>
#include <digidollar/digidollar.h>

#include <qt/addresstablemodel.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/paymentserver.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/transactiontablemodel.h>

#include <common/args.h> // for GetBoolArg
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <psbt.h>
#include <util/translation.h>
#include <util/error.h>
#include <util/strencodings.h> // for strprintf
#include <wallet/coincontrol.h>
#include <wallet/spend.h> // for AvailableCoins, CreateTransaction
#include <wallet/wallet.h> // for CRecipient
#include <univalue.h>
#include <wallet/digidollarwallet.h> // for DigiDollarWallet
#include <wallet/digidollarmintcapability.h>
#include <base58.h> // for CDigiDollarAddress
#include <uint256.h>
#include <hash.h>
#include <crypto/sha256.h> // for CSHA256
#include <util/time.h> // for GetTime
#include <logging.h> // for LogPrintf
#include <digidollar/txbuilder.h> // for MintTxBuilder
#include <oracle/mock_oracle.h> // for MockOracleManager
#include <QUrl> // for QUrl::toPercentEncoding
#include <kernel/chainparams.h> // for CChainParams

#include <stdint.h>
#include <algorithm>
#include <functional>
#include <optional>

#include <QDebug>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

using wallet::CCoinControl;
using wallet::CRecipient;
using wallet::DEFAULT_DISABLE_WALLET;

WalletModel::WalletModel(std::unique_ptr<interfaces::Wallet> wallet, ClientModel& client_model, const PlatformStyle *platformStyle, QObject *parent) :
    QObject(parent),
    m_wallet(std::move(wallet)),
    m_client_model(&client_model),
    m_node(client_model.node()),
    optionsModel(client_model.getOptionsModel()),
    timer(new QTimer(this))
{
    fHaveWatchOnly = m_wallet->haveWatchOnly();
    addressTableModel = new AddressTableModel(this);
    transactionTableModel = new TransactionTableModel(platformStyle, this);
    recentRequestsTableModel = new RecentRequestsTableModel(this);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

void WalletModel::startPollBalance()
{
    // Update the cached balance right away, so every view can make use of it,
    // so them don't need to waste resources recalculating it.
    pollBalanceChanged();


    // This timer will be fired repeatedly to update the balance
    // Since the QTimer::timeout is a private signal, it cannot be used
    // in the GUIUtil::ExceptionSafeConnect directly.
    connect(timer, &QTimer::timeout, this, &WalletModel::timerTimeout);
    GUIUtil::ExceptionSafeConnect(this, &WalletModel::timerTimeout, this, &WalletModel::pollBalanceChanged);
    timer->start(MODEL_UPDATE_DELAY);
}

void WalletModel::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!m_client_model) timer->stop();
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if(cachedEncryptionStatus != newEncryptionStatus) {
        Q_EMIT encryptionStatusChanged();
    }
}

void WalletModel::pollBalanceChanged()
{
    // Avoid recomputing wallet balances unless a TransactionChanged or
    // BlockTip notification was received.
    if (!fForceCheckBalanceChanged && m_cached_last_update_tip == getLastBlockProcessed()) return;

    // Try to get balances and return early if locks can't be acquired. This
    // avoids the GUI from getting stuck on periodical polls if the core is
    // holding the locks for a longer time - for example, during a wallet
    // rescan.
    interfaces::WalletBalances new_balances;
    uint256 block_hash;
    if (!m_wallet->tryGetBalances(new_balances, block_hash)) {
        return;
    }

    if (fForceCheckBalanceChanged || block_hash != m_cached_last_update_tip) {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        m_cached_last_update_tip = block_hash;

        checkBalanceChanged(new_balances);
        if(transactionTableModel)
            transactionTableModel->updateConfirmations();
    }
}

void WalletModel::checkBalanceChanged(const interfaces::WalletBalances& new_balances)
{
    if (new_balances.balanceChanged(m_cached_balances)) {
        m_cached_balances = new_balances;
        Q_EMIT balanceChanged(new_balances);
    }
}

interfaces::WalletBalances WalletModel::getCachedBalance() const
{
    return m_cached_balances;
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString &address, const QString &label,
        bool isMine, wallet::AddressPurpose purpose, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::validateAddress(const QString& address) const
{
    return IsValidDestinationString(address.toStdString());
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction &transaction, const CCoinControl& coinControl)
{
    CAmount total = 0;
    bool fSubtractFeeFromAmount = false;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if(recipients.empty())
    {
        return OK;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    for (const SendCoinsRecipient &rcp : recipients)
    {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;
        {   // User-entered digibyte address / amount:
            if(!validateAddress(rcp.address))
            {
                return InvalidAddress;
            }
            if(rcp.amount <= 0)
            {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CRecipient recipient{DecodeDestination(rcp.address.toStdString()), rcp.amount, rcp.fSubtractFeeFromAmount};
            vecSend.push_back(recipient);

            total += rcp.amount;
        }
    }
    if(setAddress.size() != nAddresses)
    {
        return DuplicateAddress;
    }

    // If no coin was manually selected, use the cached balance
    // Future: can merge this call with 'createTransaction'.
    CAmount nBalance = getAvailableBalance(&coinControl);

    if(total > nBalance)
    {
        return AmountExceedsBalance;
    }

    try {
        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;

        auto& newTx = transaction.getWtx();
        const auto& res = m_wallet->createTransaction(vecSend, coinControl, /*sign=*/!wallet().privateKeysDisabled(), nChangePosRet, nFeeRequired);
        newTx = res ? *res : nullptr;
        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && newTx)
            transaction.reassignAmounts(nChangePosRet);

        if(!newTx)
        {
            if(!fSubtractFeeFromAmount && (total + nFeeRequired) > nBalance)
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(util::ErrorString(res).translated),
                CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // Reject absurdly high fee. (This can never happen because the
        // wallet never creates transactions with fee greater than
        // m_default_max_tx_fee. This merely a belt-and-suspenders check).
        if (nFeeRequired > m_wallet->getDefaultMaxTxFee()) {
            return AbsurdFee;
        }
    } catch (const std::runtime_error& err) {
        // Something unexpected happened, instruct user to report this bug.
        Q_EMIT message(tr("Send Coins"), QString::fromStdString(err.what()),
                       CClientUIInterface::MSG_ERROR);
        return TransactionCreationFailed;
    }

    return SendCoinsReturn(OK);
}

void WalletModel::sendCoins(WalletModelTransaction& transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        std::vector<std::pair<std::string, std::string>> vOrderForm;
        for (const SendCoinsRecipient &rcp : transaction.getRecipients())
        {
            if (!rcp.message.isEmpty()) // Message from normal digibyte:URI (digibyte:123...?message=example)
                vOrderForm.emplace_back("Message", rcp.message.toStdString());
        }

        auto& newTx = transaction.getWtx();
        wallet().commitTransaction(newTx, /*value_map=*/{}, std::move(vOrderForm));

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *newTx;
        transaction_array.append((const char*)ssTx.data(), ssTx.size());
    }

    // Add addresses / update labels that we've sent to the address book,
    // and emit coinsSent signal for each recipient
    for (const SendCoinsRecipient &rcp : transaction.getRecipients())
    {
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = DecodeDestination(strAddress);
            std::string strLabel = rcp.label.toStdString();
            {
                // Check if we have a new address or an updated label
                std::string name;
                if (!m_wallet->getAddress(
                     dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr))
                {
                    m_wallet->setAddressBook(dest, strLabel, wallet::AddressPurpose::SEND);
                }
                else if (name != strLabel)
                {
                    m_wallet->setAddressBook(dest, strLabel, {}); // {} means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(this, rcp, transaction_array);
    }

    checkBalanceChanged(m_wallet->getBalances()); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits
}

OptionsModel* WalletModel::getOptionsModel() const
{
    return optionsModel;
}

AddressTableModel* WalletModel::getAddressTableModel() const
{
    return addressTableModel;
}

TransactionTableModel* WalletModel::getTransactionTableModel() const
{
    return transactionTableModel;
}

RecentRequestsTableModel* WalletModel::getRecentRequestsTableModel() const
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if(!m_wallet->isCrypted())
    {
        // A previous bug allowed for watchonly wallets to be encrypted (encryption keys set, but nothing is actually encrypted).
        // To avoid misrepresenting the encryption status of such wallets, we only return NoKeys for watchonly wallets that are unencrypted.
        if (m_wallet->privateKeysDisabled()) {
            return NoKeys;
        }
        return Unencrypted;
    }
    else if(m_wallet->isLocked())
    {
        return Locked;
    }
    else
    {
        return Unlocked;
    }
}

bool WalletModel::setWalletEncrypted(const SecureString& passphrase)
{
    return m_wallet->encryptWallet(passphrase);
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase)
{
    if(locked)
    {
        // Lock
        return m_wallet->lock();
    }
    else
    {
        // Unlock
        return m_wallet->unlock(passPhrase);
    }
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    m_wallet->lock(); // Make sure wallet is locked before attempting pass change
    return m_wallet->changeWalletPassphrase(oldPass, newPass);
}

// Handlers for core signals
static void NotifyUnload(WalletModel* walletModel)
{
    qDebug() << "NotifyUnload";
    bool invoked = QMetaObject::invokeMethod(walletModel, "unload");
    assert(invoked);
}

static void NotifyKeyStoreStatusChanged(WalletModel *walletmodel)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
    assert(invoked);
}

static void NotifyAddressBookChanged(WalletModel *walletmodel,
        const CTxDestination &address, const std::string &label, bool isMine,
        wallet::AddressPurpose purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(EncodeDestination(address));
    QString strLabel = QString::fromStdString(label);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + QString::number(static_cast<uint8_t>(purpose)) + " status=" + QString::number(status);
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
                              Q_ARG(QString, strAddress),
                              Q_ARG(QString, strLabel),
                              Q_ARG(bool, isMine),
                              Q_ARG(wallet::AddressPurpose, purpose),
                              Q_ARG(int, status));
    assert(invoked);
}

static void NotifyTransactionChanged(WalletModel *walletmodel, const uint256 &hash, ChangeType status)
{
    Q_UNUSED(hash);
    Q_UNUSED(status);
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
    assert(invoked);
}

static void ShowProgress(WalletModel *walletmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    bool invoked = QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
    assert(invoked);
}

static void NotifyWatchonlyChanged(WalletModel *walletmodel, bool fHaveWatchonly)
{
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
                              Q_ARG(bool, fHaveWatchonly));
    assert(invoked);
}

static void NotifyCanGetAddressesChanged(WalletModel* walletmodel)
{
    bool invoked = QMetaObject::invokeMethod(walletmodel, "canGetAddressesChanged");
    assert(invoked);
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    m_handler_unload = m_wallet->handleUnload(std::bind(&NotifyUnload, this));
    m_handler_status_changed = m_wallet->handleStatusChanged(std::bind(&NotifyKeyStoreStatusChanged, this));
    m_handler_address_book_changed = m_wallet->handleAddressBookChanged(std::bind(NotifyAddressBookChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    m_handler_transaction_changed = m_wallet->handleTransactionChanged(std::bind(NotifyTransactionChanged, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_show_progress = m_wallet->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_watch_only_changed = m_wallet->handleWatchOnlyChanged(std::bind(NotifyWatchonlyChanged, this, std::placeholders::_1));
    m_handler_can_get_addrs_changed = m_wallet->handleCanGetAddressesChanged(std::bind(NotifyCanGetAddressesChanged, this));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    m_handler_unload->disconnect();
    m_handler_status_changed->disconnect();
    m_handler_address_book_changed->disconnect();
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
    m_handler_watch_only_changed->disconnect();
    m_handler_can_get_addrs_changed->disconnect();
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
    bool was_locked = getEncryptionStatus() == Locked;
    if(was_locked)
    {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock();
    }
    // If wallet is still locked, unlock was failed or cancelled, mark context as invalid
    bool valid = getEncryptionStatus() != Locked;

    return UnlockContext(this, valid, was_locked);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *_wallet, bool _valid, bool _relock):
        wallet(_wallet),
        valid(_valid),
        relock(_relock)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && relock)
    {
        wallet->setWalletLocked(true);
    }
}

bool WalletModel::bumpFee(uint256 hash, uint256& new_hash)
{
    CCoinControl coin_control;
    coin_control.m_signal_bip125_rbf = true;
    std::vector<bilingual_str> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    if (!m_wallet->createBumpTransaction(hash, coin_control, errors, old_fee, new_fee, mtx)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Increasing transaction fee failed") + "<br />(" +
            (errors.size() ? QString::fromStdString(errors[0].translated) : "") +")");
        return false;
    }

    const bool create_psbt = m_wallet->privateKeysDisabled();

    // allow a user based fee verification
    /*: Asks a user if they would like to manually increase the fee of a transaction that has already been created. */
    QString questionString = tr("Do you want to increase the fee?");
    questionString.append("<br />");
    questionString.append("<table style=\"text-align: left;\">");
    questionString.append("<tr><td>");
    questionString.append(tr("Current fee:"));
    questionString.append("</td><td>");
    questionString.append(DigiByteUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), old_fee));
    questionString.append("</td></tr><tr><td>");
    questionString.append(tr("Increase:"));
    questionString.append("</td><td>");
    questionString.append(DigiByteUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), new_fee - old_fee));
    questionString.append("</td></tr><tr><td>");
    questionString.append(tr("New fee:"));
    questionString.append("</td><td>");
    questionString.append(DigiByteUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), new_fee));
    questionString.append("</td></tr></table>");

    // Display warning in the "Confirm fee bump" window if the "Coin Control Features" option is enabled
    if (getOptionsModel()->getCoinControlFeatures()) {
        questionString.append("<br><br>");
        questionString.append(tr("Warning: This may pay the additional fee by reducing change outputs or adding inputs, when necessary. It may add a new change output if one does not already exist. These changes may potentially leak privacy."));
    }

    const bool enable_send{!wallet().privateKeysDisabled() || wallet().hasExternalSigner()};
    const bool always_show_unsigned{getOptionsModel()->getEnablePSBTControls()};
    auto confirmationDialog = new SendConfirmationDialog(tr("Confirm fee bump"), questionString, "", "", SEND_CONFIRM_DELAY, enable_send, always_show_unsigned, nullptr);
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);
    // TODO: Replace QDialog::exec() with safer QDialog::show().
    const auto retval = static_cast<QMessageBox::StandardButton>(confirmationDialog->exec());

    // cancel sign&broadcast if user doesn't want to bump the fee
    if (retval != QMessageBox::Yes && retval != QMessageBox::Save) {
        return false;
    }

    WalletModel::UnlockContext ctx(requestUnlock());
    if(!ctx.isValid())
    {
        return false;
    }

    // Short-circuit if we are returning a bumped transaction PSBT to clipboard
    if (retval == QMessageBox::Save) {
        // "Create Unsigned" clicked
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        const TransactionError err = wallet().fillPSBT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, nullptr, psbtx, complete);
        if (err != TransactionError::OK || complete) {
            QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't draft transaction."));
            return false;
        }
        // Serialize the PSBT
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
        Q_EMIT message(tr("PSBT copied"), tr("Copied to clipboard", "Fee-bump PSBT saved"), CClientUIInterface::MSG_INFORMATION);
        return true;
    }

    assert(!m_wallet->privateKeysDisabled() || wallet().hasExternalSigner());


    // sign bumped transaction
    if (!m_wallet->signBumpTransaction(mtx)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't sign transaction."));
        return false;
    }
    // commit the bumped transaction
    if(!m_wallet->commitBumpTransaction(hash, std::move(mtx), errors, new_hash)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Could not commit transaction") + "<br />(" +
            QString::fromStdString(errors[0].translated)+")");
        return false;
    }
    return true;
}

bool WalletModel::displayAddress(std::string sAddress) const
{
    CTxDestination dest = DecodeDestination(sAddress);
    bool res = false;
    try {
        res = m_wallet->displayAddress(dest);
    } catch (const std::runtime_error& e) {
        QMessageBox::critical(nullptr, tr("Can't display address"), e.what());
    }
    return res;
}

bool WalletModel::isWalletEnabled()
{
   return !gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET);
}

QString WalletModel::getWalletName() const
{
    return QString::fromStdString(m_wallet->getWalletName());
}

QString WalletModel::getDisplayName() const
{
    const QString name = getWalletName();
    return name.isEmpty() ? "["+tr("default wallet")+"]" : name;
}

bool WalletModel::isMultiwallet() const
{
    return m_node.walletLoader().getWallets().size() > 1;
}

void WalletModel::refresh(bool pk_hash_only)
{
    addressTableModel = new AddressTableModel(this, pk_hash_only);
}

uint256 WalletModel::getLastBlockProcessed() const
{
    return m_client_model ? m_client_model->getBestBlockHash() : uint256{};
}

CAmount WalletModel::getAvailableBalance(const CCoinControl* control)
{
    // No selected coins, return the cached balance
    if (!control || !control->HasSelected()) {
        const interfaces::WalletBalances& balances = getCachedBalance();
        CAmount available_balance = balances.balance;
        // if wallet private keys are disabled, this is a watch-only wallet
        // so, let's include the watch-only balance.
        if (balances.have_watch_only && m_wallet->privateKeysDisabled()) {
            available_balance += balances.watch_only_balance;
        }
        return available_balance;
    }
    // Fetch balance from the wallet, taking into account the selected coins
    return wallet().getAvailableBalance(*control);
}

// DigiDollar method implementations
WalletModel::DigiDollarSendResult WalletModel::sendDigiDollar(const QString& address, CAmount amount, const QString& comment,
                                                              const std::vector<COutPoint>* preset_dd_inputs)
{
    // Validate address format first
    if (!validateDigiDollarAddress(address)) {
        return DigiDollarSendResult(InvalidAddress, "", "Invalid DigiDollar address format");
    }

    if (m_wallet->privateKeysDisabled()) {
        return DigiDollarSendResult(TransactionCreationFailed, "", "Private keys are disabled for this wallet");
    }

    // Check if wallet is locked
    if (getEncryptionStatus() == Locked) {
        return DigiDollarSendResult(TransactionCreationFailed, "", "Wallet is locked. Please unlock to send DigiDollar.");
    }

    // Check if amount is positive
    if (amount <= 0) {
        return DigiDollarSendResult(InvalidAmount, "", "Send amount must be positive");
    }

    // Check if we have sufficient DigiDollar balance
    CAmount currentBalance = getDigiDollarBalance();
    if (amount > currentBalance) {
        return DigiDollarSendResult(AmountExceedsBalance, "",
            QString("Insufficient DigiDollar balance. Available: %1, Requested: %2")
                .arg(QString::number(currentBalance / 100.0, 'f', 2))
                .arg(QString::number(amount / 100.0, 'f', 2)));
    }

    try {
        // Get DigiDollar wallet instance from wallet interface
        DigiDollarWallet* ddWallet = m_wallet->getDigiDollarWallet();
        if (!ddWallet) {
            LogPrintf("DigiDollar Qt: DigiDollar wallet not available\n");
            return DigiDollarSendResult(TransactionCreationFailed, "", "DigiDollar wallet not initialized");
        }

        // Create DigiDollar address object
        CDigiDollarAddress recipientAddr(address.toStdString());

        // Call backend to create and send transaction
        std::string txid;
        std::string error;
        bool success = ddWallet->TransferDigiDollar(recipientAddr, amount, txid, error,
                                                    nullptr, preset_dd_inputs, comment.toStdString());

        if (!success) {
            // Transaction creation or sending failed
            LogPrintf("DigiDollar Qt: Send failed - %s\n", error);
            return DigiDollarSendResult(TransactionCreationFailed, "", QString::fromStdString(error));
        }

        // Transaction successfully created and broadcast
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Send successful - %d cents to %s, txid: %s\n",
                  amount, address.toStdString(), txid);

        // PHASE 7.4-7.6: Emit signals for UI updates
        // Force balance refresh to update all widgets
        checkBalanceChanged(m_wallet->getBalances());

        // Emit pollBalanceChanged to trigger UI refresh across all widgets
        Q_EMIT pollBalanceChanged();
        Q_EMIT digiDollarChanged();

        return DigiDollarSendResult(OK, QString::fromStdString(txid), "");

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: Send exception - %s\n", e.what());
        return DigiDollarSendResult(TransactionCreationFailed, "", QString::fromStdString(e.what()));
    }
}

QString WalletModel::getDigiDollarMintWalletError() const
{
    const wallet::CWallet* wallet = m_wallet->wallet();
    if (!wallet) return tr("Wallet not available");
    return QString::fromStdString(wallet::GetDigiDollarMintWalletError(*wallet).translated);
}

WalletModel::DigiDollarMintResult WalletModel::mintDigiDollar(CAmount ddAmount, int lockTier)
{
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: ========== MINT DIGIDOLLAR START ==========\n");
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: mintDigiDollar called - Amount: %d cents, Tier: %d\n", ddAmount, lockTier);

    // Validate lock tier
    if (lockTier < 0 || lockTier > 9) {
        LogPrintf("DigiDollar Qt: ERROR - Invalid lock tier: %d\n", lockTier);
        return DigiDollarMintResult(InvalidAmount, "", "", "Invalid lock tier. Must be between 0 and 9 (0 = 1 hour testing).");
    }
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Lock tier validation passed\n");

    const QString capability_error = getDigiDollarMintWalletError();
    if (!capability_error.isEmpty()) {
        LogPrintf("DigiDollar Qt: Cannot mint: %s\n", capability_error.toStdString());
        return DigiDollarMintResult(TransactionCreationFailed, "", "", capability_error);
    }

    // Check if wallet is locked
    if (getEncryptionStatus() == Locked) {
        LogPrintf("DigiDollar Qt: ERROR - Wallet is locked\n");
        return DigiDollarMintResult(TransactionCreationFailed, "", "", "Wallet is locked. Please unlock to mint DigiDollar.");
    }
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Wallet encryption check passed\n");

    // Check if amount is positive
    if (ddAmount <= 0) {
        LogPrintf("DigiDollar Qt: ERROR - Invalid amount: %d\n", ddAmount);
        return DigiDollarMintResult(InvalidAmount, "", "", "Mint amount must be positive");
    }
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Amount validation passed\n");

    // Calculate required collateral
    CAmount requiredCollateral = calculateRequiredCollateral(ddAmount, lockTier);
    CAmount availableDGBBalance = getAvailableDGBBalance();

    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Required collateral: %d sats (%.8f DGB)\n",
              requiredCollateral, requiredCollateral / 100000000.0);
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Available balance: %d sats (%.8f DGB)\n",
              availableDGBBalance, availableDGBBalance / 100000000.0);

    if (requiredCollateral > availableDGBBalance) {
        LogPrintf("DigiDollar Qt: ERROR - Insufficient balance\n");
        return DigiDollarMintResult(AmountExceedsBalance, "", "",
            QString("Insufficient DGB balance for collateral. Required: %1, Available: %2")
                .arg(QString::number(requiredCollateral / 100000000.0, 'f', 8))
                .arg(QString::number(availableDGBBalance / 100000000.0, 'f', 8)));
    }
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Balance check passed\n");

    try {
        // Step 1: Convert lock tier to lock days for TxBuilder
        const int lockDaysForTier[10] = {0, 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650};
        int lockDays = lockDaysForTier[lockTier];
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 1 - Lock days for tier %d: %d days\n", lockTier, lockDays);

        // Step 2: Get current blockchain height from client model
        if (!m_client_model) {
            LogPrintf("DigiDollar Qt: ERROR - Client model not available\n");
            return DigiDollarMintResult(TransactionCreationFailed, "", "", "Client model not available");
        }
        int currentHeight = m_client_model->getNumBlocks();
        const int mintHeight = currentHeight + 1;
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 2 - Current blockchain height: %d, mint height: %d\n",
                  currentHeight, mintHeight);

        // Step 3: Get oracle price (MockOracleManager for RegTest only, RPC for testnet/mainnet)
        // Oracle price format: micro-USD per DGB (1,000,000 = $1.00)
        CAmount oraclePriceMicroUSD = 0;
        ChainType chainType = Params().GetChainType();
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 3 - Getting oracle price, ChainType=%d\n", (int)chainType);

        if (chainType == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
            // RegTest uses mock oracle
            oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Using MockOracle price: %ld micro-USD ($%.6f/DGB)\n",
                      oraclePriceMicroUSD, oraclePriceMicroUSD / 1000000.0);
        } else {
            // Testnet/Mainnet uses real oracle via RPC
            try {
                UniValue params(UniValue::VARR);
                UniValue result = m_node.executeRpc("getoracleprice", params, "");
                const UniValue& priceVal = result.find_value("price_micro_usd");
                if (!priceVal.isNull()) {
                    oraclePriceMicroUSD = priceVal.getInt<int64_t>();
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Using RPC oracle price: %ld micro-USD ($%.6f/DGB)\n",
                              oraclePriceMicroUSD, oraclePriceMicroUSD / 1000000.0);
                }
            } catch (const std::exception& e) {
                LogPrintf("DigiDollar Qt: ERROR getting oracle price from RPC: %s\n", e.what());
            }
        }

        std::optional<int> candidateHealth;
        UniValue candidateHealthSnapshot;
        if (DigiDollar::IsThawDayActive(Params().GetConsensus(), mintHeight)) {
            try {
                UniValue rpc_params(UniValue::VARR);
                const UniValue response = m_node.executeRpc("getoracleprice", rpc_params, "");
                const UniValue& status = response.find_value("mint_volatility");
                if (!status.find_value("quote_available").get_bool())
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", "No valid oracle quote for the next block.");
                if (!status.find_value("ready").get_bool())
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", QString::fromStdString(status.find_value("data_error").get_str()));
                if (status.find_value("minting_restricted").get_bool())
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", "minting-volatility-pause");
                oraclePriceMicroUSD = status.find_value("candidate_price_micro_usd").getInt<int64_t>();
                const UniValue protection = m_node.executeRpc("getprotectionstatus", rpc_params, "");
                candidateHealthSnapshot = protection.find_value("next_block_health");
                if (!candidateHealthSnapshot.find_value("ready").get_bool() ||
                    candidateHealthSnapshot.find_value("candidate_height").getInt<int>() != mintHeight)
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", "Candidate health is unavailable; wait for synchronization and retry.");
                if (candidateHealthSnapshot.find_value("oracle_price_micro_usd").getInt<int64_t>() != oraclePriceMicroUSD)
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", "Oracle quote changed; retry mint construction.");
                candidateHealth = candidateHealthSnapshot.find_value("health_percentage").getInt<int>();
                if (*candidateHealth < 100)
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", "Minting is blocked by candidate emergency health.");
                oraclePriceMicroUSD = candidateHealthSnapshot.find_value("oracle_price_micro_usd").getInt<int64_t>();
            } catch (...) {
                return DigiDollarMintResult(TransactionCreationFailed, "", "", "Mint eligibility is unavailable. Wait for synchronization and try again.");
            }
        }

        if (oraclePriceMicroUSD <= 0) {
            LogPrintf("DigiDollar Qt: ERROR - Oracle price not available\n");
            return DigiDollarMintResult(TransactionCreationFailed, "", "", "Oracle price not available. Cannot mint DigiDollar.");
        }

        // Convert to the format expected by the mint transaction builder
        // The builder expects micro-USD (1,000,000 = $1.00)
        CAmount oraclePrice = oraclePriceMicroUSD;

        // Step 4: Get wallet pointer for accessing UTXOs and signing
        wallet::CWallet* pWallet = wallet().wallet();
        if (!pWallet) {
            LogPrintf("DigiDollar Qt: ERROR - Wallet pointer not available\n");
            return DigiDollarMintResult(TransactionCreationFailed, "", "", "Wallet not available");
        }
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 4 - Wallet pointer obtained\n");

        // Step 5: Collect available UTXOs from wallet for collateral
        // Build UTXO map for value lookup
        std::map<COutPoint, CAmount> utxoValues;
        std::vector<COutPoint> availableUtxos;
        CAmount totalAvailable = 0;

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 5 - Collecting UTXOs from wallet...\n");
        {
            LOCK(pWallet->cs_wallet);

            // Get all available coins from wallet
            auto coins = wallet().listCoins();
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: listCoins returned %d destinations\n", coins.size());

            for (const auto& [dest, outputs] : coins) {
                LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Processing destination with %d outputs\n", outputs.size());
                for (const auto& outpoint_txout : outputs) {
                    const COutPoint& outpoint = std::get<0>(outpoint_txout);
                    const interfaces::WalletTxOut& wtxout = std::get<1>(outpoint_txout);

                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Checking UTXO %s:%d - Value: %d, Depth: %d, Spent: %s\n",
                              outpoint.hash.GetHex(), outpoint.n, wtxout.txout.nValue,
                              wtxout.depth_in_main_chain, wtxout.is_spent ? "YES" : "NO");

                    // Only use confirmed, spendable UTXOs
                    if (wtxout.depth_in_main_chain > 0 && !wtxout.is_spent) {
                        availableUtxos.push_back(outpoint);
                        utxoValues[outpoint] = wtxout.txout.nValue;
                        totalAvailable += wtxout.txout.nValue;
                        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Added UTXO - Total now: %d sats\n", totalAvailable);
                    }
                }
            }
        }

        if (availableUtxos.empty()) {
            LogPrintf("DigiDollar Qt: ERROR - No available UTXOs found\n");
            return DigiDollarMintResult(TransactionCreationFailed, "", "",
                "No available UTXOs for collateral. Please ensure wallet has confirmed DGB balance.");
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Found %d available UTXOs totaling %d satoshis (%.8f DGB)\n",
                  availableUtxos.size(), totalAvailable, totalAvailable / 100000000.0);

        // Step 6: Derive owner key for the mint position from the wallet.
        // This keeps Qt-created positions recoverable from wallet descriptors.
        CKey ownerKey;
        {
            LOCK(pWallet->cs_wallet);
            ownerKey = pWallet->GetHDKeyForDigiDollar("dd-owner");
        }
        if (!ownerKey.IsValid()) {
            return DigiDollarMintResult(TransactionCreationFailed, "", "",
                tr("The wallet could not generate a DigiDollar owner key. Check that it is unlocked and that its receiving addresses have the required private keys."));
        }
        CPubKey ownerPubKey = ownerKey.GetPubKey();
        CKeyID ownerKeyID = ownerPubKey.GetID();

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 6 - Generated owner key - PubKey: %s, KeyID: %s\n",
                  HexStr(ownerPubKey), HexStr(ownerKeyID));

        // Step 7: Build mint transaction using custom MintTxBuilder that has UTXO access
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 7 - Building mint transaction...\n");

        // Create a custom builder class that can access UTXO values
        class QtMintTxBuilder : public DigiDollar::MintTxBuilder {
        private:
            const std::map<COutPoint, CAmount>& m_utxo_values;
        public:
            QtMintTxBuilder(const CChainParams& params, int height, CAmount price,
                          const std::map<COutPoint, CAmount>& utxo_values)
                : MintTxBuilder(params, height, price), m_utxo_values(utxo_values) {}

            CAmount GetDGBFromUTXO(const COutPoint& outpoint) const override {
                auto it = m_utxo_values.find(outpoint);
                if (it != m_utxo_values.end()) {
                    return it->second;
                }
                return 0; // UTXO not found
            }
        };

        QtMintTxBuilder builder(Params(), mintHeight, oraclePrice, utxoValues);
        if (candidateHealth) builder.SetCandidateHealth(*candidateHealth);

        DigiDollar::TxBuilderMintParams params;
        params.ddAmount = ddAmount;
        params.lockDays = lockDays;
        params.lockTier = lockTier;  // Store tier explicitly in OP_RETURN for exact reconstruction
        params.ownerKey = ownerKey;
        // DigiDollar Qt mint uses a fixed high rate; txbuilder also enforces
        // the DD minimum fee floor on constructed transactions.
        // TxBuilder formula: (vsize * feeRate) / 1000
        // For ~200 vB tx: (200 * 500000) / 1000 = 100,000 sats.
        params.feeRate = 500000;
        params.utxos = availableUtxos;

        // CRITICAL FIX: Get a proper change address from the wallet for DGB change output
        // This ensures the wallet recognizes the change output as its own!
        {
            LOCK(pWallet->cs_wallet);
            auto op_dest = pWallet->GetNewChangeDestination(OutputType::BECH32);
            if (op_dest) {
                params.dgbChangeDest = *op_dest;
                LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt Mint: Using wallet change address for DGB change output\n");
            } else {
                LogPrintf("DigiDollar Qt Mint: WARNING - Could not get change destination!\n");
            }
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: TxBuilder params - DD: %d cents, Days: %d, Height: %d, Price: %d, UTXOs: %d\n",
                  ddAmount, lockDays, currentHeight, oraclePrice, availableUtxos.size());

        DigiDollar::TxBuilderResult result = builder.BuildMintTransaction(params);

        // Auto-consolidate if mint failed due to UTXO fragmentation
        // (mirrors the RPC mintdigidollar consolidation logic)
        std::string consolidation_txid;
        if (!result.success && result.error.find("Too many small UTXOs") != std::string::npos) {
            LogPrintf("DigiDollar Qt: UTXO fragmentation detected (%zu UTXOs). Auto-consolidating...\n",
                      availableUtxos.size());

            auto sort_available_utxos_by_value = [&]() {
                std::sort(availableUtxos.begin(), availableUtxos.end(),
                    [&](const COutPoint& a, const COutPoint& b) {
                        const CAmount av = utxoValues.count(a) ? utxoValues.at(a) : 0;
                        const CAmount bv = utxoValues.count(b) ? utxoValues.at(b) : 0;
                        if (av != bv) return av > bv;
                        return a < b;
                    });
            };

            auto commit_consolidation = [&](const CTransactionRef& consolidation_tx) -> std::optional<QString> {
                std::string commit_error;
                bool commit_success = false;
                LOCK(pWallet->cs_wallet);
                commit_success = pWallet->CommitTransaction(consolidation_tx, {}, {}, &commit_error);
                if (!commit_success) {
                    return QString("Auto-consolidation transaction rejected by mempool: %1")
                        .arg(QString::fromStdString(commit_error));
                }
                return std::nullopt;
            };

            sort_available_utxos_by_value();

            CAmount minRequired = result.collateralRequired + 20000000; // collateral + 0.2 DGB margin
            if (totalAvailable < minRequired) {
                return DigiDollarMintResult(AmountExceedsBalance, "", "",
                    QString("Insufficient funds for collateral. Need %1 DGB, have %2 DGB.")
                        .arg(result.collateralRequired / 100000000.0, 0, 'f', 2)
                        .arg(totalAvailable / 100000000.0, 0, 'f', 2));
            }

            CTxDestination consolidationDest;
            {
                LOCK(pWallet->cs_wallet);
                auto op_dest = pWallet->GetNewChangeDestination(OutputType::BECH32);
                if (!op_dest) {
                    return DigiDollarMintResult(TransactionCreationFailed, "", "",
                        "Failed to get consolidation address");
                }
                consolidationDest = *op_dest;
            }

            // Multi-pass consolidation: MAX_STANDARD_TX_WEIGHT is 400k WU.
            // P2WPKH input ~271 WU. Conservative limit: 1400 inputs per pass.
            static const size_t MAX_CONSOLIDATION_INPUTS = 1400;
            static const int MAX_CONSOLIDATION_PASSES = 10;
            int pass = 0;
            std::vector<COutPoint> consolidatedUtxos;
            std::map<COutPoint, CAmount> consolidatedValues;

            for (size_t offset = 0; offset < availableUtxos.size() && pass < MAX_CONSOLIDATION_PASSES;) {
                ++pass;
                size_t batch_size = std::min(availableUtxos.size() - offset, MAX_CONSOLIDATION_INPUTS);
                LogPrintf("DigiDollar Qt: Consolidation pass %d — sweeping %zu of %zu UTXOs\n",
                          pass, batch_size, availableUtxos.size());

                wallet::CCoinControl coin_control;
                CAmount batchTotal = 0;
                for (size_t i = 0; i < batch_size; ++i) {
                    const COutPoint& utxo = availableUtxos[offset + i];
                    coin_control.Select(utxo);
                    batchTotal += utxoValues[utxo];
                }
                coin_control.m_allow_other_inputs = false;

                wallet::CRecipient recipient{consolidationDest, batchTotal, /*subtract_fee=*/true};
                std::vector<wallet::CRecipient> recipients = {recipient};

                auto consolidation_result = wallet::CreateTransaction(*pWallet, recipients, /*change_pos=*/-1, coin_control, /*sign=*/true);
                if (!consolidation_result) {
                    return DigiDollarMintResult(TransactionCreationFailed, "", "",
                        QString("Auto-consolidation pass %1 failed: %2")
                            .arg(pass)
                            .arg(QString::fromStdString(util::ErrorString(consolidation_result).original)));
                }

                const CTransactionRef& consolidation_tx = consolidation_result->tx;
                consolidation_txid = consolidation_tx->GetHash().GetHex();
                if (auto error = commit_consolidation(consolidation_tx)) {
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", *error);
                }

                LogPrintf("DigiDollar Qt: Consolidation pass %d tx: %s (swept %.2f DGB from %zu inputs)\n",
                          pass, consolidation_txid, batchTotal / 100000000.0, batch_size);

                COutPoint consolidated_outpoint(consolidation_tx->GetHash(), 0);
                consolidatedUtxos.push_back(consolidated_outpoint);
                consolidatedValues[consolidated_outpoint] = consolidation_tx->vout[0].nValue;
                offset += batch_size;
            }

            if (consolidatedUtxos.empty() || consolidatedUtxos.size() > MAX_CONSOLIDATION_PASSES) {
                return DigiDollarMintResult(TransactionCreationFailed, "", "",
                    "Auto-consolidation failed: too many fragmented UTXOs. Try manually consolidating UTXOs.");
            }

            availableUtxos = std::move(consolidatedUtxos);
            utxoValues = std::move(consolidatedValues);

            LogPrintf("DigiDollar Qt: After consolidation: %zu UTXOs available (passes: %d)\n",
                      availableUtxos.size(), pass);

            // Retry mint with consolidated UTXOs
            QtMintTxBuilder retryBuilder(Params(), mintHeight, oraclePrice, utxoValues);
            if (candidateHealth) retryBuilder.SetCandidateHealth(*candidateHealth);
            params.utxos = availableUtxos;
            result = retryBuilder.BuildMintTransaction(params);
        }

        if (!result.success) {
            LogPrintf("DigiDollar Qt: ERROR - BuildMintTransaction failed: %s\n", result.error);
            return DigiDollarMintResult(TransactionCreationFailed, "", "",
                QString::fromStdString("Failed to build mint transaction: " + result.error));
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Transaction built successfully!\n");
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - Inputs: %d, Outputs: %d\n", result.tx.vin.size(), result.tx.vout.size());
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - Collateral required: %d sats (%.8f DGB)\n",
                  result.collateralRequired, result.collateralRequired / 100000000.0);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - Total fees: %d sats (%.8f DGB)\n",
                  result.totalFees, result.totalFees / 100000000.0);

        // Step 8: Sign the transaction
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 8 - Signing transaction...\n");
        bool signSuccess = false;
        {
            LOCK(pWallet->cs_wallet);
            signSuccess = pWallet->SignTransaction(result.tx);
        }

        if (!signSuccess) {
            LogPrintf("DigiDollar Qt: ERROR - Failed to sign transaction\n");
            // Check if transaction has any inputs that need signing
            LogPrintf("DigiDollar Qt: Transaction has %d inputs\n", result.tx.vin.size());
            for (size_t i = 0; i < result.tx.vin.size(); i++) {
                LogPrintf("DigiDollar Qt: Input %d: %s:%d - scriptSig size: %d\n",
                          i, result.tx.vin[i].prevout.hash.GetHex(), result.tx.vin[i].prevout.n,
                          result.tx.vin[i].scriptSig.size());
            }
            return DigiDollarMintResult(TransactionCreationFailed, "", "",
                "Failed to sign mint transaction. Check wallet keys.");
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Transaction signed successfully\n");

        // Step 9: Create transaction reference for broadcast
        CTransactionRef txRef = MakeTransactionRef(result.tx);
        uint256 txId = txRef->GetHash();
        uint256 positionId = txId; // Position ID is the mint transaction ID

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 9 - Transaction ID: %s\n", txId.GetHex());

        DigiDollarWallet* ddWallet = m_wallet->getDigiDollarWallet();
        if (!ddWallet) {
            LogPrintf("DigiDollar Qt: ERROR - DD wallet not available, nothing to save the mint in\n");
            return DigiDollarMintResult(TransactionCreationFailed, "", "",
                "DigiDollar wallet not initialized. Nothing was sent.");
        }

        // Step 10: Save the owner key and the position record BEFORE the
        // transaction leaves this node. Once it is sent it can be mined
        // whether or not this process survives, so the records that make the
        // vault redeemable have to be on disk first. Every write reports
        // failure, and a failure stops the mint here, before anything is sent.
        // DigiByte has 15-second blocks: 4 blocks/min * 60 min/hr * 24 hr/day = 5760 blocks/day
        int64_t lockBlocks = DigiDollar::LockDaysToBlocks(lockDays);
        int64_t unlockHeight = mintHeight + lockBlocks + DigiDollar::MINT_LOCK_CONFIRMATION_BUFFER_BLOCKS;
        WalletCollateralPosition position(positionId, ddAmount, result.collateralRequired, lockTier, unlockHeight);
        position.owner_keyid = ownerKeyID;  // Store the owner key ID for later transfer signing
        {
            std::string persist_error;
            if (!ddWallet->RecordPendingMint(*txRef, position, ownerKey, persist_error)) {
                // Drop whatever partial record was written; the owner key is kept.
                std::string cleanup_error;
                if (!ddWallet->ReleaseMintAttempt(positionId, cleanup_error)) {
                    LogPrintf("DigiDollar Qt: cleanup after failed save of %s: %s\n",
                              positionId.GetHex(), cleanup_error);
                }
                LogPrintf("DigiDollar Qt: ERROR - Could not save the mint before sending it: %s\n", persist_error);
                return DigiDollarMintResult(TransactionCreationFailed, "", "",
                    QString("Mint not broadcast: %1. Nothing was sent; check the wallet file and free disk space, then try again.")
                        .arg(QString::fromStdString(persist_error)));
            }
        }
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 10 - Saved position %s (%d DD cents) and its owner key before broadcast\n",
                  positionId.GetHex(), ddAmount);

        // From here until the transaction has been committed, anything that
        // stops this mint must not leave those records behind, because they
        // would describe a transaction that was never sent. This releases them
        // whichever way the mint gives up, including the check below that the
        // chain and the oracle quote have not moved.
        DigiDollarWallet::SavedMintCleanup saved_mint_cleanup(*ddWallet, positionId, "DigiDollar Qt Mint");

        // Step 11: Commit through the wallet relay path, which broadcasts once
        // and updates wallet/mempool state from the same code path.
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Step 11 - Committing transaction through wallet relay...\n");

        std::string commit_error;
        bool commit_success = false;
        {
            LOCK(pWallet->cs_wallet);
            // The RPC copies chain state and releases its lock before the wallet commit.
            if (candidateHealth) {
                UniValue rpc_params(UniValue::VARR);
                const UniValue protection = m_node.executeRpc("getprotectionstatus", rpc_params, "");
                const UniValue& current = protection.find_value("next_block_health");
                if (current.write() != candidateHealthSnapshot.write())
                    return DigiDollarMintResult(TransactionCreationFailed, "", "", "Candidate health or oracle quote changed; retry mint construction.");
            }
            commit_success = pWallet->CommitTransaction(txRef, {}, {}, &commit_error);
        }
        if (!commit_success) {
            LogPrintf("DigiDollar Qt: ERROR - Failed to commit transaction: %s\n", commit_error);
            // The mempool (or the wallet itself) refused the transaction. The
            // cleanup set up above releases what this attempt reserved as the
            // mint gives up: its DGB inputs, the coin locks on the collateral
            // and token outputs, and its active status. The owner key and the
            // record of the attempt stay, so a reorg or a late confirmation can
            // bring the vault back. Abandoning the transaction also keeps a
            // refused mint out of the transaction list, which otherwise shows
            // any DigiDollar-shaped wallet transaction as collateral lock
            // activity that never happened.
            return DigiDollarMintResult(TransactionCreationFailed, "", "",
                QString("Failed to broadcast transaction: %1").arg(QString::fromStdString(commit_error)));
        }
        // The transaction is in the wallet now, so the records saved above
        // describe something real. Keep them.
        saved_mint_cleanup.KeepRecords();
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Transaction broadcast successful!\n");
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Position stored in wallet - ID: %s, DD: %d, DGB: %d, Tier: %d\n",
                  positionId.GetHex(), ddAmount, result.collateralRequired, lockTier);

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: ========== MINT DIGIDOLLAR SUCCESS ==========\n");
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Summary:\n");
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - TxID: %s\n", txId.GetHex());
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - Position ID: %s\n", positionId.GetHex());
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - DD Minted: %d cents ($%.2f)\n", ddAmount, ddAmount / 100.0);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - Collateral: %d sats (%.8f DGB)\n",
                  result.collateralRequired, result.collateralRequired / 100000000.0);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - Fees: %d sats (%.8f DGB)\n",
                  result.totalFees, result.totalFees / 100000000.0);
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: - Lock tier: %d (%d days)\n", lockTier, lockDays);

        Q_EMIT digiDollarChanged();

        return DigiDollarMintResult(OK,
                                  QString::fromStdString(txId.GetHex()),
                                  QString::fromStdString(positionId.GetHex()),
                                  "",
                                  result.collateralRequired);

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: ========== MINT DIGIDOLLAR FAILED ==========\n");
        LogPrintf("DigiDollar Qt: Exception: %s\n", e.what());
        return DigiDollarMintResult(TransactionCreationFailed, "", "", QString::fromStdString(e.what()));
    }
}

WalletModel::DigiDollarRedeemResult WalletModel::redeemDigiDollar(const QString& positionId, CAmount amount, const QString& redeemAddress)
{
    // Validate position ID format
    if (positionId.length() != 64 || !positionId.contains(QRegularExpression("^[0-9a-fA-F]{64}$"))) {
        return DigiDollarRedeemResult(InvalidAmount, "", "Invalid position ID format");
    }

    // Check if wallet is locked
    if (getEncryptionStatus() == Locked) {
        return DigiDollarRedeemResult(TransactionCreationFailed, "", "Wallet is locked. Please unlock to redeem DigiDollar.");
    }

    // Check if amount is positive
    if (amount <= 0) {
        return DigiDollarRedeemResult(InvalidAmount, "", "Redeem amount must be positive");
    }

    // Validate redemption address if provided
    if (!redeemAddress.isEmpty()) {
        if (!validateAddress(redeemAddress)) {
            return DigiDollarRedeemResult(InvalidAddress, "", "Invalid DGB address for redemption");
        }
    }

    try {
        // Convert position ID to uint256
        uint256 positionIdHash;
        positionIdHash.SetHex(positionId.toStdString());
        if (positionIdHash.IsNull()) {
            return DigiDollarRedeemResult(InvalidAmount, "", "Invalid position ID hex format");
        }

        // The RPC captures its candidate before taking wallet locks; do not hold one here.
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Calling redeemdigidollar RPC - Position: %s, Amount: %d cents\n",
                  positionId.toStdString(), amount);

        UniValue params(UniValue::VARR);
        params.push_back(positionId.toStdString());
        params.push_back(amount);
        if (!redeemAddress.isEmpty()) {
            params.push_back(redeemAddress.toStdString());
        }

        UniValue result;
        try {
            // Construct wallet URI like rpcconsole does
            QByteArray encodedName = QUrl::toPercentEncoding(getWalletName());
            std::string uri = "/wallet/" + std::string(encodedName.constData(), encodedName.length());
            result = m_node.executeRpc("redeemdigidollar", params, uri);
        } catch (const UniValue& objError) {
            std::string errorMsg = objError.find_value("message").get_str();
            LogPrintf("DigiDollar Qt: Redeem RPC failed - %s\n", errorMsg);
            return DigiDollarRedeemResult(TransactionCreationFailed, "", QString::fromStdString(errorMsg));
        } catch (const std::exception& e) {
            LogPrintf("DigiDollar Qt: Redeem RPC exception - %s\n", e.what());
            return DigiDollarRedeemResult(TransactionCreationFailed, "", QString::fromStdString(e.what()));
        }

        // Extract transaction ID from result
        std::string txid = result.find_value("txid").get_str();
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Redemption successful - TxID: %s\n", txid);

        Q_EMIT digiDollarChanged();

        return DigiDollarRedeemResult(OK, QString::fromStdString(txid), "");

    } catch (const std::exception& e) {
        return DigiDollarRedeemResult(TransactionCreationFailed, "", QString::fromStdString(e.what()));
    }
}

CAmount WalletModel::getDigiDollarBalance() const
{
    // Query actual DigiDollar balance from wallet
    // DD balance is tracked separately from regular DGB balance
    // Balance is stored in cents (e.g., 10000 = $100.00 DD)

    try {
        if (m_wallet->privateKeysDisabled()) {
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: getDigiDollarBalance returning 0 for private-key-disabled wallet\n");
            return 0;
        }

        // Get DigiDollar wallet instance from wallet interface
        DigiDollarWallet* ddWallet = m_wallet->getDigiDollarWallet();
        if (!ddWallet) {
            LogPrintf("DigiDollar Qt: DD wallet not available\n");
            return 0;
        }

        // Get the total DD balance
        CAmount balance = ddWallet->GetTotalDDBalance();

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: getDigiDollarBalance returning %d cents\n", balance);
        return balance;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: getDigiDollarBalance exception - %s\n", e.what());
        return 0;
    }
}

CAmount WalletModel::getPendingDigiDollarBalance() const
{
    try {
        if (m_wallet->privateKeysDisabled()) {
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: getPendingDigiDollarBalance returning 0 for private-key-disabled wallet\n");
            return 0;
        }

        DigiDollarWallet* ddWallet = m_wallet->getDigiDollarWallet();
        if (!ddWallet) {
            return 0;
        }

        CAmount pending = ddWallet->GetPendingDDBalance();
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: getPendingDigiDollarBalance returning %d cents\n", pending);
        return pending;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: getPendingDigiDollarBalance exception - %s\n", e.what());
        return 0;
    }
}

CAmount WalletModel::getLockedCollateral() const
{
    try {
        DigiDollarWallet* ddWallet = m_wallet->getDigiDollarWallet();
        if (!ddWallet) {
            return 0;
        }

        CAmount locked = ddWallet->GetLockedCollateral();
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: getLockedCollateral returning %d satoshis\n", locked);
        return locked;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: getLockedCollateral exception - %s\n", e.what());
        return 0;
    }
}

CAmount WalletModel::getAvailableDGBBalance() const
{
    // Get the regular DGB balance from the wallet
    const interfaces::WalletBalances& balances = getCachedBalance();
    return balances.balance;
}

bool WalletModel::validateDigiDollarAddress(const QString& address) const
{
    return CDigiDollarAddress::IsValidDigiDollarAddressForCurrentNetwork(address.toStdString());
}

CAmount WalletModel::calculateRequiredCollateral(CAmount ddAmount, int lockTier) const
{
    if (lockTier < 0 || lockTier > 9) {
        return 0;
    }

    // Get oracle price based on network type
    // MockOracleManager for RegTest only, RPC for testnet/mainnet
    CAmount oraclePriceMicroUSD = 0;
    ChainType chainType = Params().GetChainType();

    if (chainType == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
        oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
    } else {
        // Get from RPC for testnet/mainnet
        try {
            UniValue params(UniValue::VARR);
            UniValue result = m_node.executeRpc("getoracleprice", params, "");
            const UniValue& priceVal = result.find_value("price_micro_usd");
            if (!priceVal.isNull()) {
                oraclePriceMicroUSD = priceVal.getInt<int64_t>();
            }
        } catch (...) {
            // If RPC fails, return 0 to indicate calculation failed
            return 0;
        }
    }

    const int currentHeight = m_client_model ? m_client_model->getNumBlocks() : 0;
    const bool thaw_active = DigiDollar::IsThawDayActive(Params().GetConsensus(), currentHeight + 1);
    std::optional<int> candidateHealth;
    if (thaw_active) {
        try {
            UniValue rpc_params(UniValue::VARR);
            const UniValue response = m_node.executeRpc("getoracleprice", rpc_params, "");
            const UniValue& status = response.find_value("mint_volatility");
            if (!status.find_value("quote_available").get_bool()) return 0;
            oraclePriceMicroUSD = status.find_value("candidate_price_micro_usd").getInt<int64_t>();
            const UniValue protection = m_node.executeRpc("getprotectionstatus", rpc_params, "");
            const UniValue& health = protection.find_value("next_block_health");
            if (!health.find_value("ready").get_bool()) return 0;
            candidateHealth = health.find_value("health_percentage").getInt<int>();
            oraclePriceMicroUSD = health.find_value("oracle_price_micro_usd").getInt<int64_t>();
        } catch (...) { return 0; }
    }

    if (oraclePriceMicroUSD <= 0) {
        return 0;
    }

    static constexpr int LOCK_DAYS_FOR_TIER[10] = {0, 30, 90, 180, 365, 730, 1095, 1825, 2555, 3650};
    DigiDollar::MintTxBuilder builder(Params(), thaw_active ? currentHeight + 1 : currentHeight, oraclePriceMicroUSD);
    if (candidateHealth) builder.SetCandidateHealth(*candidateHealth);
    return builder.CalculateRequiredCollateral(ddAmount, LOCK_DAYS_FOR_TIER[lockTier]);
}

UniValue WalletModel::executeRpc(const std::string& command, const UniValue& params) const
{
    // Construct wallet URI like rpcconsole does
    QByteArray encodedName = QUrl::toPercentEncoding(getWalletName());
    std::string uri = "/wallet/" + std::string(encodedName.constData(), encodedName.length());
    return m_node.executeRpc(command, params, uri);
}

QString WalletModel::getNewDigiDollarAddress(const QString& label)
{
    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: getNewDigiDollarAddress called with label: %s\n", label.toStdString());

    try {
        // Get the wallet pointer
        wallet::CWallet* pWallet = wallet().wallet();
        if (!pWallet) {
            LogPrintf("DigiDollar Qt: ERROR - Wallet pointer not available\n");
            return QString();
        }

        if (m_wallet->privateKeysDisabled()) {
            LogPrintf("DigiDollar Qt: refusing to generate DigiDollar address for private-key-disabled wallet\n");
            return QString();
        }

        // Generate a new Taproot (P2TR) destination for DigiDollar
        // DigiDollar addresses must be P2TR (Taproot) type
        auto result = m_wallet->getNewDestination(OutputType::BECH32M, label.toStdString());

        if (!result) {
            LogPrintf("DigiDollar Qt: ERROR - Failed to generate new destination: %s\n",
                     util::ErrorString(result).translated);
            return QString();
        }

        CTxDestination dest = *result;

        // Convert the destination to DigiDollar address format
        // DigiDollar addresses use DD (mainnet), TD (testnet), RD (regtest) prefixes
        std::string ddAddress = EncodeDigiDollarAddress(dest);

        if (ddAddress.empty()) {
            LogPrintf("DigiDollar Qt: ERROR - Failed to encode DigiDollar address\n");
            return QString();
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar Qt: Generated DD address: %s\n", ddAddress);

        // Add address to address book with label if provided
        // Use DIGIDOLLAR purpose to distinguish from regular DGB addresses
        if (!label.isEmpty()) {
            m_wallet->setAddressBook(dest, label.toStdString(), wallet::AddressPurpose::DIGIDOLLAR);
        } else {
            m_wallet->setAddressBook(dest, "", wallet::AddressPurpose::DIGIDOLLAR);
        }

        return QString::fromStdString(ddAddress);

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar Qt: getNewDigiDollarAddress exception - %s\n", e.what());
        return QString();
    }
}

DigiDollarWallet* WalletModel::getDigiDollarWallet() const
{
    return m_wallet ? m_wallet->getDigiDollarWallet() : nullptr;
}
