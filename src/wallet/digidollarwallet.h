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
#include <tuple>
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
    bool is_expired_mint;   // Transient: an unconfirmed mint whose lock window has passed

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

enum class DDTransferPlanError : uint8_t {
    NONE,
    INVALID_REQUEST,
    INSUFFICIENT_DD_INPUTS,
    INVALID_DD_INPUTS,
    INSUFFICIENT_DGB_FEE_INPUTS,
    CAPACITY,
};

/** A single-lock snapshot of the wallet's DigiDollar balance categories. */
struct DigiDollarBalanceSummary {
    CAmount confirmed_total{0};
    CAmount spendable{0};
    CAmount paymaster_reserved{0};
    CAmount pending{0};
};

struct DDTransferPlan {
    std::vector<std::pair<std::string, CAmount>> recipients;
    std::vector<COutPoint> dd_utxos;
    std::vector<CAmount> dd_amounts;
    std::vector<COutPoint> fee_utxos;
    std::vector<CAmount> fee_amounts;
    CAmount total_amount{0};
    CAmount selected_dd_total{0};
    CAmount selected_fee_total{0};
    CAmount dd_change{0};
    size_t projected_vsize{0};
    CAmount estimated_fee{0};
    DDTransferPlanError error_code{DDTransferPlanError::NONE};
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

    // The mint positions that were in the wallet file when this wallet was
    // opened. It is how the automatic cleanup tells records left behind by an
    // earlier run from records a mint in this run is still working on.
    std::set<uint256> m_positions_read_at_open GUARDED_BY(cs_dd_wallet);

    /**
     * Take the main wallet lock, when there is a wallet, and then the
     * DigiDollar one, always in that order.
     *
     * The two locks are taken the same way the LOCK macro takes a lock, so a
     * build with lock checking turned on knows they are held. Without that,
     * every main-wallet function called from here stops the program with
     * "lock cs_wallet not held", and nothing notices a thread taking the two
     * locks the other way round, which is how a node hangs.
     */
    [[nodiscard]] std::pair<UniqueLock<RecursiveMutex>,
                            UniqueLock<RecursiveMutex>> LockDDWallet() const
    {
        return std::pair<UniqueLock<RecursiveMutex>, UniqueLock<RecursiveMutex>>(
            std::piecewise_construct,
            std::forward_as_tuple(m_wallet ? &m_wallet->cs_wallet : nullptr,
                                  "cs_wallet", __FILE__, __LINE__),
            std::forward_as_tuple(&cs_dd_wallet, "cs_dd_wallet", __FILE__, __LINE__));
    }

    /**
     * Stop the node at once, naming the lock and the line that took it, if a
     * wallet lock is held where the chain is about to be asked a question.
     *
     * The chain takes its own lock and then calls into the wallet. So a thread
     * that holds a wallet lock and then waits for the chain, running against a
     * thread going the other way, leaves the node stuck with no way out. This
     * check turns that into an immediate, named stop while a build with lock
     * checking is under test. It does nothing in a release build.
     *
     * It is a plain function rather than the AssertLockNotHeld macro so that a
     * compiler which tracks locks does not object to checking a lock it
     * believes is held, which is exactly the case worth checking.
     */
    void RequireNoWalletLockForChainQuery(const char* file, int line) const;

    /**
     * The chain behind this wallet, for a question that has to wait for the
     * chain's own lock. Every such question goes through here so the rule
     * above is checked in one place.
     */
    interfaces::Chain& ChainForQuery(const char* file, int line) const
    {
        RequireNoWalletLockForChainQuery(file, line);
        return m_wallet->chain();
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
     * @return true only when the key is cached in memory and written to the
     *         wallet database. On failure nothing is cached, so a caller that
     *         must not proceed without a persisted key (a mint about to be
     *         broadcast) can stop.
     * @note If wallet is encrypted, key is encrypted before storage.
     */
    bool StoreOwnerKey(const uint256& dd_timelock_id, const CKey& key);

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

    /** Outcome of looking for the owner key of a vault. */
    enum class OwnerKeyRecovery {
        Found,               // the side table already had it
        Recovered,           // found among the wallet's own keys, verified against the vault, and saved
        PositionNotFound,    // no position record for this id
        MintTxMissing,       // the mint transaction is not in this wallet
        WalletLocked,        // the wallet is encrypted and locked
        NoPrivateKeys,       // watch-only wallet
        NoMatchingKey,       // none of this wallet's keys is the vault owner
        DatabaseWriteFailed  // the key was found but could not be saved
    };

    /**
     * Find the owner key of a minted position when the wallet's own record of
     * it has gone missing.
     *
     * The mint built the DigiDollar token output from the owner's public key
     * by a fixed calculation. Redoing that calculation on a candidate key and
     * comparing the result to the output says whether that key owns this
     * collateral. Candidates are the wallet's own keys, including the ones it
     * keeps ready beyond the last address it has used. A key that matches is
     * saved before it is returned. A new key is never made up.
     */
    OwnerKeyRecovery RecoverOwnerKey(const uint256& dd_timelock_id, CKey& key);

    // ====================================================================
    // DD ADDRESS KEY MANAGEMENT (for received DD tokens)
    // ====================================================================

    /**
     * Store DD address key for a P2TR output key
     * Called when getdigidollaraddress generates a new address
     * @param output_key The XOnlyPubKey (output key) from the P2TR address
     * @param key The private key that can sign for this address
     * @return true only when the key is cached and written to the wallet database
     * @note If wallet is encrypted, key is encrypted before storage.
     */
    bool StoreAddressKey(const XOnlyPubKey& output_key, const CKey& key);

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
     * Get the wallet's total owned DD balance (confirmed only).
     *
     * This includes confirmed DD inputs reserved for an active Paymaster
     * session or provider pool. Use GetSpendableDDBalance() for affordability
     * checks and GetPaymasterReservedDDBalance() for the reserved portion.
     * Unconfirmed trusted UTXOs are excluded — use GetPendingDDBalance() for
     * those.
     * @return Confirmed wallet-owned DD balance in cents
     */
    CAmount GetTotalDDBalance() const;

    /** Get all display and affordability balance categories in one snapshot. */
    DigiDollarBalanceSummary GetDDBalanceSummary() const;

    /**
     * Get confirmed DD currently selectable by an ordinary wallet spend.
     * Active Paymaster reservations and provider-pool carriers are excluded.
     * @return Confirmed spendable DD balance in cents
     */
    CAmount GetSpendableDDBalance() const;

    /**
     * Get confirmed wallet-owned DD protected by Paymaster reservations.
     * This includes provider-pool carrier outputs and client inputs bound to
     * an active Paymaster session.
     * @return Confirmed Paymaster-reserved DD balance in cents
     */
    CAmount GetPaymasterReservedDDBalance() const;

    /**
     * Get pending DD balance.
     * These are unconfirmed DD UTXOs, including trusted wallet-created mints
     * and transfer change. Pending DD is not spendable until confirmed.
     * @return Pending DD balance in cents
     */
    CAmount GetPendingDDBalance() const;

    /**
     * Scan wallet UTXOs for DigiDollar outputs and populate dd_balances map,
     * then check the recorded positions against the chain.
     * Should be called ONLY at wallet startup, not on every block!
     *
     * Must be called with no wallet lock held, because the position check asks
     * the chain which collateral coins still exist. Callers that already hold
     * the wallet lock call RebuildDDUTXOs() instead.
     * @return Number of DD UTXOs found
     */
    size_t ScanForDDUTXOs();

    /**
     * Rebuild the DigiDollar UTXO map and balances from the wallet's own
     * transactions, and nothing more. This asks the chain nothing, so it is
     * safe to call while the wallet lock is held. It records that the position
     * check is still owed; the wallet runs that check when the chain tip next
     * moves, at a point where no wallet lock is held.
     * @return Number of DD UTXOs found
     */
    size_t RebuildDDUTXOs();

    /**
     * Note that the recorded positions need checking against the chain,
     * without doing the check here. Safe to call while the wallet lock is
     * held. The wallet runs the check when the chain tip next moves, and the
     * commands and wallet screens that report positions check them before
     * they answer.
     */
    void RequestPositionStateCheck();

    /**
     * Post-rescan validation: check every active position against the UTXO set.
     * If the collateral output (COutPoint(dd_timelock_id, 0)) is NOT in the
     * UTXO set, the position was redeemed and is marked is_active = false.
     *
     * This is a belt-and-suspenders fix for wallet restore scenarios where
     * ProcessDDTxForRescan fails to detect REDEEM transactions (e.g., custom
     * Taproot MAST scripts not recognized as "ours" during rescan).
     *
     * Called at the end of ScanForDDUTXOs(). Must be called with no wallet
     * lock held: it asks the chain which collateral coins still exist, and the
     * chain takes its own lock before it calls back into the wallet.
     * @return Number of positions corrected (marked inactive)
     */
    size_t ValidatePositionStates();

    /**
     * Reconcile cached collateral-position active flags against chainstate.
     *
     * This lightweight wrapper is safe for RPC/Qt/read paths that need a
     * current position view without forcing a full wallet DD UTXO rescan.
     * Must be called with no wallet lock held, for the same reason as
     * ValidatePositionStates().
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
     * Add a collateral position to the wallet: the position record, its DD
     * token output and a mint history row.
     * @param position The position to add
     * @return true only when every record was written to the wallet database
     */
    bool AddCollateralPosition(const WalletCollateralPosition& position);

    // ====================================================================
    // MINT ATTEMPT LIFECYCLE
    // ====================================================================

    /**
     * Save everything needed to recover a mint before it is broadcast: the
     * owner key, the position record, the DD token output and the history
     * row. The transaction is not in the wallet yet, so the output indexes
     * are read from the transaction itself.
     * @return true only when every write succeeded; error explains a failure
     */
    bool RecordPendingMint(const CTransaction& mint_tx,
                           const WalletCollateralPosition& position,
                           const CKey& owner_key,
                           std::string& error);

    /** What the wallet currently knows about the transaction of a mint attempt. */
    enum class MintAttemptState {
        NotInWallet,   // no wallet transaction for this position id
        Confirmed,     // the mint is in the active chain
        InMempool,     // unconfirmed, but this node's mempool still holds it
        Local,         // unconfirmed, not in the mempool, lock window still open
        Expired,       // unconfirmed, not in the mempool, lock window has passed
        Abandoned,     // marked abandoned in the wallet while the window is still open
        Conflicted     // a conflicting transaction confirmed instead
    };

    MintAttemptState GetMintAttemptState(const uint256& dd_timelock_id) const;

    /**
     * Whether a block at next_block_height could still include a mint with
     * this position's tier and unlock height. This is the same rule the chain
     * applies: when a block includes a mint, the lock still left to run must
     * be at least the full length of the tier the mint claims.
     */
    static bool MintLockWindowHasPassed(const WalletCollateralPosition& position, int next_block_height);

    /**
     * Whether the automatic cleanup may give up the records of a mint whose
     * transaction is not in the wallet at all.
     *
     * True only for a position that was already in the wallet file when this
     * wallet was opened, and only once no block on this chain could include the
     * mint any more. A mint writes its position record before it hands the
     * transaction to the wallet, so records with no transaction are also what a
     * mint being built right now looks like, and that mint releases its own
     * records if it gives up. Callers of ReleaseMintAttempt are not held to
     * this, because a mint releasing its own records knows it has stopped.
     */
    bool UnsentMintRecordsCanBeReleased(const uint256& dd_timelock_id) const;

    /**
     * Whether the automatic cleanup may give up this mint attempt, given what
     * the wallet knows about its transaction right now.
     *
     * The cleanup asks this to choose, and the release asks it again before it
     * changes anything, so the two can never drift apart. Every part of the
     * answer is read from the wallet's own height, its transactions and its
     * position records, so it is only true for as long as the wallet locks are
     * held.
     */
    bool MintAttemptCanBeSwept(const uint256& dd_timelock_id, MintAttemptState state) const;

    /** Who is asking for a mint attempt to be released. */
    enum class MintReleaseCaller {
        //! A mint giving up its own records. It has stopped, and it is the only
        //! thing that could still send the transaction, so its records are
        //! released at once, whatever the height says.
        StoppedMint,
        //! The cleanup that runs on its own, on every block and when the wallet
        //! is opened. It decided this mint was finished a moment earlier, with
        //! the DigiDollar lock let go in between, so the release asks again.
        AutomaticSweep,
    };

    /**
     * Give up on a mint attempt that can no longer confirm here and release
     * only what this attempt reserved: the wallet transaction is abandoned so
     * its DGB inputs are spendable again, the collateral and DD token coin
     * locks are removed, the DD token output is no longer tracked, and the
     * position is marked inactive. The owner key, the transaction and the
     * position record stay, so a reorg or a late confirmation can bring the
     * vault back. Refuses (returns false) while the mint is confirmed or still
     * in the mempool.
     *
     * The caller says who is asking. The automatic cleanup chose this mint
     * earlier and its decision can have gone stale, so for that caller the
     * release checks again, under the locks, that the mint is still one it may
     * give up, and refuses if a block could include the mint once more. A mint
     * giving up its own records is not held to that: it knows it has stopped.
     */
    bool ReleaseMintAttempt(const uint256& dd_timelock_id, std::string& error,
                            MintReleaseCaller caller = MintReleaseCaller::StoppedMint);

    /**
     * Undoes a mint that was saved but never sent.
     *
     * A mint writes its owner key and its position record before it hands the
     * transaction to the wallet. It has to: once the transaction is out it can
     * be mined whether or not this process survives, and those records are the
     * only things that let the owner redeem the vault.
     *
     * Between that save and the commit the mint can still stop. A block can
     * arrive, the price the oracle would sign for the next block can change, a
     * check can fail, or the wallet itself can refuse the transaction. Records
     * left behind then describe a transaction that does not exist: the wallet
     * counts the token towards its DigiDollar balance and offers it for
     * spending, so the owner is shown DigiDollars that cannot be spent.
     *
     * Create one of these as soon as the records are saved. If it goes out of
     * scope before KeepRecords() has been called, for any reason at all, it
     * releases what the attempt reserved. The owner key stays, because keeping
     * it costs nothing and it is the one thing that could not be worked out
     * again if a signed transaction did reach a block after all.
     */
    class SavedMintCleanup
    {
    public:
        SavedMintCleanup(DigiDollarWallet& dd_wallet, const uint256& position_id, const char* whose_mint)
            : m_dd_wallet(dd_wallet), m_position_id(position_id), m_whose_mint(whose_mint) {}
        ~SavedMintCleanup();

        SavedMintCleanup(const SavedMintCleanup&) = delete;
        SavedMintCleanup& operator=(const SavedMintCleanup&) = delete;

        //! The transaction reached the wallet, so the records describe
        //! something real. Keep them.
        void KeepRecords() { m_keep_records = true; }

    private:
        DigiDollarWallet& m_dd_wallet;
        const uint256 m_position_id;
        const char* const m_whose_mint;
        bool m_keep_records{false};
    };

    /**
     * Release every active position whose mint attempt is expired, abandoned
     * or conflicted on the current chain. Called when a block connects and
     * when position state is reconciled for RPC readers.
     * @return number of attempts released
     */
    size_t ReconcileExpiredMintAttempts();

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
     * @param error Optional. When this returns false it is filled in with the
     *              reason, in words a caller can show the user.
     * @return true if transaction created successfully
     */
    bool TransferDigiDollar(const CDigiDollarAddress& to, CAmount amount, CTransactionRef& tx_out,
                            std::string* error = nullptr);

    /**
     * Create redemption transaction using transaction builders
     * @param dd_timelock_id Position to redeem
     * @param amount Amount of DD to redeem
     * @param tx_out Output transaction reference
     * @param error Optional. When this returns false it is filled in with the
     *              reason, in words a caller can show the user.
     * @return true if transaction created successfully
     */
    bool RedeemDigiDollar(const uint256& dd_timelock_id, const CAmount& amount, CTransactionRef& tx_out,
                          std::string* error = nullptr);

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
                                const std::string& comment = "",
                                bool allow_paymaster_pool_inputs = false,
                                const DDTransferPlan* exact_plan = nullptr);

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
     * Return a bounded view of the already loaded DD history.
     *
     * This deliberately does not inspect CWallet transactions, recover
     * descriptor ownership, or recalculate confirmations. Qt uses it only as
     * an immediate startup seed while GetDDTransactionHistory() builds the
     * authoritative view on a worker thread.
     */
    std::vector<DDTransaction> GetStoredDDTransactionHistory(size_t count,
                                                              size_t skip = 0) const;

    /**
     * Persist the client-side history row for a finalized Paymaster transfer.
     *
     * Paymaster transactions are assembled collaboratively and therefore do
     * not pass through TransferDigiDollarMany(), which normally records the
     * outgoing DD row. This method replaces any provisional receive row for
     * wallet-owned change with one idempotent outgoing row for the complete
     * DD wallet outflow (recipient amount plus Paymaster service fee).
     */
    bool RecordPaymasterSendHistory(const uint256& txid,
                                    const CScript& recipient_script,
                                    CAmount total_outflow,
                                    std::string& error);

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
     * Runs with the main wallet lock already held, so it asks the chain
     * nothing. The block's timestamp has to be handed in for that reason.
     *
     * @param ptx Transaction reference
     * @param block_height Block height of the transaction
     * @param block_time Timestamp of that block, read by the caller from the
     *                   block it already has, before it took the wallet lock
     */
    void ProcessDDTxForRescan(const CTransactionRef& ptx, int block_height, int64_t block_time);

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
    bool SelectDDCoins(const CAmount& target_amount, const std::vector<COutPoint>& preset_inputs, std::vector<COutPoint>& selected_utxos, CAmount& selected_total, std::vector<CAmount>* amounts = nullptr, std::string* error = nullptr, bool allow_paymaster_pool_inputs = false) const;
    /** Snapshot all confirmed ordinary DD inputs and require their sum to
     * match a previously previewed send-all balance. Active Paymaster
     * reservations and provider-pool DD are excluded. */
    bool SelectAllSpendableDDCoins(CAmount expected_total,
                                   std::vector<COutPoint>& selected_utxos,
                                   CAmount& selected_total,
                                   std::vector<CAmount>* amounts,
                                   std::string& error) const;
    bool PlanDigiDollarTransfer(const std::vector<std::pair<CDigiDollarAddress, CAmount>>& recipients, DDTransferPlan& plan, std::string& error, const std::vector<COutPoint>* preset_dd_inputs = nullptr, bool allow_paymaster_pool_inputs = false) const;
    /** Build and sign a previously validated transfer plan without inserting
     * it into the wallet or broadcasting it. */
    bool BuildDigiDollarTransfer(const DDTransferPlan& plan,
                                 CMutableTransaction& transaction,
                                 std::string& error);
    bool SelectFeeCoins(const CAmount& fee_amount, std::vector<COutPoint>& selected_utxos, CAmount& selected_total, std::vector<CAmount>* selected_amounts = nullptr, const std::vector<COutPoint>* exclude_utxos = nullptr) const;
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

    /**
     * Write the position record, DD token output and mint history row. When
     * the mint transaction is not in the wallet yet, mint_tx supplies the
     * output indexes. Every write is checked.
     */
    bool RecordCollateralPosition(const WalletCollateralPosition& position,
                                  const CTransaction* mint_tx,
                                  std::string& error);

    /** Recompute the cached locked-collateral total from active positions. */
    void RefreshLockedCollateral();

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
