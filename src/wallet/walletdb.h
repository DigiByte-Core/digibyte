// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_WALLET_WALLETDB_H
#define DIGIBYTE_WALLET_WALLETDB_H

#include <key.h>
#include <paymaster/protocol.h>
#include <paymaster/provider.h>
#include <paymaster/recovery.h>
#include <paymaster/reputation.h>
#include <paymaster/reservation.h>
#include <paymaster/sponsorship.h>
#include <script/sign.h>
#include <wallet/db.h>
#include <wallet/walletutil.h>

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

class CScript;
class uint160;
class uint256;
struct CBlockLocator;
struct DDTransaction;
struct WalletCollateralPosition;
struct WalletDDBalance;
class CDigiDollarOutput;

namespace wallet {
class CKeyPool;
class CMasterKey;
class CWallet;
class CWalletTx;
struct WalletContext;

/**
 * Overview of wallet database classes:
 *
 * - WalletBatch is an abstract modifier object for the wallet database, and encapsulates a database
 *   batch update as well as methods to act on the database. It should be agnostic to the database implementation.
 *
 * The following classes are implementation specific:
 * - BerkeleyEnvironment is an environment in which the database exists.
 * - BerkeleyDatabase represents a wallet database.
 * - BerkeleyBatch is a low-level database batch update.
 */

static const bool DEFAULT_FLUSHWALLET = true;

/** Error statuses for the wallet database.
 * Values are in order of severity. When multiple errors occur, the most severe (highest value) will be returned.
 */
enum class DBErrors : int {
    LOAD_OK = 0,
    NEED_RESCAN = 1,
    NEED_REWRITE = 2,
    EXTERNAL_SIGNER_SUPPORT_REQUIRED = 3,
    NONCRITICAL_ERROR = 4,
    TOO_NEW = 5,
    UNKNOWN_DESCRIPTOR = 6,
    LOAD_FAIL = 7,
    UNEXPECTED_LEGACY_ENTRY = 8,
    CORRUPT = 9,
};

namespace DBKeys {
extern const std::string ACENTRY;
extern const std::string ACTIVEEXTERNALSPK;
extern const std::string ACTIVEINTERNALSPK;
extern const std::string BESTBLOCK;
extern const std::string BESTBLOCK_NOMERKLE;
extern const std::string CRYPTED_KEY;
extern const std::string CSCRIPT;
extern const std::string DEFAULTKEY;
extern const std::string DESTDATA;
extern const std::string FLAGS;
extern const std::string HDCHAIN;
extern const std::string KEY;
extern const std::string KEYMETA;
extern const std::string LOCKED_UTXO;
extern const std::string MASTER_KEY;
extern const std::string MINVERSION;
extern const std::string NAME;
extern const std::string OLD_KEY;
extern const std::string ORDERPOSNEXT;
extern const std::string POOL;
extern const std::string PURPOSE;
extern const std::string SETTINGS;
extern const std::string TX;
extern const std::string VERSION;
extern const std::string WALLETDESCRIPTOR;
extern const std::string WALLETDESCRIPTORCKEY;
extern const std::string WALLETDESCRIPTORKEY;
extern const std::string WATCHMETA;
extern const std::string WATCHS;

// DigiDollar database keys
extern const std::string DD_POSITION;                    // "ddposition" - DDTimeLocks (time-locked DGB backing DigiDollars)
extern const std::string DD_TRANSACTION;                 // "ddtx"       - DD transaction history
extern const std::string DD_BALANCE;                     // "ddbalance"  - DD balance per address
extern const std::string DD_OUTPUT;                      // "ddutxo"     - DD UTXO tracking
extern const std::string DD_METADATA;                    // "ddmeta"     - DD wallet metadata
extern const std::string DD_ADDRESS_KEY;                 // "ddaddrkey"  - DD address keys for received tokens (plaintext)
extern const std::string DD_OWNER_KEY;                   // "ddownerkey" - DD owner keys for minted tokens (plaintext)
extern const std::string DD_CRYPTED_ADDRESS_KEY;         // "ddcaddrkey" - encrypted DD address keys
extern const std::string DD_CRYPTED_OWNER_KEY;           // "ddcownerkey" - encrypted DD owner keys
extern const std::string ORACLE_KEY;                     // "oraclekey"  - Oracle private keys by oracle_id
extern const std::string ORACLE_CRYPTED_KEY;             // "oracleckey" - encrypted Oracle private keys by oracle_id
extern const std::string PAYMASTER_SESSION;              // "pmsession"  - payment sessions by request_id
extern const std::string PAYMASTER_RECOVERY;             // "pmrecovery" - idempotent self-recovery by request_id
extern const std::string PAYMASTER_ALT_RECOVERY;         // "pmaltrecovery" - authenticated alternative recovery by recovery_id
extern const std::string PAYMASTER_ALT_RECOVERY_REQUEST; // "pmaltrecoveryreq" - client request_id to recovery_id
extern const std::string PAYMASTER_ATTEMPT;              // "pmattempt"  - provider attempts by attempt_id
extern const std::string PAYMASTER_CAPACITY;             // "pmcapacity" - validated capacity snapshots by snapshot id
extern const std::string PAYMASTER_CAPACITY_SLOT;        // "pmcapacityslot" - resource commitment to snapshot id
extern const std::string PAYMASTER_CAPACITY_RESOURCE;    // "pmcapacityresource" - (provider,outpoint) live binding
extern const std::string PAYMASTER_CAPACITY_RESPONSE;    // "pmcapresponse" - canonical request hash to signed proof
extern const std::string PAYMASTER_CAPACITY_NONCE;       // "pmcapnonce" - client nonce to canonical request hash
extern const std::string PAYMASTER_CAPACITY_SESSION;     // "pmcapsession" - provider/request/session key to request hash
extern const std::string PAYMASTER_CAPACITY_RELEASE;     // "pmcaprelease" - durable expired-reservation release
extern const std::string PAYMASTER_TEMPLATE;             // "pmtemplate" - template commitment to attempt index
extern const std::string PAYMASTER_UNSIGNED_TX;          // "pmtxid" - unsigned transaction id to attempt index
extern const std::string PAYMASTER_SESSION_ID;           // "pmsessionid" - session_id to request_id index
extern const std::string PAYMASTER_RESERVATION;          // "pmreserve" - input reservations by outpoint
extern const std::string PAYMASTER_TOMBSTONE;            // "pmtombstone" - permanent request idempotency
extern const std::string PAYMASTER_PROVIDER_COMMIT;      // "pmcommit" - final provider commit by commit key
extern const std::string PAYMASTER_USER_AUTH;            // "pmauth" - accepted user authorization by commit key
extern const std::string PAYMASTER_IDENTITY;             // "pmidentity" - public provider identity metadata
extern const std::string PAYMASTER_POLICY;               // "pmpolicy" - validated provider policy
extern const std::string PAYMASTER_SETTINGS;             // "pmsettings" - provider enablement and policy binding
extern const std::string PAYMASTER_PROVIDER_SAFETY;      // "pmprovidersafety" - local provider loss ceilings
extern const std::string PAYMASTER_CLIENT_SAFETY;        // "pmclientsafety" - local client fee ceilings
extern const std::string PAYMASTER_PROVIDER_BUDGET;      // "pmproviderbudget" - provider fee reservations
extern const std::string PAYMASTER_CLIENT_FEES;          // "pmclientfees" - client service-fee reservations
extern const std::string PAYMASTER_SPONSOR_AUTH;         // "pmsponsor" - hashed restricted sponsorship authorization
extern const std::string PAYMASTER_PROVIDER_POOL;        // "pmpool" - validated admission and operational pool entries
extern const std::string PAYMASTER_LIQUIDITY_POLICY;     // "pmliquidity" - automatic pool targets and fee ceilings
extern const std::string PAYMASTER_MAINTENANCE_LEDGER;   // "pmmaintenance" - restartable maintenance operations
extern const std::string PAYMASTER_FINANCE_LEDGER;       // "pmfinance" - durable provider finance history
extern const std::string PAYMASTER_BACKUP_STATUS;        // "pmbackup" - full-wallet backup acknowledgement
extern const std::string PAYMASTER_CARRIER_WITHDRAWAL;   // "pmcarrierwithdraw" - last reviewed carrier withdrawal plan
extern const std::string PAYMASTER_ANNOUNCE_SEQ;         // "pmannounceseq" - last allocated provider announcement sequence
extern const std::string PAYMASTER_RESULT;               // "pmresult" - latest signed result by commit key
extern const std::string PAYMASTER_RELIABILITY;          // "pmreliability" - local provider reliability record
extern const std::string PAYMASTER_OUTCOME;              // "pmoutcome" - idempotent reliability outcome marker
extern const std::string PAYMASTER_EQUIVOCATION_PENDING; // "pmequivocationpending" - verified conflict awaiting atomic promotion
extern const std::string PAYMASTER_EQUIVOCATION;         // "pmequivocation" - signed conflict evidence by evidence id
extern const std::string PAYMASTER_PROVIDER_BLOCK;       // "pmproviderblock" - permanent local provider deny-list

// Keys in this set pertain only to the legacy wallet (LegacyScriptPubKeyMan) and are removed during migration from legacy to descriptors.
extern const std::unordered_set<std::string> LEGACY_TYPES;
} // namespace DBKeys

/* simple HD chain data model */
class CHDChain
{
public:
    uint32_t nExternalChainCounter;
    uint32_t nInternalChainCounter;
    CKeyID seed_id;                   //!< seed hash160
    int64_t m_next_external_index{0}; // Next index in the keypool to be used. Memory only.
    int64_t m_next_internal_index{0}; // Next index in the keypool to be used. Memory only.

    static const int VERSION_HD_BASE = 1;
    static const int VERSION_HD_CHAIN_SPLIT = 2;
    static const int CURRENT_VERSION = VERSION_HD_CHAIN_SPLIT;
    int nVersion;

    CHDChain() { SetNull(); }

    SERIALIZE_METHODS(CHDChain, obj)
    {
        READWRITE(obj.nVersion, obj.nExternalChainCounter, obj.seed_id);
        if (obj.nVersion >= VERSION_HD_CHAIN_SPLIT) {
            READWRITE(obj.nInternalChainCounter);
        }
    }

    void SetNull()
    {
        nVersion = CHDChain::CURRENT_VERSION;
        nExternalChainCounter = 0;
        nInternalChainCounter = 0;
        seed_id.SetNull();
    }

    bool operator==(const CHDChain& chain) const
    {
        return seed_id == chain.seed_id;
    }
};

class CKeyMetadata
{
public:
    static const int VERSION_BASIC = 1;
    static const int VERSION_WITH_HDDATA = 10;
    static const int VERSION_WITH_KEY_ORIGIN = 12;
    static const int CURRENT_VERSION = VERSION_WITH_KEY_ORIGIN;
    int nVersion;
    int64_t nCreateTime;         // 0 means unknown
    std::string hdKeypath;       // optional HD/bip32 keypath. Still used to determine whether a key is a seed. Also kept for backwards compatibility
    CKeyID hd_seed_id;           // id of the HD seed used to derive this key
    KeyOriginInfo key_origin;    // Key origin info with path and fingerprint
    bool has_key_origin = false; //!< Whether the key_origin is useful

    CKeyMetadata()
    {
        SetNull();
    }
    explicit CKeyMetadata(int64_t nCreateTime_)
    {
        SetNull();
        nCreateTime = nCreateTime_;
    }

    SERIALIZE_METHODS(CKeyMetadata, obj)
    {
        READWRITE(obj.nVersion, obj.nCreateTime);
        if (obj.nVersion >= VERSION_WITH_HDDATA) {
            READWRITE(obj.hdKeypath, obj.hd_seed_id);
        }
        if (obj.nVersion >= VERSION_WITH_KEY_ORIGIN) {
            READWRITE(obj.key_origin);
            READWRITE(obj.has_key_origin);
        }
    }

    void SetNull()
    {
        nVersion = CKeyMetadata::CURRENT_VERSION;
        nCreateTime = 0;
        hdKeypath.clear();
        hd_seed_id.SetNull();
        key_origin.clear();
        has_key_origin = false;
    }
};

/** Access to the wallet database.
 * Opens the database and provides read and write access to it. Each read and write is its own transaction.
 * Multiple operation transactions can be started using TxnBegin() and committed using TxnCommit()
 * Otherwise the transaction will be committed when the object goes out of scope.
 * Optionally (on by default) it will flush to disk on close.
 * Every 1000 writes will automatically trigger a flush to disk.
 */
class WalletBatch
{
private:
    template <typename K, typename T>
    bool WriteIC(const K& key, const T& value, bool fOverwrite = true)
    {
        if (!m_batch->Write(key, value, fOverwrite)) {
            return false;
        }
        m_database.IncrementUpdateCounter();
        if (m_database.nUpdateCounter % 1000 == 0) {
            m_batch->Flush();
        }
        return true;
    }

    template <typename K>
    bool EraseIC(const K& key)
    {
        if (!m_batch->Erase(key)) {
            return false;
        }
        m_database.IncrementUpdateCounter();
        if (m_database.nUpdateCounter % 1000 == 0) {
            m_batch->Flush();
        }
        return true;
    }

public:
    explicit WalletBatch(WalletDatabase& database, bool _fFlushOnClose = true) : m_batch(database.MakeBatch(_fFlushOnClose)),
                                                                                 m_database(database)
    {
    }
    WalletBatch(const WalletBatch&) = delete;
    WalletBatch& operator=(const WalletBatch&) = delete;

    bool WriteName(const std::string& strAddress, const std::string& strName);
    bool EraseName(const std::string& strAddress);

    bool WritePurpose(const std::string& strAddress, const std::string& purpose);
    bool ErasePurpose(const std::string& strAddress);

    bool WriteTx(const CWalletTx& wtx);
    bool EraseTx(uint256 hash);

    bool WriteKeyMetadata(const CKeyMetadata& meta, const CPubKey& pubkey, const bool overwrite);
    bool WriteKey(const CPubKey& vchPubKey, const CPrivKey& vchPrivKey, const CKeyMetadata& keyMeta);
    bool WriteCryptedKey(const CPubKey& vchPubKey, const std::vector<unsigned char>& vchCryptedSecret, const CKeyMetadata& keyMeta);
    bool WriteMasterKey(unsigned int nID, const CMasterKey& kMasterKey);

    bool WriteCScript(const uint160& hash, const CScript& redeemScript);

    bool WriteWatchOnly(const CScript& script, const CKeyMetadata& keymeta);
    bool EraseWatchOnly(const CScript& script);

    bool WriteBestBlock(const CBlockLocator& locator);
    bool ReadBestBlock(CBlockLocator& locator);

    bool WriteOrderPosNext(int64_t nOrderPosNext);

    bool ReadPool(int64_t nPool, CKeyPool& keypool);
    bool WritePool(int64_t nPool, const CKeyPool& keypool);
    bool ErasePool(int64_t nPool);

    bool WriteMinVersion(int nVersion);

    bool WriteDescriptorKey(const uint256& desc_id, const CPubKey& pubkey, const CPrivKey& privkey);
    bool WriteCryptedDescriptorKey(const uint256& desc_id, const CPubKey& pubkey, const std::vector<unsigned char>& secret);
    bool WriteDescriptor(const uint256& desc_id, const WalletDescriptor& descriptor);
    bool WriteDescriptorDerivedCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index, uint32_t der_index);
    bool WriteDescriptorParentCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index);
    bool WriteDescriptorLastHardenedCache(const CExtPubKey& xpub, const uint256& desc_id, uint32_t key_exp_index);
    bool WriteDescriptorCacheItems(const uint256& desc_id, const DescriptorCache& cache);

    bool WriteLockedUTXO(const COutPoint& output);
    bool EraseLockedUTXO(const COutPoint& output);

    // DigiDollar UTXO persistence methods (Fix #5)
    bool WriteDDUTXO(const COutPoint& outpoint, const CAmount& dd_amount);
    bool ReadDDUTXO(const COutPoint& outpoint, CAmount& dd_amount);
    bool EraseDDUTXO(const COutPoint& outpoint);

    // DigiDollar persistence write methods
    bool WriteDDTimeLock(const WalletCollateralPosition& position);
    bool WriteDDTransaction(const DDTransaction& ddtx);
    bool WriteDDBalance(const std::string& address, const WalletDDBalance& balance);
    bool WriteDDOutput(const uint256& output_id, const CDigiDollarOutput& output);
    bool WriteDDMetadata(const std::string& key, const std::string& value);

    // DigiDollar persistence read methods
    bool ReadDDTimeLock(const uint256& dd_timelock_id, WalletCollateralPosition& position);
    bool ReadDDTransaction(const uint256& txid, DDTransaction& ddtx);
    bool ReadDDBalance(const std::string& address, WalletDDBalance& balance);
    bool ReadDDOutput(const uint256& output_id, CDigiDollarOutput& output);
    bool ReadDDMetadata(const std::string& key, std::string& value);

    // DigiDollar persistence erase methods
    bool EraseDDTimeLock(const uint256& dd_timelock_id);
    bool EraseDDTransaction(const uint256& txid);
    bool EraseDDBalance(const std::string& address);
    bool EraseDDOutput(const uint256& output_id);

    // DigiDollar address key persistence (for received DD tokens)
    bool WriteDDAddressKey(const std::array<unsigned char, 32>& output_key, const CKey& key);
    bool ReadDDAddressKey(const std::array<unsigned char, 32>& output_key, CKey& key);
    bool EraseDDAddressKey(const std::array<unsigned char, 32>& output_key);

    // DigiDollar owner key persistence (for minted DD token vault redemption)
    bool WriteDDOwnerKey(const uint256& dd_timelock_id, const CKey& key);
    bool ReadDDOwnerKey(const uint256& dd_timelock_id, CKey& key);
    bool EraseDDOwnerKey(const uint256& dd_timelock_id);

    // Encrypted DigiDollar address key persistence (T4-03a: wallet encryption support)
    bool WriteCryptedDDAddressKey(const std::array<unsigned char, 32>& output_key,
                                  const CPubKey& pubkey,
                                  const std::vector<unsigned char>& vchCryptedSecret);
    bool ReadCryptedDDAddressKey(const std::array<unsigned char, 32>& output_key,
                                 CPubKey& pubkey,
                                 std::vector<unsigned char>& vchCryptedSecret);
    bool EraseCryptedDDAddressKey(const std::array<unsigned char, 32>& output_key);

    // Encrypted DigiDollar owner key persistence (T4-03a: wallet encryption support)
    bool WriteCryptedDDOwnerKey(const uint256& dd_timelock_id,
                                const CPubKey& pubkey,
                                const std::vector<unsigned char>& vchCryptedSecret);
    bool ReadCryptedDDOwnerKey(const uint256& dd_timelock_id,
                               CPubKey& pubkey,
                               std::vector<unsigned char>& vchCryptedSecret);
    bool EraseCryptedDDOwnerKey(const uint256& dd_timelock_id);

    // Oracle key persistence
    bool WriteOracleKey(uint32_t oracle_id, const CKey& key);
    bool HasOracleKey(uint32_t oracle_id);
    bool ReadOracleKey(uint32_t oracle_id, CKey& key);
    bool EraseOracleKey(uint32_t oracle_id);
    bool WriteCryptedOracleKey(uint32_t oracle_id,
                               const CPubKey& pubkey,
                               const std::vector<unsigned char>& vchCryptedSecret);
    bool HasCryptedOracleKey(uint32_t oracle_id);
    bool ReadCryptedOracleKey(uint32_t oracle_id,
                              CPubKey& pubkey,
                              std::vector<unsigned char>& vchCryptedSecret);
    bool EraseCryptedOracleKey(uint32_t oracle_id);

    // DigiDollar Paymaster persistence. Durable retry and recovery records may
    // contain transaction metadata and signed artifacts. They do not receive
    // Paymaster-specific field encryption; secret sponsorship capabilities are
    // redacted before persistence and only their binding hashes are written.
    // Read*WithStatus methods preserve absence, unsupported-version, and
    // corruption as distinct outcomes. Security-sensitive callers must use
    // them whenever treating an absent record differently from a broken one.
    // The bool Read* wrappers are intended only where every non-FOUND outcome
    // has the same fail-closed meaning; Has* checks key presence without
    // asserting that the stored value is valid.
    bool WritePaymasterSession(const DigiDollar::Paymaster::PaymentSession& session, bool overwrite = true);
    DatabaseReadStatus ReadPaymasterSessionWithStatus(
        const std::string& request_id,
        DigiDollar::Paymaster::PaymentSession& session);
    bool ReadPaymasterSession(const std::string& request_id, DigiDollar::Paymaster::PaymentSession& session);
    bool HasPaymasterSession(const std::string& request_id);
    bool ListPaymasterSessions(std::vector<DigiDollar::Paymaster::PaymentSession>& sessions);
    bool ErasePaymasterSession(const std::string& request_id);
    bool WritePaymasterRecovery(const DigiDollar::Paymaster::SelfRecoveryRecord& recovery, bool overwrite = false);
    DatabaseReadStatus ReadPaymasterRecoveryWithStatus(
        const std::string& request_id,
        DigiDollar::Paymaster::SelfRecoveryRecord& recovery);
    bool ReadPaymasterRecovery(const std::string& request_id, DigiDollar::Paymaster::SelfRecoveryRecord& recovery);
    bool ErasePaymasterRecovery(const std::string& request_id);
    bool WritePaymasterAlternativeRecovery(
        const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
        bool overwrite = true);
    bool ReadPaymasterAlternativeRecovery(
        const uint256& recovery_id,
        DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery);
    DatabaseReadStatus ReadPaymasterAlternativeRecoveryWithStatus(
        const uint256& recovery_id,
        DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery);
    bool ListPaymasterAlternativeRecoveries(
        std::vector<DigiDollar::Paymaster::AlternativeRecoveryRecord>& recoveries);
    bool ErasePaymasterAlternativeRecovery(const uint256& recovery_id);
    bool WritePaymasterAlternativeRecoveryRequest(
        const std::string& request_id, const uint256& recovery_id,
        bool overwrite = false);
    bool ReadPaymasterAlternativeRecoveryRequest(
        const std::string& request_id, uint256& recovery_id);
    DatabaseReadStatus ReadPaymasterAlternativeRecoveryRequestWithStatus(
        const std::string& request_id, uint256& recovery_id);
    bool ErasePaymasterAlternativeRecoveryRequest(
        const std::string& request_id);
    bool WritePaymasterAttempt(const DigiDollar::Paymaster::ProviderAttempt& attempt, bool overwrite = true);
    DatabaseReadStatus ReadPaymasterAttemptWithStatus(
        const uint256& attempt_id,
        DigiDollar::Paymaster::ProviderAttempt& attempt);
    bool ReadPaymasterAttempt(const uint256& attempt_id, DigiDollar::Paymaster::ProviderAttempt& attempt);
    bool HasPaymasterAttempt(const uint256& attempt_id);
    bool ErasePaymasterAttempt(const uint256& attempt_id);
    bool WritePaymasterCapacitySnapshot(
        const DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot,
        bool overwrite = false);
    DatabaseReadStatus ReadPaymasterCapacitySnapshotWithStatus(
        const uint256& snapshot_id,
        DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot);
    bool ReadPaymasterCapacitySnapshot(
        const uint256& snapshot_id,
        DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot);
    bool ListPaymasterCapacitySnapshots(
        std::vector<DigiDollar::Paymaster::ValidatedCapacitySnapshot>& snapshots);
    bool ErasePaymasterCapacitySnapshot(const uint256& snapshot_id);
    bool WritePaymasterCapacitySlot(const uint256& resource_commitment,
                                    const uint256& snapshot_id,
                                    bool overwrite = false);
    bool ReadPaymasterCapacitySlot(const uint256& resource_commitment,
                                   uint256& snapshot_id);
    bool ErasePaymasterCapacitySlot(const uint256& resource_commitment);
    bool WritePaymasterCapacityResource(
        const DigiDollar::Paymaster::CapacityResourceBinding& binding,
        bool overwrite = false);
    DatabaseReadStatus ReadPaymasterCapacityResourceWithStatus(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        const COutPoint& outpoint,
        DigiDollar::Paymaster::CapacityResourceBinding& binding);
    bool ReadPaymasterCapacityResource(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        const COutPoint& outpoint,
        DigiDollar::Paymaster::CapacityResourceBinding& binding);
    bool ErasePaymasterCapacityResource(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        const COutPoint& outpoint);
    bool WritePaymasterCapacityResponse(const uint256& request_hash,
                                        const std::vector<unsigned char>& response,
                                        bool overwrite = false);
    bool ReadPaymasterCapacityResponse(const uint256& request_hash,
                                       std::vector<unsigned char>& response);
    bool ListPaymasterCapacityResponses(
        std::vector<std::pair<uint256, std::vector<unsigned char>>>& responses);
    bool ErasePaymasterCapacityResponse(const uint256& request_hash);
    bool WritePaymasterCapacityNonce(const uint256& client_nonce,
                                     const uint256& request_hash,
                                     bool overwrite = false);
    bool ReadPaymasterCapacityNonce(const uint256& client_nonce,
                                    uint256& request_hash);
    bool ErasePaymasterCapacityNonce(const uint256& client_nonce);
    bool WritePaymasterCapacitySession(const uint256& session_key,
                                       const uint256& request_hash,
                                       bool overwrite = false);
    bool ReadPaymasterCapacitySession(const uint256& session_key,
                                      uint256& request_hash);
    bool ErasePaymasterCapacitySession(const uint256& session_key);
    bool WritePaymasterCapacityRelease(
        const DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release,
        bool overwrite = false);
    DatabaseReadStatus ReadPaymasterCapacityReleaseWithStatus(
        const uint256& request_hash,
        DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release);
    bool ReadPaymasterCapacityRelease(
        const uint256& request_hash,
        DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release);
    bool HasPaymasterCapacityRelease(const uint256& request_hash);
    bool ErasePaymasterCapacityRelease(const uint256& request_hash);
    bool WritePaymasterTemplate(const uint256& template_commitment, const uint256& attempt_id, bool overwrite = false);
    DatabaseReadStatus ReadPaymasterTemplateWithStatus(
        const uint256& template_commitment, uint256& attempt_id);
    bool ReadPaymasterTemplate(const uint256& template_commitment, uint256& attempt_id);
    bool ErasePaymasterTemplate(const uint256& template_commitment);
    bool WritePaymasterUnsignedTx(const uint256& unsigned_txid, const uint256& attempt_id, bool overwrite = false);
    DatabaseReadStatus ReadPaymasterUnsignedTxWithStatus(
        const uint256& unsigned_txid, uint256& attempt_id);
    bool ReadPaymasterUnsignedTx(const uint256& unsigned_txid, uint256& attempt_id);
    bool ErasePaymasterUnsignedTx(const uint256& unsigned_txid);
    bool WritePaymasterSessionId(const uint256& session_id, const std::string& request_id, bool overwrite = true);
    DatabaseReadStatus ReadPaymasterSessionIdWithStatus(
        const uint256& session_id, std::string& request_id);
    bool ReadPaymasterSessionId(const uint256& session_id, std::string& request_id);
    bool ErasePaymasterSessionId(const uint256& session_id);
    bool WritePaymasterReservation(const DigiDollar::Paymaster::InputReservation& reservation, bool overwrite = true);
    DatabaseReadStatus ReadPaymasterReservationWithStatus(
        const COutPoint& outpoint,
        DigiDollar::Paymaster::InputReservation& reservation);
    bool ReadPaymasterReservation(const COutPoint& outpoint, DigiDollar::Paymaster::InputReservation& reservation);
    bool ErasePaymasterReservation(const COutPoint& outpoint);
    bool WritePaymasterTombstone(const DigiDollar::Paymaster::IdempotencyTombstone& tombstone, bool overwrite = false);
    DatabaseReadStatus ReadPaymasterTombstoneWithStatus(
        const std::string& request_id,
        DigiDollar::Paymaster::IdempotencyTombstone& tombstone);
    bool ReadPaymasterTombstone(const std::string& request_id, DigiDollar::Paymaster::IdempotencyTombstone& tombstone);
    bool HasPaymasterTombstone(const std::string& request_id);
    bool WritePaymasterProviderCommit(const DigiDollar::Paymaster::ProviderCommitRecord& commit, bool overwrite = false);
    DatabaseReadStatus ReadPaymasterProviderCommitWithStatus(
        const uint256& commit_key,
        DigiDollar::Paymaster::ProviderCommitRecord& commit);
    bool ReadPaymasterProviderCommit(const uint256& commit_key, DigiDollar::Paymaster::ProviderCommitRecord& commit);
    bool ErasePaymasterProviderCommit(const uint256& commit_key);
    bool ListPaymasterProviderCommits(
        std::vector<DigiDollar::Paymaster::ProviderCommitRecord>& commits);
    bool WritePaymasterUserAuthorization(const DigiDollar::Paymaster::UserAuthorizationRecord& authorization, bool overwrite = false);
    DatabaseReadStatus ReadPaymasterUserAuthorizationWithStatus(
        const uint256& commit_key,
        DigiDollar::Paymaster::UserAuthorizationRecord& authorization);
    bool ReadPaymasterUserAuthorization(const uint256& commit_key, DigiDollar::Paymaster::UserAuthorizationRecord& authorization);
    bool ErasePaymasterUserAuthorization(const uint256& commit_key);
    bool WritePaymasterIdentity(const DigiDollar::Paymaster::ProviderIdentityRecord& identity, bool overwrite = false);
    DatabaseReadStatus ReadPaymasterIdentityWithStatus(
        DigiDollar::Paymaster::ProviderIdentityRecord& identity);
    bool ReadPaymasterIdentity(DigiDollar::Paymaster::ProviderIdentityRecord& identity);
    bool WritePaymasterPolicy(const DigiDollar::Paymaster::ProviderPolicy& policy, bool overwrite = true);
    DatabaseReadStatus ReadPaymasterPolicyWithStatus(
        DigiDollar::Paymaster::ProviderPolicy& policy);
    bool ReadPaymasterPolicy(DigiDollar::Paymaster::ProviderPolicy& policy);
    bool WritePaymasterSettings(const DigiDollar::Paymaster::ProviderSettings& settings, bool overwrite = true);
    DatabaseReadStatus ReadPaymasterSettingsWithStatus(
        DigiDollar::Paymaster::ProviderSettings& settings);
    bool ReadPaymasterSettings(DigiDollar::Paymaster::ProviderSettings& settings);
    bool HasPaymasterSettings();
    bool WritePaymasterProviderSafetyPolicy(
        const DigiDollar::Paymaster::ProviderSafetyPolicy& policy,
        bool overwrite = true);
    DatabaseReadStatus ReadPaymasterProviderSafetyPolicyWithStatus(
        DigiDollar::Paymaster::ProviderSafetyPolicy& policy);
    bool ReadPaymasterProviderSafetyPolicy(
        DigiDollar::Paymaster::ProviderSafetyPolicy& policy);
    bool HasPaymasterProviderSafetyPolicy();
    bool WritePaymasterClientSafetyPolicy(
        const DigiDollar::Paymaster::ClientSafetyPolicy& policy,
        bool overwrite = true);
    DatabaseReadStatus ReadPaymasterClientSafetyPolicyWithStatus(
        DigiDollar::Paymaster::ClientSafetyPolicy& policy);
    bool ReadPaymasterClientSafetyPolicy(
        DigiDollar::Paymaster::ClientSafetyPolicy& policy);
    bool HasPaymasterClientSafetyPolicy();
    bool WritePaymasterProviderBudgetLedger(
        const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
        bool overwrite = true);
    bool ReadPaymasterProviderBudgetLedger(
        DigiDollar::Paymaster::ProviderBudgetLedger& ledger);
    DatabaseReadStatus ReadPaymasterProviderBudgetLedgerWithStatus(
        DigiDollar::Paymaster::ProviderBudgetLedger& ledger);
    bool HasPaymasterProviderBudgetLedger();
    bool WritePaymasterClientFeeLedger(
        const DigiDollar::Paymaster::ClientFeeLedger& ledger,
        bool overwrite = true);
    bool ReadPaymasterClientFeeLedger(
        DigiDollar::Paymaster::ClientFeeLedger& ledger);
    DatabaseReadStatus ReadPaymasterClientFeeLedgerWithStatus(
        DigiDollar::Paymaster::ClientFeeLedger& ledger);
    bool HasPaymasterClientFeeLedger();
    bool WritePaymasterSponsorshipAuthorization(
        const DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization,
        bool overwrite = false);
    DatabaseReadStatus ReadPaymasterSponsorshipAuthorizationWithStatus(
        const uint256& capability_hash,
        DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization);
    bool ReadPaymasterSponsorshipAuthorization(
        const uint256& capability_hash,
        DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization);
    bool WritePaymasterProviderPool(
        const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
        bool overwrite = true);
    bool ReadPaymasterProviderPool(
        std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries);
    DatabaseReadStatus ReadPaymasterProviderPoolWithStatus(
        std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries);
    bool HasPaymasterProviderPool();
    bool WritePaymasterLiquidityPolicy(
        const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
        bool overwrite = true);
    DatabaseReadStatus ReadPaymasterLiquidityPolicyWithStatus(
        DigiDollar::Paymaster::ProviderLiquidityPolicy& policy);
    bool ReadPaymasterLiquidityPolicy(
        DigiDollar::Paymaster::ProviderLiquidityPolicy& policy);
    bool HasPaymasterLiquidityPolicy();
    bool WritePaymasterMaintenanceLedger(
        const DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger,
        bool overwrite = true);
    bool ReadPaymasterMaintenanceLedger(
        DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger);
    DatabaseReadStatus ReadPaymasterMaintenanceLedgerWithStatus(
        DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger);
    bool HasPaymasterMaintenanceLedger();
    bool WritePaymasterFinanceLedger(
        const DigiDollar::Paymaster::ProviderFinanceLedger& ledger,
        bool overwrite = true);
    bool ReadPaymasterFinanceLedger(
        DigiDollar::Paymaster::ProviderFinanceLedger& ledger);
    DatabaseReadStatus ReadPaymasterFinanceLedgerWithStatus(
        DigiDollar::Paymaster::ProviderFinanceLedger& ledger);
    bool HasPaymasterFinanceLedger();
    bool WritePaymasterBackupStatus(
        const DigiDollar::Paymaster::ProviderBackupStatus& status,
        bool overwrite = true);
    DatabaseReadStatus ReadPaymasterBackupStatusWithStatus(
        DigiDollar::Paymaster::ProviderBackupStatus& status);
    bool ReadPaymasterBackupStatus(
        DigiDollar::Paymaster::ProviderBackupStatus& status);
    bool HasPaymasterBackupStatus();
    bool WritePaymasterCarrierWithdrawalPlan(
        const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan,
        bool overwrite = true);
    DatabaseReadStatus ReadPaymasterCarrierWithdrawalPlanWithStatus(
        DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan);
    bool ReadPaymasterCarrierWithdrawalPlan(
        DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan);
    bool HasPaymasterCarrierWithdrawalPlan();
    bool ErasePaymasterCarrierWithdrawalPlan();
    bool WritePaymasterAnnouncementSequence(uint64_t sequence, bool overwrite = true);
    bool ReadPaymasterAnnouncementSequence(uint64_t& sequence);
    bool WritePaymasterResult(const DigiDollar::Paymaster::PaymasterResult& result,
                              bool overwrite = true);
    bool ReadPaymasterResult(const uint256& commit_key,
                             DigiDollar::Paymaster::PaymasterResult& result);
    DatabaseReadStatus ReadPaymasterResultWithStatus(
        const uint256& commit_key,
        DigiDollar::Paymaster::PaymasterResult& result);
    bool HasPaymasterResult(const uint256& commit_key);
    bool ErasePaymasterResult(const uint256& commit_key);
    bool WritePaymasterReliability(
        const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
        bool overwrite = true);
    DatabaseReadStatus ReadPaymasterReliabilityWithStatus(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        DigiDollar::Paymaster::PaymasterReliabilityRecord& record);
    bool ReadPaymasterReliability(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        DigiDollar::Paymaster::PaymasterReliabilityRecord& record);
    bool ListPaymasterReliability(
        std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord>& records);
    bool ErasePaymasterReliability(const DigiDollar::Paymaster::PaymasterId& provider_id);
    bool WritePaymasterEquivocationEvidence(
        const DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
        bool overwrite = false);
    DatabaseReadStatus ReadPaymasterEquivocationEvidence(
        const uint256& evidence_id,
        DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence);
    bool WritePaymasterPendingEquivocation(
        const DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
        bool overwrite = false);
    DatabaseReadStatus ReadPaymasterPendingEquivocation(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence);
    DatabaseReadStatus ListPaymasterPendingEquivocations(
        std::vector<DigiDollar::Paymaster::PaymasterEquivocationEvidence>& evidence);
    bool ErasePaymasterPendingEquivocation(
        const DigiDollar::Paymaster::PaymasterId& provider_id);
    bool WritePaymasterProviderBlock(
        const DigiDollar::Paymaster::PaymasterProviderBlock& block,
        bool overwrite = false);
    DatabaseReadStatus ReadPaymasterProviderBlock(
        const DigiDollar::Paymaster::PaymasterId& provider_id,
        DigiDollar::Paymaster::PaymasterProviderBlock& block);
    bool ListPaymasterProviderBlocks(
        std::vector<DigiDollar::Paymaster::PaymasterProviderBlock>& blocks);
    bool WritePaymasterOutcomeMarker(
        const DigiDollar::Paymaster::PaymasterOutcomeMarker& marker,
        bool overwrite = false);
    bool ReadPaymasterOutcomeMarker(
        const uint256& attempt_id,
        DigiDollar::Paymaster::PaymasterOutcomeMarker& marker);
    DatabaseReadStatus ReadPaymasterOutcomeMarkerWithStatus(
        const uint256& attempt_id,
        DigiDollar::Paymaster::PaymasterOutcomeMarker& marker);
    bool ErasePaymasterOutcomeMarker(const uint256& attempt_id);

    bool WriteAddressPreviouslySpent(const CTxDestination& dest, bool previously_spent);
    bool WriteAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& receive_request);
    bool EraseAddressReceiveRequest(const CTxDestination& dest, const std::string& id);
    bool EraseAddressData(const CTxDestination& dest);

    bool WriteActiveScriptPubKeyMan(uint8_t type, const uint256& id, bool internal);
    bool EraseActiveScriptPubKeyMan(uint8_t type, bool internal);

    DBErrors LoadWallet(CWallet* pwallet);
    DBErrors FindWalletTxHashes(std::vector<uint256>& tx_hashes);
    DBErrors ZapSelectTx(std::vector<uint256>& vHashIn, std::vector<uint256>& vHashOut);

    //! write the hdchain model (external chain child index counter)
    bool WriteHDChain(const CHDChain& chain);

    //! Delete records of the given types
    bool EraseRecords(const std::unordered_set<std::string>& types);

    bool WriteWalletFlags(const uint64_t flags);
    //! Begin a new transaction
    bool TxnBegin();
    //! Commit current transaction
    bool TxnCommit();
    //! Abort current transaction
    bool TxnAbort();

    //! Get database cursor for iteration
    std::unique_ptr<DatabaseCursor> GetNewCursor() { return m_batch->GetNewCursor(); }

private:
    template <typename K, typename T>
    DatabaseReadStatus ReadPaymasterVersionedRecord(const K& key, T& value);

    std::unique_ptr<DatabaseBatch> m_batch;
    WalletDatabase& m_database;
};

//! Compacts BDB state so that wallet.dat is self-contained (if there are changes)
void MaybeCompactWalletDB(WalletContext& context);

bool LoadKey(CWallet* pwallet, DataStream& ssKey, DataStream& ssValue, std::string& strErr);
bool LoadCryptedKey(CWallet* pwallet, DataStream& ssKey, DataStream& ssValue, std::string& strErr);
bool LoadEncryptionKey(CWallet* pwallet, DataStream& ssKey, DataStream& ssValue, std::string& strErr);
bool LoadHDChain(CWallet* pwallet, DataStream& ssValue, std::string& strErr);
} // namespace wallet

#endif // DIGIBYTE_WALLET_WALLETDB_H
