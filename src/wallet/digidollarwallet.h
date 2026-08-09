// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_DIGIDOLLARWALLET_H
#define DIGIBYTE_WALLET_DIGIDOLLARWALLET_H

#include <consensus/amount.h>
#include <key.h>
#include <sync.h>
#include <support/allocators/secure.h>
#include <wallet/wallet.h>
#include <digidollar/digidollar.h>
#include <digidollar/txbuilder.h>
#include <base58.h>

#include <array>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Use existing CDigiDollarAddress from base58.h

/**
 * DigiDollar transaction structure for wallet
 * Represents a DD transaction from wallet perspective
 */
struct DDTransaction {
    std::string txid;
    CAmount amount;         // DD amount in cents
    uint64_t timestamp;
    int confirmations;
    bool incoming;          // true for receives, false for sends
    std::string address;    // counterparty address
    std::string category;   // "send", "receive", "mint", "redeem"
    int blockheight;        // Block height when confirmed (-1 if unconfirmed)
    std::string blockhash;  // Block hash when confirmed (empty if unconfirmed)
    CAmount fee;            // Transaction fee paid (0 if not applicable)
    std::string comment;    // Transaction comment (empty if none)
    bool abandoned;         // Whether transaction was abandoned
    int lock_tier;          // Lock tier for mints (0-9), -1 for non-mint transactions
    bool in_mempool;        // Transient display state, not serialized
    bool is_local;          // Transient display state for unconfirmed, unrelayed wallet txs

    DDTransaction();

    SERIALIZE_METHODS(DDTransaction, obj)
    {
        READWRITE(obj.txid);
        READWRITE(obj.amount);
        READWRITE(obj.timestamp);
        READWRITE(obj.confirmations);
        READWRITE(obj.incoming);
        READWRITE(obj.address);
        READWRITE(obj.category);
        READWRITE(obj.blockheight);
        READWRITE(obj.blockhash);
        READWRITE(obj.fee);
        READWRITE(obj.comment);
        READWRITE(obj.abandoned);
        READWRITE(obj.lock_tier);
    }
};

// =============================================================================
// PHASE 5 WALLET CORE STRUCTURES (Tasks 5.1-5.3)
// =============================================================================

/**
 * Database schema for wallet.dat DD extension (Task 5.1)
 * Defines structures for storing DD wallet data
 */
struct WalletDDBalance {
    CDigiDollarAddress address;
    CAmount balance;
    int64_t last_updated;

    WalletDDBalance() : balance(0), last_updated(0) {}
    WalletDDBalance(const CDigiDollarAddress& addr, CAmount bal)
        : address(addr), balance(bal), last_updated(0) {}

    SERIALIZE_METHODS(WalletDDBalance, obj)
    {
        READWRITE(obj.address);
        READWRITE(obj.balance);
        READWRITE(obj.last_updated);
    }
};

struct WalletCollateralPosition {
    uint256 dd_timelock_id;  // txid of mint tx
    CAmount dd_minted;
    CAmount dgb_collateral;
    uint32_t lock_tier;
    int64_t unlock_height;
    bool is_active;
    CKeyID owner_keyid;      // Key ID that owns this DD time-lock

    WalletCollateralPosition()
        : dd_minted(0), dgb_collateral(0), lock_tier(0), unlock_height(0), is_active(false) {}
    WalletCollateralPosition(const uint256& id, CAmount dd, CAmount dgb, uint32_t tier, int64_t height)
        : dd_timelock_id(id), dd_minted(dd), dgb_collateral(dgb), lock_tier(tier), unlock_height(height), is_active(true) {}

    SERIALIZE_METHODS(WalletCollateralPosition, obj)
    {
        READWRITE(obj.dd_timelock_id);
        READWRITE(obj.dd_minted);
        READWRITE(obj.dgb_collateral);
        READWRITE(obj.lock_tier);
        READWRITE(obj.unlock_height);
        READWRITE(obj.is_active);
        READWRITE(obj.owner_keyid);
    }
};

/**
 * Represents a spendable DigiDollar UTXO
 * DD UTXOs may be mint token outputs or later transfer outputs.
 */
struct DDUtxo {
    COutPoint outpoint;     // UTXO reference
    CAmount dd_amount;      // DD amount in cents
    bool is_spendable;      // Always true for active DDTimeLocks

    DDUtxo(const COutPoint& out, CAmount amt)
        : outpoint(out), dd_amount(amt), is_spendable(true) {}
};

struct DDTransferPlan {
    std::vector<std::pair<std::string, CAmount>> recipients;
    std::vector<COutPoint> dd_utxos;
    std::vector<CAmount> dd_amounts;
    CAmount total_amount{0};
    CAmount selected_dd_total{0};
    CAmount dd_change{0};
    size_t projected_vsize{0};
    CAmount estimated_fee{0};
};

/** Outcome of DigiDollarWallet::SelectRedemptionFeeCoins(). */
enum class DDFeeSelectionResult {
    OK,
    //! The wallet cannot fund the fee: not enough spendable DGB once the cost of
    //! spending each fee input is accounted for, or the DGB is too fragmented.
    INSUFFICIENT_FUNDS,
    //! The redemption cannot be built as specified: invalid fee rate, or the
    //! projected transaction exceeds MAX_STANDARD_TX_WEIGHT.
    INVALID_TRANSACTION,
};

/**
 * DigiDollar wallet functionality
 * Provides high-level interface for DD operations
 * Enhanced with Phase 5 core functions (Tasks 5.1-5.3)
 */
class DigiDollarWallet {
public:
    /**
     * Mutex guarding all DigiDollar wallet data structures.
     * Must be held when accessing dd_utxos, dd_owner_keys, dd_address_keys,
     * collateral_positions, dd_balances, transaction_history, and balance totals.
     *
     * Lock ordering: When both cs_wallet and cs_dd_wallet are needed, ALWAYS
     * acquire cs_wallet first to prevent deadlocks. Use LOCK2(m_wallet->cs_wallet,
     * cs_dd_wallet) for methods that need both.
     */
    mutable RecursiveMutex cs_dd_wallet;

private:
    // Mock data for testing - in real implementation would use actual wallet
    CAmount mockBalance GUARDED_BY(cs_dd_wallet);
    std::vector<DDTransaction> mockHistory GUARDED_BY(cs_dd_wallet);
    std::vector<CDigiDollarOutput> mockUTXOs GUARDED_BY(cs_dd_wallet);

    // Phase 5 additions: Core wallet data structures (Task 5.1)
    std::map<std::string, WalletDDBalance> dd_balances GUARDED_BY(cs_dd_wallet);
    std::map<uint256, WalletCollateralPosition> collateral_positions GUARDED_BY(cs_dd_wallet);
    std::vector<DDTransaction> transaction_history GUARDED_BY(cs_dd_wallet);
    std::set<uint256> pending_outgoing_dd_txs GUARDED_BY(cs_dd_wallet);

    // FIX #1: Track actual DD UTXOs (not just positions)
    // Maps (txid, vout) → DD amount in cents
    // This replaces the broken assumption that DD is always at (mint_txid, 1)
    std::map<COutPoint, CAmount> dd_utxos GUARDED_BY(cs_dd_wallet);

    // Internal state tracking (Task 5.2)
    CAmount total_dd_balance GUARDED_BY(cs_dd_wallet);
    CAmount locked_collateral GUARDED_BY(cs_dd_wallet);

    // DD owner keys storage (for signing transfers) — plaintext, only when wallet is NOT encrypted
    // Maps dd_timelock_id -> owner CKey
    std::map<uint256, CKey> dd_owner_keys GUARDED_BY(cs_dd_wallet);

    // DD address keys storage (for received DD tokens) — plaintext, only when wallet is NOT encrypted
    // Maps XOnlyPubKey (output_key from P2TR address) -> owner CKey
    // This enables spending DD received at addresses we generated via getdigidollaraddress
    std::map<std::array<unsigned char, 32>, CKey> dd_address_keys GUARDED_BY(cs_dd_wallet);

    // Encrypted DD owner keys (T4-03a: wallet encryption support)
    // Maps dd_timelock_id -> (pubkey, encrypted_secret)
    // Used when wallet is encrypted; decrypted on-demand via GetOwnerKey()
    std::map<uint256, std::pair<CPubKey, std::vector<unsigned char>>> dd_crypted_owner_keys GUARDED_BY(cs_dd_wallet);

    // Encrypted DD address keys (T4-03a: wallet encryption support)
    // Maps output_key_bytes -> (pubkey, encrypted_secret)
    std::map<std::array<unsigned char, 32>, std::pair<CPubKey, std::vector<unsigned char>>> dd_crypted_address_keys GUARDED_BY(cs_dd_wallet);

    // Ephemeral cache for DD output keys that failed descriptor ownership recovery.
    // This prevents repeated wallet-wide P2TR descriptor scans on UI/history refreshes.
    mutable std::set<std::array<unsigned char, 32>> dd_foreign_output_keys GUARDED_BY(cs_dd_wallet);

    // Pointer to wallet for UTXO access
    wallet::CWallet* m_wallet{nullptr};

    bool m_position_state_validation_pending GUARDED_BY(cs_dd_wallet){false};

    /** Null-safe dual lock: acquires cs_wallet (if m_wallet != nullptr) then cs_dd_wallet. */
    [[nodiscard]] std::pair<std::unique_lock<RecursiveMutex>,
                            std::unique_lock<RecursiveMutex>> LockDDWallet() const
    {
        if (m_wallet) {
            std::unique_lock<RecursiveMutex> wl(m_wallet->cs_wallet);
            std::unique_lock<RecursiveMutex> dl(cs_dd_wallet);
            return {std::move(wl), std::move(dl)};
        }
        return {std::unique_lock<RecursiveMutex>(),
                std::unique_lock<RecursiveMutex>(cs_dd_wallet)};
    }

public:
    DigiDollarWallet();
    DigiDollarWallet(wallet::CWallet* wallet);
    virtual ~DigiDollarWallet() = default;

    // Set wallet pointer (for initialization)
    void SetWallet(wallet::CWallet* wallet) { m_wallet = wallet; }

    // ====================================================================
    // DD OWNER KEY MANAGEMENT (for Taproot/Descriptor wallet compatibility)
    // ====================================================================

    /**
     * Encrypt all existing plaintext DD keys using the wallet master key.
     * Called from CWallet::EncryptWallet() after master key is set up.
     * Converts all dd_owner_keys and dd_address_keys from plaintext to encrypted,
     * erases plaintext entries from wallet.dat, and writes encrypted entries.
     * @param vMasterKey The wallet encryption master key
     * @param encrypted_batch Optional batch for atomic write (used during EncryptWallet)
     * @return true if all keys encrypted successfully
     */
    bool EncryptDDKeys(const wallet::CKeyingMaterial& vMasterKey, wallet::WalletBatch* encrypted_batch = nullptr);

    /**
     * Store DD owner key for a time-lock position
     * @param dd_timelock_id The time-lock position ID (mint tx hash)
     * @param key The owner private key
     * @note If wallet is encrypted, key is encrypted before storage.
     *       Persists the key to the wallet database for survival across restarts.
     */
    void StoreOwnerKey(const uint256& dd_timelock_id, const CKey& key);

    /**
     * Load DD owner keys from wallet database
     * Called during wallet initialization to restore persisted keys
     * @return Number of keys loaded
     */
    size_t LoadDDOwnerKeys();

    /**
     * Retrieve DD owner key for a time-lock position
     * If wallet is encrypted, decrypts the key using the master encryption key.
     * Wallet must be unlocked for this to succeed on encrypted wallets.
     * @param dd_timelock_id The time-lock position ID
     * @param key Output parameter for the key
     * @return true if key found and decrypted successfully
     */
    bool GetOwnerKey(const uint256& dd_timelock_id, CKey& key) const;

    // ====================================================================
    // DD ADDRESS KEY MANAGEMENT (for received DD tokens)
    // ====================================================================

    /**
     * Store DD address key for a P2TR output key
     * Called when getdigidollaraddress generates a new address
     * @param output_key The XOnlyPubKey (output key) from the P2TR address
     * @param key The private key that can sign for this address
     * @note If wallet is encrypted, key is encrypted before storage.
     *       Persists the key to the wallet database for survival across restarts.
     */
    void StoreAddressKey(const XOnlyPubKey& output_key, const CKey& key);

    /**
     * Load DD address keys from wallet database
     * Called during wallet initialization to restore persisted keys
     * @return Number of keys loaded
     */
    size_t LoadDDAddressKeys();

    /**
     * Retrieve DD address key for a P2TR output key
     * Used when spending received DD tokens.
     * If wallet is encrypted, decrypts the key using the master encryption key.
     * Wallet must be unlocked for this to succeed on encrypted wallets.
     * @param output_key The XOnlyPubKey (output key) from the P2TR address
     * @param key Output parameter for the private key
     * @return true if key found and decrypted successfully
     */
    bool GetAddressKey(const XOnlyPubKey& output_key, CKey& key) const;

    /**
     * Retrieve the internal signing key for a spendable DD P2TR output.
     * Rebuilds the DD address-key cache from descriptor wallet metadata when
     * a wallet has been restored from private descriptors.
     * @param txout The DD token output
     * @param key Output parameter for the private key
     * @return true if a spendable key was found
     */
    bool GetDDOutputSpendingKey(const CTxOut& txout, CKey& key);

    /**
     * Check if a DD output belongs to this wallet
     * Uses dd_owner_keys, dd_address_keys, and standard wallet IsMine
     * This is needed because IsMine() may fail for 0-value P2TR outputs
     * @param txout The transaction output to check
     * @param txid The transaction ID (used to look up dd_owner_keys)
     * @return true if we own this DD output
     */
    bool IsDDOutputMine(const CTxOut& txout, const uint256& txid) const;
    bool IsMyDDAddress(const std::string& addrStr) const;
    std::vector<std::string> GetKnownDDAddresses() const;
    void ClearDDOwnershipCache();

    /**
     * Check if a DD output belongs to this wallet using COutPoint
     * This overload checks dd_utxos first (source of truth for owned DD)
     * which is essential for detecting TRANSFER change outputs after wallet restore.
     * Mirrors how normal DGB uses mapWallet as source of truth for UTXO ownership.
     * @param outpoint The transaction outpoint to check
     * @return true if we own this DD output
     */
    bool IsDDOutputMine(const COutPoint& outpoint) const;

    // ====================================================================
    // PHASE 5 TASK 5.1: DATABASE EXTENSION FUNCTIONS
    // ====================================================================

    /**
     * Write DD balance to wallet database
     * @param addr DD address
     * @param balance Balance amount in cents
     * @return true if write successful
     */
    bool WriteDDBalance(const CDigiDollarAddress& addr, const CAmount& balance);

    /**
     * Write DDTimeLock (time-locked DGB backing DigiDollars) to wallet database
     * @param position DDTimeLock data (collateral position)
     * @return true if write successful
     */
    bool WriteDDTimeLock(const WalletCollateralPosition& position);

    /**
     * Update position active status
     * @param dd_timelock_id Position identifier
     * @param active New active status
     * @return true if update successful
     */
    bool UpdatePositionStatus(const uint256& dd_timelock_id, bool active);

    // ====================================================================
    // PHASE 5 TASK 5.2: BALANCE TRACKING FUNCTIONS
    // ====================================================================

    /**
     * Get DD balance for specific address or total if no address provided
     * @param addr DD address (empty for total balance)
     * @return DD balance in cents
     */
    CAmount GetDDBalance(const CDigiDollarAddress& addr = CDigiDollarAddress()) const;

    /**
     * Get total DD balance across all addresses (confirmed only).
     * Unconfirmed trusted UTXOs are excluded — use GetPendingDDBalance() for those.
     * @return Confirmed DD balance in cents
     */
    CAmount GetTotalDDBalance() const;

    /**
     * Get pending DD balance.
     * These are unconfirmed DD UTXOs, including trusted wallet-created mints
     * and transfer change. Pending DD is not spendable until confirmed.
     * @return Pending DD balance in cents
     */
    CAmount GetPendingDDBalance() const;

    /**
     * Scan wallet UTXOs for DigiDollar outputs and populate dd_balances map
     * Should be called ONLY at wallet startup, not on every block!
     * @return Number of DD UTXOs found
     */
    size_t ScanForDDUTXOs();

    /**
     * Post-rescan validation: check every active position against the UTXO set.
     * If the collateral output (COutPoint(dd_timelock_id, 0)) is NOT in the
     * UTXO set, the position was redeemed and is marked is_active = false.
     *
     * This is a belt-and-suspenders fix for wallet restore scenarios where
     * ProcessDDTxForRescan fails to detect REDEEM transactions (e.g., custom
     * Taproot MAST scripts not recognized as "ours" during rescan).
     *
     * Called automatically at the end of ScanForDDUTXOs().
     * @return Number of positions corrected (marked inactive)
     */
    size_t ValidatePositionStates();

    /**
     * Reconcile cached collateral-position active flags against chainstate.
     *
     * This lightweight wrapper is safe for RPC/Qt/read paths that need a
     * current position view without forcing a full wallet DD UTXO rescan.
     * @return Number of positions corrected
     */
    size_t ReconcilePositionStates();

    /**
     * Retry a position-state validation that was skipped before chainstate was ready.
     * Returns 0 if no retry is pending or if no positions were corrected.
     */
    size_t RetryPendingPositionStateValidation();

    /** Return whether startup/rescan position-state validation still needs a retry. */
    bool HasPendingPositionStateValidation() const;

    /**
     * Process a single transaction for DD UTXOs (incremental update)
     * Called during block processing - much faster than full rescan
     * @param tx Transaction to process
     * @param txid Transaction ID
     * @return true if any DD UTXOs were added/removed
     */
    bool ProcessTransactionForDD(const CTransaction& tx, const uint256& txid);

    /**
     * Get total locked collateral from active positions
     * @return Locked collateral amount in satoshis
     */
    CAmount GetLockedCollateral() const;

    /**
     * Get list of DigiDollar Time-Locked DGB vaults (DDTimeLocks)
     * These are Time-Locked DGB backing DigiDollar value
     * @param active_only Only return active time locks if true
     * @return Vector of DDTimeLock positions
     */
    std::vector<WalletCollateralPosition> GetDDTimeLocks(bool active_only = true) const;

    /**
     * Get tracked DigiDollar UTXOs. By default this returns spendable confirmed
     * DD UTXOs; read-only display code may opt in to include pending outputs.
     * @return Vector of DD UTXOs (output index 1 of each DDTimeLock)
     */
    std::vector<DDUtxo> GetDDUTXOs(bool include_unconfirmed = false) const;

    /**
     * Get DD amount from UTXO using DDTimeLock cache
     * @param outpoint UTXO outpoint (should be dd_timelock_id with n=1)
     * @return DD amount in cents, or 0 if UTXO not found or invalid
     */
    CAmount GetDDFromUTXO(const COutPoint& outpoint) const;

    /**
     * Add DD UTXO to tracking map (FIX #1)
     * @param outpoint UTXO outpoint (txid, vout)
     * @param dd_amount DD amount in cents
     */
    void AddDDUTXO(const COutPoint& outpoint, CAmount dd_amount);

    /**
     * Remove DD UTXO from tracking map when spent (FIX #1)
     * @param outpoint UTXO outpoint to remove
     */
    void RemoveDDUTXO(const COutPoint& outpoint);

    /**
     * Check if a DD UTXO exists in the tracking map (regardless of IsSpent status).
     * Used to verify UTXO persistence across TX lifecycle.
     * @param outpoint UTXO outpoint to check
     * @return true if the UTXO is tracked in dd_utxos map
     */
    bool HasDDUTXO(const COutPoint& outpoint) const;

    /**
     * Check if DD token UTXO for a position is still unspent
     * Used to determine if a vault can still be redeemed
     * @param dd_timelock_id Position ID (mint tx hash)
     * @return true if DD token UTXO is still spendable
     */
    bool IsDDTokenUnspent(const uint256& dd_timelock_id) const;

    /**
     * Add a collateral position to the wallet
     * @param position The position to add
     */
    void AddCollateralPosition(const WalletCollateralPosition& position);

    // ====================================================================
    // PHASE 5 TASK 5.3: TRANSACTION CREATION FUNCTIONS
    // ====================================================================

    /**
     * Create mint transaction using transaction builders
     * @param dd_amount Amount of DD to mint (in cents)
     * @param lock_tier Lock tier (0-9)
     * @param tx_out Output transaction reference
     * @return true if transaction created successfully
     */
    bool MintDigiDollar(const CAmount& dd_amount, uint32_t lock_tier, CTransactionRef& tx_out);

    /**
     * Create transfer transaction using transaction builders
     * @param to Recipient DD address
     * @param amount Amount to transfer in cents
     * @param tx_out Output transaction reference
     * @return true if transaction created successfully
     */
    bool TransferDigiDollar(const CDigiDollarAddress& to, CAmount amount, CTransactionRef& tx_out);

    /**
     * Create redemption transaction using transaction builders
     * @param dd_timelock_id Position to redeem
     * @param amount Amount of DD to redeem
     * @param tx_out Output transaction reference
     * @return true if transaction created successfully
     */
    bool RedeemDigiDollar(const uint256& dd_timelock_id, const CAmount& amount, CTransactionRef& tx_out);

    /**
     * Transfer DigiDollars to another address
     * @param to Recipient DD address
     * @param amount Amount to transfer in cents
     * @param txid Output parameter for transaction ID
     * @param error Output parameter for error message
     * @return true if transfer successful, false otherwise
     */
    bool TransferDigiDollarMany(const std::vector<std::pair<CDigiDollarAddress, CAmount>>& recipients,
                                std::string& txid, std::string& error,
                                CAmount* dd_change_out = nullptr,
                                const std::vector<COutPoint>* preset_dd_inputs = nullptr,
                                const std::string& comment = "");

    bool TransferDigiDollar(const CDigiDollarAddress& to, CAmount amount,
                            std::string& txid, std::string& error,
                            CAmount* dd_change_out = nullptr,
                            const std::vector<COutPoint>* preset_dd_inputs = nullptr,
                            const std::string& comment = "");

    /**
     * Get current DD balance (legacy method)
     * @return DD balance in cents
     */
    CAmount GetDDBalanceLegacy() const;

    /**
     * Get DD transaction history
     * @return Vector of DD transactions
     */
    std::vector<DDTransaction> GetDDTransactionHistory() const;

    /**
     * Validate DD address format
     * @param address DD address string to validate
     * @return true if valid, false otherwise
     */
    bool ValidateDDAddress(const std::string& address) const;

    // ====================================================================
    // WALLET RESTORE FUNCTIONS (Position reconstruction during rescan)
    // ====================================================================

    /**
     * Extract DD amount from OP_RETURN metadata in a DD transaction
     * @param tx Transaction to extract from
     * @param dd_amount Output: extracted DD amount in cents
     * @return true if extraction successful
     */
    static bool ExtractDDAmountFromOpReturn(const CTransaction& tx, CAmount& dd_amount);

    /**
     * Extract unlock height from OP_RETURN metadata in a DD mint transaction
     * @param tx Transaction to extract from
     * @param unlock_height Output: extracted unlock height
     * @return true if extraction successful
     */
    static bool ExtractUnlockHeightFromOpReturn(const CTransaction& tx, int64_t& unlock_height);

    /**
     * Extract lock tier from OP_RETURN metadata in a DD mint transaction
     * New MINT format includes tier explicitly to avoid derivation timing issues.
     * @param tx Transaction to extract from
     * @param lock_tier Output: extracted lock tier (0-9)
     * @return true if extraction successful (false for old txs without tier)
     */
    static bool ExtractTierFromOpReturn(const CTransaction& tx, uint32_t& lock_tier);

    /**
     * Derive lock tier from mint height and unlock height
     * DEPRECATED: Use ExtractTierFromOpReturn when available.
     * Kept for backward compatibility with older transactions.
     * @param mint_height Block height when minted
     * @param unlock_height Block height when unlockable
     * @return Lock tier (0-9)
     */
    static uint32_t DeriveLockTierFromHeight(int64_t mint_height, int64_t unlock_height);

    /**
     * Extract full position data from a mint transaction
     * @param tx Mint transaction
     * @param block_height Block height of the transaction
     * @param pos_out Output: extracted position data
     * @return true if extraction successful
     */
    bool ExtractPositionFromMintTx(const CTransaction& tx, int block_height, WalletCollateralPosition& pos_out);

    /**
     * Process DD transaction during wallet rescan to rebuild positions
     * Called by wallet.cpp SyncTransaction() when rescanning_old_block=true
     * Reconstructs position data from MINT transactions and marks positions
     * inactive when REDEEM transactions are found.
     * @param ptx Transaction reference
     * @param block_height Block height of the transaction
     */
    void ProcessDDTxForRescan(const CTransactionRef& ptx, int block_height);

    /**
     * Refresh cached position metadata from the authoritative mint transaction.
     * Existing wallet DB rows can predate current mint metadata rules; callers
     * use this before showing or signing redemptions.
     * @param position_id Mint transaction hash / position ID
     * @return true if authoritative metadata was available and cache now matches it
     */
    bool RefreshPositionMetadataFromMintTx(const uint256& position_id);

    /**
     * Resolve the actual collateral output index from the mint transaction.
     * Consensus identifies mint collateral by structure, not by fixed vout.
     */
    bool GetMintCollateralOutpoint(const uint256& position_id, COutPoint& collateral_outpoint) const;
    bool GetMintDDTokenOutpoint(const uint256& position_id, COutPoint& dd_token_outpoint) const;

    // ====================================================================
    // Redemption Functions (Task 3.9)
    // ====================================================================

    /**
     * Redeem DigiDollars and unlock collateral
     * @param collateralUtxo The collateral UTXO to unlock
     * @param ddAmount Amount of DD to redeem and burn
     * @param path Redemption path to use
     * @param txid Output parameter for transaction ID
     * @param error Output parameter for error message
     * @return true if redemption successful, false otherwise
     */
    bool RedeemDigiDollar(const COutPoint& collateralUtxo,
                         CAmount ddAmount,
                         DigiDollar::RedemptionPath path,
                         std::string& txid,
                         std::string& error);

    /**
     * Get list of redeemable positions
     * @return Vector of positions that can be redeemed
     */
    std::vector<DigiDollar::RedeemablePosition> GetRedeemablePositions() const;

    /**
     * Calculate redemption value for a position
     * @param position Position to calculate value for
     * @return Estimated DGB return amount
     */
    CAmount CalculateRedemptionValue(const COutPoint& position) const;

    /**
     * Check if position can be redeemed
     * @param position Position to check
     * @param availablePath Output parameter for best available path
     * @return true if position can be redeemed
     */
    bool CanRedeem(const COutPoint& position, DigiDollar::RedemptionPath& availablePath) const;

    /**
     * Get redemption transaction history
     * @return Vector of redemption transactions
     */
    std::vector<DDTransaction> GetRedemptionHistory() const;

    /**
     * Estimate redemption transaction fees
     * @param position Position to redeem
     * @param path Redemption path to use
     * @return Estimated fee amount in satoshis
     */
    CAmount EstimateRedemptionFee(const COutPoint& position, DigiDollar::RedemptionPath path) const;

    /**
     * Get current DGB balance (for fee payments)
     * @return DGB balance in satoshis
     */
    CAmount GetDGBBalance() const;

    // ====================================================================
    // PHASE 2: STATE MANAGEMENT - DD BURNING & POSITION CLOSURE (Task 6)
    // ====================================================================

    /**
     * Burn DigiDollars by selecting and marking UTXOs as spent (Task 6a)
     * @param amount Amount of DD to burn in cents
     * @param burnedUtxos Output parameter: list of UTXOs that were burned
     * @return true if burning successful, false otherwise
     */
    bool BurnDigiDollars(CAmount amount, std::vector<COutPoint>& burnedUtxos);

    /**
     * Close collateral position after full redemption (Task 6b)
     *
     * SECURITY: Only full redemptions are allowed. Partial redemptions are
     * architecturally impossible in the UTXO model — the entire collateral
     * UTXO is consumed as vin[0]. The user MUST burn all DD minted against
     * this collateral to release it (enforced by consensus at validation.cpp).
     *
     * @param outpoint The collateral position outpoint (dd_timelock_id)
     * @return true if position closed successfully
     */
    bool CloseCollateralPosition(const COutPoint& outpoint);

    // ====================================================================
    // PHASE 5.2: UTXO SET UPDATE FUNCTIONS
    // ====================================================================

    /**
     * Mark DD UTXOs as spent after transaction committed
     * @param spent_utxos Vector of UTXOs that were consumed as inputs
     * @return true if UTXOs marked successfully
     */
    bool MarkDDUTXOsSpent(const std::vector<COutPoint>& spent_utxos);

    /**
     * Add new DD UTXO from change output
     * @param tx The committed transaction
     * @param change_vout Index of change output in transaction
     * @param dd_amount DD amount in change output
     * @return true if UTXO added successfully
     */
    bool AddDDChangeUTXO(const CTransactionRef& tx, uint32_t change_vout, CAmount dd_amount);

    /**
     * Complete UTXO set update after transfer
     * Marks inputs spent, adds change output
     * @param tx The committed transaction
     * @param input_utxos DD UTXOs used as inputs
     * @param change_vout Index of change output (-1 if no change)
     * @param change_amount DD amount in change output
     * @return true if update successful
     */
    bool UpdateDDUTXOSet(const CTransactionRef& tx,
                         const std::vector<COutPoint>& input_utxos,
                         int change_vout,
                         CAmount change_amount);

    // ====================================================================
    // PHASE 5.3: DDTIMELOCK STATUS MANAGEMENT FUNCTIONS
    // ====================================================================

    /**
     * Update DDTimeLock status after DD transfer or redemption
     * @param dd_timelock_id The DDTimeLock position ID
     * @param new_status New active status (true = active, false = fully redeemed)
     * @return true if status updated successfully
     */
    bool UpdateDDTimeLockStatus(const uint256& dd_timelock_id, bool new_status);


    /**
     * Get DDTimeLock lifecycle status
     * @param dd_timelock_id The DDTimeLock position ID
     * @return Status string: "active", "fully_redeemed", "not_found"
     */
    std::string GetDDTimeLockStatus(const uint256& dd_timelock_id) const;

    /**
     * Check if DDTimeLock is redeemable (unlocked and has DD remaining)
     * @param dd_timelock_id The DDTimeLock position ID
     * @param current_height Current blockchain height
     * @return true if redeemable
     */
    bool IsDDTimeLockRedeemable(const uint256& dd_timelock_id, int current_height) const;

    // Test/mock functions
    void SetMockBalance(CAmount balance);
    void AddMockTransaction(const DDTransaction& tx);
    void AddMockUTXO(const CDigiDollarOutput& utxo);
    void ClearMockData();

    // Phase 5 test helpers
    void SetMockDDBalance(const CDigiDollarAddress& addr, CAmount balance);
    void AddMockPosition(const uint256& id, CAmount dd, CAmount dgb, uint32_t tier, int64_t height);
    size_t GetBalanceCount() const;
    size_t GetPositionCount() const;
    size_t GetCachedForeignDDOutputCount() const;

    /**
     * Check if an outpoint is locked by DigiDollar (either collateral or DD token).
     * Used to protect DD locks from being wiped by UnlockAllCoins.
     */
    bool IsLockedByDD(const COutPoint& outpoint) const;
    void ClearWalletData();

    // Coin selection and fee calculation helpers (public for testing and integration)
    bool SelectDDCoins(const CAmount& target_amount, std::vector<COutPoint>& selected_utxos, CAmount& selected_total, std::vector<CAmount>* amounts = nullptr) const;
    bool SelectDDCoins(const CAmount& target_amount, const std::vector<COutPoint>& preset_inputs, std::vector<COutPoint>& selected_utxos, CAmount& selected_total, std::vector<CAmount>* amounts = nullptr, std::string* error = nullptr) const;
    bool PlanDigiDollarTransfer(const std::vector<std::pair<CDigiDollarAddress, CAmount>>& recipients, DDTransferPlan& plan, std::string& error, const std::vector<COutPoint>* preset_dd_inputs = nullptr) const;
    /**
     * Select spendable DGB UTXOs to fund a DigiDollar transaction fee.
     *
     * Spending an input is not free: each extra fee input costs
     * DigiDollar::EstimateInputSpendCost(fee_rate) to spend. UTXOs are therefore
     * priced by effective value (value minus that cost); UTXOs with a
     * non-positive effective value are skipped, and `fee_amount` must be covered
     * by the sum of the effective values, not by the raw sum.
     *
     * @param[in]  fee_amount       Fee, in satoshis, the selection must still
     *                              cover after paying for its own inputs.
     * @param[out] selected_utxos   Chosen outpoints.
     * @param[out] selected_total   RAW total value of the chosen outpoints.
     * @param[out] selected_amounts Per-outpoint values, parallel to selected_utxos.
     * @param[in]  exclude_utxos    Outpoints that must not be selected.
     * @param[in]  minimize_inputs  Prefer high-value UTXOs so the selection uses
     *                              as few inputs as possible. Off by default,
     *                              which keeps the smallest-first order DD
     *                              transfers have always used: it spends down
     *                              small UTXOs at the cost of a larger fee.
     *                              Redemption opts in because it must converge
     *                              on an exact fee.
     * @param[in]  fee_rate         Fee rate, in satoshis per kvB, the transaction
     *                              will pay. Must be positive; it is what makes
     *                              the per-input cost meaningful, so a caller
     *                              spending at a rate other than the DigiDollar
     *                              minimum has to pass its own.
     */
    bool SelectFeeCoins(const CAmount& fee_amount, std::vector<COutPoint>& selected_utxos, CAmount& selected_total, std::vector<CAmount>* selected_amounts = nullptr, const std::vector<COutPoint>* exclude_utxos = nullptr, bool minimize_inputs = false, CAmount fee_rate = DigiDollar::MIN_DD_FEE_RATE) const;

    /**
     * Select the DGB fee inputs for a DigiDollar redemption, converging on the
     * fee the built transaction will actually owe.
     *
     * The fee of a DigiDollar transaction depends on how many inputs it ends up
     * with, so it cannot be known from a fixed size guess: each selection round
     * re-projects the redemption transaction and re-selects against the updated
     * fee. Fills params.feeUtxos / params.feeAmounts on success and excludes the
     * collateral outpoint and the DD UTXOs being burned from the selection, so
     * that a redemption can never pay its fee out of the collateral it releases
     * or the DD it burns.
     *
     * @param[in,out] params        Redemption parameters; fee inputs are filled in.
     * @param[out]    error         Actionable message when the result is not OK.
     * @param[out]    projected_fee Upper bound on the fee the built redemption
     *                              will owe (the projection carries a worst-case
     *                              output set, so the final fee can be lower).
     */
    DDFeeSelectionResult SelectRedemptionFeeCoins(DigiDollar::TxBuilderRedeemParams& params, std::string& error, CAmount* projected_fee = nullptr) const;

    CAmount CalculateTransactionFee(const CMutableTransaction& tx) const;

    // Phase 3.1: P2TR signing for DD inputs (Schnorr signatures)
    bool SignDDInputs(CMutableTransaction& tx,
                      const std::vector<COutPoint>& dd_utxos,
                      const std::vector<COutPoint>& fee_utxos);

    // Phase 3.2: Fee input signing (standard P2TR/P2WPKH DGB inputs)
    bool SignFeeInputs(CMutableTransaction& tx,
                       const std::vector<COutPoint>& fee_utxos,
                       size_t dd_input_count);

    // Phase 3.3: Complete transaction signing coordination
    bool SignTransaction(CMutableTransaction& tx,
                        const std::vector<COutPoint>& dd_utxos,
                        const std::vector<COutPoint>& fee_utxos);

    // Redemption-specific signing (includes collateral input)
    bool SignRedemptionTransaction(CMutableTransaction& tx,
                                   const COutPoint& collateral_outpoint,
                                   const std::vector<COutPoint>& dd_utxos,
                                   const std::vector<COutPoint>& fee_utxos,
                                   const CKey& owner_key);

    // Phase 4.1: Mempool submission
    bool CommitDDTransaction(const CTransactionRef& tx, std::string& error);

    // Phase 4.3: Confirmation tracking
    /**
     * Get confirmation count for DD transaction
     * @param txid Transaction ID to check
     * @return Number of confirmations (0 if unconfirmed or not found)
     */
    int GetDDTransactionConfirmations(const uint256& txid) const;

    /**
     * Update confirmation counts for all DD transactions when new block arrives
     * @param block_hash Hash of the newly connected block
     */
    void UpdateDDConfirmations(const uint256& block_hash);

    /**
     * Get all unconfirmed DD transactions (0 confirmations)
     * @return Vector of transaction IDs with 0 confirmations
     */
    std::vector<uint256> GetUnconfirmedDDTransactions() const;

    /**
     * Process incoming DD transaction and add to history
     * Called when wallet receives a new transaction
     * @param tx The transaction
     * @param txid Transaction ID
     */
    void ProcessIncomingTransaction(const CTransactionRef& tx, const uint256& txid);

    // ====================================================================
    // PHASE 6: RECEIVE OPERATIONS (Tasks 6.1-6.3)
    // ====================================================================

    /**
     * Detect if transaction has DD outputs to our wallet (Task 6.1)
     * Checks each transaction output to see if it's a DD output owned by this wallet
     * @param tx Transaction to check
     * @param our_dd_outputs Output: Vector of (vout_index, dd_amount) for our outputs
     * @return true if any outputs are ours
     */
    bool DetectIncomingDDOutputs(const CTransactionRef& tx,
                                 std::vector<std::pair<uint32_t, CAmount>>& our_dd_outputs);

    /**
     * Add received DD UTXO to spendable set (Task 6.3)
     * Creates a WalletCollateralPosition for received DD (not from our mint)
     * Similar to AddDDChangeUTXO() but for DD received from others
     * @param tx The transaction containing the DD output
     * @param vout_index Index of DD output we received
     * @param dd_amount DD amount received
     * @return true if UTXO added successfully
     */
    bool AddReceivedDDUTXO(const CTransactionRef& tx, uint32_t vout_index, CAmount dd_amount);

    /**
     * Add redemption transaction to history
     * Records redemption in transaction_history and persists to database
     * @param tx DDTransaction object with redemption details
     * @return true if successfully added and persisted
     */
    bool AddRedemptionToHistory(const DDTransaction& tx);

    /**
     * Process incoming DD transaction (Tasks 6.1-6.3 combined)
     * Main coordinator for receiving DD:
     * 1. Detects DD outputs to our wallet
     * 2. Credits balance (automatic via UTXO-derived approach)
     * 3. Adds received UTXOs to spendable set
     * @param tx The incoming transaction
     * @return true if processing successful
     */
    bool ProcessIncomingDDTransaction(const CTransactionRef& tx);

protected:
    // Internal helper functions for Phase 5
    bool ValidateMintParams(const CAmount& dd_amount, uint32_t lock_tier) const;
    bool ValidateTransferParams(const CDigiDollarAddress& to, const CAmount& amount) const;
    bool ValidateRedeemParams(const uint256& dd_timelock_id, const CAmount& amount) const;

private:
    /**
     * Helper: Load all positions from database
     */
    size_t LoadPositionsFromDatabase();

    /**
     * Helper: Load all DD balances from database
     */
    size_t LoadBalancesFromDatabase();

    /**
     * Helper: Load all DD transactions from database
     */
    size_t LoadTransactionsFromDatabase();

    /**
     * Recalculate totals from loaded data
     */
    void RecalculateTotals();

public:
    /**
     * Load all DigiDollar data from wallet database
     * Called during wallet initialization
     * @return Number of items loaded
     */
    size_t LoadFromDatabase();
};

/**
 * Utility functions for DigiDollar wallet operations
 */
namespace DigiDollarWalletUtils {
    /**
     * Convert lock tier to lock period in days
     * @param tier Lock tier (0-9): 0=1h, 1=30d, 2=90d, 3=180d, 4=1y, 5=2y, 6=3y, 7=5y, 8=7y, 9=10y
     * @return Lock period in days
     */
    int GetLockDaysForTier(uint32_t tier);

    /**
     * Calculate minimum collateral ratio for lock tier
     * @param tier Lock tier (0-9)
     * @return Collateral ratio percentage
     */
    int GetMinCollateralRatio(uint32_t tier);

    /**
     * Validate DD address format
     * @param address DD address string
     * @return true if valid format
     */
    bool IsValidDDAddress(const std::string& address);
}

#endif // DIGIBYTE_WALLET_DIGIDOLLARWALLET_H
