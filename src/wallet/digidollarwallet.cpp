// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/digidollarwallet.h>
#include <wallet/wallet.h>
#include <wallet/crypter.h>
#include <wallet/spend.h>
#include <wallet/receive.h>
#include <wallet/coincontrol.h>
#include <wallet/scriptpubkeyman.h>
#include <common/args.h>
#include <interfaces/chain.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <digidollar/scripts.h>
#include <util/strencodings.h>
#include <logging.h>
#include <util/time.h>
#include <kernel/chainparams.h>
#include <chainparams.h>
#include <crypto/common.h>
#include <streams.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/interpreter.h>
#include <random.h>
#include <key_io.h>
#include <oracle/mock_oracle.h>
#include <oracle/bundle_manager.h>
#include <coins.h>
#include <policy/policy.h>

#include <algorithm>
#include <limits>
#include <map>
#include <regex>
#include <set>

namespace {
static constexpr CAmount TRANSFER_BUILDER_PRICE_UNUSED{1};    // Transfers are price-independent.

CScript BuildDDTransferMetadataScript(const std::vector<CAmount>& amounts)
{
    CScript metadata;
    metadata << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(2);
    for (const CAmount amount : amounts) metadata << CScriptNum(amount);
    return metadata;
}

bool PreflightDDTransferCapacity(const DigiDollar::TxBuilderTransferParams& params,
                                 CAmount selected_dd_total,
                                 CAmount total_dd_out,
                                 std::string& error,
                                 size_t* projected_vsize = nullptr,
                                 CAmount* projected_fee = nullptr)
{
    std::vector<CAmount> dd_output_amounts;
    dd_output_amounts.reserve(params.recipients.size() + 1);
    for (const auto& [address, amount] : params.recipients) dd_output_amounts.push_back(amount);
    const CAmount dd_change = selected_dd_total - total_dd_out;
    if (dd_change > 0) dd_output_amounts.push_back(dd_change);

    const CScript metadata = BuildDDTransferMetadataScript(dd_output_amounts);
    if (metadata.size() > MAX_OP_RETURN_RELAY) {
        error = strprintf("Too many DigiDollar outputs for one transaction: metadata is %u bytes, standard relay limit is %u bytes. Reduce recipients or split into multiple sendmanydigidollar calls.",
                          static_cast<unsigned>(metadata.size()), MAX_OP_RETURN_RELAY);
        return false;
    }

    CMutableTransaction projected;
    projected.SetDigiDollarType(::DD_TX_TRANSFER);
    for (const auto& utxo : params.ddUtxos) projected.vin.push_back(CTxIn(utxo));
    for (const auto& utxo : params.feeUtxos) projected.vin.push_back(CTxIn(utxo));

    for (const auto& [address, amount] : params.recipients) {
        CTxDestination dest = DecodeDigiDollarAddress(address);
        const auto* taproot = std::get_if<WitnessV1Taproot>(&dest);
        if (!taproot) {
            error = "Invalid DigiDollar recipient address during capacity preflight";
            return false;
        }
        CScript dd_script;
        dd_script << OP_1 << ToByteVector(*taproot);
        projected.vout.push_back(CTxOut(0, dd_script));
    }
    if (dd_change > 0) {
        CScript change_script;
        change_script << OP_1 << std::vector<unsigned char>(32, 0);
        projected.vout.push_back(CTxOut(0, change_script));
    }
    // Worst-case DGB fee change output; including it makes the preflight deterministic and conservative.
    projected.vout.push_back(CTxOut(1, CScript() << OP_0 << std::vector<unsigned char>(20, 0)));
    projected.vout.push_back(CTxOut(0, metadata));

    const size_t vsize = DigiDollar::EstimateTransactionVSize(projected);
    const int64_t weight = static_cast<int64_t>(vsize) * WITNESS_SCALE_FACTOR;
    if (weight > MAX_STANDARD_TX_WEIGHT) {
        error = strprintf("Projected DigiDollar transaction is too large: %d weight units (%u vB), standard limit is %d WU. Reduce recipients or consolidate DD/DGB UTXOs first.",
                          weight, static_cast<unsigned>(vsize), MAX_STANDARD_TX_WEIGHT);
        return false;
    }
    const CAmount fee = std::max<CAmount>((static_cast<CAmount>(vsize) * DigiDollar::MIN_DD_FEE_RATE) / 1000, DigiDollar::MIN_DD_TX_FEE);
    if (projected_vsize) *projected_vsize = vsize;
    if (projected_fee) *projected_fee = fee;
    return true;
}

/** Worst-case (largest) output script the redemption builder can emit. */
CScript WorstCaseDDOutputScript()
{
    CScript script;
    script << OP_1 << std::vector<unsigned char>(32, 0);
    return script;
}

/**
 * Project the redemption transaction RedeemTxBuilder::BuildRedemptionTransaction()
 * will build for `params` (including the fee inputs currently held in
 * params.feeUtxos) and derive the fee it will owe. Redemption counterpart of
 * PreflightDDTransferCapacity: the fee follows from the built transaction, so it
 * cannot be pinned down by a fixed size guess.
 */
bool PreflightDDRedemptionCapacity(const DigiDollar::TxBuilderRedeemParams& params,
                                   std::string& error,
                                   size_t* projected_vsize = nullptr,
                                   CAmount* projected_fee = nullptr)
{
    if (params.feeRate <= 0) {
        error = strprintf("Invalid DigiDollar redemption fee rate: %lld sat/kvB. The fee rate must be positive.",
                          static_cast<long long>(params.feeRate));
        return false;
    }

    CMutableTransaction projected;
    projected.SetDigiDollarType(::DD_TX_REDEEM);
    projected.vin.push_back(CTxIn(params.collateralOutpoint));
    for (const auto& utxo : params.ddUtxos) projected.vin.push_back(CTxIn(utxo));
    for (const auto& utxo : params.feeUtxos) projected.vin.push_back(CTxIn(utxo));

    // Returned collateral.
    projected.vout.push_back(CTxOut(params.collateralAmount,
                                    params.collateralDest.has_value()
                                        ? GetScriptForDestination(*params.collateralDest)
                                        : WorstCaseDDOutputScript()));
    // Worst-case DD change output plus the OP_RETURN that carries its amount,
    // and the DGB fee change output. Including them unconditionally keeps the
    // projection deterministic and never under-states the built transaction.
    projected.vout.push_back(CTxOut(0, WorstCaseDDOutputScript()));
    CScript metadata;
    metadata << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(3) << CScriptNum(params.ddToRedeem);
    projected.vout.push_back(CTxOut(0, metadata));
    projected.vout.push_back(CTxOut(1, params.dgbChangeDest.has_value()
                                           ? GetScriptForDestination(*params.dgbChangeDest)
                                           : WorstCaseDDOutputScript()));

    const size_t vsize = DigiDollar::EstimateTransactionVSize(projected);
    const int64_t weight = static_cast<int64_t>(vsize) * WITNESS_SCALE_FACTOR;
    if (weight > MAX_STANDARD_TX_WEIGHT) {
        error = strprintf("Projected DigiDollar redemption is too large: %d weight units (%u vB) across %u inputs, standard limit is %d WU. Consolidate DGB or DD UTXOs first.",
                          weight, static_cast<unsigned>(vsize), static_cast<unsigned>(projected.vin.size()), MAX_STANDARD_TX_WEIGHT);
        return false;
    }
    const CAmount fee = std::max<CAmount>((static_cast<CAmount>(vsize) * params.feeRate) / 1000, DigiDollar::MIN_DD_TX_FEE);
    if (projected_vsize) *projected_vsize = vsize;
    if (projected_fee) *projected_fee = fee;
    return true;
}
} // namespace

static bool DDChangeIsStandard(CAmount selected_total, CAmount target_amount)
{
    if (selected_total < target_amount) return false;
    const CAmount change = selected_total - target_amount;
    return change == 0 || change >= Params().GetDigiDollarParams().minOutputAmount;
}

static std::string DDChangePolicyError(CAmount selected_total, CAmount target_amount)
{
    if (selected_total < target_amount) {
        return strprintf("Insufficient selected DD input amount. Selected: %lld cents, Required: %lld cents",
                         static_cast<long long>(selected_total), static_cast<long long>(target_amount));
    }

    const CAmount change = selected_total - target_amount;
    return strprintf("Selected DD input change is below minimum DigiDollar output. Change: %lld cents, Minimum: %lld cents",
                     static_cast<long long>(change),
                     static_cast<long long>(Params().GetDigiDollarParams().minOutputAmount));
}

static bool IsStandardDDTokenOutput(const CTxOut& txout)
{
    int witness_version = -1;
    std::vector<unsigned char> witness_program;
    return txout.nValue == 0 &&
           txout.scriptPubKey.IsWitnessProgram(witness_version, witness_program) &&
           witness_version == 1 &&
           witness_program.size() == WITNESS_V1_TAPROOT_SIZE;
}

static bool IsCanonicalP2TROutput(const CScript& script)
{
    int witness_version = -1;
    std::vector<unsigned char> witness_program;
    return script.IsWitnessProgram(witness_version, witness_program) &&
           witness_version == 1 &&
           witness_program.size() == WITNESS_V1_TAPROOT_SIZE;
}

struct MintOutputIndexes {
    uint32_t collateral_index{std::numeric_limits<uint32_t>::max()};
    uint32_t dd_token_index{std::numeric_limits<uint32_t>::max()};
    CAmount collateral_amount{0};
};

static bool FindMintOutputIndexes(const CTransaction& tx, MintOutputIndexes& indexes)
{
    indexes = {};

    if (DigiDollar::GetDigiDollarTxType(tx) != DigiDollar::DD_TX_MINT) {
        return false;
    }

    int collateral_count = 0;
    int dd_token_count = 0;
    for (uint32_t i = 0; i < tx.vout.size(); ++i) {
        const CTxOut& txout = tx.vout[i];
        if (!IsCanonicalP2TROutput(txout.scriptPubKey)) {
            continue;
        }

        if (txout.nValue > 0) {
            ++collateral_count;
            indexes.collateral_index = i;
            indexes.collateral_amount = txout.nValue;
        } else if (txout.nValue == 0) {
            ++dd_token_count;
            indexes.dd_token_index = i;
        }
    }

    return collateral_count == 1 &&
           dd_token_count == 1 &&
           indexes.collateral_amount > 0;
}

static std::vector<CAmount> ExtractDDMetadataAmounts(const CTransaction& tx, int expected_type)
{
    std::vector<CAmount> amounts;
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

        try {
            CScriptNum tx_type(data, true);
            if (tx_type.getint() != expected_type) return {};

            while (script.GetOp(pc, opcode, data)) {
                if (data.empty()) continue;
                CScriptNum amount(data, true, 8);
                const CAmount value = amount.GetInt64();
                if (value > 0) amounts.push_back(value);
            }
        } catch (const scriptnum_error&) {
            return {};
        }
        break;
    }
    return amounts;
}

// CDigiDollarAddress is defined in base58.h - no need to redefine

// =============================================================================
// DDTransaction Implementation
// =============================================================================

DDTransaction::DDTransaction()
    : amount(0), timestamp(0), confirmations(0), incoming(false), category("unknown"),
      blockheight(-1), blockhash(""), fee(0), comment(""), abandoned(false), lock_tier(-1),
      in_mempool(false), is_local(false) {}

// =============================================================================
// DigiDollarWallet Implementation
// =============================================================================

DigiDollarWallet::DigiDollarWallet() : mockBalance(0), total_dd_balance(0), locked_collateral(0), m_wallet(nullptr) {
    // Initialize with some test data for development
    LogPrintf("DigiDollar: Wallet initialized\n");
}

DigiDollarWallet::DigiDollarWallet(wallet::CWallet* wallet) : mockBalance(0), total_dd_balance(0), locked_collateral(0), m_wallet(wallet) {
    LogPrintf("DigiDollar: Wallet initialized with CWallet pointer\n");

    // Load existing DigiDollar data from database
    if (m_wallet) {
        size_t loaded = LoadFromDatabase();
        LogPrintf("DigiDollarWallet: Initialized with %d items from database\n", loaded);
    }
}

size_t DigiDollarWallet::LoadFromDatabase()
{
    LOCK(cs_dd_wallet);
    if (!m_wallet) {
        LogPrint(BCLog::WALLETDB, "DigiDollarWallet::LoadFromDatabase - No wallet pointer\n");
        return 0;
    }

    LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loading data from database...\n");

    size_t positions_loaded = LoadPositionsFromDatabase();
    size_t balances_loaded = LoadBalancesFromDatabase();
    size_t txs_loaded = LoadTransactionsFromDatabase();

    // FIX #1: Load DD UTXOs from database
    size_t utxos_loaded = 0;
    wallet::WalletBatch batch(m_wallet->GetDatabase());
    dd_utxos.clear();

    std::unique_ptr<wallet::DatabaseCursor> cursor = batch.GetNewCursor();
    if (cursor) {
        wallet::DatabaseCursor::Status status = wallet::DatabaseCursor::Status::MORE;
        while (status == wallet::DatabaseCursor::Status::MORE) {
            DataStream key{};
            DataStream value{};
            status = cursor->Next(key, value);

            if (status != wallet::DatabaseCursor::Status::MORE) break;

            std::string key_type;
            key >> key_type;

            if (key_type == wallet::DBKeys::DD_OUTPUT) {
                COutPoint outpoint;
                key >> outpoint;

                CAmount dd_amount;
                value >> dd_amount;

                dd_utxos[outpoint] = dd_amount;
                utxos_loaded++;

                LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded DD UTXO %s:%u (%lld cents)\n",
                        outpoint.hash.ToString(), outpoint.n, static_cast<long long>(dd_amount));
            }
        }
    }

    // Load DD address keys for received tokens (FIX for wallet restart key loss)
    size_t addr_keys_loaded = LoadDDAddressKeys();

    // Load DD owner keys for minted tokens (FIX for vault redemption after restart)
    size_t owner_keys_loaded = LoadDDOwnerKeys();

    size_t total = positions_loaded + balances_loaded + txs_loaded + utxos_loaded + addr_keys_loaded + owner_keys_loaded;

    LogPrintf("DigiDollarWallet: Loaded %zu positions, %zu balances, %zu transactions, %zu DD UTXOs, %zu DD address keys, %zu DD owner keys\n",
              positions_loaded, balances_loaded, txs_loaded, utxos_loaded, addr_keys_loaded, owner_keys_loaded);

    // CRITICAL FIX: Lock collateral UTXOs for all active DD positions
    // This ensures the wallet's regular DGB coin selection never picks
    // collateral UTXOs after a restart. Without this, the wallet could
    // create transactions that try to spend time-locked collateral,
    // causing them to get stuck as unconfirmed.
    size_t locked_count = 0;
    if (m_wallet) {
        wallet::WalletBatch lock_batch(m_wallet->GetDatabase());
        for (const auto& [pos_id, pos] : collateral_positions) {
            if (pos.is_active) {
                COutPoint collateralOutpoint(pos.dd_timelock_id, 0);
                COutPoint ddTokenOutpoint(pos.dd_timelock_id, 1);
                auto tx_it = m_wallet->mapWallet.find(pos.dd_timelock_id);
                if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                    MintOutputIndexes mint_outputs;
                    if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                        collateralOutpoint = COutPoint(pos.dd_timelock_id, mint_outputs.collateral_index);
                        ddTokenOutpoint = COutPoint(pos.dd_timelock_id, mint_outputs.dd_token_index);
                    }
                }
                if (!m_wallet->IsLockedCoin(collateralOutpoint)) {
                    if (m_wallet->LockCoin(collateralOutpoint, &lock_batch)) {
                        locked_count++;
                    }
                }
                if (!m_wallet->IsLockedCoin(ddTokenOutpoint)) {
                    if (m_wallet->LockCoin(ddTokenOutpoint, &lock_batch)) {
                        locked_count++;
                    }
                }
            }
        }
        if (locked_count > 0) {
            LogPrintf("DigiDollarWallet: Locked %zu collateral/DD-token UTXOs from %zu active positions\n",
                     locked_count, collateral_positions.size());
        }
    }

    // Recalculate totals
    RecalculateTotals();

    return total;
}

size_t DigiDollarWallet::LoadPositionsFromDatabase()
{
    LOCK(cs_dd_wallet);
    wallet::WalletBatch batch(m_wallet->GetDatabase());
    size_t count = 0;

    // Clear in-memory positions
    collateral_positions.clear();

    // Iterate through database using cursor
    std::unique_ptr<wallet::DatabaseCursor> cursor = batch.GetNewCursor();
    if (!cursor) {
        LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Failed to get database cursor\n");
        return 0;
    }

    wallet::DatabaseCursor::Status status = wallet::DatabaseCursor::Status::MORE;
    while (status == wallet::DatabaseCursor::Status::MORE) {
        DataStream key{};
        DataStream value{};
        status = cursor->Next(key, value);

        if (status != wallet::DatabaseCursor::Status::MORE) break;

        // Check if this is a position entry
        std::string key_type;
        key >> key_type;

        if (key_type == wallet::DBKeys::DD_POSITION) {
            uint256 dd_timelock_id;
            key >> dd_timelock_id;

            WalletCollateralPosition position;
            value >> position;

            collateral_positions[dd_timelock_id] = position;
            count++;

            LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded position %s\n",
                     dd_timelock_id.ToString());
        }
    }

    return count;
}

size_t DigiDollarWallet::LoadBalancesFromDatabase()
{
    LOCK(cs_dd_wallet);
    wallet::WalletBatch batch(m_wallet->GetDatabase());
    size_t count = 0;

    dd_balances.clear();

    std::unique_ptr<wallet::DatabaseCursor> cursor = batch.GetNewCursor();
    if (!cursor) return 0;

    wallet::DatabaseCursor::Status status = wallet::DatabaseCursor::Status::MORE;
    while (status == wallet::DatabaseCursor::Status::MORE) {
        DataStream key{};
        DataStream value{};
        status = cursor->Next(key, value);

        if (status != wallet::DatabaseCursor::Status::MORE) break;

        std::string key_type;
        key >> key_type;

        if (key_type == wallet::DBKeys::DD_BALANCE) {
            std::string address;
            key >> address;

            WalletDDBalance balance;
            value >> balance;

            dd_balances[address] = balance;
            count++;

            LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded balance for %s\n", address);
        }
    }

    return count;
}

size_t DigiDollarWallet::LoadTransactionsFromDatabase()
{
    LOCK(cs_dd_wallet);
    wallet::WalletBatch batch(m_wallet->GetDatabase());
    size_t count = 0;

    transaction_history.clear();

    std::unique_ptr<wallet::DatabaseCursor> cursor = batch.GetNewCursor();
    if (!cursor) return 0;

    wallet::DatabaseCursor::Status status = wallet::DatabaseCursor::Status::MORE;
    while (status == wallet::DatabaseCursor::Status::MORE) {
        DataStream key{};
        DataStream value{};
        status = cursor->Next(key, value);

        if (status != wallet::DatabaseCursor::Status::MORE) break;

        std::string key_type;
        key >> key_type;

        if (key_type == wallet::DBKeys::DD_TRANSACTION) {
            uint256 txid;
            key >> txid;

            DDTransaction ddtx;
            value >> ddtx;

            transaction_history.push_back(ddtx);
            count++;

            LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded transaction %s\n", ddtx.txid);
        }
    }

    return count;
}

void DigiDollarWallet::RecalculateTotals()
{
    LOCK(cs_dd_wallet);
    // Recalculate total DD balance
    total_dd_balance = 0;
    for (const auto& [addr, bal] : dd_balances) {
        total_dd_balance += bal.balance;
    }

    // Recalculate locked collateral
    locked_collateral = 0;
    for (const auto& [id, pos] : collateral_positions) {
        if (pos.is_active) {
            locked_collateral += pos.dgb_collateral;
        }
    }

    LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Totals - DD Balance: %lld, Locked: %lld\n",
             static_cast<long long>(total_dd_balance), static_cast<long long>(locked_collateral));
}

// =============================================================================
// T4-03a: Encrypt existing DD keys when wallet encryption is enabled
// =============================================================================

bool DigiDollarWallet::EncryptDDKeys(const wallet::CKeyingMaterial& vMasterKey, wallet::WalletBatch* encrypted_batch)
{
    LOCK(cs_dd_wallet);
    LogPrintf("DigiDollarWallet: Encrypting %zu owner keys and %zu address keys\n",
              dd_owner_keys.size(), dd_address_keys.size());

    bool use_external_batch = (encrypted_batch != nullptr);

    // Encrypt all plaintext owner keys
    for (const auto& [timelock_id, key] : dd_owner_keys) {
        CPubKey pubkey = key.GetPubKey();
        wallet::CKeyingMaterial vchSecret(key.begin(), key.end());
        std::vector<unsigned char> vchCryptedSecret;

        if (!wallet::EncryptSecret(vMasterKey, vchSecret, pubkey.GetHash(), vchCryptedSecret)) {
            LogPrintf("DigiDollarWallet: ERROR - Failed to encrypt DD owner key for timelock %s\n",
                      timelock_id.ToString());
            return false;
        }

        dd_crypted_owner_keys[timelock_id] = std::make_pair(pubkey, vchCryptedSecret);

        // Persist to database
        if (use_external_batch) {
            if (!encrypted_batch->WriteCryptedDDOwnerKey(timelock_id, pubkey, vchCryptedSecret)) {
                LogPrintf("DigiDollarWallet: ERROR - Failed to write encrypted DD owner key to database\n");
                return false;
            }
        } else if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteCryptedDDOwnerKey(timelock_id, pubkey, vchCryptedSecret)) {
                LogPrintf("DigiDollarWallet: ERROR - Failed to write encrypted DD owner key to database\n");
                return false;
            }
        }
    }

    // Encrypt all plaintext address keys
    for (const auto& [key_bytes, key] : dd_address_keys) {
        CPubKey pubkey = key.GetPubKey();
        wallet::CKeyingMaterial vchSecret(key.begin(), key.end());
        std::vector<unsigned char> vchCryptedSecret;

        if (!wallet::EncryptSecret(vMasterKey, vchSecret, pubkey.GetHash(), vchCryptedSecret)) {
            LogPrintf("DigiDollarWallet: ERROR - Failed to encrypt DD address key %s\n",
                      HexStr(key_bytes));
            return false;
        }

        dd_crypted_address_keys[key_bytes] = std::make_pair(pubkey, vchCryptedSecret);

        // Persist to database
        if (use_external_batch) {
            if (!encrypted_batch->WriteCryptedDDAddressKey(key_bytes, pubkey, vchCryptedSecret)) {
                LogPrintf("DigiDollarWallet: ERROR - Failed to write encrypted DD address key to database\n");
                return false;
            }
        } else if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteCryptedDDAddressKey(key_bytes, pubkey, vchCryptedSecret)) {
                LogPrintf("DigiDollarWallet: ERROR - Failed to write encrypted DD address key to database\n");
                return false;
            }
        }
    }

    // Erase plaintext keys from the DATABASE before clearing memory.
    // Without this, a forensic attacker could read wallet.dat and find
    // plaintext DD_OWNER_KEY / DD_ADDRESS_KEY entries alongside encrypted ones.
    if (m_wallet) {
        wallet::WalletBatch* erase_batch = use_external_batch ? encrypted_batch : nullptr;
        std::unique_ptr<wallet::WalletBatch> local_batch;
        if (!erase_batch) {
            local_batch = std::make_unique<wallet::WalletBatch>(m_wallet->GetDatabase());
            erase_batch = local_batch.get();
        }
        for (const auto& [timelock_id, key] : dd_owner_keys) {
            if (!erase_batch->EraseDDOwnerKey(timelock_id)) {
                LogPrintf("DigiDollarWallet: WARNING - Failed to erase plaintext DD owner key %s from database\n",
                          timelock_id.ToString());
            }
        }
        for (const auto& [key_bytes, key] : dd_address_keys) {
            if (!erase_batch->EraseDDAddressKey(key_bytes)) {
                LogPrintf("DigiDollarWallet: WARNING - Failed to erase plaintext DD address key %s from database\n",
                          HexStr(key_bytes));
            }
        }
    }

    // Clear plaintext keys from memory — they are now encrypted
    dd_owner_keys.clear();
    dd_address_keys.clear();
    dd_foreign_output_keys.clear();

    LogPrintf("DigiDollarWallet: Successfully encrypted %zu owner keys and %zu address keys\n",
              dd_crypted_owner_keys.size(), dd_crypted_address_keys.size());
    return true;
}

void DigiDollarWallet::StoreAddressKey(const XOnlyPubKey& output_key, const CKey& key)
{
    LOCK(cs_dd_wallet);
    std::array<unsigned char, 32> key_bytes;
    std::copy(output_key.begin(), output_key.end(), key_bytes.begin());
    dd_foreign_output_keys.erase(key_bytes);

    LogPrintf("DigiDollarWallet: Storing DD address key for output key %s\n",
              HexStr(output_key));

    // If wallet is encrypted, encrypt the key before storage (T4-03a)
    if (m_wallet && m_wallet->IsCrypted()) {
        CPubKey pubkey = key.GetPubKey();
        wallet::CKeyingMaterial vchSecret(key.begin(), key.end());
        std::vector<unsigned char> vchCryptedSecret;

        if (!wallet::EncryptSecret(m_wallet->GetEncryptionKey(), vchSecret, pubkey.GetHash(), vchCryptedSecret)) {
            LogPrintf("DigiDollarWallet: ERROR - Failed to encrypt DD address key\n");
            return;
        }

        // Store encrypted in memory
        dd_crypted_address_keys[key_bytes] = std::make_pair(pubkey, vchCryptedSecret);
        // Remove any plaintext version from memory
        dd_address_keys.erase(key_bytes);

        // Persist encrypted to database
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteCryptedDDAddressKey(key_bytes, pubkey, vchCryptedSecret)) {
                LogPrintf("DigiDollarWallet: WARNING - Failed to persist encrypted DD address key to database\n");
            } else {
                LogPrintf("DigiDollarWallet: Persisted encrypted DD address key to database\n");
            }
        }
    } else {
        // Store plaintext in memory
        dd_address_keys[key_bytes] = key;

        // Persist plaintext to wallet database
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteDDAddressKey(key_bytes, key)) {
                LogPrintf("DigiDollarWallet: WARNING - Failed to persist DD address key to database\n");
            } else {
                LogPrintf("DigiDollarWallet: Persisted DD address key to database\n");
            }
        }
    }
}

size_t DigiDollarWallet::LoadDDAddressKeys()
{
    LOCK(cs_dd_wallet);
    if (!m_wallet) {
        LogPrint(BCLog::WALLETDB, "DigiDollarWallet::LoadDDAddressKeys - No wallet pointer\n");
        return 0;
    }

    wallet::WalletBatch batch(m_wallet->GetDatabase());
    size_t count = 0;

    // Clear in-memory maps before loading
    dd_address_keys.clear();
    dd_crypted_address_keys.clear();
    dd_foreign_output_keys.clear();

    // Iterate through database to find DD address keys (both plaintext and encrypted)
    std::unique_ptr<wallet::DatabaseCursor> cursor = batch.GetNewCursor();
    if (cursor) {
        wallet::DatabaseCursor::Status status = wallet::DatabaseCursor::Status::MORE;
        while (status == wallet::DatabaseCursor::Status::MORE) {
            DataStream key_stream{};
            DataStream value_stream{};
            status = cursor->Next(key_stream, value_stream);

            if (status != wallet::DatabaseCursor::Status::MORE) break;

            std::string key_type;
            key_stream >> key_type;

            if (key_type == wallet::DBKeys::DD_ADDRESS_KEY) {
                // Plaintext DD address key (unencrypted wallet)
                std::array<unsigned char, 32> output_key_bytes;
                key_stream >> output_key_bytes;

                CPrivKey privkey;
                value_stream >> privkey;

                std::array<unsigned char, CPubKey::COMPRESSED_SIZE> compressed_dummy{};
                compressed_dummy[0] = 0x02;
                CPubKey dummy_pubkey(compressed_dummy.begin(), compressed_dummy.end());

                CKey key;
                if (key.Load(privkey, dummy_pubkey, /*fSkipCheck=*/true)) {
                    dd_address_keys[output_key_bytes] = key;
                    count++;

                    LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded plaintext DD address key %s\n",
                            HexStr(output_key_bytes));
                } else {
                    LogPrintf("DigiDollarWallet: WARNING - Failed to load DD address key from database\n");
                }
            } else if (key_type == wallet::DBKeys::DD_CRYPTED_ADDRESS_KEY) {
                // Encrypted DD address key (T4-03a: encrypted wallet)
                std::array<unsigned char, 32> output_key_bytes;
                key_stream >> output_key_bytes;

                std::pair<CPubKey, std::vector<unsigned char>> val;
                value_stream >> val;

                dd_crypted_address_keys[output_key_bytes] = val;
                count++;

                LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded encrypted DD address key %s\n",
                        HexStr(output_key_bytes));
            }
        }
    }

    LogPrintf("DigiDollarWallet: Loaded %zu DD address keys (%zu plaintext, %zu encrypted) from database\n",
              count, dd_address_keys.size(), dd_crypted_address_keys.size());
    return count;
}

bool DigiDollarWallet::IsDDOutputMine(const CTxOut& txout, const uint256& txid) const
{
    auto locks = LockDDWallet();
    // First check if this is a DD output (P2TR with value=0)
    if (txout.nValue != 0 || txout.scriptPubKey.size() != 34 || txout.scriptPubKey[0] != OP_1) {
        return false;
    }

    // Try standard wallet IsMine first — require SPENDABLE to exclude watch-only
    // SECURITY [T4-04]: Using ISMINE_SPENDABLE prevents watch-only DD balance contamination
    if (m_wallet && (m_wallet->IsMine(txout) & wallet::ISMINE_SPENDABLE)) {
        return true;
    }

    // Check if this is a MINT output that we've already identified as ours
    // This handles the case where dd_owner_keys is empty (e.g., after wallet restore)
    // but we've already processed the MINT tx and added it to collateral_positions
    if (collateral_positions.count(txid) > 0) {
        // This txid is a MINT we own, so its canonical DD token output is ours.
        return true;
    }

    // Extract the P2TR output key from the scriptPubKey
    // P2TR scripts are: OP_1 <32-byte-output-key>
    std::vector<unsigned char> output_key_bytes(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end());
    std::array<unsigned char, 32> output_key_array;
    std::copy(output_key_bytes.begin(), output_key_bytes.end(), output_key_array.begin());

    // Check dd_owner_keys - first try the specific txid, then check ALL owner keys
    // This is needed because TRANSFER change outputs use the owner key from the original
    // MINT (stored under MINT txid), not the TRANSFER txid.
    CKey owner_key;
    if (GetOwnerKey(txid, owner_key)) {
        // Compute what the tweaked key should be from this owner_key
        XOnlyPubKey owner_xonly(owner_key.GetPubKey());
        auto tweaked = owner_xonly.CreateTapTweak(nullptr);
        if (tweaked) {
            // Check if tweaked key matches output key
            if (std::equal(output_key_bytes.begin(), output_key_bytes.end(),
                          tweaked->first.begin())) {
                return true;
            }
        }
    }

    // Check ALL owner keys - necessary for TRANSFER change outputs where the key
    // is from the original MINT but we're checking with the TRANSFER's txid
    for (const auto& [key_txid, key] : dd_owner_keys) {
        if (key_txid == txid) continue;  // Already checked above
        XOnlyPubKey owner_xonly(key.GetPubKey());
        auto tweaked = owner_xonly.CreateTapTweak(nullptr);
        if (tweaked) {
            if (std::equal(output_key_bytes.begin(), output_key_bytes.end(),
                          tweaked->first.begin())) {
                return true;
            }
        }
    }

    // T4-03a: Also check encrypted owner keys. We can check pubkey-derived
    // tweaked keys without decrypting the secret, which lets locked encrypted
    // wallets recognize their own DD outputs during rescan without exposing keys.
    for (const auto& [key_txid, crypted_pair] : dd_crypted_owner_keys) {
        const CPubKey& pubkey = crypted_pair.first;
        XOnlyPubKey owner_xonly(pubkey);
        auto tweaked = owner_xonly.CreateTapTweak(nullptr);
        if (tweaked) {
            if (std::equal(output_key_bytes.begin(), output_key_bytes.end(),
                          tweaked->first.begin())) {
                return true;
            }
        }
    }

    // Check dd_address_keys (for DD addresses generated via getdigidollaraddress)
    XOnlyPubKey output_key(output_key_bytes);
    CKey address_key;
    if (GetAddressKey(output_key, address_key)) {
        return true;
    }

    if (dd_foreign_output_keys.count(output_key_array) > 0) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine - cached foreign output_key=%s\n",
                 HexStr(output_key_bytes));
        return false;
    }

    // WALLET RESTORE FIX: After descriptor import, dd_address_keys may be
    // only partially rebuilt. DD addresses are created by taking a wallet key
    // and applying TapTweak(nullptr). The wallet has the base keys from
    // descriptors, but not necessarily every DD-tweaked output cached yet.
    // Try to find a wallet key that, when DD-tweaked, matches this output key.
    if (m_wallet) {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine: trying descriptor key derivation for output_key=%s\n",
                 HexStr(output_key_bytes));

        LOCK(m_wallet->cs_wallet);

        int spk_man_count = 0;
        int p2tr_script_count = 0;
        int provider_count = 0;
        int spenddata_count = 0;
        int key_count = 0;
        bool found_target_in_scripts = false;

        // Enumerate ALL P2TR scripts from all descriptor managers
        // This includes keys that were reserved via GetNewDestination but not used in transactions
        for (auto* spk_man : m_wallet->GetAllScriptPubKeyMans()) {
            auto* desc_spk = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(spk_man);
            if (!desc_spk) continue;
            spk_man_count++;

            // Get all scripts this descriptor knows about
            auto scripts = desc_spk->GetScriptPubKeys();
            for (const auto& script : scripts) {
                // Skip non-P2TR scripts
                if (script.size() != 34 || script[0] != OP_1) {
                    continue;
                }
                p2tr_script_count++;

                // Extract the output key from this script (bytes 2-33)
                std::vector<unsigned char> script_output_key(script.begin() + 2, script.end());

                // Check if THIS script has our target output_key (direct match - no tweak needed)
                if (std::equal(output_key_bytes.begin(), output_key_bytes.end(), script_output_key.begin())) {
                    found_target_in_scripts = true;
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine - output_key found directly in descriptor script\n");

                    // Get signing provider with keys for this script
                    auto provider = desc_spk->GetSigningProviderWithKeys(script);
                    if (provider) {
                        CTxDestination dest;
                        if (ExtractDestination(script, dest)) {
                            auto* taproot_dest = std::get_if<WitnessV1Taproot>(&dest);
                            if (taproot_dest) {
                                TaprootSpendData spenddata;
                                if (provider->GetTaprootSpendData(XOnlyPubKey(*taproot_dest), spenddata)) {
                                    CKey internal_key;
                                    if (provider->GetKeyByXOnly(spenddata.internal_key, internal_key)) {
                                        // Store the internal key for this DD output
                                        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine - direct descriptor match, storing internal key\n");
                                        const_cast<DigiDollarWallet*>(this)->StoreAddressKey(output_key, internal_key);
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine - descriptor script matched but key extraction failed\n");
                }

                // Get signing provider with keys for this script
                auto provider = desc_spk->GetSigningProviderWithKeys(script);
                if (!provider) continue;
                provider_count++;

                // Extract destination and get Taproot spend data
                CTxDestination dest;
                if (!ExtractDestination(script, dest)) {
                    continue;
                }

                auto* taproot_dest = std::get_if<WitnessV1Taproot>(&dest);
                if (!taproot_dest) {
                    continue;
                }

                TaprootSpendData spenddata;
                if (!provider->GetTaprootSpendData(XOnlyPubKey(*taproot_dest), spenddata)) {
                    continue;
                }
                spenddata_count++;

                if (!spenddata.internal_key.IsFullyValid()) {
                    continue;
                }

                CKey test_key;
                if (!provider->GetKeyByXOnly(spenddata.internal_key, test_key)) {
                    continue;
                }
                key_count++;

                // Apply DD tweak (nullptr merkle root) and check if it matches
                XOnlyPubKey test_xonly(test_key.GetPubKey());
                auto tweaked = test_xonly.CreateTapTweak(nullptr);

                // Debug: Log first few computed DD output keys
                if (key_count <= 3) {
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine - key %d: internal=%s, pubkey=%s, dd_tweaked=%s\n",
                             key_count,
                             HexStr(Span<const unsigned char>(spenddata.internal_key.begin(), spenddata.internal_key.end())),
                             HexStr(Span<const unsigned char>(test_xonly.begin(), test_xonly.end())),
                             tweaked ? HexStr(Span<const unsigned char>(tweaked->first.begin(), tweaked->first.end())) : "FAILED");
                }

                if (tweaked && std::equal(output_key_bytes.begin(), output_key_bytes.end(),
                                         tweaked->first.begin())) {
                    // Found a match! Cache it in dd_address_keys for future lookups
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine - found key via descriptor scan, caching DD address key\n");
                    const_cast<DigiDollarWallet*>(this)->StoreAddressKey(output_key, test_key);
                    return true;
                }
            }
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: IsDDOutputMine - no match found. Stats: spk_mans=%d, p2tr_scripts=%d, providers=%d, spenddata=%d, keys=%d, target_in_scripts=%d\n",
                 spk_man_count, p2tr_script_count, provider_count, spenddata_count, key_count, found_target_in_scripts);
        dd_foreign_output_keys.insert(output_key_array);
    }

    return false;
}

bool DigiDollarWallet::IsDDOutputMine(const COutPoint& outpoint) const
{
    auto locks = LockDDWallet();
    // PRIMARY CHECK: If it's in dd_utxos, we own it
    // This is the source of truth for DD ownership (like mapWallet for DGB)
    // CRITICAL for detecting TRANSFER change outputs after wallet restore,
    // where dd_owner_keys is empty and IsDDOutputMine(txout, txid) would fail.
    if (dd_utxos.find(outpoint) != dd_utxos.end()) {
        LogPrint(BCLog::DIGIDOLLAR, "IsDDOutputMine(COutPoint): %s:%u found in dd_utxos - returning true\n",
                 outpoint.hash.GetHex(), outpoint.n);
        return true;
    }

    // SECONDARY CHECK: Fall back to txout-based check for new outputs
    // This handles outputs we haven't yet added to dd_utxos
    if (m_wallet) {
        LOCK(m_wallet->cs_wallet);
        auto it = m_wallet->mapWallet.find(outpoint.hash);
        if (it != m_wallet->mapWallet.end() && outpoint.n < it->second.tx->vout.size()) {
            return IsDDOutputMine(it->second.tx->vout[outpoint.n], outpoint.hash);
        }
    }

    return false;
}

bool DigiDollarWallet::IsMyDDAddress(const std::string& addrStr) const
{
    auto locks = LockDDWallet();
    // Check dd_balances (addresses that have received DD)
    if (dd_balances.count(addrStr) > 0) return true;
    // Check dd_address_keys via output_key from DD address
    CDigiDollarAddress dd_addr(addrStr);
    if (dd_addr.IsValid()) {
        CTxDestination dest = dd_addr.GetDigiDollarDestination();
        if (auto* tr = std::get_if<WitnessV1Taproot>(&dest)) {
            std::array<unsigned char, 32> key_bytes;
            std::copy(tr->begin(), tr->end(), key_bytes.begin());
            if (dd_address_keys.count(key_bytes) > 0) return true;
        }
        // Fallback: standard wallet IsMine
        if (m_wallet) {
            LOCK(m_wallet->cs_wallet);
            if (m_wallet->IsMine(dest) & wallet::ISMINE_SPENDABLE) return true;
        }
    }
    return false;
}

std::vector<std::string> DigiDollarWallet::GetKnownDDAddresses() const
{
    LOCK(cs_dd_wallet);
    std::set<std::string> addresses;

    for (const auto& [addr, balance] : dd_balances) {
        if (addr == "total" || addr.rfind("test_addr_", 0) == 0) continue;
        CDigiDollarAddress dd_addr(addr);
        if (dd_addr.IsValid()) {
            addresses.insert(addr);
        }
    }

    auto add_output_key = [&addresses](const std::array<unsigned char, 32>& key_bytes) {
        XOnlyPubKey output_key(Span<const unsigned char>(key_bytes.data(), key_bytes.size()));
        if (!output_key.IsFullyValid()) return;
        const std::string addr = EncodeDigiDollarAddress(WitnessV1Taproot(output_key));
        if (!addr.empty()) {
            addresses.insert(addr);
        }
    };

    for (const auto& [key_bytes, key] : dd_address_keys) {
        add_output_key(key_bytes);
    }
    for (const auto& [key_bytes, key] : dd_crypted_address_keys) {
        add_output_key(key_bytes);
    }

    return {addresses.begin(), addresses.end()};
}

void DigiDollarWallet::StoreOwnerKey(const uint256& dd_timelock_id, const CKey& key)
{
    LOCK(cs_dd_wallet);
    dd_foreign_output_keys.clear();

    LogPrintf("DigiDollarWallet: Storing DD owner key for timelock %s\n",
              dd_timelock_id.ToString());

    // If wallet is encrypted, encrypt the key before storage (T4-03a)
    if (m_wallet && m_wallet->IsCrypted()) {
        CPubKey pubkey = key.GetPubKey();
        wallet::CKeyingMaterial vchSecret(key.begin(), key.end());
        std::vector<unsigned char> vchCryptedSecret;

        if (!wallet::EncryptSecret(m_wallet->GetEncryptionKey(), vchSecret, pubkey.GetHash(), vchCryptedSecret)) {
            LogPrintf("DigiDollarWallet: ERROR - Failed to encrypt DD owner key\n");
            return;
        }

        // Store encrypted in memory
        dd_crypted_owner_keys[dd_timelock_id] = std::make_pair(pubkey, vchCryptedSecret);
        // Remove any plaintext version from memory
        dd_owner_keys.erase(dd_timelock_id);

        // Persist encrypted to database
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteCryptedDDOwnerKey(dd_timelock_id, pubkey, vchCryptedSecret)) {
                LogPrintf("DigiDollarWallet: WARNING - Failed to persist encrypted DD owner key to database\n");
            } else {
                LogPrintf("DigiDollarWallet: Persisted encrypted DD owner key to database\n");
            }
        }
    } else {
        // Store plaintext in memory
        dd_owner_keys[dd_timelock_id] = key;

        // Persist plaintext to wallet database
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteDDOwnerKey(dd_timelock_id, key)) {
                LogPrintf("DigiDollarWallet: WARNING - Failed to persist DD owner key to database\n");
            } else {
                LogPrintf("DigiDollarWallet: Persisted DD owner key to database\n");
            }
        }
    }
}

size_t DigiDollarWallet::LoadDDOwnerKeys()
{
    LOCK(cs_dd_wallet);
    if (!m_wallet) {
        LogPrint(BCLog::WALLETDB, "DigiDollarWallet::LoadDDOwnerKeys - No wallet pointer\n");
        return 0;
    }

    wallet::WalletBatch batch(m_wallet->GetDatabase());
    size_t count = 0;

    // Clear in-memory maps before loading
    dd_owner_keys.clear();
    dd_crypted_owner_keys.clear();
    dd_foreign_output_keys.clear();

    // Iterate through database to find DD owner keys (both plaintext and encrypted)
    std::unique_ptr<wallet::DatabaseCursor> cursor = batch.GetNewCursor();
    if (cursor) {
        wallet::DatabaseCursor::Status status = wallet::DatabaseCursor::Status::MORE;
        while (status == wallet::DatabaseCursor::Status::MORE) {
            DataStream key_stream{};
            DataStream value_stream{};
            status = cursor->Next(key_stream, value_stream);

            if (status != wallet::DatabaseCursor::Status::MORE) break;

            std::string key_type;
            key_stream >> key_type;

            if (key_type == wallet::DBKeys::DD_OWNER_KEY) {
                // Plaintext DD owner key (unencrypted wallet)
                uint256 dd_timelock_id;
                key_stream >> dd_timelock_id;

                CPrivKey privkey;
                value_stream >> privkey;

                std::array<unsigned char, CPubKey::COMPRESSED_SIZE> compressed_dummy{};
                compressed_dummy[0] = 0x02;
                CPubKey dummy_pubkey(compressed_dummy.begin(), compressed_dummy.end());

                CKey key;
                if (key.Load(privkey, dummy_pubkey, /*fSkipCheck=*/true)) {
                    dd_owner_keys[dd_timelock_id] = key;
                    count++;

                    LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded plaintext DD owner key for timelock %s\n",
                            dd_timelock_id.ToString());
                } else {
                    LogPrintf("DigiDollarWallet: WARNING - Failed to load DD owner key from database\n");
                }
            } else if (key_type == wallet::DBKeys::DD_CRYPTED_OWNER_KEY) {
                // Encrypted DD owner key (T4-03a: encrypted wallet)
                uint256 dd_timelock_id;
                key_stream >> dd_timelock_id;

                std::pair<CPubKey, std::vector<unsigned char>> val;
                value_stream >> val;

                dd_crypted_owner_keys[dd_timelock_id] = val;
                count++;

                LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Loaded encrypted DD owner key for timelock %s\n",
                        dd_timelock_id.ToString());
            }
        }
    }

    LogPrintf("DigiDollarWallet: Loaded %zu DD owner keys (%zu plaintext, %zu encrypted) from database\n",
              count, dd_owner_keys.size(), dd_crypted_owner_keys.size());
    return count;
}

// =============================================================================
// Thread-safe accessor implementations (moved from inline in header for locking)
// =============================================================================

bool DigiDollarWallet::GetOwnerKey(const uint256& dd_timelock_id, CKey& key) const
{
    LOCK(cs_dd_wallet);

    // First try plaintext keys (unencrypted wallet)
    auto it = dd_owner_keys.find(dd_timelock_id);
    if (it != dd_owner_keys.end()) {
        key = it->second;
        return true;
    }

    // Try encrypted keys (T4-03a: encrypted wallet)
    auto cit = dd_crypted_owner_keys.find(dd_timelock_id);
    if (cit != dd_crypted_owner_keys.end()) {
        if (!m_wallet) return false;
        if (m_wallet->IsLocked()) {
            LogPrintf("DigiDollarWallet: Cannot decrypt DD owner key — wallet is locked\n");
            return false;
        }
        const CPubKey& pubkey = cit->second.first;
        const std::vector<unsigned char>& vchCryptedSecret = cit->second.second;
        wallet::CKeyingMaterial vchSecret;
        if (!wallet::DecryptSecret(m_wallet->GetEncryptionKey(), vchCryptedSecret, pubkey.GetHash(), vchSecret)) {
            LogPrintf("DigiDollarWallet: ERROR - Failed to decrypt DD owner key for timelock %s\n",
                      dd_timelock_id.ToString());
            return false;
        }
        if (vchSecret.size() != 32) return false;
        key.Set(vchSecret.begin(), vchSecret.end(), pubkey.IsCompressed());
        return key.VerifyPubKey(pubkey);
    }

    return false;
}

bool DigiDollarWallet::GetAddressKey(const XOnlyPubKey& output_key, CKey& key) const
{
    LOCK(cs_dd_wallet);
    std::array<unsigned char, 32> key_bytes;
    std::copy(output_key.begin(), output_key.end(), key_bytes.begin());

    // First try plaintext keys (unencrypted wallet)
    auto it = dd_address_keys.find(key_bytes);
    if (it != dd_address_keys.end()) {
        key = it->second;
        return true;
    }

    // Try encrypted keys (T4-03a: encrypted wallet)
    auto cit = dd_crypted_address_keys.find(key_bytes);
    if (cit != dd_crypted_address_keys.end()) {
        if (!m_wallet) return false;
        if (m_wallet->IsLocked()) {
            LogPrintf("DigiDollarWallet: Cannot decrypt DD address key — wallet is locked\n");
            return false;
        }
        const CPubKey& pubkey = cit->second.first;
        const std::vector<unsigned char>& vchCryptedSecret = cit->second.second;
        wallet::CKeyingMaterial vchSecret;
        if (!wallet::DecryptSecret(m_wallet->GetEncryptionKey(), vchCryptedSecret, pubkey.GetHash(), vchSecret)) {
            LogPrintf("DigiDollarWallet: ERROR - Failed to decrypt DD address key\n");
            return false;
        }
        if (vchSecret.size() != 32) return false;
        key.Set(vchSecret.begin(), vchSecret.end(), pubkey.IsCompressed());
        return key.VerifyPubKey(pubkey);
    }

    return false;
}

bool DigiDollarWallet::GetDDOutputSpendingKey(const CTxOut& txout, CKey& key)
{
    auto locks = LockDDWallet();
    if (txout.nValue != 0 || txout.scriptPubKey.size() != 34 || txout.scriptPubKey[0] != OP_1) {
        return false;
    }

    std::vector<unsigned char> output_key_bytes(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end());
    XOnlyPubKey output_key(output_key_bytes);

    if (GetAddressKey(output_key, key)) {
        return true;
    }

    if (!m_wallet) {
        return false;
    }

    auto cache_key = [this, &output_key, &key]() {
        StoreAddressKey(output_key, key);
        return true;
    };

    auto try_provider = [&](const SigningProvider& provider, const XOnlyPubKey& script_output_key) {
        TaprootSpendData spenddata;
        if (provider.GetTaprootSpendData(script_output_key, spenddata) && spenddata.internal_key.IsFullyValid()) {
            CKey candidate_key;
            if (provider.GetKeyByXOnly(spenddata.internal_key, candidate_key)) {
                XOnlyPubKey candidate_xonly(candidate_key.GetPubKey());
                auto tweaked = candidate_xonly.CreateTapTweak(nullptr);
                if (tweaked && tweaked->first == output_key) {
                    key = candidate_key;
                    return cache_key();
                }
            }
        }

        CKey candidate_key;
        if (provider.GetKeyByXOnly(output_key, candidate_key)) {
            key = candidate_key;
            return cache_key();
        }

        return false;
    };

    for (wallet::ScriptPubKeyMan* spk_man : m_wallet->GetScriptPubKeyMans(txout.scriptPubKey)) {
        if (auto* desc_spk = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(spk_man)) {
            auto provider = desc_spk->GetSigningProviderWithKeys(txout.scriptPubKey);
            if (provider && try_provider(*provider, output_key)) {
                LogPrintf("DigiDollar: GetDDOutputSpendingKey - recovered key from exact descriptor script\n");
                return true;
            }
        } else {
            auto provider = m_wallet->GetSolvingProvider(txout.scriptPubKey);
            if (provider && try_provider(*provider, output_key)) {
                LogPrintf("DigiDollar: GetDDOutputSpendingKey - recovered key from wallet provider\n");
                return true;
            }
        }
    }

    for (auto* spk_man : m_wallet->GetAllScriptPubKeyMans()) {
        auto* desc_spk = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(spk_man);
        if (!desc_spk) continue;

        for (const auto& script : desc_spk->GetScriptPubKeys()) {
            if (script.size() != 34 || script[0] != OP_1) {
                continue;
            }

            CTxDestination dest;
            if (!ExtractDestination(script, dest)) {
                continue;
            }

            const auto* taproot_dest = std::get_if<WitnessV1Taproot>(&dest);
            if (!taproot_dest) {
                continue;
            }

            auto provider = desc_spk->GetSigningProviderWithKeys(script);
            if (provider && try_provider(*provider, XOnlyPubKey(*taproot_dest))) {
                LogPrintf("DigiDollar: GetDDOutputSpendingKey - recovered key by descriptor scan\n");
                return true;
            }
        }
    }

    return false;
}

void DigiDollarWallet::AddDDUTXO(const COutPoint& outpoint, CAmount dd_amount)
{
    LOCK(cs_dd_wallet);
    dd_utxos[outpoint] = dd_amount;
}

void DigiDollarWallet::RemoveDDUTXO(const COutPoint& outpoint)
{
    LOCK(cs_dd_wallet);
    dd_utxos.erase(outpoint);
}

bool DigiDollarWallet::HasDDUTXO(const COutPoint& outpoint) const
{
    LOCK(cs_dd_wallet);
    return dd_utxos.find(outpoint) != dd_utxos.end();
}

void DigiDollarWallet::SetMockBalance(CAmount balance)
{
    LOCK(cs_dd_wallet);
    mockBalance = balance;
}

void DigiDollarWallet::AddMockTransaction(const DDTransaction& tx)
{
    LOCK(cs_dd_wallet);
    mockHistory.push_back(tx);
}

void DigiDollarWallet::AddMockUTXO(const CDigiDollarOutput& utxo)
{
    LOCK(cs_dd_wallet);
    mockUTXOs.push_back(utxo);
}

void DigiDollarWallet::ClearMockData()
{
    LOCK(cs_dd_wallet);
    mockBalance = 0;
    mockHistory.clear();
    mockUTXOs.clear();
}

size_t DigiDollarWallet::GetBalanceCount() const
{
    LOCK(cs_dd_wallet);
    return dd_balances.size();
}

size_t DigiDollarWallet::GetPositionCount() const
{
    LOCK(cs_dd_wallet);
    return collateral_positions.size();
}

size_t DigiDollarWallet::GetCachedForeignDDOutputCount() const
{
    LOCK(cs_dd_wallet);
    return dd_foreign_output_keys.size();
}

void DigiDollarWallet::ClearDDOwnershipCache()
{
    LOCK(cs_dd_wallet);
    dd_foreign_output_keys.clear();
}

bool DigiDollarWallet::IsLockedByDD(const COutPoint& outpoint) const
{
    auto locks = LockDDWallet();
    if (dd_utxos.count(outpoint)) return true;
    for (const auto& [id, pos] : collateral_positions) {
        if (!pos.is_active) continue;

        COutPoint collateral_outpoint(id, 0);
        if (m_wallet) {
            auto tx_it = m_wallet->mapWallet.find(id);
            if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                MintOutputIndexes mint_outputs;
                if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                    collateral_outpoint = COutPoint(id, mint_outputs.collateral_index);
                }
            }
        }

        if (collateral_outpoint == outpoint) return true;
    }
    return false;
}


bool DigiDollarWallet::TransferDigiDollarMany(const std::vector<std::pair<CDigiDollarAddress, CAmount>>& recipients,
                                             std::string& txid, std::string& error,
                                             CAmount* dd_change_out,
                                             const std::vector<COutPoint>* preset_dd_inputs,
                                             const std::string& comment) {
    auto locks = LockDDWallet();
    // Clear previous results
    txid.clear();
    error.clear();
    if (dd_change_out) *dd_change_out = 0;

    try {
        LogPrintf("DigiDollar: Starting multi-recipient transfer - %zu recipients\n", recipients.size());
        if (!m_wallet || m_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            error = "DigiDollar send requires a wallet with private keys enabled";
            LogPrintf("DigiDollar: Transfer blocked because wallet cannot sign DD spends\n");
            return false;
        }

        DDTransferPlan plan;
        if (!PlanDigiDollarTransfer(recipients, plan, error, preset_dd_inputs)) {
            LogPrintf("DigiDollar: Transfer planning failed - %s\n", error);
            return false;
        }
        const CAmount totalAmount = plan.total_amount;
        const CAmount selectedDDTotal = plan.selected_dd_total;

        // Check balance using new balance tracking
        CAmount currentBalance = GetTotalDDBalance();
        if (currentBalance == 0) {
            // Fallback to legacy mock balance for testing
            currentBalance = mockBalance;
        }

        if (totalAmount > currentBalance) {
            error = strprintf("Insufficient DD balance. Available: %lld cents, Required: %lld cents",
                            static_cast<long long>(currentBalance), static_cast<long long>(totalAmount));
            LogPrintf("DigiDollar: Insufficient balance - available: %lld, required: %lld\n",
                     static_cast<long long>(currentBalance), static_cast<long long>(totalAmount));
            return false;
        }

        DigiDollar::TxBuilderTransferParams params;
        params.recipients = plan.recipients;
        // DigiDollar transactions MUST pay at least 0.1 DGB fee to miners
        params.feeRate = DigiDollar::MIN_DD_FEE_RATE; // 0.35 DGB/kB = 0.105 DGB for 300 byte tx
        params.ddUtxos = plan.dd_utxos;
        params.ddAmounts = plan.dd_amounts;
        LogPrintf("DigiDollar: Transfer - Selected %zu DD UTXOs totaling %lld cents\n",
                  plan.dd_amounts.size(), static_cast<long long>(selectedDDTotal));

        size_t projected_vsize = plan.projected_vsize;
        CAmount estimatedFee = plan.estimated_fee;
        LogPrintf("DigiDollar: Preflight projected transfer size: %u vB, fee: %lld sats (%.8f DGB)\n",
                  static_cast<unsigned>(projected_vsize), static_cast<long long>(estimatedFee), estimatedFee / 100000000.0);

        // Select DGB UTXOs for fees. Re-run preflight after selection because the
        // number of fee inputs affects vsize and therefore the required fee.
        std::vector<COutPoint> exclude_dd_utxos = params.ddUtxos;
        std::vector<CAmount> fee_amounts;
        std::string preflight_error;
        CAmount selectedFeeTotal = 0;
        for (int attempt = 0; attempt < 3; ++attempt) {
            params.feeUtxos.clear();
            fee_amounts.clear();
            selectedFeeTotal = 0;
            if (!SelectFeeCoins(estimatedFee, params.feeUtxos, selectedFeeTotal, &fee_amounts, &exclude_dd_utxos)) {
                error = strprintf("Insufficient DGB balance for transaction fees (need at least %lld sats based on projected %u vB transaction)",
                                  static_cast<long long>(estimatedFee), static_cast<unsigned>(projected_vsize));
                LogPrintf("DigiDollar: Transfer failed - no DGB UTXOs available for projected fee\n");
                return false;
            }
            CAmount refinedFee = 0;
            if (!PreflightDDTransferCapacity(params, selectedDDTotal, totalAmount, preflight_error, &projected_vsize, &refinedFee)) {
                error = preflight_error;
                LogPrintf("DigiDollar: Transfer capacity preflight failed after fee selection - %s\n", error);
                return false;
            }
            if (selectedFeeTotal >= refinedFee) {
                estimatedFee = refinedFee;
                break;
            }
            estimatedFee = refinedFee;
        }
        if (selectedFeeTotal < estimatedFee) {
            error = strprintf("Insufficient DGB fee inputs after projected-size fee calculation (selected=%lld sats, required=%lld sats). Consolidate DGB UTXOs or add funds.",
                              static_cast<long long>(selectedFeeTotal), static_cast<long long>(estimatedFee));
            return false;
        }
        params.feeAmounts = fee_amounts;  // Pass actual fee UTXO amounts

        // Get spending key - handles both minted DD (in dd_owner_keys) and received DD (in wallet)
        // For minted DD: key is stored in dd_owner_keys map when we created the position
        // For received DD: key is in wallet's standard P2TR key management
        CKey spenderKey;
        bool found_key = false;

        if (!params.ddUtxos.empty()) {
            // First, try dd_owner_keys map (for minted DD)
            if (dd_owner_keys.count(params.ddUtxos[0].hash) > 0) {
                spenderKey = dd_owner_keys[params.ddUtxos[0].hash];
                found_key = true;
                LogPrintf("DigiDollar: Using stored owner key for DD UTXO %s\n", params.ddUtxos[0].hash.ToString());
            }

            // If not found (received DD), try to get key from wallet's P2TR key management
            if (!found_key && m_wallet) {
                LogPrintf("DigiDollar: DD UTXO not in dd_owner_keys, trying wallet key lookup for received DD\n");

                // Helper to get signing provider with private key access for descriptor wallets
                auto getSigningProviderWithKeys = [this](const CScript& script) -> std::unique_ptr<SigningProvider> {
                    const auto& spk_mans = m_wallet->GetScriptPubKeyMans(script);
                    if (!spk_mans.empty()) {
                        wallet::ScriptPubKeyMan* spk_man = *spk_mans.begin();
                        wallet::DescriptorScriptPubKeyMan* desc_spk_man = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(spk_man);
                        if (desc_spk_man) {
                            LogPrintf("DigiDollar: Using DescriptorScriptPubKeyMan with private keys\n");
                            return desc_spk_man->GetSigningProviderWithKeys(script);
                        }
                    }
                    // Fallback for legacy wallets
                    return m_wallet->GetSolvingProvider(script);
                };

                // Get the scriptPubKey for the DD UTXO we're spending
                const COutPoint& dd_outpoint = params.ddUtxos[0];
                LogPrintf("DigiDollar: Looking up tx %s in mapWallet (size=%d)\n",
                         dd_outpoint.hash.ToString(), m_wallet->mapWallet.size());

                // Look up the transaction to get the scriptPubKey
                auto wtx_it = m_wallet->mapWallet.find(dd_outpoint.hash);
                if (wtx_it != m_wallet->mapWallet.end()) {
                    LogPrintf("DigiDollar: Found tx in mapWallet, checking output %d (tx has %d outputs)\n",
                             dd_outpoint.n, wtx_it->second.tx->vout.size());
                    const auto& wtx = wtx_it->second;
                    if (dd_outpoint.n < wtx.tx->vout.size()) {
                        const CTxOut& txout = wtx.tx->vout[dd_outpoint.n];
                        LogPrintf("DigiDollar: Got output, scriptPubKey size=%d\n", txout.scriptPubKey.size());

                        // Get signing provider for this script WITH PRIVATE KEY ACCESS
                        auto provider = getSigningProviderWithKeys(txout.scriptPubKey);
                        if (provider) {
                            LogPrintf("DigiDollar: Got signing provider\n");
                            // Extract the P2TR destination
                            CTxDestination dest;
                            if (ExtractDestination(txout.scriptPubKey, dest)) {
                                LogPrintf("DigiDollar: Extracted destination, checking if P2TR\n");
                                if (auto* tr = std::get_if<WitnessV1Taproot>(&dest)) {
                                    LogPrintf("DigiDollar: Got P2TR destination, output key=%s\n",
                                             HexStr(Span<const unsigned char>(tr->begin(), tr->end())));
                                    // Get TaprootSpendData to find the internal key
                                    TaprootSpendData spenddata;
                                    if (provider->GetTaprootSpendData(XOnlyPubKey(*tr), spenddata)) {
                                        LogPrintf("DigiDollar: Got TaprootSpendData, internal_key valid=%d\n",
                                                 spenddata.internal_key.IsFullyValid());
                                        if (spenddata.internal_key.IsFullyValid()) {
                                            LogPrintf("DigiDollar: internal_key=%s\n",
                                                     HexStr(Span<const unsigned char>(spenddata.internal_key.begin(), spenddata.internal_key.end())));
                                            // Try to get the private key for the internal key
                                            if (provider->GetKeyByXOnly(spenddata.internal_key, spenderKey)) {
                                                found_key = true;
                                                LogPrintf("DigiDollar: Found key for received DD via GetKeyByXOnly\n");
                                            } else {
                                                LogPrintf("DigiDollar: GetKeyByXOnly failed for internal key\n");
                                            }
                                        }
                                    } else {
                                        LogPrintf("DigiDollar: GetTaprootSpendData returned false\n");
                                    }

                                    // Try dd_address_keys map (for addresses generated via getdigidollaraddress)
                                    if (!found_key) {
                                        CKey address_key;
                                        if (GetAddressKey(XOnlyPubKey(*tr), address_key)) {
                                            spenderKey = address_key;
                                            found_key = true;
                                            LogPrintf("DigiDollar: Found key for received DD via dd_address_keys map\n");
                                        } else {
                                            LogPrintf("DigiDollar: dd_address_keys lookup failed for output key\n");
                                        }
                                    }

                                    // If still not found, try brute force scan through wallet keys
                                    if (!found_key) {
                                        LogPrintf("DigiDollar: Starting brute force P2TR wallet scan\n");
                                        XOnlyPubKey target_output_key(*tr);
                                        int scanned_txs = 0;
                                        int scanned_outputs = 0;
                                        for (const auto& [txid, scan_wtx] : m_wallet->mapWallet) {
                                            scanned_txs++;
                                            for (size_t n = 0; n < scan_wtx.tx->vout.size() && !found_key; n++) {
                                                scanned_outputs++;
                                                CTxDestination out_dest;
                                                if (ExtractDestination(scan_wtx.tx->vout[n].scriptPubKey, out_dest)) {
                                                    auto out_provider = getSigningProviderWithKeys(scan_wtx.tx->vout[n].scriptPubKey);
                                                    if (out_provider) {
                                                        if (auto* scan_tr = std::get_if<WitnessV1Taproot>(&out_dest)) {
                                                            TaprootSpendData scan_spenddata;
                                                            if (out_provider->GetTaprootSpendData(XOnlyPubKey(*scan_tr), scan_spenddata)) {
                                                                if (scan_spenddata.internal_key.IsFullyValid()) {
                                                                    CKey test_key;
                                                                    if (out_provider->GetKeyByXOnly(scan_spenddata.internal_key, test_key)) {
                                                                        XOnlyPubKey test_xonly(test_key.GetPubKey());
                                                                        auto tweaked = test_xonly.CreateTapTweak(nullptr);
                                                                        if (tweaked && std::equal(target_output_key.begin(), target_output_key.end(),
                                                                                                 tweaked->first.begin())) {
                                                                            spenderKey = test_key;
                                                                            found_key = true;
                                                                            LogPrintf("DigiDollar: Found key for received DD via P2TR wallet scan\n");
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            if (found_key) break;
                                        }
                                        LogPrintf("DigiDollar: Wallet scan complete: %d txs, %d outputs scanned, found_key=%d\n",
                                                 scanned_txs, scanned_outputs, found_key);
                                    }
                                } else {
                                    LogPrintf("DigiDollar: Destination is not P2TR\n");
                                }
                            } else {
                                LogPrintf("DigiDollar: ExtractDestination failed\n");
                            }
                        } else {
                            LogPrintf("DigiDollar: GetSolvingProvider returned null\n");
                        }
                    } else {
                        LogPrintf("DigiDollar: Output index %d out of range (tx has %d outputs)\n",
                                 dd_outpoint.n, wtx.tx->vout.size());
                    }
                } else {
                    LogPrintf("DigiDollar: TX %s not found in mapWallet\n", dd_outpoint.hash.ToString());
                }
            }
        }

        if (!found_key) {
            error = "Could not find spending key for DD UTXO";
            LogPrintf("DigiDollar: Could not find spending key for DD UTXO %s:%d\n",
                     params.ddUtxos.empty() ? "empty" : params.ddUtxos[0].hash.ToString().c_str(),
                     params.ddUtxos.empty() ? 0 : params.ddUtxos[0].n);
            return false;
        }

        params.spenderKey = spenderKey;

        // CRITICAL FIX: Get a proper change address from the wallet for DGB change output
        // This ensures the wallet recognizes the change output as its own!
        // Without this, DGB would be sent to an address the wallet doesn't control.
        if (m_wallet) {
            // Get a new change address from wallet's internal keypool
            auto op_dest = m_wallet->GetNewChangeDestination(OutputType::BECH32);
            if (op_dest) {
                params.dgbChangeDest = *op_dest;
                LogPrintf("DigiDollar: Using wallet change address for DGB change output\n");
            } else {
                // Fallback: try to get any fresh address
                LogPrintf("DigiDollar: WARNING - Could not get change destination, trying fresh address\n");
                auto fresh_dest = m_wallet->GetNewDestination(OutputType::BECH32, std::string("DD_change"));
                if (fresh_dest) {
                    params.dgbChangeDest = *fresh_dest;
                    LogPrintf("DigiDollar: Using fresh wallet address for DGB change output\n");
                } else {
                    LogPrintf("DigiDollar: WARNING - No wallet change address available, DGB change may be lost!\n");
                }
            }
        }

        // BUG #5 FIX: Get real height instead of hardcoded values.
        int currentHeight = 0;
        if (m_wallet) {
            currentHeight = m_wallet->GetLastBlockHeight();
        }
        if (currentHeight <= 0) currentHeight = 100000; // Safe fallback for tests

        DigiDollar::TransferTxBuilder builder(Params(), currentHeight, TRANSFER_BUILDER_PRICE_UNUSED);
        DigiDollar::TxBuilderResult result = builder.BuildTransferTransaction(params);

        if (!result.success) {
            error = "Failed to build transaction: " + result.error;
            LogPrintf("DigiDollar: Transaction build failed - %s\n", result.error);
            return false;
        }

        // Sign the transaction before broadcasting
        LogPrintf("DigiDollar: Signing transaction with %d DD inputs and %d fee inputs\n",
                  params.ddUtxos.size(), params.feeUtxos.size());

        if (!SignTransaction(result.tx, params.ddUtxos, params.feeUtxos)) {
            error = "Failed to sign transaction";
            LogPrintf("DigiDollar: Transaction signing failed\n");
            return false;
        }

        LogPrintf("DigiDollar: Transaction signed successfully\n");

        // Commit through the wallet-owned relay path exactly once. DD state is
        // updated only after CommitTransaction accepts the transaction into the
        // wallet/mempool flow.
        CTransactionRef tx_ref = MakeTransactionRef(result.tx);

        // Get transaction ID
        txid = tx_ref->GetHash().ToString();

        if (!m_wallet) {
            LogPrintf("DigiDollar: WARNING - No wallet context, transaction built but not broadcast\n");
            error = "No wallet context for broadcasting";
            return false;
        }

        const uint256 tx_hash = tx_ref->GetHash();
        pending_outgoing_dd_txs.insert(tx_hash);
        std::string commit_error;
        const bool commit_success = CommitDDTransaction(tx_ref, commit_error);
        pending_outgoing_dd_txs.erase(tx_hash);
        if (!commit_success) {
            error = strprintf("Transaction rejected by wallet/mempool: %s", commit_error);
            LogPrintf("DigiDollar: Commit failed - %s\n", commit_error);
            return false;
        }

        LogPrintf("DigiDollar: Transaction committed successfully - txid: %s\n", txid);

        // FIX #2: CRITICAL - Time-locks (collateral positions) NEVER change during transfers!
        // Only DD UTXOs move. The locked DGB stays in place until redemption.
        // DO NOT mark positions inactive. DO NOT create new collateral positions.
        LogPrintf("DigiDollar: Updating DD UTXO set after transfer (FIX #2)\n");

        // FIX: Do NOT erase spent DD UTXOs at TX creation time!
        // The core DGB wallet never deletes UTXO data at TX creation — it uses IsSpent()
        // which already returns true for pending spends and false for abandoned TXs.
        // GetTotalDDBalance() and GetDDUTXOs() already call IsSpent() to skip spent UTXOs.
        // Erasing here would permanently lose DD tracking data if the TX is abandoned.
        // The UTXOs will be properly erased when the TX confirms in a block
        // (via ProcessTransactionForDD called from blockConnected).
        for (const auto& spent_utxo : params.ddUtxos) {
            LogPrintf("DigiDollar: DD UTXO %s:%d pending spend (will be erased on block confirm)\n",
                      spent_utxo.hash.ToString(), spent_utxo.n);
        }

        // Batch for writing new DD UTXOs (change outputs)
        wallet::WalletBatch batch(m_wallet->GetDatabase());

        // Add new DD UTXOs from transaction outputs (for change and potentially recipient if to ourselves)
        // Extract DD amounts from OP_RETURN (same logic as DetectIncomingDDOutputs)
        std::vector<CAmount> dd_amounts;
        for (const auto& txout : result.tx.vout) {
            if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) {
                CScript::const_iterator pc = txout.scriptPubKey.begin();
                opcodetype opcode;
                std::vector<unsigned char> data;

                // Skip OP_RETURN
                if (!txout.scriptPubKey.GetOp(pc, opcode)) continue;

                // Check for "DD" marker
                if (!txout.scriptPubKey.GetOp(pc, opcode, data)) continue;
                if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;

                // Get transaction type
                if (!txout.scriptPubKey.GetOp(pc, opcode, data)) continue;
                CScriptNum txType(data, true);
                int type = txType.getint();
                if (type != 2 && type != 3) continue;  // TRANSFER (2) or REDEEM (3) transactions

                // Extract DD amounts
                while (txout.scriptPubKey.GetOp(pc, opcode, data)) {
                    if (data.size() > 0) {
                        CScriptNum amount_num(data, true, 8);  // 8-byte max for large DD amounts
                        dd_amounts.push_back(amount_num.GetInt64());
                    }
                }
                break;
            }
        }

        // Now match P2TR outputs with DD amounts
        // We know the change output belongs to us because we created it with our owner key
        size_t dd_output_index = 0;
        for (size_t i = 0; i < result.tx.vout.size(); i++) {
            const CTxOut& txout = result.tx.vout[i];

            // Skip OP_RETURN and non-zero value outputs
            if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) continue;
            if (txout.nValue != 0) continue;  // DD outputs have 0 DGB value

            // Check if it's a P2TR output (OP_1 + 32 bytes)
            if (txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1) {
                // This is a DD output
                if (dd_output_index < dd_amounts.size()) {
                    CAmount dd_amount = dd_amounts[dd_output_index];

                    // Recipient DD outputs are emitted first, followed by optional DD change.
                    // The commit callback is suppressed for this outgoing tx so it cannot
                    // mis-record our DD change as a receive before the send row exists.
                    // Track change here, and also track real self-recipient outputs.
                    bool is_ours = (dd_output_index >= recipients.size());
                    if (!is_ours && m_wallet) {
                        wallet::isminetype mine = m_wallet->IsMine(txout);
                        is_ours = (mine & wallet::ISMINE_SPENDABLE);
                    }
                    if (!is_ours && txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1) {
                        std::array<unsigned char, 32> output_key;
                        std::copy(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.begin() + 34, output_key.begin());
                        is_ours = dd_address_keys.count(output_key) > 0 ||
                                  dd_crypted_address_keys.count(output_key) > 0;
                    }

                    if (is_ours) {
                        COutPoint new_utxo(result.tx.GetHash(), i);
                        dd_utxos[new_utxo] = dd_amount;
                        DigiDollar::RegisterScriptMetadata(txout.scriptPubKey,
                                                           DigiDollar::ScriptType::DD_TOKEN_OUTPUT,
                                                           dd_amount,
                                                           0);

                        // Store the owner key for this new DD UTXO so we can spend it later
                        // Use StoreOwnerKey to ensure persistence to database
                        StoreOwnerKey(result.tx.GetHash(), spenderKey);

                        // Persist DD UTXO to database
                        if (!batch.WriteDDUTXO(new_utxo, dd_amount)) {
                            LogPrintf("DigiDollar: WARNING - Failed to write DD UTXO %s:%d to database\n",
                                     new_utxo.hash.ToString(), i);
                        }

                        LogPrintf("DigiDollar: Added change DD UTXO %s:%d (%lld cents)\n",
                                  new_utxo.hash.ToString(), i, static_cast<long long>(dd_amount));
                    }
                }
                dd_output_index++;
            }
        }

        // Time-lock positions remain ACTIVE and UNCHANGED
        LogPrintf("DigiDollar: Transfer complete - time-locks preserved (still ACTIVE)\n");

        // Calculate DD change for legacy mock balance update
        CAmount dd_change = selectedDDTotal - totalAmount;

        // Update legacy mock balance for backwards compatibility
        if (mockBalance > 0) {
            mockBalance -= totalAmount;
            if (dd_change > 0) {
                mockBalance += dd_change;
            }
        }

        // Add transaction to history
        DDTransaction tx;
        tx.txid = txid;
        tx.amount = totalAmount;
        tx.timestamp = GetTime();
        tx.confirmations = 0;
        tx.incoming = false;
        tx.address = recipients.size() == 1 ? recipients.front().first.ToString() : "multiple";
        tx.category = "send";
        tx.comment = comment;

        transaction_history.push_back(tx);

        // Persist transaction to database with FRESH batch (same pattern as receives)
        // Creating a new batch ensures proper flush when block ends
        {
            wallet::WalletBatch txBatch(m_wallet->GetDatabase());
            if (!txBatch.WriteDDTransaction(tx)) {
                LogPrintf("DigiDollar: Warning - failed to persist send transaction %s to database\n", txid);
            } else {
                LogPrintf("DigiDollar: Successfully persisted send transaction %s to database\n", txid);
            }
        }

        // Verify balance updated correctly
        CAmount newBalance = GetTotalDDBalance();
        LogPrintf("DigiDollar: Transfer successful - %lld cents to %zu recipients (txid: %s)\n",
                  static_cast<long long>(totalAmount), recipients.size(), txid);
        LogPrintf("DigiDollar: Balance updated: %lld -> %lld (change: %lld)\n",
                  static_cast<long long>(currentBalance), static_cast<long long>(newBalance), static_cast<long long>(dd_change));
        if (dd_change_out) *dd_change_out = dd_change;

        return true;

    } catch (const std::exception& e) {
        error = "Transfer failed: " + std::string(e.what());
        LogPrintf("DigiDollar: Transfer exception - %s\n", error);
        return false;
    }
}

bool DigiDollarWallet::TransferDigiDollar(const CDigiDollarAddress& to, CAmount amount,
                                          std::string& txid, std::string& error,
                                          CAmount* dd_change_out,
                                          const std::vector<COutPoint>* preset_dd_inputs,
                                          const std::string& comment) {
    return TransferDigiDollarMany({{to, amount}}, txid, error, dd_change_out, preset_dd_inputs, comment);
}

CAmount DigiDollarWallet::GetDDBalanceLegacy() const {
    // Legacy function - redirect to new implementation with empty address (total balance)
    return GetDDBalance(CDigiDollarAddress());
}

std::vector<DDTransaction> DigiDollarWallet::GetDDTransactionHistory() const {
    auto locks = LockDDWallet();
    // Return actual transaction history (with mock fallback for testing)
    std::vector<DDTransaction> history = transaction_history;

    // Synthesize per-output wallet history rows from wallet transactions. The
    // database stores one DDTransaction per txid, so multi-recipient receives
    // and redemption DD change need display rows built on demand.
    if (m_wallet) {
        LOCK(m_wallet->cs_wallet);
        std::set<std::string> txids_with_synthesized_rows;
        std::vector<DDTransaction> synthesized_rows;
        std::set<std::string> local_send_txids;
        for (const auto& hist_tx : transaction_history) {
            if (!hist_tx.incoming && hist_tx.category == "send") {
                local_send_txids.insert(hist_tx.txid);
            }
        }

        auto is_local_dd_output = [this](const CTxOut& txout, const uint256& txid) {
            if (!IsStandardDDTokenOutput(txout)) return false;

            std::array<unsigned char, 32> output_key;
            std::copy(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.begin() + 34, output_key.begin());
            const bool is_mine = m_wallet->IsMine(txout.scriptPubKey);
            const bool has_plain_key = dd_address_keys.find(output_key) != dd_address_keys.end();
            const bool has_crypted_key = dd_crypted_address_keys.find(output_key) != dd_crypted_address_keys.end();
            return is_mine || has_plain_key || has_crypted_key || IsDDOutputMine(txout, txid);
        };

        auto append_local_output_rows = [&](const DDTransaction& base_tx,
                                            const CTransaction& tx,
                                            int tx_type,
                                            size_t max_dd_outputs,
                                            const std::string& category) {
            const std::vector<CAmount> dd_amounts = ExtractDDMetadataAmounts(tx, tx_type);
            if (dd_amounts.empty()) return;

            uint256 base_txid;
            base_txid.SetHex(base_tx.txid);
            size_t dd_output_index = 0;
            for (const CTxOut& txout : tx.vout) {
                if (!IsStandardDDTokenOutput(txout)) continue;

                const size_t amount_index = dd_output_index++;
                if (amount_index >= dd_amounts.size()) continue;
                if (amount_index >= max_dd_outputs) continue;
                if (!is_local_dd_output(txout, base_txid)) continue;

                CTxDestination dest;
                std::string dd_address;
                if (ExtractDestination(txout.scriptPubKey, dest)) {
                    dd_address = DigiDollar::EncodeDigiDollarAddress(dest, Params());
                }

                bool duplicate = false;
                for (const auto& existing : synthesized_rows) {
                    if (existing.txid == base_tx.txid && existing.category == category &&
                        existing.address == dd_address && existing.amount == dd_amounts[amount_index]) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;

                DDTransaction row = base_tx;
                row.amount = dd_amounts[amount_index];
                row.incoming = true;
                row.address = dd_address;
                row.category = category;
                row.fee = 0;
                row.lock_tier = -1;
                synthesized_rows.push_back(row);
                txids_with_synthesized_rows.insert(base_tx.txid);
            }
        };

        for (const auto& hist_tx : transaction_history) {
            uint256 txid;
            txid.SetHex(hist_tx.txid);
            const wallet::CWalletTx* wtx = m_wallet->GetWalletTx(txid);
            if (!wtx || !wtx->tx) continue;

            if (!hist_tx.incoming && hist_tx.category == "send" && hist_tx.amount != 0) {
                const CAmount send_amount = hist_tx.amount < 0 ? -hist_tx.amount : hist_tx.amount;
                const std::vector<CAmount> dd_amounts = ExtractDDMetadataAmounts(*wtx->tx, DD_TX_TRANSFER);
                CAmount recipient_sum = 0;
                size_t recipient_count = 0;
                for (CAmount amount : dd_amounts) {
                    recipient_sum += amount;
                    ++recipient_count;
                    if (recipient_sum >= send_amount) break;
                }
                if (recipient_sum == send_amount && recipient_count > 0) {
                    append_local_output_rows(hist_tx, *wtx->tx, DD_TX_TRANSFER, recipient_count, "receive");
                }
            } else if (hist_tx.incoming && hist_tx.category == "receive" && !local_send_txids.count(hist_tx.txid)) {
                append_local_output_rows(hist_tx, *wtx->tx, DD_TX_TRANSFER, std::numeric_limits<size_t>::max(), "receive");
            } else if (!hist_tx.incoming && hist_tx.category == "redeem") {
                append_local_output_rows(hist_tx, *wtx->tx, DD_TX_REDEEM, std::numeric_limits<size_t>::max(), "redeem_change");
            }
        }

        if (!txids_with_synthesized_rows.empty()) {
            std::vector<DDTransaction> filtered_history;
            filtered_history.reserve(history.size() + synthesized_rows.size());
            for (const auto& hist_tx : history) {
                if (txids_with_synthesized_rows.count(hist_tx.txid) &&
                    (hist_tx.category == "receive" || hist_tx.category == "redeem_change")) {
                    continue;
                }
                filtered_history.push_back(hist_tx);
            }
            filtered_history.insert(filtered_history.end(), synthesized_rows.begin(), synthesized_rows.end());
            history = std::move(filtered_history);
        }
    }

    // Add mock history for testing if present
    history.insert(history.end(), mockHistory.begin(), mockHistory.end());

    // Calculate confirmations and abandoned status on-demand
    // This is the same pattern Bitcoin Core uses - confirmations computed dynamically
    for (auto& ddtx : history) {
        uint256 txid;
        txid.SetHex(ddtx.txid);
        const bool preset_in_mempool = ddtx.in_mempool;
        const bool preset_is_local = ddtx.is_local;
        ddtx.confirmations = GetDDTransactionConfirmations(txid);
        ddtx.in_mempool = preset_in_mempool;
        ddtx.is_local = preset_is_local;

        // Check if transaction is abandoned or effectively abandoned
        ddtx.abandoned = false;
        if (m_wallet) {
            LOCK(m_wallet->cs_wallet);
            const wallet::CWalletTx* wtx = m_wallet->GetWalletTx(txid);
            if (wtx) {
                ddtx.in_mempool = wtx->InMempool();
                ddtx.is_local = ddtx.confirmations == 0 && wtx->isUnconfirmed() && !ddtx.in_mempool;

                // Set block height from the transaction state
                if (auto* conf = wtx->state<wallet::TxStateConfirmed>()) {
                    ddtx.blockheight = conf->confirmed_block_height;
                    ddtx.blockhash = conf->confirmed_block_hash.GetHex();
                } else {
                    // Transaction is not confirmed, keep default -1
                    ddtx.blockheight = -1;
                    ddtx.blockhash = "";
                }

                // Calculate actual fee for send/mint/redeem transactions
                if (!ddtx.incoming && ddtx.fee == 0 && m_wallet) {
                    CAmount debit = wallet::CachedTxGetDebit(*m_wallet, *wtx, wallet::ISMINE_ALL);
                    CAmount credit = wallet::CachedTxGetCredit(*m_wallet, *wtx, wallet::ISMINE_ALL);
                    if (debit > credit) {
                        ddtx.fee = debit - credit;
                    }
                }

                if (wtx->isAbandoned()) {
                    // Directly abandoned
                    ddtx.abandoned = true;
                    ddtx.is_local = false;
                    ddtx.confirmations = -1;
                } else if (ddtx.confirmations < 0) {
                    ddtx.is_local = false;
                    // Transaction is conflicted (negative confirmations)
                    // Check if ALL conflicting transactions are abandoned
                    // If so, this transaction should also be considered abandoned
                    std::set<uint256> conflicts = m_wallet->GetTxConflicts(*wtx);
                    bool all_conflicts_abandoned = !conflicts.empty();
                    for (const uint256& conflict_txid : conflicts) {
                        const wallet::CWalletTx* conflict_wtx = m_wallet->GetWalletTx(conflict_txid);
                        if (conflict_wtx && !conflict_wtx->isAbandoned()) {
                            // Found a conflict that is NOT abandoned (it got confirmed or is pending)
                            all_conflicts_abandoned = false;
                            break;
                        }
                    }
                    if (all_conflicts_abandoned) {
                        // All conflicts are abandoned, so treat this as abandoned too
                        ddtx.abandoned = true;
                        ddtx.confirmations = -1;
                        LogPrintf("DigiDollar: Transaction %s marked abandoned (all %zu conflicts are abandoned)\n",
                                  ddtx.txid, conflicts.size());
                    }
                }
            }
        }
    }

    // Sort by timestamp (newest first)
    std::sort(history.begin(), history.end(),
              [](const DDTransaction& a, const DDTransaction& b) {
                  return a.timestamp > b.timestamp;
              });

    LogPrintf("DigiDollar: GetDDTransactionHistory returning %zu transactions\n", history.size());
    return history;
}

bool DigiDollarWallet::ValidateDDAddress(const std::string& address) const {
    return CDigiDollarAddress::IsValidDigiDollarAddressForCurrentNetwork(address);
}

// =============================================================================
// Wallet Restore Function Implementations (Position reconstruction)
// =============================================================================

bool DigiDollarWallet::ExtractDDAmountFromOpReturn(const CTransaction& tx, CAmount& dd_amount)
{
    // Parse OP_RETURN output to extract DD amount
    // Format: OP_RETURN <"DD"> <txType> <dd_amount> [additional fields]

    for (const CTxOut& txout : tx.vout) {
        if (txout.scriptPubKey.IsUnspendable() && txout.scriptPubKey.size() > 0) {
            const CScript& script = txout.scriptPubKey;
            auto pc = script.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Skip OP_RETURN
            if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN)
                continue;

            // Get DD marker ("DD")
            if (!script.GetOp(pc, opcode, data))
                continue;
            if (data.size() != 2 || data[0] != 'D' || data[1] != 'D')
                continue;

            // Get transaction type (1=MINT, 2=TRANSFER, 3=REDEEM)
            if (!script.GetOp(pc, opcode, data))
                continue;

            uint8_t txType = 0;
            try {
                CScriptNum txTypeNum(data, false);
                txType = static_cast<uint8_t>(txTypeNum.getint());
            } catch (const scriptnum_error&) {
                continue;
            }

            // Only process MINT, TRANSFER, and REDEEM types
            if (txType < 1 || txType > 3)
                continue;

            // Get DD amount (next field in all transaction types)
            if (!script.GetOp(pc, opcode, data))
                continue;

            try {
                CScriptNum amtNum(data, false);
                dd_amount = amtNum.GetInt64();
                return dd_amount > 0;
            } catch (const scriptnum_error&) {
                continue;
            }
        }
    }

    dd_amount = 0;
    return false;
}

bool DigiDollarWallet::ExtractUnlockHeightFromOpReturn(const CTransaction& tx, int64_t& unlock_height)
{
    // Parse OP_RETURN output to extract unlock height (only for MINT transactions)
    // Format: OP_RETURN <"DD"> <1> <dd_amount> <unlock_height>

    for (const CTxOut& txout : tx.vout) {
        if (txout.scriptPubKey.IsUnspendable() && txout.scriptPubKey.size() > 0) {
            const CScript& script = txout.scriptPubKey;
            auto pc = script.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Skip OP_RETURN
            if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN)
                continue;

            // Get DD marker ("DD")
            if (!script.GetOp(pc, opcode, data))
                continue;
            if (data.size() != 2 || data[0] != 'D' || data[1] != 'D')
                continue;

            // Get transaction type - must be MINT (1)
            if (!script.GetOp(pc, opcode, data))
                continue;

            uint8_t txType = 0;
            try {
                CScriptNum txTypeNum(data, false);
                txType = static_cast<uint8_t>(txTypeNum.getint());
            } catch (const scriptnum_error&) {
                continue;
            }

            // Only MINT transactions have unlock_height
            if (txType != 1)
                continue;

            // Skip DD amount field
            if (!script.GetOp(pc, opcode, data))
                continue;

            // Get unlock_height (4th field in MINT OP_RETURN)
            if (!script.GetOp(pc, opcode, data))
                continue;

            try {
                CScriptNum heightNum(data, false);
                unlock_height = heightNum.GetInt64();
                return unlock_height > 0;
            } catch (const scriptnum_error&) {
                continue;
            }
        }
    }

    unlock_height = 0;
    return false;
}

bool DigiDollarWallet::ExtractTierFromOpReturn(const CTransaction& tx, uint32_t& lock_tier)
{
    // Parse OP_RETURN output to extract lock tier (only for new MINT transactions)
    // New format: OP_RETURN <"DD"> <1> <dd_amount> <unlock_height> <lock_tier>
    // Old format: OP_RETURN <"DD"> <1> <dd_amount> <unlock_height>
    // Returns false for old transactions without explicit tier. V1 wallet
    // position reconstruction does not fall back to height-derived tiers.

    for (const CTxOut& txout : tx.vout) {
        if (txout.scriptPubKey.IsUnspendable() && txout.scriptPubKey.size() > 0) {
            const CScript& script = txout.scriptPubKey;
            auto pc = script.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Skip OP_RETURN
            if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN)
                continue;

            // Get DD marker ("DD")
            if (!script.GetOp(pc, opcode, data))
                continue;
            if (data.size() != 2 || data[0] != 'D' || data[1] != 'D')
                continue;

            // Get transaction type - must be MINT (1)
            if (!script.GetOp(pc, opcode, data))
                continue;

            uint8_t txType = 0;
            try {
                CScriptNum txTypeNum(data, false);
                txType = static_cast<uint8_t>(txTypeNum.getint());
            } catch (const scriptnum_error&) {
                continue;
            }

            // Only MINT transactions have lock_tier
            if (txType != 1)
                continue;

            // Skip DD amount field
            if (!script.GetOp(pc, opcode, data))
                continue;

            // Skip unlock_height field
            if (!script.GetOp(pc, opcode, data))
                continue;

            // Get lock_tier (5th field in new MINT OP_RETURN)
            // If this field doesn't exist, it's an old transaction
            if (!script.GetOp(pc, opcode, data)) {
                lock_tier = 0;
                return false;  // Old format - no tier stored
            }

            try {
                CScriptNum tierNum(data, false);
                lock_tier = static_cast<uint32_t>(tierNum.getint());
                return lock_tier <= 9;  // Valid tier range is 0-9
            } catch (const scriptnum_error&) {
                continue;
            }
        }
    }

    lock_tier = 0;
    return false;
}

uint32_t DigiDollarWallet::DeriveLockTierFromHeight(int64_t mint_height, int64_t unlock_height)
{
    // DEPRECATED: This legacy height-threshold helper is retained only for
    // old diagnostics/tests. New MINT transactions store the tier explicitly
    // in OP_RETURN; use ExtractTierFromOpReturn() for V1 position reconstruction.
    // The explicit OP_RETURN tier is authoritative for the full 0..9 canonical set.
    //
    // Legacy thresholds:
    //   Tier 0:  1 hour  =     240 blocks (testing only)
    //   Tier 1: 30 days  = 172,800 blocks
    //   Tier 2: 90 days  = 518,400 blocks (3 months)
    //   Tier 3: 180 days = 1,036,800 blocks (6 months)
    //   Tier 4: 365 days = 2,102,400 blocks (1 year)
    //   Tier 5: 1095 days = 6,307,200 blocks (3 years)
    //   Tier 6: 1825 days = 10,512,000 blocks (5 years)
    //   Tier 7: 2555 days = 14,716,800 blocks (7 years)
    //   Tier 8: 3650 days = 21,024,000 blocks (10 years)

    int64_t blocks = unlock_height - mint_height;

    // Check from highest tier down using exact thresholds
    // Tier 8: 10 years = 21,024,000 blocks
    if (blocks >= 21024000) return 8;

    // Tier 7: 7 years = 14,716,800 blocks
    if (blocks >= 14716800) return 7;

    // Tier 6: 5 years = 10,512,000 blocks
    if (blocks >= 10512000) return 6;

    // Tier 5: 3 years = 6,307,200 blocks
    if (blocks >= 6307200) return 5;

    // Tier 4: 1 year = 2,102,400 blocks
    if (blocks >= 2102400) return 4;

    // Tier 3: 180 days = 1,036,800 blocks
    if (blocks >= 1036800) return 3;

    // Tier 2: 90 days = 518,400 blocks
    if (blocks >= 518400) return 2;

    // Tier 1: 30 days = 172,800 blocks
    if (blocks >= 172800) return 1;

    // Tier 0: Testing tier (<30 days)
    return 0;
}

bool DigiDollarWallet::ExtractPositionFromMintTx(const CTransaction& tx, int block_height, WalletCollateralPosition& pos_out)
{
    // Extract complete position data from a MINT transaction.
    // Consensus identifies the collateral/DD token outputs by structure:
    // one positive-value P2TR collateral output and one zero-value P2TR DD token.
    // The OP_RETURN carries ("DD" | txType=1 | dd_minted | unlock_height | lock_tier).
    //
    // NOTE: lock_tier is REQUIRED in OP_RETURN. Old format transactions without
    // explicit tier are not supported (clean testnet restart).

    if (DigiDollar::GetDigiDollarTxType(tx) != DigiDollar::DD_TX_MINT) {
        LogPrintf("DigiDollar: ExtractPositionFromMintTx - Transaction is not a DD mint\n");
        return false;
    }

    if (tx.vout.size() < 3) {
        LogPrintf("DigiDollar: ExtractPositionFromMintTx - Mint transaction has too few outputs\n");
        return false;
    }

    MintOutputIndexes mint_outputs;
    if (!FindMintOutputIndexes(tx, mint_outputs)) {
        LogPrintf("DigiDollar: ExtractPositionFromMintTx - Mint outputs are not canonical\n");
        return false;
    }

    // 1. Extract DD amount from OP_RETURN
    CAmount dd_amount = 0;
    if (!ExtractDDAmountFromOpReturn(tx, dd_amount)) {
        LogPrintf("DigiDollar: ExtractPositionFromMintTx - Failed to extract DD amount\n");
        return false;
    }

    // 2. Extract unlock height from OP_RETURN
    int64_t unlock_height = 0;
    if (!ExtractUnlockHeightFromOpReturn(tx, unlock_height)) {
        LogPrintf("DigiDollar: ExtractPositionFromMintTx - Failed to extract unlock height\n");
        return false;
    }

    // 3. Get collateral amount from the consensus-recognized output
    CAmount dgb_collateral = mint_outputs.collateral_amount;

    // 4. Extract lock tier from OP_RETURN (REQUIRED - no fallback to derivation)
    uint32_t lock_tier = 0;
    if (!ExtractTierFromOpReturn(tx, lock_tier)) {
        LogPrintf("DigiDollar: ExtractPositionFromMintTx - FAILED: No lock_tier in OP_RETURN. "
                  "Old format transactions not supported. TX: %s\n", tx.GetHash().GetHex());
        return false;
    }

    // Validate tier is in valid range (0-9)
    if (lock_tier > 9) {
        LogPrintf("DigiDollar: ExtractPositionFromMintTx - Invalid tier %u (max 9). TX: %s\n",
                  lock_tier, tx.GetHash().GetHex());
        return false;
    }

    LogPrintf("DigiDollar: ExtractPositionFromMintTx - Extracted tier %u from OP_RETURN\n", lock_tier);

    // 5. Build position structure
    pos_out.dd_timelock_id = tx.GetHash();
    pos_out.dd_minted = dd_amount;
    pos_out.dgb_collateral = dgb_collateral;
    pos_out.lock_tier = lock_tier;
    pos_out.unlock_height = unlock_height;
    pos_out.is_active = true;  // Will be updated if spent later

    return true;
}

bool DigiDollarWallet::RefreshPositionMetadataFromMintTx(const uint256& position_id)
{
    auto locks = LockDDWallet();
    if (!m_wallet) {
        LogPrintf("DigiDollar: RefreshPositionMetadataFromMintTx - no wallet pointer\n");
        return false;
    }

    auto cached_it = collateral_positions.find(position_id);
    if (cached_it == collateral_positions.end()) {
        LogPrintf("DigiDollar: RefreshPositionMetadataFromMintTx - position not found: %s\n",
                  position_id.ToString());
        return false;
    }

    auto tx_it = m_wallet->mapWallet.find(position_id);
    if (tx_it == m_wallet->mapWallet.end() || !tx_it->second.tx) {
        LogPrintf("DigiDollar: RefreshPositionMetadataFromMintTx - mint tx not found in wallet: %s\n",
                  position_id.ToString());
        return false;
    }

    const wallet::CWalletTx& wtx = tx_it->second;
    const int block_height = wtx.state<wallet::TxStateConfirmed>()
        ? wtx.state<wallet::TxStateConfirmed>()->confirmed_block_height
        : -1;

    WalletCollateralPosition authoritative;
    if (!ExtractPositionFromMintTx(*wtx.tx, block_height, authoritative)) {
        LogPrintf("DigiDollar: RefreshPositionMetadataFromMintTx - failed to parse mint metadata for %s\n",
                  position_id.ToString());
        return false;
    }

    WalletCollateralPosition repaired = authoritative;
    repaired.is_active = cached_it->second.is_active;
    repaired.owner_keyid = cached_it->second.owner_keyid;

    const bool changed =
        cached_it->second.dd_minted != repaired.dd_minted ||
        cached_it->second.dgb_collateral != repaired.dgb_collateral ||
        cached_it->second.lock_tier != repaired.lock_tier ||
        cached_it->second.unlock_height != repaired.unlock_height;
    if (!changed) {
        return true;
    }

    LogPrintf("DigiDollar: RefreshPositionMetadataFromMintTx repaired position %s metadata "
              "(DD %lld -> %lld, collateral %lld -> %lld, tier %u -> %u, unlock %lld -> %lld)\n",
              position_id.ToString(),
              static_cast<long long>(cached_it->second.dd_minted),
              static_cast<long long>(repaired.dd_minted),
              static_cast<long long>(cached_it->second.dgb_collateral),
              static_cast<long long>(repaired.dgb_collateral),
              cached_it->second.lock_tier,
              repaired.lock_tier,
              static_cast<long long>(cached_it->second.unlock_height),
              static_cast<long long>(repaired.unlock_height));

    return WriteDDTimeLock(repaired);
}

bool DigiDollarWallet::GetMintCollateralOutpoint(const uint256& position_id, COutPoint& collateral_outpoint) const
{
    auto locks = LockDDWallet();
    if (!m_wallet) {
        return false;
    }

    auto tx_it = m_wallet->mapWallet.find(position_id);
    if (tx_it == m_wallet->mapWallet.end() || !tx_it->second.tx) {
        return false;
    }

    MintOutputIndexes mint_outputs;
    if (!FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
        return false;
    }

    collateral_outpoint = COutPoint(position_id, mint_outputs.collateral_index);
    return true;
}

bool DigiDollarWallet::GetMintDDTokenOutpoint(const uint256& position_id, COutPoint& dd_token_outpoint) const
{
    auto locks = LockDDWallet();
    if (!m_wallet) {
        return false;
    }

    auto tx_it = m_wallet->mapWallet.find(position_id);
    if (tx_it == m_wallet->mapWallet.end() || !tx_it->second.tx) {
        return false;
    }

    MintOutputIndexes mint_outputs;
    if (!FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
        return false;
    }

    dd_token_outpoint = COutPoint(position_id, mint_outputs.dd_token_index);
    return true;
}

void DigiDollarWallet::ProcessDDTxForRescan(const CTransactionRef& ptx, int block_height) {
    auto locks = LockDDWallet();
    if (!m_wallet) return;

    const CTransaction& tx = *ptx;
    LogPrintf("DigiDollar: ProcessDDTxForRescan called for tx %s at height %d\n",
              tx.GetHash().GetHex(), block_height);

    // Determine DD transaction type from the VERSION FIELD (primary) and OP_RETURN (supplementary).
    //
    // BUG FIX: Previously this relied SOLELY on OP_RETURN parsing to detect the tx type.
    // Full-redemption REDEEM txs (ddChange == 0) have NO OP_RETURN, so they were invisible
    // to the rescan parser — positions were never marked as redeemed after wallet restore.
    //
    // The tx version field ALWAYS encodes the type correctly via SetDigiDollarType().
    // Use GetDigiDollarTxType() as the authoritative source; OP_RETURN is only needed
    // for supplementary data (DD amounts for change outputs).
    uint8_t ddTxType = static_cast<uint8_t>(GetDigiDollarTxType(tx));

    // Also try OP_RETURN for supplementary data / backward compat validation
    uint8_t opReturnTxType = 0;
    for (const CTxOut& txout : tx.vout) {
        if (txout.scriptPubKey.IsUnspendable() && txout.scriptPubKey.size() > 0) {
            const CScript& script = txout.scriptPubKey;
            auto pc = script.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Skip OP_RETURN
            if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN)
                continue;

            // Get DD marker
            if (!script.GetOp(pc, opcode, data))
                continue;
            if (data.size() != 2 || data[0] != 'D' || data[1] != 'D')
                continue;

            // Get transaction type
            if (!script.GetOp(pc, opcode, data))
                continue;

            try {
                CScriptNum txTypeNum(data, false);
                opReturnTxType = static_cast<uint8_t>(txTypeNum.getint());
                break;
            } catch (const scriptnum_error&) {
                continue;
            }
        }
    }

    // Log when version field detects a type that OP_RETURN missed (the bug case)
    if (ddTxType != 0 && opReturnTxType == 0) {
        LogPrintf("DigiDollar: ProcessDDTxForRescan - tx %s type %d detected via version field "
                  "(no OP_RETURN DD marker — full redemption with no DD change)\n",
                  tx.GetHash().GetHex(), ddTxType);
    }

    if (ddTxType == 1) {  // MINT transaction
        LogPrintf("DigiDollar: ProcessDDTxForRescan - Found MINT tx %s\n", tx.GetHash().GetHex());
        if (tx.vout.empty()) return;

        LOCK(m_wallet->cs_wallet);

        MintOutputIndexes mint_outputs;
        if (!FindMintOutputIndexes(tx, mint_outputs)) {
            LogPrintf("DigiDollar: ProcessDDTxForRescan - Mint outputs are not canonical, skipping\n");
            return;
        }

        // For restored wallets, IsMine(collateral) fails because the collateral uses
        // a custom MAST tree that the wallet doesn't know about. Ownership must
        // still be proven by the DD token output itself. Ordinary DGB inputs or
        // change/payment outputs in the same transaction are not proof that this
        // wallet owns the DD position.
        bool is_our_mint = false;

        // Only claim the mint if this wallet can prove spendability of the
        // consensus-recognized zero-value P2TR DD output.
        const CTxOut& dd_txout = tx.vout[mint_outputs.dd_token_index];
        if (IsDDOutputMine(dd_txout, tx.GetHash())) {
            is_our_mint = true;
            LogPrintf("DigiDollar: ProcessDDTxForRescan - Our DD mint token found at vout[%u]\n",
                      mint_outputs.dd_token_index);
        }

        if (!is_our_mint) {
            LogPrintf("DigiDollar: ProcessDDTxForRescan - Mint DD token is not ours, skipping\n");
            return;
        }

        // Extract position data from mint transaction
        WalletCollateralPosition pos;
        if (ExtractPositionFromMintTx(tx, block_height, pos)) {
            // Descriptor restores can reconstruct the position and DD UTXO from
            // chain data, but redemption still needs the mint owner key indexed
            // by mint txid. If IsDDOutputMine() recovered the key from imported
            // descriptors, promote the DD output key to the owner-key map here.
            {
                CKey owner_key;
                if (!GetOwnerKey(tx.GetHash(), owner_key)) {
                    if (GetDDOutputSpendingKey(dd_txout, owner_key)) {
                        StoreOwnerKey(tx.GetHash(), owner_key);
                        pos.owner_keyid = owner_key.GetPubKey().GetID();
                        LogPrintf("DigiDollar: Restored owner key for mint position %s from DD output descriptor\n",
                                  tx.GetHash().GetHex());
                    }
                } else {
                    pos.owner_keyid = owner_key.GetPubKey().GetID();
                }
            }

            // Check if position already exists
            auto it = collateral_positions.find(pos.dd_timelock_id);
            if (it == collateral_positions.end()) {
                // Add new position
                collateral_positions[pos.dd_timelock_id] = pos;
                WriteDDTimeLock(pos);
                LogPrintf("DigiDollar: Restored position %s from rescan (DD: %lld, DGB: %lld)\n",
                          pos.dd_timelock_id.GetHex(), pos.dd_minted, pos.dgb_collateral);

                // Also add MINT transaction to history
                std::string txid_str = tx.GetHash().GetHex();
                bool already_exists = false;
                for (const auto& existing : transaction_history) {
                    if (existing.txid == txid_str && existing.category == "mint") {
                        already_exists = true;
                        break;
                    }
                }

                if (!already_exists) {
                    DDTransaction ddtx;
                    ddtx.txid = txid_str;
                    ddtx.amount = pos.dd_minted;
                    // Get timestamp from block time (mapWallet may not be populated during rescan)
                    int64_t block_time = 0;
                    if (block_height >= 0) {
                        uint256 block_hash = m_wallet->chain().getBlockHash(block_height);
                        m_wallet->chain().findBlock(block_hash, interfaces::FoundBlock().time(block_time));
                    }
                    ddtx.timestamp = block_time;
                    ddtx.blockheight = block_height;
                    ddtx.fee = 0;
                    ddtx.address = "";
                    ddtx.category = "mint";
                    ddtx.lock_tier = static_cast<int>(pos.lock_tier);

                    transaction_history.push_back(ddtx);

                    wallet::WalletBatch batch(m_wallet->GetDatabase());
                    batch.WriteDDTransaction(ddtx);

                    LogPrintf("DigiDollar: Restored MINT transaction %s from rescan (amount: %lld)\n",
                              txid_str, static_cast<long long>(pos.dd_minted));
	                }
            } else {
                WalletCollateralPosition repaired = pos;
                repaired.is_active = it->second.is_active;
                repaired.owner_keyid = !pos.owner_keyid.IsNull() ? pos.owner_keyid : it->second.owner_keyid;

                const bool changed =
                    it->second.dd_minted != repaired.dd_minted ||
                    it->second.dgb_collateral != repaired.dgb_collateral ||
                    it->second.lock_tier != repaired.lock_tier ||
                    it->second.unlock_height != repaired.unlock_height ||
                    it->second.owner_keyid != repaired.owner_keyid;
                if (changed) {
                    WriteDDTimeLock(repaired);
                    LogPrintf("DigiDollar: Repaired existing position %s from rescan mint metadata\n",
                              repaired.dd_timelock_id.GetHex());
                }
            }

            // CRITICAL FIX: Also restore the DD UTXO
            // This is needed because dd_utxos map is not exported with descriptors
            //
            // IMPORTANT: We must ALWAYS add the DD UTXO here, even if IsSpent() returns true.
            // This is because during rescan, transactions are processed in chronological order,
            // but IsSpent() returns the CURRENT state (after full chain sync), not the historical
            // state at the time of this MINT transaction.
            //
            // If we skip adding UTXOs that are "currently spent", then when TRANSFER/REDEEM
            // transactions are processed later (in chronological order), they won't find the
            // input UTXOs in dd_utxos, and the amounts will be lost.
            //
            // The TRANSFER/REDEEM processing will remove spent UTXOs from dd_utxos, so the
            // final state will be correct.
            {
                COutPoint ddOutpoint(tx.GetHash(), mint_outputs.dd_token_index);
                // Check if not already tracked
                if (dd_utxos.find(ddOutpoint) == dd_utxos.end()) {
                    // Always add the DD UTXO - TRANSFER/REDEEM processing will remove if spent
                    dd_utxos[ddOutpoint] = pos.dd_minted;
                    LogPrintf("DigiDollar: Added DD UTXO %s:%u during MINT rescan (DD: %lld)\n",
                              tx.GetHash().GetHex(), mint_outputs.dd_token_index, pos.dd_minted);

                    // Only persist if not spent - spent UTXOs will be removed by TRANSFER/REDEEM
                    if (!m_wallet->IsSpent(ddOutpoint)) {
                        wallet::WalletBatch batch(m_wallet->GetDatabase());
                        batch.WriteDDUTXO(ddOutpoint, pos.dd_minted);
                    }

                    // Do not use wallet IsSpent(collateral) while replaying a MINT.
                    // IsSpent reports the wallet's current spend graph, not the
                    // historical chain state at this mint. Real redeems are handled
                    // when their REDEEM transaction is replayed, and ScanForDDUTXOs()
                    // cross-checks active positions against the UTXO set after rescan.
                }
            }

            // CRITICAL FIX: Lock collateral and DD token UTXOs during rescan
            // to prevent wallet coin selection from picking them for regular DGB sends.
            {
                COutPoint collateralOutpoint(tx.GetHash(), mint_outputs.collateral_index);
                COutPoint ddTokenOutpoint(tx.GetHash(), mint_outputs.dd_token_index);
                wallet::WalletBatch lock_batch(m_wallet->GetDatabase());
                if (!m_wallet->IsLockedCoin(collateralOutpoint)) {
                    m_wallet->LockCoin(collateralOutpoint, &lock_batch);
                    LogPrintf("DigiDollar: Locked collateral UTXO %s:%u during rescan\n",
                              tx.GetHash().GetHex().substr(0, 16).c_str(), mint_outputs.collateral_index);
                }
                if (!m_wallet->IsLockedCoin(ddTokenOutpoint)) {
                    m_wallet->LockCoin(ddTokenOutpoint, &lock_batch);
                    LogPrintf("DigiDollar: Locked DD token UTXO %s:%u during rescan\n",
                              tx.GetHash().GetHex().substr(0, 16).c_str(), mint_outputs.dd_token_index);
                }
            }

            // DEBUG: Summary of MINT processing
            CAmount total_dd = 0;
            for (const auto& [outpoint, amount] : dd_utxos) {
                total_dd += amount;
            }
            LogPrintf("DigiDollar: MINT tx %s - END: dd_utxos.size()=%zu, total_dd=%lld cents\n",
                      tx.GetHash().GetHex().substr(0, 16).c_str(), dd_utxos.size(), static_cast<long long>(total_dd));
        }
    }
    else if (ddTxType == 2) {  // TRANSFER transaction
        // Reconstruct send transaction history for wallet restore
        // A TRANSFER is "our send" if we owned any of the DD inputs
        LogPrintf("DigiDollar: ProcessDDTxForRescan - Found TRANSFER tx %s\n", tx.GetHash().GetHex());

        LOCK(m_wallet->cs_wallet);

        // Check if we funded any of the DD inputs (meaning we sent this transfer)
        bool is_our_send = false;
        CAmount total_dd_sent = 0;
        std::string recipient_address;
        std::vector<CScript> spent_dd_input_scripts;

        for (const CTxIn& txin : tx.vin) {
            // Create COutPoint first - use the COutPoint overload of IsDDOutputMine
            // which checks dd_utxos first (source of truth for DD ownership).
            // This is CRITICAL for detecting TRANSFER change outputs after wallet restore,
            // where dd_owner_keys is empty and the txout-based check would fail.
            COutPoint spent_outpoint(txin.prevout.hash, txin.prevout.n);

            // DEBUG: Log what we're checking
            LogPrintf("DigiDollar: TRANSFER input check - outpoint %s:%u, in_dd_utxos=%d\n",
                      spent_outpoint.hash.GetHex(), spent_outpoint.n,
                      dd_utxos.find(spent_outpoint) != dd_utxos.end() ? 1 : 0);

            // Use COutPoint overload - checks dd_utxos first, then falls back to txout check
            if (IsDDOutputMine(spent_outpoint)) {
                is_our_send = true;
                LogPrintf("DigiDollar: TRANSFER - IsDDOutputMine returned true for input %s:%u, is_our_send=true\n",
                          spent_outpoint.hash.GetHex(), spent_outpoint.n);

                auto spent_tx_it = m_wallet->mapWallet.find(spent_outpoint.hash);
                if (spent_tx_it != m_wallet->mapWallet.end() && spent_tx_it->second.tx &&
                    spent_outpoint.n < spent_tx_it->second.tx->vout.size()) {
                    const CTxOut& spent_txout = spent_tx_it->second.tx->vout[spent_outpoint.n];
                    if (IsStandardDDTokenOutput(spent_txout)) {
                        spent_dd_input_scripts.push_back(spent_txout.scriptPubKey);
                    }
                }

                // Look up the DD amount from our tracking
                auto dd_it = dd_utxos.find(spent_outpoint);
                if (dd_it != dd_utxos.end()) {
                    total_dd_sent += dd_it->second;

                    // CRITICAL FIX: Remove spent UTXO from tracking during rescan
                    // This ensures the balance is calculated correctly after wallet restore
                    LogPrintf("DigiDollar: Removing spent DD UTXO %s:%u during rescan (was %lld cents)\n",
                              txin.prevout.hash.GetHex(), txin.prevout.n, static_cast<long long>(dd_it->second));
                    dd_utxos.erase(dd_it);

                    // Also remove from database
                    wallet::WalletBatch batch(m_wallet->GetDatabase());
                    batch.EraseDDUTXO(spent_outpoint);

                    // NOTE: Do NOT set dd_minted=0 here! The dd_minted field should always
                    // reflect the original minted amount (collateral backing). Transferring
                    // DD tokens doesn't change the collateral locked in the position.
                }
            } else {
                LogPrintf("DigiDollar: TRANSFER - IsDDOutputMine returned false for input %s:%u\n",
                          spent_outpoint.hash.GetHex(), spent_outpoint.n);
            }
        }

        // DEBUG: Log is_our_send after input loop
        LogPrintf("DigiDollar: TRANSFER tx %s - after input loop: is_our_send=%d, total_dd_sent=%lld\n",
                  tx.GetHash().GetHex().substr(0, 16).c_str(), is_our_send ? 1 : 0, static_cast<long long>(total_dd_sent));

        if (is_our_send) {
            // Reconstruct the aggregate send amount recorded by the live send
            // path. DD metadata amounts are ordered to match DD outputs:
            // recipients first, optional change last.
            struct RestoredTransferOutput {
                CAmount amount;
                bool is_ours;
                std::string address;
                CScript script_pub_key;
            };
            std::vector<RestoredTransferOutput> dd_outputs;
            const std::vector<CAmount> dd_amounts = ExtractDDMetadataAmounts(tx, DD_TX_TRANSFER);
            dd_outputs.reserve(dd_amounts.size());

            size_t dd_output_index = 0;
            for (const CTxOut& txout : tx.vout) {
                if (!IsStandardDDTokenOutput(txout)) {
                    continue;
                }

                const CAmount amount = dd_output_index < dd_amounts.size() ? dd_amounts[dd_output_index] : 0;
                ++dd_output_index;
                if (amount <= 0) {
                    continue;
                }

                CTxDestination dest;
                std::string output_address;
                if (ExtractDestination(txout.scriptPubKey, dest)) {
                    output_address = DigiDollar::EncodeDigiDollarAddress(dest, Params());
                }

                dd_outputs.push_back({amount, IsDDOutputMine(txout, tx.GetHash()), output_address, txout.scriptPubKey});
            }

            CAmount transfer_amount = 0;
            size_t restored_recipient_outputs = 0;
            size_t last_non_wallet_output = std::numeric_limits<size_t>::max();
            bool all_outputs_are_ours = !dd_outputs.empty();
            for (size_t i = 0; i < dd_outputs.size(); ++i) {
                all_outputs_are_ours = all_outputs_are_ours && dd_outputs[i].is_ours;
                if (!dd_outputs[i].is_ours) {
                    last_non_wallet_output = i;
                }
            }

            auto final_output_looks_like_change = [&]() {
                if (dd_outputs.size() <= 1 || total_dd_sent <= 0) return false;

                const RestoredTransferOutput& last_output = dd_outputs.back();
                if (last_output.amount <= 0 || last_output.amount >= total_dd_sent) return false;

                CAmount recipient_prefix = 0;
                CAmount max_prior_output = 0;
                bool all_prior_positive = true;
                bool last_amount_matches_prior = false;
                for (size_t i = 0; i + 1 < dd_outputs.size(); ++i) {
                    const CAmount amount = dd_outputs[i].amount;
                    all_prior_positive = all_prior_positive && amount > 0;
                    max_prior_output = std::max(max_prior_output, amount);
                    last_amount_matches_prior = last_amount_matches_prior || amount == last_output.amount;
                    recipient_prefix += amount;
                }
                if (recipient_prefix <= 0 || recipient_prefix != total_dd_sent - last_output.amount) {
                    return false;
                }

                const bool matches_spent_input_script = std::any_of(
                    spent_dd_input_scripts.begin(), spent_dd_input_scripts.end(),
                    [&](const CScript& spent_script) { return spent_script == last_output.script_pub_key; });
                if (matches_spent_input_script) {
                    return true;
                }

                // If the previous wallet transaction is not available during a
                // restore, keep this fallback narrow: local sendmany change is
                // appended after recipient outputs and is larger than every
                // equal-sized self-fragment output in the observed RC44 case.
                return all_prior_positive && !last_amount_matches_prior && last_output.amount > max_prior_output;
            };

            if (all_outputs_are_ours) {
                restored_recipient_outputs = dd_outputs.size();
                if (final_output_looks_like_change()) {
                    restored_recipient_outputs = dd_outputs.size() - 1;
                    LogPrintf("DigiDollar: TRANSFER restore inferred final DD output as change for tx %s\n",
                              tx.GetHash().GetHex());
                }
            } else if (last_non_wallet_output != std::numeric_limits<size_t>::max()) {
                // Include all outputs through the last known external recipient.
                // Any later wallet-owned output is the best on-chain change
                // candidate during restore.
                restored_recipient_outputs = last_non_wallet_output + 1;
            } else if (!dd_amounts.empty()) {
                // Defensive fallback for malformed or non-standard historical
                // wallet rows: preserve legacy first-recipient behavior.
                transfer_amount = dd_amounts.front();
                restored_recipient_outputs = 1;
            }

            if (transfer_amount == 0 && restored_recipient_outputs > 0) {
                for (size_t i = 0; i < restored_recipient_outputs && i < dd_outputs.size(); ++i) {
                    transfer_amount += dd_outputs[i].amount;
                }
            }

            if (restored_recipient_outputs == 1 && !dd_outputs.empty()) {
                recipient_address = dd_outputs.front().address;
            } else if (restored_recipient_outputs > 1) {
                recipient_address = "multiple";
            }

            // Check if this transaction already exists in history
            bool already_exists = false;
            std::string txid_str = tx.GetHash().GetHex();
            for (const auto& existing_tx : transaction_history) {
                if (existing_tx.txid == txid_str && existing_tx.category == "send") {
                    already_exists = true;
                    break;
                }
            }

            if (!already_exists && transfer_amount > 0) {
                // Add send transaction to history
                DDTransaction ddtx;
                ddtx.txid = txid_str;
                ddtx.amount = transfer_amount;
                // Get timestamp from block time (mapWallet may not be populated during rescan)
                int64_t block_time = 0;
                if (block_height >= 0) {
                    uint256 block_hash = m_wallet->chain().getBlockHash(block_height);
                    m_wallet->chain().findBlock(block_hash, interfaces::FoundBlock().time(block_time));
                }
                ddtx.timestamp = block_time;
                ddtx.confirmations = 0;  // Will be recalculated
                ddtx.incoming = false;
                ddtx.address = recipient_address;
                ddtx.category = "send";
                ddtx.blockheight = block_height;
                ddtx.fee = 0;  // Fee info not easily recoverable during rescan

                transaction_history.push_back(ddtx);

                // Persist to database
                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.WriteDDTransaction(ddtx);

                LogPrintf("DigiDollar: Restored SEND transaction %s from rescan (amount: %lld, to: %s)\n",
                          txid_str, static_cast<long long>(transfer_amount), recipient_address);
            }
        }

        // CRITICAL FIX: Also check if we RECEIVED DD in this transfer
        // Check if any DD output (P2TR with value=0) is ours
        // NOTE: We use IsDDOutputMine() instead of m_wallet->IsMine() because
        // IsMine() may return false for 0-value P2TR outputs in descriptor wallets
        //
        // ADDITIONAL FIX: If is_our_send is true, we need to also track change outputs.
        // Change outputs are DD outputs after the first one (recipient). Without dd_owner_keys
        // (e.g., after wallet restore), IsDDOutputMine can't identify change outputs, so we
        // use the fact that we sent the transaction to infer ownership.
        LogPrintf("DigiDollar: TRANSFER tx %s - checking outputs, is_our_send=%d\n",
                  tx.GetHash().GetHex().substr(0, 16).c_str(), is_our_send ? 1 : 0);

        int dd_output_count = 0;
        for (size_t i = 0; i < tx.vout.size(); i++) {
            const CTxOut& txout = tx.vout[i];
            // Check if this is a DD output (P2TR with value=0)
            if (txout.nValue != 0 || txout.scriptPubKey.size() != 34 || txout.scriptPubKey[0] != OP_1) {
                continue;
            }
            dd_output_count++;

            // Determine if this output is ours:
            // 1. IsDDOutputMine returns true, OR
            // 2. is_our_send is true AND this is not the first DD output (i.e., it's change)
            bool is_ours = IsDDOutputMine(txout, tx.GetHash());
            LogPrintf("DigiDollar: TRANSFER output vout[%zu] - dd_output_count=%d, IsDDOutputMine=%d, is_our_send=%d\n",
                      i, dd_output_count, is_ours ? 1 : 0, is_our_send ? 1 : 0);
            // SECURITY [T4-04]: Don't blindly assume "2nd DD output = change".
            // In a multi-recipient transfer, the 2nd output may be another recipient.
            // Only claim ownership if the wallet can actually spend this output.
            if (!is_ours && is_our_send && dd_output_count > 1) {
                // Verify wallet ownership via IsMine (SPENDABLE) before claiming as change.
                // This prevents claiming foreign outputs as our change during rescan.
                if (m_wallet->IsMine(txout) & wallet::ISMINE_SPENDABLE) {
                    is_ours = true;
                    LogPrintf("DigiDollar: Identified change output via is_our_send + IsMine(SPENDABLE) at vout[%zu]\n", i);
                } else {
                    LogPrintf("DigiDollar: Skipping non-owned DD output at vout[%zu] (is_our_send but not spendable)\n", i);
                }
            }

            if (is_ours) {
                COutPoint received_utxo(tx.GetHash(), i);

                // Check if not already tracked
                // NOTE: Don't check IsSpent() here - during rescan, IsSpent returns current state,
                // not state at time of this TX. We add all received UTXOs, and subsequent
                // TRANSFER/REDEEM processing will remove spent ones in chronological order.
                if (dd_utxos.find(received_utxo) == dd_utxos.end()) {
                    // Extract DD amount from OP_RETURN
                    CAmount received_dd = 0;
                    int dd_output_index = 0;
                    // Count which DD output this is (0=first, 1=second/change)
                    for (size_t j = 0; j < i; j++) {
                        if (tx.vout[j].nValue == 0 &&
                            tx.vout[j].scriptPubKey.size() == 34 &&
                            tx.vout[j].scriptPubKey[0] == OP_1) {
                            dd_output_index++;
                        }
                    }

                    // Parse OP_RETURN for amounts
                    for (const CTxOut& out : tx.vout) {
                        if (out.scriptPubKey.IsUnspendable() && out.scriptPubKey.size() > 0) {
                            const CScript& script = out.scriptPubKey;
                            auto pc = script.begin();
                            opcodetype opcode;
                            std::vector<unsigned char> data;

                            if (!script.GetOp(pc, opcode, data)) continue;  // OP_RETURN
                            if (!script.GetOp(pc, opcode, data)) continue;  // "DD"
                            if (!script.GetOp(pc, opcode, data)) continue;  // txType

                            // Collect all amounts
                            std::vector<CAmount> amounts;
                            while (script.GetOp(pc, opcode, data) && !data.empty()) {
                                try {
                                    CScriptNum amount(data, false);
                                    amounts.push_back(amount.GetInt64());
                                } catch (...) {
                                    break;
                                }
                            }

                            if (dd_output_index < static_cast<int>(amounts.size())) {
                                received_dd = amounts[dd_output_index];
                            } else if (!amounts.empty()) {
                                received_dd = amounts[0];
                            }
                            break;
                        }
                    }

                    if (received_dd > 0) {
                        // Add to dd_utxos
                        dd_utxos[received_utxo] = received_dd;

                        // Persist to database
                        wallet::WalletBatch batch(m_wallet->GetDatabase());
                        batch.WriteDDUTXO(received_utxo, received_dd);

                        LogPrintf("DigiDollar: Restored RECEIVED DD UTXO %s:%zu from rescan (DD: %lld)\n",
                                  tx.GetHash().GetHex(), i, static_cast<long long>(received_dd));

                        // Outgoing DD transfers may create wallet-owned change outputs. Those
                        // must rebuild as UTXOs, but the live send path does not record them
                        // as receive history.
                        if (is_our_send) {
                            LogPrintf("DigiDollar: Restored outgoing DD change UTXO %s:%zu without receive history\n",
                                      tx.GetHash().GetHex(), i);
                            continue;
                        }

                        // Also add receive transaction to history if not already there
                        std::string txid_str = tx.GetHash().GetHex();
                        bool already_exists = false;
                        for (const auto& existing : transaction_history) {
                            if (existing.txid == txid_str && existing.category == "receive") {
                                already_exists = true;
                                break;
                            }
                        }

                        if (!already_exists) {
                            DDTransaction ddtx;
                            ddtx.txid = txid_str;
                            ddtx.amount = received_dd;
                            // Get timestamp from block time (mapWallet may not be populated during rescan)
                            int64_t block_time = 0;
                            if (block_height >= 0) {
                                uint256 block_hash = m_wallet->chain().getBlockHash(block_height);
                                m_wallet->chain().findBlock(block_hash, interfaces::FoundBlock().time(block_time));
                            }
                            ddtx.timestamp = block_time;
                            ddtx.confirmations = 0;
                            ddtx.incoming = true;
                            ddtx.address = "";  // Sender address not easily recoverable
                            ddtx.category = "receive";
                            ddtx.blockheight = block_height;
                            ddtx.fee = 0;

                            transaction_history.push_back(ddtx);
                            batch.WriteDDTransaction(ddtx);

                            LogPrintf("DigiDollar: Restored RECEIVE transaction %s from rescan (amount: %lld)\n",
                                      txid_str, static_cast<long long>(received_dd));
                        }
                    }
                }
            }
        }

        // DEBUG: Summary of TRANSFER processing
        CAmount total_dd = 0;
        for (const auto& [outpoint, amount] : dd_utxos) {
            total_dd += amount;
        }
        LogPrintf("DigiDollar: TRANSFER tx %s - END: dd_output_count=%d, dd_utxos.size()=%zu, total_dd=%lld cents\n",
                  tx.GetHash().GetHex().substr(0, 16).c_str(), dd_output_count, dd_utxos.size(), static_cast<long long>(total_dd));
    }
    else if (ddTxType == 3) {  // REDEEM transaction
        // Find which position was redeemed and mark inactive
        // REDEEM tx spends the mint collateral output and DD UTXOs
        LogPrintf("DigiDollar: ProcessDDTxForRescan - Found REDEEM tx %s\n", tx.GetHash().GetHex());

        LOCK(m_wallet->cs_wallet);

        CAmount redeem_spent_dd_total = 0;
        auto redeemed_position_it = collateral_positions.end();
        for (const CTxIn& txin : tx.vin) {
            auto it = collateral_positions.find(txin.prevout.hash);
            if (it == collateral_positions.end()) {
                continue;
            }

            COutPoint collateral_outpoint(txin.prevout.hash, 0);
            auto tx_it = m_wallet->mapWallet.find(txin.prevout.hash);
            if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                MintOutputIndexes mint_outputs;
                if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                    collateral_outpoint = COutPoint(txin.prevout.hash, mint_outputs.collateral_index);
                }
            }
            if (txin.prevout == collateral_outpoint) {
                redeemed_position_it = it;
                break;
            }
        }

        const bool redeems_our_position = redeemed_position_it != collateral_positions.end();
        CKey redeemed_owner_key;
        const bool have_redeemed_owner_key = redeems_our_position &&
                                             GetOwnerKey(redeemed_position_it->first, redeemed_owner_key);

        // First, remove any DD UTXOs that were spent in this REDEEM transaction
        for (const CTxIn& txin : tx.vin) {
            COutPoint spent_outpoint(txin.prevout.hash, txin.prevout.n);
            auto dd_it = dd_utxos.find(spent_outpoint);
            if (dd_it != dd_utxos.end()) {
                redeem_spent_dd_total += dd_it->second;
                LogPrintf("DigiDollar: Removing spent DD UTXO %s:%u during REDEEM rescan (was %lld cents)\n",
                          txin.prevout.hash.GetHex(), txin.prevout.n, static_cast<long long>(dd_it->second));
                dd_utxos.erase(dd_it);

                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.EraseDDUTXO(spent_outpoint);
            }
        }

        const std::vector<CAmount> redeem_change_amounts = ExtractDDMetadataAmounts(tx, DD_TX_REDEEM);
        CAmount redeem_change_total = 0;
        for (CAmount change_amount : redeem_change_amounts) {
            redeem_change_total += change_amount;
        }

        if (!redeem_change_amounts.empty()) {
            auto redeem_change_output_is_ours = [&](const CTxOut& txout, CKey& change_owner_key, bool& have_change_owner_key) {
                have_change_owner_key = false;
                if (!IsStandardDDTokenOutput(txout)) {
                    return false;
                }

                std::vector<unsigned char> output_key_bytes(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end());
                if (have_redeemed_owner_key) {
                    XOnlyPubKey owner_xonly(redeemed_owner_key.GetPubKey());
                    auto tweaked = owner_xonly.CreateTapTweak(nullptr);
                    if (tweaked && std::equal(output_key_bytes.begin(), output_key_bytes.end(), tweaked->first.begin())) {
                        change_owner_key = redeemed_owner_key;
                        have_change_owner_key = true;
                        return true;
                    }

                    LogPrintf("DigiDollar: REDEEM rescan owner key for position %s did not match DD change output in tx %s; trying descriptor ownership\n",
                              redeemed_position_it->first.GetHex(), tx.GetHash().GetHex());
                }

                if (IsDDOutputMine(txout, tx.GetHash())) {
                    if (GetDDOutputSpendingKey(txout, change_owner_key)) {
                        have_change_owner_key = true;
                    }
                    return true;
                }

                if (redeems_our_position && GetDDOutputSpendingKey(txout, change_owner_key)) {
                    have_change_owner_key = true;
                    return true;
                }

                if (redeems_our_position) {
                    LogPrintf("DigiDollar: REDEEM rescan could not prove DD change output ownership in tx %s\n",
                              tx.GetHash().GetHex());
                }

                return false;
            };

            size_t dd_output_index = 0;
            for (size_t i = 0; i < tx.vout.size(); ++i) {
                const CTxOut& txout = tx.vout[i];
                if (!IsStandardDDTokenOutput(txout)) {
                    continue;
                }

                if (dd_output_index >= redeem_change_amounts.size()) {
                    break;
                }
                const CAmount change_amount = redeem_change_amounts[dd_output_index++];
                CKey change_owner_key;
                bool have_change_owner_key = false;
                if (change_amount <= 0 || !redeem_change_output_is_ours(txout, change_owner_key, have_change_owner_key)) {
                    continue;
                }

                COutPoint change_outpoint(tx.GetHash(), i);
                dd_utxos[change_outpoint] = change_amount;
                if (have_change_owner_key) {
                    StoreOwnerKey(tx.GetHash(), change_owner_key);
                }
                DigiDollar::RegisterScriptMetadata(txout.scriptPubKey,
                                                   DigiDollar::ScriptType::DD_TOKEN_OUTPUT,
                                                   change_amount,
                                                   0);

                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.WriteDDUTXO(change_outpoint, change_amount);

                LogPrintf("DigiDollar: Restored REDEEM DD change UTXO %s:%zu during rescan (DD: %lld)\n",
                          tx.GetHash().GetHex(), i, static_cast<long long>(change_amount));
            }
        }

        for (const CTxIn& txin : tx.vin) {
            auto it = collateral_positions.find(txin.prevout.hash);
            if (it != collateral_positions.end()) {
                COutPoint collateral_outpoint(txin.prevout.hash, 0);
                auto tx_it = m_wallet->mapWallet.find(txin.prevout.hash);
                if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                    MintOutputIndexes mint_outputs;
                    if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                        collateral_outpoint = COutPoint(txin.prevout.hash, mint_outputs.collateral_index);
                    }
                }
                if (txin.prevout != collateral_outpoint) {
                    continue;
                }

                // This input spends the collateral output of a mint tx we track
                it->second.is_active = false;
                UpdatePositionStatus(it->first, false);

                CAmount restored_redeemed_dd = it->second.dd_minted;
                if (redeem_spent_dd_total > redeem_change_total) {
                    restored_redeemed_dd = redeem_spent_dd_total - redeem_change_total;
                }

                // Add REDEEM transaction to history
                std::string txid_str = tx.GetHash().GetHex();
                bool already_exists = false;
                for (const auto& existing : transaction_history) {
                    if (existing.txid == txid_str && existing.category == "redeem") {
                        already_exists = true;
                        break;
                    }
                }

                if (!already_exists) {
                    DDTransaction ddtx;
                    ddtx.txid = txid_str;
                    ddtx.amount = restored_redeemed_dd;
                    // Get timestamp from block time (mapWallet may not be populated during rescan)
                    int64_t block_time = 0;
                    if (block_height >= 0) {
                        uint256 block_hash = m_wallet->chain().getBlockHash(block_height);
                        m_wallet->chain().findBlock(block_hash, interfaces::FoundBlock().time(block_time));
                    }
                    ddtx.timestamp = block_time;
                    ddtx.blockheight = block_height;
                    ddtx.fee = 0;
                    ddtx.address = "";
                    ddtx.category = "redeem";
                    ddtx.lock_tier = static_cast<int>(it->second.lock_tier);

                    transaction_history.push_back(ddtx);

                    wallet::WalletBatch batch(m_wallet->GetDatabase());
                    batch.WriteDDTransaction(ddtx);

                    LogPrintf("DigiDollar: Restored REDEEM transaction %s from rescan (amount: %lld)\n",
                              txid_str, static_cast<long long>(ddtx.amount));
                }

                LogPrintf("DigiDollar: Marked position %s as redeemed during rescan\n",
                          it->first.GetHex());
            }
        }
    }
}

// =============================================================================
// Redemption Function Implementations (Task 3.9)
// =============================================================================

bool DigiDollarWallet::RedeemDigiDollar(const COutPoint& collateralUtxo,
                                       CAmount ddAmount,
                                       DigiDollar::RedemptionPath path,
                                       std::string& txid,
                                       std::string& error) {
    auto locks = LockDDWallet();
    // Clear previous results
    txid.clear();
    error.clear();

    try {
        LogPrintf("DigiDollar: Starting redemption - %lld cents via path %d\n", static_cast<long long>(ddAmount), static_cast<int>(path));

        // Validate inputs
        if (collateralUtxo.IsNull()) {
            error = "Invalid collateral position";
            return false;
        }

        if (ddAmount <= 0) {
            error = "Invalid redemption amount";
            return false;
        }

        // Check if position can be redeemed
        DigiDollar::RedemptionPath availablePath;
        if (!CanRedeem(collateralUtxo, availablePath)) {
            error = "Position cannot be redeemed at this time";
            return false;
        }

        // Validate that requested path matches available path
        if (path != availablePath) {
            LogPrintf("DigiDollar: Requested path %d but only path %d available\n",
                     static_cast<int>(path), static_cast<int>(availablePath));
            // Auto-select best available path
            path = availablePath;
        }

        // Handle path-specific logic for all 4 redemption paths
        switch (path) {
            case DigiDollar::RedemptionPath::NORMAL:
                LogPrintf("DigiDollar: Using NORMAL redemption (timelock expired, system healthy)\n");
                // Normal path: timelock expired, system health >= 100%
                break;

            case DigiDollar::RedemptionPath::ERR:
                LogPrintf("DigiDollar: Using ERR redemption (timelock expired, system under-collateralized)\n");
                // ERR path: timelock expired, system health < 100%
                // User burns MORE DD to get full collateral back
                break;

            default:
                LogPrintf("DigiDollar: Unknown redemption path\n");
                error = "Unknown redemption path";
                return false;
        }

        // For now, return placeholder implementation
        // Full integration requires RedeemTxBuilder connection
        error = "Redemption function implemented but awaiting full TxBuilder integration";
        LogPrintf("DigiDollar: %s\n", error);
        return false;

        // GREEN phase implementation would be:
        /*
        // Create redemption transaction using builder
        DigiDollar::RedeemTxBuilder builder(Params(), GetCurrentHeight(), GetOraclePrice());

        DigiDollar::TxBuilderRedeemParams params;
        params.collateralOutpoint = collateralUtxo;
        params.ddToRedeem = ddAmount;
        params.path = path;
        params.ownerKey = GetOwnerKey(); // Would get from wallet
        params.feeRate = GetCurrentFeeRate();
        params.ddUtxos = GetDDUTXOsForAmount(ddAmount);
        params.feeUtxos = GetFeeUTXOs();

        auto result = builder.BuildRedemptionTransaction(params);
        if (!result.success) {
            error = result.error;
            return false;
        }

        // Submit transaction to network
        if (!SubmitTransaction(result.tx)) {
            error = "Failed to submit redemption transaction";
            return false;
        }

        txid = result.tx.GetHash().ToString();

        // Update wallet state
        UpdateWalletAfterRedemption(collateralUtxo, ddAmount, result.tx);

        return true;
        */

    } catch (const std::exception& e) {
        error = "Redemption failed: " + std::string(e.what());
        LogPrintf("DigiDollar: Redemption exception - %s\n", error);
        return false;
    }
}

std::vector<DigiDollar::RedeemablePosition> DigiDollarWallet::GetRedeemablePositions() const {
    auto locks = LockDDWallet();
    // Placeholder implementation for RED phase
    std::vector<DigiDollar::RedeemablePosition> positions;

    // In GREEN phase, would query actual wallet state:
    /*
    // Scan wallet for collateral UTXOs
    for (const auto& utxo : GetCollateralUTXOs()) {
        DigiDollar::RedeemablePosition pos;
        pos.collateralOutpoint = utxo.outpoint;
        pos.ddAmount = ExtractDDAmountFromPosition(utxo);
        pos.dgbLocked = utxo.nValue;
        pos.unlockHeight = ExtractUnlockHeight(utxo);
        pos.availablePaths = DetermineAvailablePaths(utxo);
        pos.canRedeemNow = !pos.availablePaths.empty();
        pos.estimatedReturn = CalculateRedemptionValue(utxo.outpoint);
        positions.push_back(pos);
    }
    */

    return positions;
}

CAmount DigiDollarWallet::CalculateRedemptionValue(const COutPoint& position) const {
    auto locks = LockDDWallet();
    // Placeholder implementation for RED phase
    CAmount redemptionValue = 0;

    // In GREEN phase, would calculate based on:
    // NOTE: Only 2 paths exist - NORMAL and ERR. Both return 100% collateral.
    /*
    // Get position details
    auto positionData = GetCollateralPosition(position);

    // Get current oracle price
    CAmount currentPrice = GetOraclePrice();

    // Determine best available redemption path
    DigiDollar::RedemptionPath bestPath;
    if (CanRedeem(position, bestPath)) {
        // Calculate return based on path
        // Both NORMAL and ERR return 100% collateral
        // ERR differs in requiring MORE DD to burn (105-125%)
        redemptionValue = positionData.dgbLocked;

        // Subtract estimated fees
        CAmount fees = EstimateRedemptionFee(position, bestPath);
        redemptionValue = std::max(CAmount(0), redemptionValue - fees);
    }
    */

    return redemptionValue;
}

bool DigiDollarWallet::CanRedeem(const COutPoint& position, DigiDollar::RedemptionPath& availablePath) const {
    auto locks = LockDDWallet();
    // Placeholder implementation for RED phase
    availablePath = DigiDollar::RedemptionPath::NORMAL;

    // In GREEN phase, would check:
    // NOTE: Only 2 paths exist - NORMAL and ERR. Both require timelock expiry.
    /*
    // Get position details
    auto positionData = GetCollateralPosition(position);
    if (positionData.outpoint.IsNull()) {
        return false; // Position doesn't exist
    }

    int currentHeight = GetCurrentHeight();
    int systemCollateral = GetSystemCollateral();

    // Both paths require timelock to be expired
    if (currentHeight < positionData.unlockHeight) {
        return false; // Timelock not expired yet
    }

    // Determine path based on system health
    if (systemCollateral < 100) {
        // ERR path - user must burn MORE DD to get full collateral
        availablePath = DigiDollar::RedemptionPath::ERR;
    } else {
        // Normal path - user burns original DD amount
        availablePath = DigiDollar::RedemptionPath::NORMAL;
    }
    return true;
    */

    return false; // No redemption path available in RED phase
}

std::vector<DDTransaction> DigiDollarWallet::GetRedemptionHistory() const {
    LOCK(cs_dd_wallet);
    // Placeholder implementation for RED phase
    std::vector<DDTransaction> redemptions;

    // In GREEN phase, would filter transaction history:
    /*
    for (const auto& tx : GetDDTransactionHistory()) {
        if (tx.category == "redeem") {
            redemptions.push_back(tx);
        }
    }
    */

    return redemptions;
}

CAmount DigiDollarWallet::EstimateRedemptionFee(const COutPoint& position, DigiDollar::RedemptionPath path) const {
    auto locks = LockDDWallet();
    // Bug #9/#17 fix: Use actual DD fee rate for estimation
    static const CAmount MIN_DD_TX_FEE = DigiDollar::MIN_DD_TX_FEE;
    static const CAmount FEE_RATE_PER_KB = DigiDollar::MIN_DD_FEE_RATE;

    // Estimate transaction vsize based on redemption path
    // Redemption tx: 3 inputs (collateral + DD + fee), 2-3 outputs
    // With Taproot script-path spending, ~400 vbytes typical
    size_t estimatedSize = 350; // Base vsize for redemption

    switch (path) {
        case DigiDollar::RedemptionPath::NORMAL:
            estimatedSize += 50; // Script path overhead
            break;
        case DigiDollar::RedemptionPath::ERR:
            estimatedSize += 75; // ERR script path overhead
            break;
        default:
            estimatedSize += 50; // Fallback to NORMAL
            break;
    }

    // Calculate size-based fee
    CAmount size_based_fee = (estimatedSize * FEE_RATE_PER_KB) / 1000;

    // DigiDollar transactions must pay at least 0.1 DGB fee
    return std::max(size_based_fee, MIN_DD_TX_FEE);
}

CAmount DigiDollarWallet::GetDGBBalance() const {
    auto locks = LockDDWallet();
    // Placeholder implementation for RED phase
    CAmount dgbBalance = 0;

    // In GREEN phase, would sum unspent DGB UTXOs:
    /*
    for (const auto& utxo : GetDGBUTXOs()) {
        dgbBalance += utxo.nValue;
    }
    */

    return dgbBalance;
}

// =============================================================================
// PHASE 2: STATE MANAGEMENT - DD BURNING & POSITION CLOSURE (Task 6)
// =============================================================================

bool DigiDollarWallet::BurnDigiDollars(CAmount amount, std::vector<COutPoint>& burnedUtxos) {
    auto locks = LockDDWallet();
    LogPrintf("DigiDollar: BurnDigiDollars - Burning %lld DD cents\n", static_cast<long long>(amount));

    // Validate input
    if (amount <= 0) {
        LogPrintf("DigiDollar: BurnDigiDollars - Invalid amount: %lld\n", static_cast<long long>(amount));
        return false;
    }

    // Clear output parameter
    burnedUtxos.clear();

    // Step 1: Get all spendable DD UTXOs
    std::vector<DDUtxo> available_utxos = GetDDUTXOs();
    if (available_utxos.empty()) {
        LogPrintf("DigiDollar: BurnDigiDollars - No DD UTXOs available\n");
        return false;
    }

    // Step 2: Select UTXOs to burn (simple greedy selection)
    CAmount selected_amount = 0;
    std::vector<COutPoint> selected_utxos;

    for (const auto& utxo : available_utxos) {
        // Add this UTXO
        selected_utxos.push_back(utxo.outpoint);
        selected_amount += utxo.dd_amount;
        LogPrintf("DigiDollar: BurnDigiDollars - Selected UTXO %s:%u (%lld DD), total now: %lld\n",
                  utxo.outpoint.hash.ToString(), utxo.outpoint.n, static_cast<long long>(utxo.dd_amount), static_cast<long long>(selected_amount));

        // Check if we now have enough
        if (selected_amount >= amount) {
            LogPrintf("DigiDollar: BurnDigiDollars - Have enough DD (%lld >= %lld), stopping selection\n",
                      static_cast<long long>(selected_amount), static_cast<long long>(amount));
            break;
        }
    }

    // Check if we have enough DD
    if (selected_amount < amount) {
        LogPrintf("DigiDollar: BurnDigiDollars - Insufficient DD balance: need %lld, have %lld\n",
                  static_cast<long long>(amount), static_cast<long long>(selected_amount));
        return false;
    }

    // Step 3: Mark UTXOs as spent in collateral_positions (if they exist there)
    // Note: MarkDDUTXOsSpent only works for UTXOs that have collateral positions
    // For UTXOs without positions (e.g., received DD), we just remove from dd_utxos map
    bool has_positions = false;
    for (const auto& utxo : selected_utxos) {
        auto it = collateral_positions.find(utxo.hash);
        if (it != collateral_positions.end()) {
            has_positions = true;
            break;
        }
    }

    if (has_positions) {
        if (!MarkDDUTXOsSpent(selected_utxos)) {
            LogPrintf("DigiDollar: BurnDigiDollars - Failed to mark UTXOs as spent in positions\n");
            return false;
        }
    }

    // FIX: Do NOT erase DD UTXOs at TX creation time!
    // Steps 4 & 5 previously erased from dd_utxos map and database immediately.
    // This caused permanent DD balance loss if the burn TX was later abandoned.
    // Instead, the UTXOs stay in the map and are hidden from balance via IsSpent().
    // They will be properly erased when the TX confirms in a block
    // (via ProcessTransactionForDD called from blockConnected).
    for (const auto& utxo : selected_utxos) {
        LogPrintf("DigiDollar: BurnDigiDollars - UTXO %s:%d pending burn (will be erased on block confirm)\n",
                  utxo.hash.ToString(), utxo.n);
    }

    // Step 6: Balance will be automatically correct via IsSpent() filtering
    // in GetTotalDDBalance() — no need to manually recalculate here.

    // Step 7: Return burned UTXOs
    burnedUtxos = selected_utxos;

    LogPrintf("DigiDollar: BurnDigiDollars - Successfully burned %lld DD cents using %zu UTXOs\n",
              static_cast<long long>(amount), burnedUtxos.size());
    LogPrintf("DigiDollar: BurnDigiDollars - New total DD balance: %lld cents\n", static_cast<long long>(total_dd_balance));

    return true;
}

bool DigiDollarWallet::CloseCollateralPosition(const COutPoint& outpoint) {
    LOCK(cs_dd_wallet);
    LogPrintf("DigiDollar: CloseCollateralPosition - Full closure of position %s:%d\n",
              outpoint.hash.ToString(), outpoint.n);

    // Validate input
    if (outpoint.IsNull()) {
        LogPrintf("DigiDollar: CloseCollateralPosition - Invalid outpoint (null)\n");
        return false;
    }

    // Step 1: Find collateral position
    // The outpoint.hash is the dd_timelock_id (mint tx hash)
    auto it = collateral_positions.find(outpoint.hash);
    if (it == collateral_positions.end()) {
        LogPrintf("DigiDollar: CloseCollateralPosition - Position not found: %s\n",
                  outpoint.hash.ToString());
        return false;
    }

    // Store original values for logging and potential rollback
    CAmount original_dd = it->second.dd_minted;
    CAmount original_dgb = it->second.dgb_collateral;
    bool originally_active = it->second.is_active;

    // FULL REDEMPTION ONLY — mark position as inactive.
    // Partial redemptions are architecturally impossible in the UTXO model:
    // the entire collateral UTXO is consumed as vin[0], and consensus
    // validation (validation.cpp:1721) enforces ddBurned >= originalDDMinted.
    it->second.is_active = false;

    LogPrintf("DigiDollar: CloseCollateralPosition - Full redemption: position marked inactive\n");
    LogPrintf("DigiDollar: CloseCollateralPosition - Full redemption: %lld DD redeemed, %lld DGB released\n",
              static_cast<long long>(original_dd), static_cast<long long>(original_dgb));

    // Step 2: Persist changes to wallet database
    if (m_wallet) {
        wallet::WalletBatch batch(m_wallet->GetDatabase());

        if (!batch.WriteDDTimeLock(it->second)) {
            LogPrintf("DigiDollar: CloseCollateralPosition - Failed to persist position update\n");
            // Rollback in-memory changes
            it->second.dd_minted = original_dd;
            it->second.dgb_collateral = original_dgb;
            it->second.is_active = originally_active;
            return false;
        }
    }

    // Step 3: Update locked collateral tracking
    if (it->second.is_active != originally_active) {
        CAmount total_locked = 0;
        for (const auto& [id, pos] : collateral_positions) {
            if (pos.is_active) {
                total_locked += pos.dgb_collateral;
            }
        }
        locked_collateral = total_locked;

        LogPrintf("DigiDollar: CloseCollateralPosition - Updated total locked collateral: %lld DGB\n",
                  static_cast<long long>(locked_collateral));
    }

    // Step 4: Record closure in transaction history — ALWAYS "redeem" (never "partial_redeem")
    DDTransaction ddtx;
    ddtx.txid = outpoint.hash.ToString();
    ddtx.amount = original_dd;
    ddtx.timestamp = GetTime();
    ddtx.confirmations = 0;
    ddtx.incoming = false; // Redemption (DD going out, DGB coming in)
    ddtx.address = "";
    ddtx.category = "redeem";  // ONLY valid redemption category
    ddtx.lock_tier = static_cast<int>(it->second.lock_tier);

    if (m_wallet) {
        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDTransaction(ddtx)) {
            LogPrintf("DigiDollar: CloseCollateralPosition - Warning: Failed to write transaction to history\n");
            // Don't fail the entire operation for history write failure
        }
    }

    LogPrintf("DigiDollar: CloseCollateralPosition - Successfully closed position %s\n",
              outpoint.hash.ToString());

    return true;
}

// =============================================================================
// PHASE 5 TASK 5.1: DATABASE EXTENSION IMPLEMENTATIONS
// =============================================================================

bool DigiDollarWallet::WriteDDBalance(const CDigiDollarAddress& addr, const CAmount& balance) {
    LOCK(cs_dd_wallet);
    if (!m_wallet) {
        LogPrintf("ERROR: DigiDollarWallet::WriteDDBalance - No wallet pointer set\n");
        return error("DigiDollarWallet::WriteDDBalance: No wallet pointer set");
    }

    if (balance < 0) {
        LogPrintf("ERROR: DigiDollarWallet::WriteDDBalance - Negative balance: %lld\n", static_cast<long long>(balance));
        return error("DigiDollarWallet::WriteDDBalance: Negative balance not allowed");
    }

    std::string addr_str = addr.ToString();
    if (addr_str.empty()) {
        // For testing with mock addresses, use a hash of the serialized address
        // This ensures different invalid addresses get different keys
        CDataStream ss(SER_DISK, CLIENT_VERSION);
        ss << addr;
        uint256 hash = Hash(ss);
        addr_str = "test_addr_" + hash.GetHex();

        LogPrint(BCLog::WALLETDB, "DigiDollarWallet::WriteDDBalance - Using test key for invalid address: %s\n", addr_str);
    }

    try {
        // Create balance record
        WalletDDBalance bal_record(addr, balance);
        bal_record.last_updated = GetTime();

        // Update in-memory cache first (works in both test and production mode)
        dd_balances[addr_str] = bal_record;

        // Recalculate total balance
        CAmount total = 0;
        for (const auto& [address, bal] : dd_balances) {
            total += bal.balance;
        }
        total_dd_balance = total;

        // Write to database (only if wallet pointer exists - production mode)
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());

            LogPrintf("DEBUG: DigiDollarWallet::WriteDDBalance - Writing to database: addr=%s, balance=%lld\n", addr_str, static_cast<long long>(balance));
            if (!batch.WriteDDBalance(addr_str, bal_record)) {
                LogPrintf("ERROR: DigiDollarWallet::WriteDDBalance - Database write failed for %s\n", addr_str);
                return error("DigiDollarWallet::WriteDDBalance: Database write failed for %s", addr_str.c_str());
            }

            // Persist total balance metadata
            batch.WriteDDMetadata("total_dd_balance", std::to_string(total));
        } else {
            // Testing mode - no database, just in-memory
            LogPrintf("DigiDollarWallet: WriteDDBalance in test mode (no database) - addr: %s\n", addr_str);
        }

        LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Wrote balance %lld for %s (total: %lld)\n",
                 static_cast<long long>(balance), addr_str, static_cast<long long>(total));
        return true;

    } catch (const std::exception& e) {
        LogPrintf("ERROR: DigiDollarWallet::WriteDDBalance - Exception: %s\n", e.what());
        return error("DigiDollarWallet::WriteDDBalance: Exception - %s", e.what());
    }
}

bool DigiDollarWallet::WriteDDTimeLock(const WalletCollateralPosition& position) {
    LOCK(cs_dd_wallet);
    try {
        if (position.dd_timelock_id.IsNull()) {
            return error("DigiDollarWallet::WriteDDTimeLock: Invalid position ID");
        }

        // Update in-memory cache first (works in both test and production mode)
        collateral_positions[position.dd_timelock_id] = position;

        // Write to database (only if wallet pointer exists - production mode)
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteDDTimeLock(position)) {
                return error("DigiDollarWallet::WriteDDTimeLock: Database write failed for %s",
                             position.dd_timelock_id.ToString());
            }
        } else {
            // Testing mode - no database, just in-memory
            LogPrintf("DigiDollarWallet: WriteDDTimeLock in test mode (no database) - ID: %s\n",
                      position.dd_timelock_id.GetHex());
        }

        // Recalculate locked collateral if active
        if (position.is_active) {
            CAmount total_locked = 0;
            for (const auto& [id, pos] : collateral_positions) {
                if (pos.is_active) {
                    total_locked += pos.dgb_collateral;
                }
            }
            locked_collateral = total_locked;

            // Write metadata to database (only in production mode)
            if (m_wallet) {
                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.WriteDDMetadata("locked_collateral", std::to_string(total_locked));
            }
        }

        LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Wrote DDTimeLock %s (DD: %lld, DGB: %lld, tier: %u)\n",
                 position.dd_timelock_id.ToString(), static_cast<long long>(position.dd_minted), static_cast<long long>(position.dgb_collateral), position.lock_tier);
        return true;

    } catch (const std::exception& e) {
        return error("DigiDollarWallet::WriteDDTimeLock: Exception - %s", e.what());
    }
}

bool DigiDollarWallet::UpdatePositionStatus(const uint256& dd_timelock_id, bool active) {
    auto locks = LockDDWallet();
    if (dd_timelock_id.IsNull()) {
        return error("DigiDollarWallet::UpdatePositionStatus: Invalid position ID");
    }

    // Check if position exists in memory
    auto it = collateral_positions.find(dd_timelock_id);
    if (it == collateral_positions.end()) {
        return error("DigiDollarWallet::UpdatePositionStatus: Position %s not found",
                     dd_timelock_id.ToString());
    }

    // Update status in memory
    it->second.is_active = active;

    // FIX: Do NOT erase DD UTXOs from tracking map when deactivating a position
    // at TX creation time. The DD UTXO stays in the map and is hidden from balance
    // via IsSpent(). It will be properly erased when the TX confirms in a block
    // (via ProcessTransactionForDD called from blockConnected) or during rescan
    // (via ProcessDDTxForRescan). This prevents permanent DD balance loss if the
    // TX is later abandoned.
    COutPoint dd_outpoint(dd_timelock_id, 1);
    if (!active) {
        LogPrintf("DigiDollar: DD UTXO %s:%d pending spend (position deactivated, will be erased on block confirm)\n",
                  dd_outpoint.hash.ToString(), dd_outpoint.n);
    }

    // Write updated position to database (if wallet pointer is set)
    if (m_wallet) {
        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDTimeLock(it->second)) {
            return error("DigiDollarWallet::UpdatePositionStatus: Database write failed");
        }

        // Recalculate locked collateral
        CAmount total_locked = 0;
        for (const auto& [id, pos] : collateral_positions) {
            if (pos.is_active) {
                total_locked += pos.dgb_collateral;
            }
        }
        locked_collateral = total_locked;
        batch.WriteDDMetadata("locked_collateral", std::to_string(total_locked));

        LogPrint(BCLog::WALLETDB, "DigiDollarWallet: Updated position %s status to %s (locked: %lld)\n",
                 dd_timelock_id.ToString(), active ? "active" : "inactive", static_cast<long long>(total_locked));
    } else {
        LogPrintf("DigiDollarWallet: Updated mock position %s status to %s - NO DB\n",
                  dd_timelock_id.ToString(), active ? "active" : "inactive");
    }

    return true;
}

// =============================================================================
// PHASE 5 TASK 5.2: BALANCE TRACKING IMPLEMENTATIONS
// =============================================================================

CAmount DigiDollarWallet::GetDDBalance(const CDigiDollarAddress& addr) const {
    LOCK(cs_dd_wallet);
    try {
        std::string key = addr.ToString();
        if (key.empty()) {
            // For testing with mock addresses, use a hash of the serialized address
            // This ensures different invalid addresses get different keys
            CDataStream ss(SER_DISK, CLIENT_VERSION);
            ss << addr;
            uint256 hash = Hash(ss);
            std::string test_key = "test_addr_" + hash.GetHex();

            // Check if we have this test address
            auto it = dd_balances.find(test_key);
            if (it != dd_balances.end()) {
                return it->second.balance;
            }

            // Return 0 if not found
            return 0;
        }

        auto it = dd_balances.find(key);
        CAmount balance = (it != dd_balances.end()) ? it->second.balance : 0;

        LogPrintf("DigiDollar: GetDDBalance for %s returned %lld cents\n", key, static_cast<long long>(balance));
        return balance;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: GetDDBalance exception - %s\n", e.what());
        return 0;
    }
}

CAmount DigiDollarWallet::GetTotalDDBalance() const {
    auto locks = LockDDWallet();
    try {
        // Calculate SPENDABLE balance from confirmed DD UTXOs only.
        // DigiDollar does not allow trusted-unconfirmed DD chaining; transfer
        // change must confirm before it can be selected again.
        CAmount balance = 0;
        for (const auto& [outpoint, dd_amount] : dd_utxos) {
            if (!m_wallet) {
                // Testing scenario: count all UTXOs in map
                balance += dd_amount;
            } else if (!m_wallet->IsSpent(outpoint)) {
                LOCK(m_wallet->cs_wallet);
                const wallet::CWalletTx* wtx = m_wallet->GetWalletTx(outpoint.hash);
                if (wtx && m_wallet->GetTxDepthInMainChain(*wtx) < 1) {
                    LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: GetTotalDDBalance skipping unconfirmed DD UTXO %s:%u (%lld cents)\n",
                             outpoint.hash.ToString(), outpoint.n, static_cast<long long>(dd_amount));
                    continue;
                }
                balance += dd_amount;
            }
        }

        LogPrintf("DigiDollar: GetTotalDDBalance calculated %lld spendable cents from %zu UTXOs\n",
                  static_cast<long long>(balance), dd_utxos.size());
        return balance;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: GetTotalDDBalance exception - %s\n", e.what());
        return 0;
    }
}

CAmount DigiDollarWallet::GetPendingDDBalance() const {
    auto locks = LockDDWallet();
    try {
        // Pending balance: all unconfirmed DD UTXOs. None are spendable until
        // confirmed, even if they are trusted wallet-created change.
        CAmount pending = 0;
        if (!m_wallet) {
            return 0; // No pending in test scenarios
        }

        for (const auto& [outpoint, dd_amount] : dd_utxos) {
            if (m_wallet->IsSpent(outpoint)) {
                continue;
            }

            LOCK(m_wallet->cs_wallet);
            const wallet::CWalletTx* wtx = m_wallet->GetWalletTx(outpoint.hash);
            if (wtx && m_wallet->GetTxDepthInMainChain(*wtx) == 0 && wtx->isUnconfirmed()) {
                pending += dd_amount;
                LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: GetPendingDDBalance counting unconfirmed DD UTXO %s:%u (%lld cents)\n",
                         outpoint.hash.ToString(), outpoint.n, static_cast<long long>(dd_amount));
            }
        }

        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: GetPendingDDBalance calculated %lld pending cents\n",
                 static_cast<long long>(pending));
        return pending;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: GetPendingDDBalance exception - %s\n", e.what());
        return 0;
    }
}

CAmount DigiDollarWallet::GetLockedCollateral() const {
    LOCK(cs_dd_wallet);
    try {
        CAmount locked = 0;
        for (const auto& entry : collateral_positions) {
            if (entry.second.is_active) {
                locked += entry.second.dgb_collateral;
            }
        }

        LogPrintf("DigiDollar: GetLockedCollateral returned %lld satoshis\n", static_cast<long long>(locked));
        return locked;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: GetLockedCollateral exception - %s\n", e.what());
        return 0;
    }
}

std::vector<WalletCollateralPosition> DigiDollarWallet::GetDDTimeLocks(bool active_only) const {
    LOCK(cs_dd_wallet);
    std::vector<WalletCollateralPosition> positions;

    try {
        for (const auto& entry : collateral_positions) {
            if (!active_only || entry.second.is_active) {
                positions.push_back(entry.second);
            }
        }

        LogPrintf("DigiDollar: GetDDTimeLocks returned %zu time-locked positions (active_only=%s)\n",
                  positions.size(), active_only ? "true" : "false");
        return positions;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: GetDDTimeLocks exception - %s\n", e.what());
        return positions; // Return empty vector
    }
}

std::vector<DDUtxo> DigiDollarWallet::GetDDUTXOs(bool include_unconfirmed) const {
    auto locks = LockDDWallet();
    std::vector<DDUtxo> utxos;

    LogPrintf("DigiDollar: GetDDUTXOs - Scanning dd_utxos map (FIX #1)\n");

    // FIX #1: Use actual tracked UTXOs instead of assuming positions
    for (const auto& [outpoint, dd_amount] : dd_utxos) {
        // Verify UTXO is still unspent in wallet
        if (m_wallet && m_wallet->IsSpent(outpoint)) {
            LogPrintf("DigiDollar: Skipping spent UTXO %s:%d\n",
                      outpoint.hash.ToString(), outpoint.n);
            continue; // Skip spent
        }

        // Only confirmed DD UTXOs may be selected. Read-only balance display
        // can opt in to include pending UTXOs without making them spendable.
        if (m_wallet && !include_unconfirmed) {
            LOCK(m_wallet->cs_wallet);
            const wallet::CWalletTx* wtx = m_wallet->GetWalletTx(outpoint.hash);
            if (wtx && m_wallet->GetTxDepthInMainChain(*wtx) < 1) {
                LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: Skipping unconfirmed DD UTXO %s:%u (%lld cents) in coin selection\n",
                         outpoint.hash.ToString(), outpoint.n, static_cast<long long>(dd_amount));
                continue;
            }
        }

        DDUtxo utxo(outpoint, dd_amount);
        utxos.push_back(utxo);

        LogPrintf("DigiDollar: Found DD UTXO %s:%u (%lld cents)\n",
                  outpoint.hash.ToString(), outpoint.n, static_cast<long long>(dd_amount));
    }

    LogPrintf("DigiDollar: GetDDUTXOs - Found %zu spendable UTXOs\n", utxos.size());
    return utxos;
}

CAmount DigiDollarWallet::GetDDFromUTXO(const COutPoint& outpoint) const {
    auto locks = LockDDWallet();
    // FIX #1: Look up UTXO in dd_utxos map (not collateral_positions)
    auto it = dd_utxos.find(outpoint);
    if (it == dd_utxos.end()) {
        LogPrintf("DigiDollar: GetDDFromUTXO - UTXO %s:%d not found in dd_utxos map\n",
                  outpoint.hash.ToString(), outpoint.n);
        return 0;
    }

    CAmount dd_amount = it->second;

    // Verify UTXO is still unspent
    if (m_wallet && m_wallet->IsSpent(outpoint)) {
        LogPrintf("DigiDollar: GetDDFromUTXO - UTXO %s:%d is spent\n",
                  outpoint.hash.ToString(), outpoint.n);
        return 0;
    }

    LogPrintf("DigiDollar: GetDDFromUTXO - Found %lld cents for UTXO %s:%u\n",
              static_cast<long long>(dd_amount), outpoint.hash.ToString(), outpoint.n);

    return dd_amount;
}

bool DigiDollarWallet::IsDDTokenUnspent(const uint256& dd_timelock_id) const {
    auto locks = LockDDWallet();
    auto it = std::find_if(dd_utxos.begin(), dd_utxos.end(),
        [&](const auto& entry) { return entry.first.hash == dd_timelock_id; });
    if (it == dd_utxos.end()) {
        // Not in our tracking map - could be already spent/transferred
        LogPrintf("DigiDollar: IsDDTokenUnspent - no DD UTXO for mint %s in dd_utxos map\n",
                  dd_timelock_id.ToString());
        return false;
    }

    const COutPoint& dd_token_outpoint = it->first;

    // Verify UTXO is still unspent in the wallet
    if (m_wallet && m_wallet->IsSpent(dd_token_outpoint)) {
        LogPrintf("DigiDollar: IsDDTokenUnspent - UTXO %s:%u is spent (transferred away)\n",
                  dd_timelock_id.ToString(), dd_token_outpoint.n);
        return false;
    }

    LogPrintf("DigiDollar: IsDDTokenUnspent - UTXO %s:%u is still unspent (%lld cents)\n",
              dd_timelock_id.ToString(), dd_token_outpoint.n, static_cast<long long>(it->second));
    return true;
}

void DigiDollarWallet::AddCollateralPosition(const WalletCollateralPosition& position) {
    LOCK(cs_dd_wallet);
    try {
        // Write position to database (this also updates the in-memory map)
        if (!WriteDDTimeLock(position)) {
            LogPrintf("DigiDollar: Failed to write position to database\n");
            return;
        }

        LogPrintf("DigiDollar: Added collateral position - ID: %s, DD: %lld, DGB: %lld, Tier: %u, Active: %s\n",
                  position.dd_timelock_id.GetHex(), static_cast<long long>(position.dd_minted), static_cast<long long>(position.dgb_collateral),
                  position.lock_tier, position.is_active ? "YES" : "NO");

        // FIX #1: Add DD UTXO to tracking map.
        COutPoint dd_outpoint(position.dd_timelock_id, 1);
        if (m_wallet) {
            auto tx_it = m_wallet->mapWallet.find(position.dd_timelock_id);
            if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                MintOutputIndexes mint_outputs;
                if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                    dd_outpoint = COutPoint(position.dd_timelock_id, mint_outputs.dd_token_index);
                }
            }
        }
        dd_utxos[dd_outpoint] = position.dd_minted;
        LogPrintf("DigiDollar: Added DD UTXO to tracking - %s:%u (%lld cents)\n",
                  dd_outpoint.hash.ToString(), dd_outpoint.n, static_cast<long long>(position.dd_minted));

        // Persist DD UTXO to database
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteDDUTXO(dd_outpoint, position.dd_minted)) {
                LogPrintf("DigiDollar: WARNING - Failed to persist DD UTXO to database\n");
            }
        }

        // Also add a transaction record for mint
        DDTransaction tx;
        tx.txid = position.dd_timelock_id.GetHex();
        tx.amount = position.dd_minted;
        tx.timestamp = GetTime();
        tx.confirmations = 0; // Will be updated when confirmed
        tx.incoming = true;
        tx.address = "";
        tx.category = "mint";
        tx.lock_tier = static_cast<int>(position.lock_tier);  // Set lock tier from position
        transaction_history.push_back(tx);

        // Persist transaction to database
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteDDTransaction(tx)) {
                LogPrintf("DigiDollar: WARNING - Failed to persist mint transaction to database\n");
            }
        }

        LogPrintf("DigiDollar: Added mint transaction to history - TxID: %s\n", tx.txid);
    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: AddCollateralPosition exception - %s\n", e.what());
    }
}

bool DigiDollarWallet::AddRedemptionToHistory(const DDTransaction& tx) {
    LOCK(cs_dd_wallet);
    try {
        bool updated = false;
        for (auto& existing : transaction_history) {
            if (existing.txid == tx.txid && existing.category == tx.category) {
                existing = tx;
                updated = true;
                break;
            }
        }
        if (!updated) {
            transaction_history.push_back(tx);
        }

        // Persist to database
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteDDTransaction(tx)) {
                LogPrintf("DigiDollar: WARNING - Failed to persist redemption transaction to database\n");
                return false;
            }
        }

        LogPrintf("DigiDollar: Added redemption transaction to history - TxID: %s, Amount: %lld cents\n",
                  tx.txid, static_cast<long long>(tx.amount));
        return true;
    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: AddRedemptionToHistory exception - %s\n", e.what());
        return false;
    }
}

size_t DigiDollarWallet::ScanForDDUTXOs() {
    auto locks = LockDDWallet();
    if (!m_wallet) {
        LogPrintf("DigiDollar: ScanForDDUTXOs called but no wallet pointer set\n");
        return 0;
    }

    try {
        LogPrintf("DigiDollar: Starting UTXO scan for DD outputs (OP_RETURN parsing)\n");

        // Clear existing tracking data for fresh scan
        dd_balances.clear();
        dd_utxos.clear();  // Clear UTXO tracking for fresh scan
        dd_foreign_output_keys.clear();
        total_dd_balance = 0;

        size_t dd_utxo_count = 0;

        // Lock wallet for thread-safe access
        LOCK(m_wallet->cs_wallet);

        // Iterate through all wallet transactions
        for (const auto& [txid, wtx] : m_wallet->mapWallet) {
            if (wtx.isAbandoned() || wtx.isConflicted()) {
                continue;
            }

            const DigiDollarTxType versionTxType = GetDigiDollarTxType(*wtx.tx);
            if (versionTxType == DD_TX_NONE) {
                continue;
            }

            // First, find the OP_RETURN output and extract DD amounts
            std::vector<CAmount> ddAmounts;
            int ddTxType = static_cast<int>(versionTxType);  // 1=MINT, 2=TRANSFER, 3=REDEEM

            for (size_t i = 0; i < wtx.tx->vout.size(); ++i) {
                const CScript& script = wtx.tx->vout[i].scriptPubKey;
                if (script.size() > 0 && script[0] == OP_RETURN) {
                    // Found OP_RETURN - try to parse DD data
                    auto pc = script.begin();
                    opcodetype opcode;
                    std::vector<unsigned char> data;

                    // Skip OP_RETURN
                    if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) continue;

                    // Get DD marker
                    if (!script.GetOp(pc, opcode, data)) continue;
                    if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;

                    // Get transaction type
                    if (!script.GetOp(pc, opcode, data)) continue;
                    try {
                        CScriptNum txTypeNum(data, false);
                        if (txTypeNum.getint() != ddTxType) {
                            ddAmounts.clear();
                            break;
                        }
                    } catch (const scriptnum_error&) {
                        continue;
                    }

                    if (ddTxType == 1) {
                        // MINT: Format is <"DD"> <1> <ddAmount> <lockHeight>
                        if (script.GetOp(pc, opcode, data)) {
                            try {
                                CScriptNum amtNum(data, false);
                                ddAmounts.push_back(amtNum.GetInt64());
                            } catch (const scriptnum_error&) {}
                        }
                    } else if (ddTxType == 2) {
                        // TRANSFER: Format is <"DD"> <2> <amount1> <amount2> ... <amountN>
                        while (script.GetOp(pc, opcode, data)) {
                            try {
                                CScriptNum amtNum(data, false);
                                CAmount amt = amtNum.GetInt64();
                                if (amt > 0 && amt <= 100000000000LL) {
                                    ddAmounts.push_back(amt);
                                }
                            } catch (const scriptnum_error&) {
                                break;
                            }
                        }
                    } else if (ddTxType == 3) {
                        // REDEEM: Format is <"DD"> <3> <change_amount>
                        // DD change from redemption when more DD UTXOs selected than needed
                        if (script.GetOp(pc, opcode, data)) {
                            try {
                                CScriptNum amtNum(data, false);
                                CAmount changeAmt = amtNum.GetInt64();
                                if (changeAmt > 0 && changeAmt <= 100000000000LL) {
                                    ddAmounts.push_back(changeAmt);
                                    LogPrintf("DigiDollar: ScanForDDUTXOs - Found REDEEM tx with DD change: %lld cents\n", static_cast<long long>(changeAmt));
                                }
                            } catch (const scriptnum_error&) {}
                        }
                    }
                    break; // Found and parsed OP_RETURN
                }
            }

            if (ddAmounts.empty()) {
                continue; // No DD amounts found in this transaction
            }

            // Now find DD outputs (P2TR with value=0) and match to amounts
            size_t ddOutputIndex = 0;
            for (size_t n = 0; n < wtx.tx->vout.size(); ++n) {
                const CTxOut& txout = wtx.tx->vout[n];

                // DD outputs are P2TR (34 bytes, starts with OP_1) with value = 0
                // For MINT: DD token is at output 1
                // For TRANSFER: DD tokens are outputs 0..N-1 (before DGB change and OP_RETURN)
                if (txout.scriptPubKey.size() == 34 &&
                    txout.scriptPubKey[0] == OP_1 &&
                    txout.nValue == 0) {

                    // For MINT, skip output 0 (vault) - DD token is at index 1
                    if (ddTxType == 1 && n == 0) {
                        continue; // Skip vault output
                    }

                    // Check if we have an amount for this DD output
                    size_t amountIndex = (ddTxType == 1) ? 0 : ddOutputIndex;
                    if (amountIndex >= ddAmounts.size()) {
                        continue; // No amount for this output
                    }

                    CAmount dd_amount = ddAmounts[amountIndex];
                    ddOutputIndex++;

                    // Check if we own this output
                    // First check standard wallet ownership
                    wallet::isminetype mine = m_wallet->IsMine(txout);
                    bool is_ours = (mine & wallet::ISMINE_SPENDABLE);

                    // Check dd_owner_keys - but VERIFY the key actually controls this output
                    // The owner key, when tweaked, should produce the output key
                    // This prevents marking recipient outputs as "ours" in transfer transactions
                    if (!is_ours) {
                        CKey owner_key;
                        if (GetOwnerKey(txid, owner_key)) {
                            // Extract the actual output key from scriptPubKey
                            std::vector<unsigned char> output_key_bytes(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end());

                            // Compute what the tweaked key should be from this owner_key
                            XOnlyPubKey owner_xonly(owner_key.GetPubKey());
                            auto tweaked = owner_xonly.CreateTapTweak(nullptr);
                            if (tweaked) {
                                // Check if tweaked key matches output key
                                if (std::equal(output_key_bytes.begin(), output_key_bytes.end(),
                                              tweaked->first.begin())) {
                                    is_ours = true;
                                    LogPrintf("DigiDollar: ScanForDDUTXOs - DD %s:%d owned via dd_owner_keys (verified, txType=%d)\n",
                                              txid.GetHex(), n, ddTxType);
                                } else {
                                    LogPrintf("DigiDollar: ScanForDDUTXOs - DD %s:%d has dd_owner_key but tweaked key doesn't match output\n",
                                              txid.GetHex(), n);
                                }
                            }
                        }
                    }

                    // If standard wallet doesn't recognize it, check dd_address_keys
                    // (for DD addresses generated via getdigidollaraddress)
                    if (!is_ours) {
                        // Extract the P2TR output key from the scriptPubKey
                        // P2TR scripts are: OP_1 <32-byte-output-key>
                        if (txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1) {
                            std::vector<unsigned char> output_key_bytes(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end());
                            XOnlyPubKey output_key(output_key_bytes);

                            // Check if we have the key for this output in dd_address_keys
                            CKey address_key;
                            if (GetAddressKey(output_key, address_key)) {
                                is_ours = true;
                                LogPrintf("DigiDollar: ScanForDDUTXOs - Output %s:%d owned via dd_address_keys\n",
                                          txid.GetHex(), n);
                            }
                        }
                    }

                    if (!is_ours) {
                        continue; // Not owned by us or not spendable
                    }

                    // Check if output is already spent
                    COutPoint outpoint(txid, n);
                    if (m_wallet->IsSpent(outpoint)) {
                        continue; // Already spent
                    }

                    // Add to dd_utxos map for coin selection (CRITICAL FIX)
                    // This enables GetDDUTXOs() to find received DD tokens
                    dd_utxos[outpoint] = dd_amount;

                    // CRITICAL: Register in global metadata registry for validation
                    // This enables ExtractDDAmount() to find DD amounts during redemption
                    DigiDollar::RegisterScriptMetadata(txout.scriptPubKey, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, dd_amount, 0);

                    // Add to balance tracking
                    std::string key = "total"; // Aggregate key

                    if (dd_balances.find(key) == dd_balances.end()) {
                        CDigiDollarAddress emptyAddr; // Empty address for total
                        dd_balances[key] = WalletDDBalance(emptyAddr, 0);
                    }

                    dd_balances[key].balance += dd_amount;
                    total_dd_balance += dd_amount;
                    dd_utxo_count++;

                    LogPrintf("DigiDollar: Found DD UTXO %s:%zu - Amount: %lld cents (txType=%d, added to dd_utxos)\n",
                              txid.GetHex(), n, static_cast<long long>(dd_amount), ddTxType);
                }
            }
        }

        LogPrintf("DigiDollar: Scan complete - Found %zu DD UTXOs, Total balance: %lld cents\n",
                  dd_utxo_count, static_cast<long long>(total_dd_balance));

        // Post-scan validation: cross-check active positions against the UTXO set.
        // Catches redeemed vaults that ProcessDDTxForRescan missed (e.g., custom
        // Taproot MAST scripts not recognized as "ours" during wallet restore).
        size_t corrected = ValidatePositionStates();
        if (corrected > 0) {
            LogPrintf("DigiDollar: ValidatePositionStates corrected %zu positions after scan\n", corrected);
        }

        return dd_utxo_count;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: ScanForDDUTXOs exception - %s\n", e.what());
        return 0;
    }
}

size_t DigiDollarWallet::ValidatePositionStates()
{
    // NOTE: caller (ScanForDDUTXOs) already holds LockDDWallet and cs_wallet.
    // Helpers below may re-enter those recursive locks but must keep the same
    // cs_wallet -> cs_dd_wallet order.
    if (!m_wallet) {
        LogPrintf("DigiDollar: ValidatePositionStates - no wallet pointer\n");
        return 0;
    }

    // Collect collateral outpoints for real collateral positions. DD change
    // compatibility entries have no collateral and must not be reconciled here.
    std::map<COutPoint, Coin> coins_to_check;
    std::map<uint256, COutPoint> position_outpoints;
    std::vector<uint256> position_ids;
    for (const auto& [id, pos] : collateral_positions) {
        if (pos.dgb_collateral > 0) {
            COutPoint collateral_outpoint(pos.dd_timelock_id, 0);
            auto tx_it = m_wallet->mapWallet.find(pos.dd_timelock_id);
            if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                MintOutputIndexes mint_outputs;
                if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                    collateral_outpoint = COutPoint(pos.dd_timelock_id, mint_outputs.collateral_index);
                }
            }
            coins_to_check[collateral_outpoint] = Coin();
            position_outpoints[id] = collateral_outpoint;
            position_ids.push_back(id);
        }
    }

    if (position_ids.empty()) {
        m_position_state_validation_pending = false;
        return 0;
    }

    if (!m_wallet->chain().isReadyToBroadcast()) {
        LogPrintf("DigiDollar: ValidatePositionStates skipped while chainstate is not ready\n");
        m_position_state_validation_pending = true;
        return 0;
    }

    LogPrintf("DigiDollar: ValidatePositionStates checking %zu collateral positions against UTXO set\n",
              position_ids.size());

    // Query the UTXO set (chainstate + mempool) for all collateral outpoints
    m_wallet->chain().findCoins(coins_to_check);

    size_t corrected = 0;
    for (const auto& id : position_ids) {
        COutPoint collateral_outpoint = position_outpoints.count(id) ? position_outpoints.at(id) : COutPoint(id, 0);
        const Coin& coin = coins_to_check[collateral_outpoint];
        auto it = collateral_positions.find(id);
        if (it == collateral_positions.end()) {
            continue;
        }

        if (RefreshPositionMetadataFromMintTx(id)) {
            it = collateral_positions.find(id);
            if (it == collateral_positions.end()) {
                continue;
            }
        } else {
            LogPrintf("DigiDollar: ValidatePositionStates - could not verify mint metadata for %s\n",
                      id.ToString());
        }

        const bool wallet_spent = m_wallet->IsSpent(collateral_outpoint);
        const auto mint_tx_it = m_wallet->mapWallet.find(id);
        const bool mint_unconfirmed =
            mint_tx_it != m_wallet->mapWallet.end() &&
            mint_tx_it->second.tx &&
            mint_tx_it->second.isUnconfirmed();

        if (mint_unconfirmed && coin.IsSpent() && !wallet_spent) {
            // The mint is still unconfirmed. It may be in the mempool, or it
            // may be wallet-local only (for example when -blocksonly prevented
            // relay). In either case, the collateral outpoint is not guaranteed
            // to appear in the chain/mempool UTXO view yet. Do not mark the
            // vault inactive; UI/RPC callers should treat it as confirming, not
            // redeemed.
            LogPrint(BCLog::DIGIDOLLAR,
                     "DigiDollar: ValidatePositionStates - Position %s mint still unconfirmed; keeping active\n",
                     id.GetHex());
        } else if (coin.IsSpent() || wallet_spent) {
            // Collateral is NOT in the UTXO set or is reserved by a wallet
            // mempool spend, so the position is not currently spendable.
            if (it->second.is_active) {
                it->second.is_active = false;
                LogPrintf("DigiDollar: ValidatePositionStates - Position %s collateral spent or pending spend, marking inactive\n",
                          id.GetHex());

                // Persist to database
                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.WriteDDTimeLock(it->second);
                ++corrected;
            }
        } else if (!it->second.is_active) {
            // Collateral is still live. A prior unconfirmed redeem may have left
            // the mempool or been reorged out, so restore the wallet view.
            it->second.is_active = true;
            LogPrintf("DigiDollar: ValidatePositionStates - Position %s collateral is live, marking active\n",
                      id.GetHex());

            wallet::WalletBatch batch(m_wallet->GetDatabase());
            batch.WriteDDTimeLock(it->second);
            ++corrected;
        }
    }

    m_position_state_validation_pending = false;
    return corrected;
}

size_t DigiDollarWallet::ReconcilePositionStates()
{
    if (!m_wallet) {
        LogPrintf("DigiDollar: ReconcilePositionStates - no wallet pointer\n");
        return 0;
    }

    std::map<COutPoint, Coin> coins_to_check;
    std::map<uint256, COutPoint> position_outpoints;
    std::vector<uint256> position_ids;
    {
        auto locks = LockDDWallet();
        for (const auto& [id, pos] : collateral_positions) {
            if (pos.dgb_collateral > 0) {
                COutPoint collateral_outpoint(pos.dd_timelock_id, 0);
                auto tx_it = m_wallet->mapWallet.find(pos.dd_timelock_id);
                if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                    MintOutputIndexes mint_outputs;
                    if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                        collateral_outpoint = COutPoint(pos.dd_timelock_id, mint_outputs.collateral_index);
                    }
                }
                coins_to_check[collateral_outpoint] = Coin();
                position_outpoints[id] = collateral_outpoint;
                position_ids.push_back(id);
            }
        }
    }

    if (position_ids.empty()) {
        LOCK(cs_dd_wallet);
        m_position_state_validation_pending = false;
        return 0;
    }

    if (!m_wallet->chain().isReadyToBroadcast()) {
        LogPrintf("DigiDollar: ReconcilePositionStates skipped while chainstate is not ready\n");
        LOCK(cs_dd_wallet);
        m_position_state_validation_pending = true;
        return 0;
    }

    LogPrintf("DigiDollar: ReconcilePositionStates checking %zu collateral positions against UTXO set\n",
              position_ids.size());

    // Do not hold cs_wallet/cs_dd_wallet while entering node::FindCoins; it
    // takes cs_main and mempool locks and debug builds abort on the inverted
    // wallet->chain lock order. Re-lock below only to apply/persist changes.
    m_wallet->chain().findCoins(coins_to_check);

    size_t corrected = 0;
    {
        auto locks = LockDDWallet();
        for (const auto& id : position_ids) {
            auto it = collateral_positions.find(id);
            if (it == collateral_positions.end()) {
                continue;
            }

            if (RefreshPositionMetadataFromMintTx(id)) {
                it = collateral_positions.find(id);
                if (it == collateral_positions.end()) {
                    continue;
                }
            } else {
                LogPrintf("DigiDollar: ReconcilePositionStates - could not verify mint metadata for %s\n",
                          id.ToString());
            }

            const COutPoint collateral_outpoint =
                position_outpoints.count(id) ? position_outpoints.at(id) : COutPoint(it->second.dd_timelock_id, 0);
            auto coin_it = coins_to_check.find(collateral_outpoint);
            const bool coin_spent = coin_it == coins_to_check.end() || coin_it->second.IsSpent();
            const bool wallet_spent = m_wallet->IsSpent(collateral_outpoint);
            const auto mint_tx_it = m_wallet->mapWallet.find(id);
            const bool mint_unconfirmed =
                mint_tx_it != m_wallet->mapWallet.end() &&
                mint_tx_it->second.tx &&
                mint_tx_it->second.isUnconfirmed();

            if (mint_unconfirmed && coin_spent && !wallet_spent) {
                // The mint is still unconfirmed. It may be in the mempool, or it
                // may be wallet-local only (for example when -blocksonly prevented
                // relay). In either case, the collateral outpoint is not guaranteed
                // to appear in the chain/mempool UTXO view yet. Do not mark the
                // vault inactive; the Qt vault tab will display it as Confirming
                // until it confirms.
                LogPrint(BCLog::DIGIDOLLAR,
                         "DigiDollar: ReconcilePositionStates - Position %s mint still unconfirmed; keeping active\n",
                         id.GetHex());
            } else if (coin_spent || wallet_spent) {
                if (it->second.is_active) {
                    it->second.is_active = false;
                    LogPrintf("DigiDollar: ReconcilePositionStates - Position %s collateral spent or pending spend, marking inactive\n",
                              id.GetHex());
                    wallet::WalletBatch batch(m_wallet->GetDatabase());
                    batch.WriteDDTimeLock(it->second);
                    ++corrected;
                }
            } else if (!it->second.is_active) {
                it->second.is_active = true;
                LogPrintf("DigiDollar: ReconcilePositionStates - Position %s collateral is live, marking active\n",
                          id.GetHex());
                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.WriteDDTimeLock(it->second);
                ++corrected;
            }
        }
        m_position_state_validation_pending = false;
    }

    return corrected;
}

size_t DigiDollarWallet::RetryPendingPositionStateValidation()
{
    {
        LOCK(cs_dd_wallet);
        if (!m_position_state_validation_pending) {
            return 0;
        }
    }

    return ReconcilePositionStates();
}

bool DigiDollarWallet::HasPendingPositionStateValidation() const
{
    LOCK(cs_dd_wallet);
    return m_position_state_validation_pending;
}

// =============================================================================
// INCREMENTAL DD UTXO PROCESSING (Performance Fix)
// Process a single transaction instead of full wallet rescan on every block
// =============================================================================

bool DigiDollarWallet::ProcessTransactionForDD(const CTransaction& tx, const uint256& txid) {
    auto locks = LockDDWallet();
    if (!m_wallet) {
        return false;
    }

    bool changed = false;

    try {
        // Step 1: Check if this transaction SPENDS any of our DD UTXOs
        // This is the ONLY spend-path erase for dd_utxos during normal operation.
        // Called from CWallet::blockConnected — TX has 1 confirmation (in a block).
        // Matches mainnet DGB behavior: UTXOs only pruned when spending TX confirms.
        for (const CTxIn& txin : tx.vin) {
            auto it = dd_utxos.find(txin.prevout);
            if (it != dd_utxos.end()) {
                // This TX spends one of our DD UTXOs - remove it (confirmed in block)
                CAmount spent_amount = it->second;
                total_dd_balance -= spent_amount;
                dd_utxos.erase(it);
                changed = true;

                // Persist erasure to database so it survives wallet restart
                if (m_wallet) {
                    wallet::WalletBatch batch(m_wallet->GetDatabase());
                    batch.EraseDDUTXO(txin.prevout);
                }

                LogPrint(BCLog::WALLETDB, "DigiDollar: ProcessTxForDD - Erased confirmed-spent DD UTXO %s:%d (%lld cents)\n",
                         txin.prevout.hash.ToString(), txin.prevout.n, static_cast<long long>(spent_amount));
            }
        }

        const DigiDollarTxType versionTxType = GetDigiDollarTxType(tx);
        if (versionTxType == DD_TX_NONE) {
            return changed;
        }

        // Step 2: Check if this transaction CREATES DD outputs we own
        // First find OP_RETURN with DD marker and parse amounts
        std::vector<CAmount> ddAmounts;
        int ddTxType = static_cast<int>(versionTxType);

        for (size_t i = 0; i < tx.vout.size(); ++i) {
            const CScript& script = tx.vout[i].scriptPubKey;
            if (script.size() > 0 && script[0] == OP_RETURN) {
                auto pc = script.begin();
                opcodetype opcode;
                std::vector<unsigned char> data;

                if (!script.GetOp(pc, opcode, data) || opcode != OP_RETURN) continue;
                if (!script.GetOp(pc, opcode, data)) continue;
                if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;

                if (!script.GetOp(pc, opcode, data)) continue;
                try {
                    CScriptNum txTypeNum(data, false);
                    if (txTypeNum.getint() != ddTxType) {
                        ddAmounts.clear();
                        break;
                    }
                } catch (const scriptnum_error&) {
                    continue;
                }

                if (ddTxType == 1) {
                    // MINT
                    if (script.GetOp(pc, opcode, data)) {
                        try {
                            CScriptNum amtNum(data, false);
                            ddAmounts.push_back(amtNum.GetInt64());
                        } catch (const scriptnum_error&) {}
                    }
                } else if (ddTxType == 2 || ddTxType == 3) {
                    // TRANSFER or REDEEM (with change)
                    while (script.GetOp(pc, opcode, data)) {
                        try {
                            CScriptNum amtNum(data, false);
                            CAmount amt = amtNum.GetInt64();
                            if (amt > 0 && amt <= 100000000000LL) {
                                ddAmounts.push_back(amt);
                            }
                        } catch (const scriptnum_error&) {
                            break;
                        }
                    }
                }
                break;
            }
        }

        if (ddAmounts.empty()) {
            return changed; // No DD amounts in this TX
        }

        // Match DD amounts to P2TR outputs we own (same logic as ScanForDDUTXOs)
        LOCK(m_wallet->cs_wallet);
        size_t ddOutputIndex = 0;

        for (size_t n = 0; n < tx.vout.size(); ++n) {
            const CTxOut& txout = tx.vout[n];

            // DD outputs are P2TR with value=0
            if (txout.nValue != 0) continue;
            if (txout.scriptPubKey.size() != 34 || txout.scriptPubKey[0] != OP_1) continue;

            const size_t ddAmountIndex = ddOutputIndex++;
            if (ddAmountIndex >= ddAmounts.size()) continue;

            // Check if we own this output
            wallet::isminetype mine = m_wallet->IsMine(txout);
            bool is_ours = (mine & wallet::ISMINE_SPENDABLE);

            // Check dd_owner_keys with tweaked key verification
            if (!is_ours) {
                CKey owner_key;
                if (GetOwnerKey(txid, owner_key)) {
                    std::vector<unsigned char> output_key_bytes(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end());
                    XOnlyPubKey owner_xonly(owner_key.GetPubKey());
                    auto tweaked = owner_xonly.CreateTapTweak(nullptr);
                    if (tweaked && std::equal(output_key_bytes.begin(), output_key_bytes.end(),
                                              tweaked->first.begin())) {
                        is_ours = true;
                    }
                }
            }

            // Check dd_address_keys
            if (!is_ours) {
                std::vector<unsigned char> output_key_bytes(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end());
                XOnlyPubKey output_key(output_key_bytes);
                CKey address_key;
                if (GetAddressKey(output_key, address_key)) {
                    is_ours = true;
                }
            }

            if (!is_ours) continue;

            // Check if already spent
            COutPoint outpoint(txid, n);
            if (m_wallet->IsSpent(outpoint)) continue;

            // This is our DD output - add it
            {
                CAmount dd_amount = ddAmounts[ddAmountIndex];

                if (dd_utxos.find(outpoint) == dd_utxos.end()) {
                    dd_utxos[outpoint] = dd_amount;
                    total_dd_balance += dd_amount;
                    changed = true;

                    // Persist new DD UTXO to database so it survives wallet restart
                    if (m_wallet) {
                        wallet::WalletBatch batch(m_wallet->GetDatabase());
                        batch.WriteDDUTXO(outpoint, dd_amount);
                    }

                    LogPrint(BCLog::WALLETDB, "DigiDollar: ProcessTxForDD - Added DD UTXO %s:%zu (%lld cents)\n",
                             txid.ToString(), n, static_cast<long long>(dd_amount));
                }

                if (ddTxType == DD_TX_MINT) {
                    int block_height = -1;
                    auto wtx_it = m_wallet->mapWallet.find(txid);
                    if (wtx_it != m_wallet->mapWallet.end()) {
                        if (auto* conf = wtx_it->second.state<wallet::TxStateConfirmed>()) {
                            block_height = conf->confirmed_block_height;
                        }
                    }

                    WalletCollateralPosition pos;
                    if (ExtractPositionFromMintTx(tx, block_height, pos)) {
                        CKey owner_key;
                        if (!GetOwnerKey(txid, owner_key) && GetDDOutputSpendingKey(txout, owner_key)) {
                            StoreOwnerKey(txid, owner_key);
                        }
                        if (GetOwnerKey(txid, owner_key)) {
                            pos.owner_keyid = owner_key.GetPubKey().GetID();
                        }

                        auto existing_position = collateral_positions.find(pos.dd_timelock_id);
                        if (existing_position == collateral_positions.end() ||
                            existing_position->second.dd_minted != pos.dd_minted ||
                            existing_position->second.dgb_collateral != pos.dgb_collateral ||
                            existing_position->second.lock_tier != pos.lock_tier ||
                            existing_position->second.unlock_height != pos.unlock_height ||
                            existing_position->second.owner_keyid != pos.owner_keyid) {
                            WriteDDTimeLock(pos);
                            changed = true;
                            LogPrint(BCLog::WALLETDB,
                                     "DigiDollar: ProcessTxForDD - Added/repaired mint position %s from confirmed wallet tx\n",
                                     txid.ToString());
                        }

                        MintOutputIndexes mint_outputs;
                        if (FindMintOutputIndexes(tx, mint_outputs)) {
                            wallet::WalletBatch lock_batch(m_wallet->GetDatabase());
                            const COutPoint collateral_outpoint(txid, mint_outputs.collateral_index);
                            if (!m_wallet->IsLockedCoin(collateral_outpoint)) {
                                m_wallet->LockCoin(collateral_outpoint, &lock_batch);
                            }
                            if (!m_wallet->IsLockedCoin(outpoint)) {
                                m_wallet->LockCoin(outpoint, &lock_batch);
                            }
                        }
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: ProcessTransactionForDD exception - %s\n", e.what());
    }

    return changed;
}

// =============================================================================
// PHASE 5 TASK 5.3: TRANSACTION CREATION IMPLEMENTATIONS
// =============================================================================

bool DigiDollarWallet::MintDigiDollar(const CAmount& dd_amount, uint32_t lock_tier, CTransactionRef& tx_out) {
    auto locks = LockDDWallet();
    try {
        LogPrintf("DigiDollar: MintDigiDollar called - amount: %lld, tier: %u\n", static_cast<long long>(dd_amount), lock_tier);
        if (!m_wallet || m_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            LogPrintf("DigiDollar: MintDigiDollar blocked because wallet cannot sign DD mints\n");
            return false;
        }

        // RED phase implementation - validation only
        if (!ValidateMintParams(dd_amount, lock_tier)) {
            LogPrintf("DigiDollar: MintDigiDollar validation failed\n");
            return false;
        }

        // For RED phase, return false as transaction creation not implemented
        LogPrintf("DigiDollar: MintDigiDollar not fully implemented (RED phase)\n");
        return false;

        // GREEN phase implementation would be:
        /*
        // Create mint transaction using MintTxBuilder
        DigiDollar::MintTxBuilder builder(Params(), GetCurrentHeight(), GetOraclePrice());

        DigiDollar::TxBuilderMintParams params;
        params.ddAmount = dd_amount;
        params.lockDays = DigiDollarWallet::GetLockDaysForTier(lock_tier);
        params.ownerKey = GetWalletKey();
        params.feeRate = GetCurrentFeeRate();
        params.utxos = GetAvailableUTXOs();

        auto result = builder.BuildMintTransaction(params);
        if (!result.success) {
            LogPrintf("DigiDollar: Mint transaction build failed - %s\n", result.error);
            return false;
        }

        tx_out = MakeTransactionRef(result.tx);

        // Create and store position in wallet
        uint256 positionId = result.tx.GetHash();
        WalletCollateralPosition position(positionId, dd_amount, result.collateralRequired, lock_tier,
                                         GetCurrentHeight() + builder.LockDaysToBlocks(params.lockDays));

        // PERSIST POSITION TO DATABASE
        if (!WriteDDTimeLock(position)) {
            LogPrintf("DigiDollarWallet::MintDigiDollar - Failed to write position to database\n");
            // Don't fail the mint, but log the error
        }

        // Create DD transaction record for history
        DDTransaction ddtx;
        ddtx.txid = positionId.ToString();
        ddtx.amount = dd_amount;
        ddtx.timestamp = GetTime();
        ddtx.confirmations = 0;
        ddtx.incoming = false;
        ddtx.address = "";  // Mint has no counterparty
        ddtx.category = "mint";

        // PERSIST TRANSACTION TO DATABASE
        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDTransaction(ddtx)) {
            LogPrintf("DigiDollarWallet::MintDigiDollar - Failed to write transaction to database\n");
        }

        return true;
        */

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: MintDigiDollar exception - %s\n", e.what());
        return false;
    }
}

bool DigiDollarWallet::TransferDigiDollar(const CDigiDollarAddress& to, CAmount amount, CTransactionRef& tx_out) {
    auto locks = LockDDWallet();
    try {
        LogPrintf("DigiDollar: TransferDigiDollar called - to: %s, amount: %lld\n", to.ToString(), static_cast<long long>(amount));
        if (!m_wallet || m_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            LogPrintf("DigiDollar: TransferDigiDollar blocked because wallet cannot sign DD spends\n");
            return false;
        }

        // Validate transfer parameters
        if (!ValidateTransferParams(to, amount)) {
            LogPrintf("DigiDollar: TransferDigiDollar validation failed\n");
            return false;
        }

        // Phase 2.1: Select DD UTXOs to cover the amount
        std::vector<COutPoint> dd_utxos;
        CAmount selectedDDTotal = 0;
        if (!SelectDDCoins(amount, dd_utxos, selectedDDTotal)) {
            LogPrintf("DigiDollar: Insufficient DD balance for transfer (need %lld, have %lld)\n",
                      static_cast<long long>(amount), static_cast<long long>(GetTotalDDBalance()));
            return false;
        }

        // Phase 2.1: Create mock transaction structure for fee estimation
        CMutableTransaction estimateTx;
        estimateTx.vin.resize(dd_utxos.size() + 1);  // DD inputs + 1 fee input
        estimateTx.vout.resize(2);  // Recipient + change outputs

        // Phase 2.1: Calculate estimated fee
        CAmount estimatedFee = CalculateTransactionFee(estimateTx);

        // Phase 2.1: Select DGB UTXOs for fees
        // CRITICAL: Exclude DD UTXOs from fee selection to prevent double-spend
        std::vector<COutPoint> exclude_dd_utxos = dd_utxos;
        std::vector<COutPoint> fee_utxos;
        std::vector<CAmount> fee_amounts;
        CAmount selectedFeeTotal = 0;
        if (!SelectFeeCoins(estimatedFee, fee_utxos, selectedFeeTotal, &fee_amounts, &exclude_dd_utxos)) {
            // Fallback: Create mock fee UTXO for testing (Phase 2.1 temporary)
            LogPrintf("DigiDollar: No DGB UTXOs found, using mock UTXO for fees\n");
            uint256 mockFeeTxid;
            mockFeeTxid.SetHex("fee1234567890abcdef1234567890abcdef1234567890abcdef1234567890ab");
            COutPoint mockFeeUtxo(mockFeeTxid, 1);
            fee_utxos.push_back(mockFeeUtxo);
            fee_amounts.push_back(estimatedFee * 2);
            selectedFeeTotal = estimatedFee * 2;  // Ensure sufficient
        }

        // Phase 2.1: Build transfer transaction using TxBuilder
        DigiDollar::TxBuilderTransferParams params;
        params.recipients.push_back({to.ToString(), amount});
        params.ddUtxos = dd_utxos;

        // Populate DD amounts for each UTXO
        for (const auto& utxo : dd_utxos) {
            CAmount dd_amount = GetDDFromUTXO(utxo);
            params.ddAmounts.push_back(dd_amount);
        }

        params.feeUtxos = fee_utxos;
        params.feeAmounts = fee_amounts;  // Pass actual fee UTXO amounts
        // DigiDollar transactions MUST pay at least 0.1 DGB fee to miners
        params.feeRate = DigiDollar::MIN_DD_FEE_RATE; // 0.35 DGB/kB = 0.105 DGB for 300 byte tx

        // Get the spending key from wallet
        // For DD transfers, we need the key that owns the first DD UTXO
        LogPrintf("DigiDollar: TransferDigiDollar - Starting key lookup, dd_utxos.size()=%d\n", dd_utxos.size());
        if (dd_utxos.empty()) {
            LogPrintf("DigiDollar: No DD UTXOs available for transfer\n");
            return false;
        }

        LogPrintf("DigiDollar: TransferDigiDollar - First DD UTXO: %s:%d\n", dd_utxos[0].hash.ToString(), dd_utxos[0].n);

        // Try to get the spending key - two cases:
        // 1. Minted DD: Key is in dd_owner_keys map (stored when we minted)
        // 2. Received DD: Key is in wallet's standard key management (P2TR key)
        CKey spenderKey;
        bool found_key = false;

        // First, try to get owner key from DD owner keys map (for minted DD)
        LogPrintf("DigiDollar: TransferDigiDollar - collateral_positions.size()=%d\n", collateral_positions.size());
        auto it = collateral_positions.find(dd_utxos[0].hash);
        if (it != collateral_positions.end()) {
            // This is minted DD - try to get the stored owner key
            LogPrintf("DigiDollar: TransferDigiDollar - Found in collateral_positions, trying GetOwnerKey\n");
            if (GetOwnerKey(dd_utxos[0].hash, spenderKey)) {
                found_key = true;
                LogPrintf("DigiDollar: Retrieved owner key for DD transfer from minted position %s\n", dd_utxos[0].hash.ToString());
            } else {
                LogPrintf("DigiDollar: TransferDigiDollar - GetOwnerKey returned false\n");
            }
        } else {
            LogPrintf("DigiDollar: TransferDigiDollar - UTXO NOT in collateral_positions (this is received DD)\n");
        }

        // If not found (received DD), try to get key from wallet's P2TR key management
        if (!found_key && m_wallet) {
            LogPrintf("DigiDollar: TransferDigiDollar - Trying wallet key lookup for received DD, m_wallet=%p\n", (void*)m_wallet);

            // Helper to get signing provider with private key access for descriptor wallets
            auto getSigningProviderWithKeys = [this](const CScript& script) -> std::unique_ptr<SigningProvider> {
                const auto& spk_mans = m_wallet->GetScriptPubKeyMans(script);
                if (!spk_mans.empty()) {
                    wallet::ScriptPubKeyMan* spk_man = *spk_mans.begin();
                    wallet::DescriptorScriptPubKeyMan* desc_spk_man = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(spk_man);
                    if (desc_spk_man) {
                        LogPrintf("DigiDollar: TransferDigiDollar - Using DescriptorScriptPubKeyMan with private keys\n");
                        return desc_spk_man->GetSigningProviderWithKeys(script);
                    }
                }
                // Fallback for legacy wallets
                return m_wallet->GetSolvingProvider(script);
            };

            // Get the scriptPubKey for the DD UTXO we're spending
            // We need to find the actual output being spent
            const COutPoint& dd_outpoint = dd_utxos[0];

            // Look up the transaction to get the scriptPubKey
            LogPrintf("DigiDollar: TransferDigiDollar - Looking up tx %s in mapWallet (size=%d)\n",
                     dd_outpoint.hash.ToString(), m_wallet->mapWallet.size());
            auto wtx_it = m_wallet->mapWallet.find(dd_outpoint.hash);
            if (wtx_it != m_wallet->mapWallet.end()) {
                LogPrintf("DigiDollar: TransferDigiDollar - Found tx in mapWallet\n");
                const auto& wtx = wtx_it->second;
                if (dd_outpoint.n < wtx.tx->vout.size()) {
                    const CTxOut& txout = wtx.tx->vout[dd_outpoint.n];
                    LogPrintf("DigiDollar: TransferDigiDollar - Got output %d, scriptPubKey size=%d\n",
                             dd_outpoint.n, txout.scriptPubKey.size());

                    // Get signing provider for this script WITH PRIVATE KEY ACCESS
                    auto provider = getSigningProviderWithKeys(txout.scriptPubKey);
                    if (provider) {
                        LogPrintf("DigiDollar: TransferDigiDollar - Got signing provider\n");
                        // Extract the P2TR destination
                        CTxDestination dest;
                        if (ExtractDestination(txout.scriptPubKey, dest)) {
                            LogPrintf("DigiDollar: TransferDigiDollar - ExtractDestination succeeded, dest index=%d\n",
                                     dest.index());
                            if (auto* tr = std::get_if<WitnessV1Taproot>(&dest)) {
                                LogPrintf("DigiDollar: TransferDigiDollar - Got WitnessV1Taproot destination\n");
                                // Get TaprootSpendData to find the internal key
                                TaprootSpendData spenddata;
                                XOnlyPubKey output_key(*tr);
                                LogPrintf("DigiDollar: TransferDigiDollar - Output key: %s\n", HexStr(output_key));
                                if (provider->GetTaprootSpendData(output_key, spenddata)) {
                                    LogPrintf("DigiDollar: TransferDigiDollar - GetTaprootSpendData succeeded\n");
                                    if (spenddata.internal_key.IsFullyValid()) {
                                        LogPrintf("DigiDollar: TransferDigiDollar - Internal key valid: %s\n",
                                                 HexStr(spenddata.internal_key));
                                        // Try to get the private key for the internal key
                                        if (provider->GetKeyByXOnly(spenddata.internal_key, spenderKey)) {
                                            found_key = true;
                                            LogPrintf("DigiDollar: Found key for received DD via GetKeyByXOnly\n");
                                        } else {
                                            LogPrintf("DigiDollar: TransferDigiDollar - GetKeyByXOnly FAILED for internal key\n");
                                        }
                                    } else {
                                        LogPrintf("DigiDollar: TransferDigiDollar - Internal key NOT valid\n");
                                    }
                                } else {
                                    LogPrintf("DigiDollar: TransferDigiDollar - GetTaprootSpendData FAILED\n");
                                    // Try alternate approach: look for key that matches the output key directly
                                    LogPrintf("DigiDollar: TransferDigiDollar - Trying to get key by output key directly\n");
                                    if (provider->GetKeyByXOnly(output_key, spenderKey)) {
                                        found_key = true;
                                        LogPrintf("DigiDollar: TransferDigiDollar - Found key by output key directly\n");
                                    } else {
                                        LogPrintf("DigiDollar: TransferDigiDollar - GetKeyByXOnly by output key FAILED\n");
                                    }
                                }

                                // Try dd_address_keys map (for addresses generated via getdigidollaraddress)
                                if (!found_key) {
                                    CKey address_key;
                                    if (GetAddressKey(XOnlyPubKey(*tr), address_key)) {
                                        spenderKey = address_key;
                                        found_key = true;
                                        LogPrintf("DigiDollar: TransferDigiDollar - Found key for received DD via dd_address_keys map\n");
                                    } else {
                                        LogPrintf("DigiDollar: TransferDigiDollar - dd_address_keys lookup failed for output key\n");
                                    }
                                }

                                // If still not found, try brute force scan through wallet keys
                                if (!found_key) {
                                    LogPrintf("DigiDollar: TransferDigiDollar - Starting brute force wallet scan\n");
                                    XOnlyPubKey target_output_key(*tr);
                                    int scan_count = 0;
                                    for (const auto& [txid, scan_wtx] : m_wallet->mapWallet) {
                                        for (size_t n = 0; n < scan_wtx.tx->vout.size() && !found_key; n++) {
                                            CTxDestination out_dest;
                                            if (ExtractDestination(scan_wtx.tx->vout[n].scriptPubKey, out_dest)) {
                                                auto out_provider = getSigningProviderWithKeys(scan_wtx.tx->vout[n].scriptPubKey);
                                                if (out_provider) {
                                                    if (auto* scan_tr = std::get_if<WitnessV1Taproot>(&out_dest)) {
                                                        TaprootSpendData scan_spenddata;
                                                        if (out_provider->GetTaprootSpendData(XOnlyPubKey(*scan_tr), scan_spenddata)) {
                                                            if (scan_spenddata.internal_key.IsFullyValid()) {
                                                                CKey test_key;
                                                                if (out_provider->GetKeyByXOnly(scan_spenddata.internal_key, test_key)) {
                                                                    scan_count++;
                                                                    XOnlyPubKey test_xonly(test_key.GetPubKey());
                                                                    auto tweaked = test_xonly.CreateTapTweak(nullptr);
                                                                    if (tweaked && std::equal(target_output_key.begin(), target_output_key.end(),
                                                                                             tweaked->first.begin())) {
                                                                        spenderKey = test_key;
                                                                        found_key = true;
                                                                        LogPrintf("DigiDollar: Found key for received DD via P2TR wallet scan\n");
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        if (found_key) break;
                                    }
                                    LogPrintf("DigiDollar: TransferDigiDollar - Brute force scan checked %d keys, found_key=%d\n",
                                             scan_count, found_key);
                                }
                            } else {
                                LogPrintf("DigiDollar: TransferDigiDollar - Destination is NOT WitnessV1Taproot\n");
                            }
                        } else {
                            LogPrintf("DigiDollar: TransferDigiDollar - ExtractDestination FAILED\n");
                        }
                    } else {
                        LogPrintf("DigiDollar: TransferDigiDollar - GetSolvingProvider returned NULL\n");
                    }
                } else {
                    LogPrintf("DigiDollar: TransferDigiDollar - Output index %d out of range (vout size=%d)\n",
                             dd_outpoint.n, wtx.tx->vout.size());
                }
            } else {
                LogPrintf("DigiDollar: TransferDigiDollar - TX NOT FOUND in mapWallet!\n");
            }
        } else if (!m_wallet) {
            LogPrintf("DigiDollar: TransferDigiDollar - m_wallet is NULL, cannot look up wallet keys\n");
        }

        if (!found_key) {
            LogPrintf("DigiDollar: Could not find spending key for DD UTXO %s:%d\n",
                     dd_utxos[0].hash.ToString(), dd_utxos[0].n);
            return false;
        }

        params.spenderKey = spenderKey;

        // CRITICAL FIX: Get a proper change address from the wallet for DGB change output
        // This ensures the wallet recognizes the change output as its own!
        // Without this, DGB would be sent to an address the wallet doesn't control.
        if (m_wallet) {
            // Get a new change address from wallet's internal keypool
            auto op_dest = m_wallet->GetNewChangeDestination(OutputType::BECH32);
            if (op_dest) {
                params.dgbChangeDest = *op_dest;
                LogPrintf("DigiDollar: Using wallet change address for DGB change output\n");
            } else {
                // Fallback: try to get any fresh address
                LogPrintf("DigiDollar: WARNING - Could not get change destination, trying fresh address\n");
                auto fresh_dest = m_wallet->GetNewDestination(OutputType::BECH32, std::string("DD_change"));
                if (fresh_dest) {
                    params.dgbChangeDest = *fresh_dest;
                    LogPrintf("DigiDollar: Using fresh wallet address for DGB change output\n");
                } else {
                    LogPrintf("DigiDollar: WARNING - No wallet change address available, DGB change may be lost!\n");
                }
            }
        }

        // BUG #5 FIX: Get real height.
        int currentHeight = m_wallet ? m_wallet->GetLastBlockHeight() : 100000;

        // Build transaction
        DigiDollar::TransferTxBuilder builder(Params(), currentHeight, TRANSFER_BUILDER_PRICE_UNUSED);
        DigiDollar::TxBuilderResult result = builder.BuildTransferTransaction(params);

        if (!result.success) {
            LogPrintf("DigiDollar: Transfer transaction build failed - %s\n", result.error);
            return false;
        }

        // Create transaction reference
        tx_out = MakeTransactionRef(result.tx);

        if (m_wallet) {
            std::string commit_error;
            if (!CommitDDTransaction(tx_out, commit_error)) {
                LogPrintf("DigiDollar: Commit failed - %s\n", commit_error);
                return false;
            }

            LogPrintf("DigiDollar: Transaction committed successfully - txid: %s\n",
                     tx_out->GetHash().ToString());
        } else {
            LogPrintf("DigiDollar: WARNING - No wallet context, transaction built but not broadcast (test mode)\n");
            // In test mode (m_wallet == nullptr), return true since transaction was successfully built
            // Broadcast isn't possible without wallet context, but transaction construction succeeded
        }

        // CRITICAL: DD Transfers DON'T create/destroy time-locks!
        // Time-locks stay intact until redemption.
        // Only DD token UTXOs move between wallets.
        //
        // What we need to do:
        // 1. Mark spent DD UTXOs as spent (NOT the time-lock position!)
        // 2. Add new DD UTXOs from transaction outputs (for change)
        // 3. DO NOT modify time-lock positions at all
        //
        // The time-lock position stays ACTIVE because the DGB is still locked!
        // Only the DD ownership changes.

        LogPrintf("DigiDollar: Transfer completed - DD ownership transferred, time-locks unchanged\n");

        // The transaction is built correctly by txbuilder:
        // - Inputs: DD UTXOs being spent
        // - Outputs: DD to recipient, DD change (if any), DGB change (if any)
        //
        // Balance will update automatically when we detect our change output
        // (either in this wallet if sending to self, or in receiving wallet)
        //
        // IMPORTANT: We do NOT mark positions inactive or create new positions!
        // The collateral positions represent time-locked DGB, which doesn't move.

        // Create DD transaction record for history
        DDTransaction ddtx;
        ddtx.txid = tx_out->GetHash().ToString();
        ddtx.amount = amount;
        ddtx.timestamp = GetTime();
        ddtx.confirmations = 0;
        ddtx.incoming = false;
        ddtx.address = to.ToString();
        ddtx.category = "send";

        // Add to transaction history
        transaction_history.push_back(ddtx);

        // Persist transaction to database if wallet available
        if (m_wallet) {
            wallet::WalletBatch batch(m_wallet->GetDatabase());
            if (!batch.WriteDDTransaction(ddtx)) {
                LogPrintf("DigiDollarWallet::TransferDigiDollar - Failed to write transaction\n");
            }
        }

        // Log balance update
        CAmount newBalance = GetTotalDDBalance();
        CAmount dd_change = selectedDDTotal - amount; // Calculate change from selected inputs
        LogPrintf("DigiDollar: Transfer successful - %lld cents to %s (txid: %s)\n",
                  static_cast<long long>(amount), to.ToString(), ddtx.txid);
        LogPrintf("DigiDollar: Balance after transfer: %lld (change: %lld)\n",
                  static_cast<long long>(newBalance), static_cast<long long>(dd_change));

        return true;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: TransferDigiDollar exception - %s\n", e.what());
        return false;
    }
}

bool DigiDollarWallet::RedeemDigiDollar(const uint256& dd_timelock_id, const CAmount& amount, CTransactionRef& tx_out) {
    auto locks = LockDDWallet();
    try {
        LogPrintf("DigiDollar: RedeemDigiDollar called - position: %s, amount: %lld\n", dd_timelock_id.ToString(), static_cast<long long>(amount));
        if (!m_wallet || m_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            LogPrintf("DigiDollar: RedeemDigiDollar blocked because wallet cannot sign DD redemptions\n");
            return false;
        }

        ValidatePositionStates();
        if (!RefreshPositionMetadataFromMintTx(dd_timelock_id)) {
            LogPrintf("DigiDollar: RedeemDigiDollar blocked because mint metadata could not be verified for %s\n",
                      dd_timelock_id.ToString());
            return false;
        }

        // Validation
        if (!ValidateRedeemParams(dd_timelock_id, amount)) {
            LogPrintf("DigiDollar: RedeemDigiDollar validation failed\n");
            return false;
        }

        // Get position details
        auto it = collateral_positions.find(dd_timelock_id);
        if (it == collateral_positions.end() || !it->second.is_active) {
            LogPrintf("DigiDollar: Position not found or inactive\n");
            return false;
        }

        // BUG #5 FIX: Get real height and oracle price
        int currentHeight = m_wallet ? m_wallet->GetLastBlockHeight() : 100000;
        CAmount oraclePrice = OracleBundleManager::GetInstance().GetLatestPrice();
        if (oraclePrice <= 0 && Params().GetChainType() == ChainType::REGTEST &&
            MockOracleManager::GetInstance().IsEnabled()) {
            oraclePrice = MockOracleManager::GetInstance().GetCurrentPrice();
        }
        if (oraclePrice <= 0) {
            LogPrintf("DigiDollar: RedeemDigiDollar blocked because no oracle price is available\n");
            return false;
        }

        DigiDollar::RedeemTxBuilder builder(Params(), currentHeight, oraclePrice);

        DigiDollar::TxBuilderRedeemParams params;
        if (!GetMintCollateralOutpoint(dd_timelock_id, params.collateralOutpoint)) {
            LogPrintf("DigiDollar: RedeemDigiDollar blocked because collateral outpoint could not be resolved for %s\n",
                      dd_timelock_id.ToString());
            return false;
        }
        params.ddToRedeem = amount;
        params.path = builder.DetermineRedemptionPath(params);

        // Get wallet spending key for this position. Never synthesize a fallback
        // key here: encrypted locked wallets must fail with unlock-needed
        // semantics, and missing DD owner keys are a hard redeem failure.
        CKey ownerKey;
        if (!GetOwnerKey(dd_timelock_id, ownerKey)) {
            if (m_wallet && m_wallet->IsCrypted() && m_wallet->IsLocked()) {
                LogPrintf("DigiDollar: RedeemDigiDollar blocked because encrypted wallet is locked and DD owner key cannot be decrypted\n");
            } else {
                LogPrintf("DigiDollar: RedeemDigiDollar blocked because no DD owner key is available for position %s\n",
                         dd_timelock_id.ToString());
            }
            return false;
        }
        params.ownerKey = ownerKey;

        // Pass the wallet's authoritative position metadata into the builder.
        // The builder cannot safely reconstruct this from a bare txid in wallet
        // context; tx.nLockTime must match the original unlock height exactly.
        params.collateralAmount = it->second.dgb_collateral;
        params.ddMinted = it->second.dd_minted;
        params.unlockHeight = static_cast<uint32_t>(it->second.unlock_height);

        // CRITICAL FIX: Get wallet addresses for BOTH collateral return AND DGB change
        // This ensures collateral and change are SEPARATE outputs, not merged
        // Try BECH32M first (Taproot), fallback to BECH32 for legacy wallets
        if (m_wallet) {
            LOCK(m_wallet->cs_wallet);
            std::string label = "";  // Empty label

            // Get address for returned collateral
            auto op_dest = m_wallet->GetNewDestination(OutputType::BECH32M, label);
            if (!op_dest) {
                // Legacy wallet fallback: try BECH32 (SegWit v0)
                LogPrintf("DigiDollar: BECH32M not available, trying BECH32 for legacy wallet\n");
                op_dest = m_wallet->GetNewDestination(OutputType::BECH32, label);
            }
            if (op_dest) {
                params.collateralDest = *op_dest;
                LogPrintf("DigiDollar: Using wallet destination for returned collateral\n");
            } else {
                LogPrintf("DigiDollar: WARNING - Could not get wallet address for collateral, using owner key (wallet may not recognize)\n");
                LogPrintf("DigiDollar: Error: %s\n", util::ErrorString(op_dest).original);
            }

            // Get SEPARATE address for DGB change from fee inputs
            auto op_change = m_wallet->GetNewDestination(OutputType::BECH32M, label);
            if (!op_change) {
                // Legacy wallet fallback
                LogPrintf("DigiDollar: BECH32M not available for change, trying BECH32 for legacy wallet\n");
                op_change = m_wallet->GetNewDestination(OutputType::BECH32, label);
            }
            if (op_change) {
                params.dgbChangeDest = *op_change;
                LogPrintf("DigiDollar: Using separate wallet destination for DGB change\n");
            } else {
                LogPrintf("DigiDollar: WARNING - Could not get wallet address for DGB change, will use collateralDest (may merge outputs)\n");
                LogPrintf("DigiDollar: Error: %s\n", util::ErrorString(op_change).original);
            }
        }

        // DigiDollar transactions MUST pay at least 0.1 DGB fee to miners
        params.feeRate = DigiDollar::MIN_DD_FEE_RATE; // 0.35 DGB/kB = 0.105 DGB for 300 byte tx

        // Select DD UTXOs to burn (any DD can be used - DD is fungible)
        // The key is to burn the EXACT amount that was minted for this vault
        CAmount selectedTotal = 0;
        if (!SelectDDCoins(amount, params.ddUtxos, selectedTotal, &params.ddAmounts)) {
            LogPrintf("DigiDollar: Insufficient DD balance for redemption\n");
            return false;
        }
        LogPrintf("DigiDollar: Selected %zu DD UTXOs totaling %lld cents for redemption of %lld cents\n",
                  params.ddUtxos.size(), static_cast<long long>(selectedTotal), static_cast<long long>(amount));
        LogPrintf("DigiDollar: ddAmounts size: %zu\n", params.ddAmounts.size());
        for (size_t i = 0; i < params.ddAmounts.size(); i++) {
            LogPrintf("DigiDollar: ddAmounts[%zu] = %lld cents\n", i, static_cast<long long>(params.ddAmounts[i]));
        }

        // Select DGB UTXOs for fees. The fee depends on how many inputs the
        // redemption ends up carrying, so converge on it from the projected
        // transaction instead of guessing a fixed size. This also excludes the
        // collateral outpoint and the DD UTXOs being burned from selection.
        std::string fee_error;
        CAmount projectedFee = 0;
        if (SelectRedemptionFeeCoins(params, fee_error, &projectedFee) != DDFeeSelectionResult::OK) {
            LogPrintf("DigiDollar: %s\n", fee_error);
            return false;
        }
        LogPrintf("DigiDollar: Selected %zu DGB fee input(s); projected redemption fee at most %lld sats\n",
                  params.feeUtxos.size(), static_cast<long long>(projectedFee));

        LogPrintf("DigiDollar: CALLING BuildRedemptionTransaction now...\n");
        auto result = builder.BuildRedemptionTransaction(params);
        LogPrintf("DigiDollar: BuildRedemptionTransaction returned success=%d, error='%s'\n",
                  result.success, result.error.c_str());
        if (!result.success) {
            LogPrintf("DigiDollar: Redemption transaction build failed - %s\n", result.error);
            return false;
        }
        LogPrintf("DigiDollar: BuildRedemptionTransaction SUCCESS, transaction has %d inputs and %d outputs\n",
                  result.tx.vin.size(), result.tx.vout.size());

        // Sign the transaction (includes collateral, DD, and fee inputs)
        LogPrintf("DigiDollar: ABOUT TO CALL SignRedemptionTransaction with %d DD inputs and %d fee inputs\n",
                  params.ddUtxos.size(), params.feeUtxos.size());
        CMutableTransaction mtx(result.tx);
        if (!SignRedemptionTransaction(mtx, params.collateralOutpoint, params.ddUtxos, params.feeUtxos, ownerKey)) {
            LogPrintf("DigiDollar: Failed to sign redemption transaction\n");
            return false;
        }
        LogPrintf("DigiDollar: SignRedemptionTransaction RETURNED SUCCESS\n");

        tx_out = MakeTransactionRef(mtx);

        // Broadcast transaction
        std::string error;
        if (!CommitDDTransaction(tx_out, error)) {
            LogPrintf("DigiDollar: Failed to broadcast redemption transaction - %s\n", error);
            return false;
        }

        // Track DD change output if there was any
        if (result.ddChange > 0) {
            // DD change is at vout[1] (after DGB return at vout[0])
            // Find the DD change output (P2TR with 0 value)
            uint256 txid = tx_out->GetHash();
            for (size_t i = 0; i < tx_out->vout.size(); i++) {
                const CTxOut& vout = tx_out->vout[i];
                // DD change output: zero value, P2TR (OP_1 + 32 bytes)
                if (vout.nValue == 0 && vout.scriptPubKey.size() == 34 && vout.scriptPubKey[0] == OP_1) {
                    COutPoint changeOutpoint(txid, i);
                    dd_utxos[changeOutpoint] = result.ddChange;
                    StoreOwnerKey(txid, ownerKey);  // Store owner key for change

                    // Persist to database
                    wallet::WalletBatch batch(m_wallet->GetDatabase());
                    batch.WriteDDUTXO(changeOutpoint, result.ddChange);

                    LogPrintf("DigiDollar: Tracked DD change output %s:%d (%d cents)\n",
                              txid.ToString(), i, result.ddChange);
                    break;
                }
            }
        }

        // FIX: Do NOT erase spent DD UTXOs at TX creation time!
        // They stay in dd_utxos and are hidden from balance via IsSpent().
        // Erasure happens when the TX confirms in a block (ProcessTransactionForDD).
        for (const auto& spentUtxo : params.ddUtxos) {
            LogPrintf("DigiDollar: DD UTXO %s:%d pending spend (redeem, will be erased on block confirm)\n",
                      spentUtxo.hash.ToString(), spentUtxo.n);
        }

        // Mark position as inactive
        if (!UpdatePositionStatus(dd_timelock_id, false)) {
            LogPrintf("DigiDollarWallet::RedeemDigiDollar - Failed to update position status\n");
        }

        // Record redemption transaction
        DDTransaction ddtx;
        ddtx.txid = tx_out->GetHash().ToString();
        ddtx.amount = amount;
        ddtx.timestamp = GetTime();
        ddtx.confirmations = 0;
        ddtx.incoming = true;  // Receiving DGB back
        ddtx.address = "";
        ddtx.category = "redeem";
        ddtx.fee = result.totalFees;  // Bug #17 fix: record actual fee paid
        ddtx.lock_tier = static_cast<int>(it->second.lock_tier);  // Set lock tier from redeemed position

        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDTransaction(ddtx)) {
            LogPrintf("DigiDollarWallet::RedeemDigiDollar - Failed to write transaction\n");
        }

        return true;

    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: RedeemDigiDollar exception - %s\n", e.what());
        return false;
    }
}

// =============================================================================
// PHASE 5.3: DDTIMELOCK STATUS MANAGEMENT IMPLEMENTATIONS
// =============================================================================

bool DigiDollarWallet::UpdateDDTimeLockStatus(const uint256& dd_timelock_id, bool new_status) {
    LOCK(cs_dd_wallet);
    // Validate input
    if (dd_timelock_id.IsNull()) {
        LogPrintf("DigiDollar: UpdateDDTimeLockStatus - Invalid DDTimeLock ID (null)\n");
        return false;
    }

    // Find DDTimeLock position
    auto it = collateral_positions.find(dd_timelock_id);
    if (it == collateral_positions.end()) {
        LogPrintf("DigiDollar: UpdateDDTimeLockStatus - DDTimeLock not found: %s\n",
                  dd_timelock_id.ToString());
        return false;
    }

    // Update status in memory
    bool old_status = it->second.is_active;
    it->second.is_active = new_status;

    // Persist to database if wallet available
    if (m_wallet) {
        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDTimeLock(it->second)) {
            LogPrintf("DigiDollar: UpdateDDTimeLockStatus - Failed to persist status update\n");
            // Rollback in-memory change
            it->second.is_active = old_status;
            return false;
        }

        // Recalculate locked collateral
        CAmount total_locked = 0;
        for (const auto& [id, pos] : collateral_positions) {
            if (pos.is_active) {
                total_locked += pos.dgb_collateral;
            }
        }
        locked_collateral = total_locked;

        LogPrintf("DigiDollar: Updated DDTimeLock %s status: %s → %s (locked collateral: %d)\n",
                  dd_timelock_id.ToString(),
                  old_status ? "ACTIVE" : "INACTIVE",
                  new_status ? "ACTIVE" : "INACTIVE",
                  total_locked);
    } else {
        LogPrintf("DigiDollar: Updated DDTimeLock %s status to %s (NO DB - mock mode)\n",
                  dd_timelock_id.ToString(),
                  new_status ? "ACTIVE" : "INACTIVE");
    }

    return true;
}


std::string DigiDollarWallet::GetDDTimeLockStatus(const uint256& dd_timelock_id) const {
    LOCK(cs_dd_wallet);
    // Validate input
    if (dd_timelock_id.IsNull()) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: GetDDTimeLockStatus - Invalid DDTimeLock ID (null)\n");
        return "not_found";
    }

    // Find DDTimeLock position
    auto it = collateral_positions.find(dd_timelock_id);
    if (it == collateral_positions.end()) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: GetDDTimeLockStatus - DDTimeLock not found: %s\n",
                 dd_timelock_id.ToString());
        return "not_found";
    }

    const WalletCollateralPosition& position = it->second;

    // Determine status based on is_active and dd_minted
    if (!position.is_active) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: DDTimeLock %s status: fully_redeemed\n",
                 dd_timelock_id.ToString());
        return "fully_redeemed";
    }

    // Active position
    LogPrint(BCLog::WALLETDB, "DigiDollar: DDTimeLock %s status: active (%d DD)\n",
             dd_timelock_id.ToString(), position.dd_minted);
    return "active";
}

bool DigiDollarWallet::IsDDTimeLockRedeemable(const uint256& dd_timelock_id, int current_height) const {
    LOCK(cs_dd_wallet);
    // Validate input
    if (dd_timelock_id.IsNull()) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: IsDDTimeLockRedeemable - Invalid DDTimeLock ID (null)\n");
        return false;
    }

    // Find DDTimeLock position
    auto it = collateral_positions.find(dd_timelock_id);
    if (it == collateral_positions.end()) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: IsDDTimeLockRedeemable - DDTimeLock not found: %s\n",
                 dd_timelock_id.ToString());
        return false;
    }

    const WalletCollateralPosition& position = it->second;

    // Must be active
    if (!position.is_active) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: DDTimeLock %s not redeemable - inactive\n",
                 dd_timelock_id.ToString());
        return false;
    }

    // Must be unlocked (current height >= unlock height)
    if (current_height < position.unlock_height) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: DDTimeLock %s not redeemable - still locked (height %d < unlock %d)\n",
                 dd_timelock_id.ToString(), current_height, position.unlock_height);
        return false;
    }

    // Must have DD remaining
    if (position.dd_minted == 0) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: DDTimeLock %s not redeemable - no DD remaining\n",
                 dd_timelock_id.ToString());
        return false;
    }

    LogPrint(BCLog::WALLETDB, "DigiDollar: DDTimeLock %s is REDEEMABLE (height: %d, unlock: %d, DD: %d)\n",
             dd_timelock_id.ToString(), current_height, position.unlock_height, position.dd_minted);

    return true;
}

// =============================================================================
// PHASE 5 TEST HELPERS AND UTILITY FUNCTIONS
// =============================================================================

void DigiDollarWallet::SetMockDDBalance(const CDigiDollarAddress& addr, CAmount balance) {
    LOCK(cs_dd_wallet);
    WriteDDBalance(addr, balance);
}

void DigiDollarWallet::AddMockPosition(const uint256& id, CAmount dd, CAmount dgb, uint32_t tier, int64_t height) {
    LOCK(cs_dd_wallet);
    WalletCollateralPosition position(id, dd, dgb, tier, height);

    // For testing without a wallet pointer, directly update in-memory cache
    if (!m_wallet) {
        collateral_positions[id] = position;

        // FIX #1: Also add DD UTXO to dd_utxos map
        // DD UTXOs are always at output index 1 of DDTimeLock mint transactions
        COutPoint dd_utxo(id, 1);
        dd_utxos[dd_utxo] = dd;

        // FIX #2: Generate and store owner key for this position (needed for transfers)
        CKey ownerKey;
        ownerKey.MakeNewKey(true);
        StoreOwnerKey(id, ownerKey);

        LogPrintf("DigiDollarWallet: Added mock position %s (DD: %d, DGB: %d) - NO DB\n",
                  id.ToString(), dd, dgb);
        LogPrintf("DigiDollarWallet: Added DD UTXO %s:%d with amount %d\n",
                  id.ToString(), 1, dd);
        LogPrintf("DigiDollarWallet: Stored owner key for position %s\n", id.ToString());
    } else {
        WriteDDTimeLock(position);
    }
}

void DigiDollarWallet::ClearWalletData() {
    LOCK(cs_dd_wallet);
    dd_balances.clear();
    collateral_positions.clear();
    transaction_history.clear();
    total_dd_balance = 0;
    locked_collateral = 0;

    dd_utxos.clear();

    dd_owner_keys.clear();
    dd_address_keys.clear();
    dd_crypted_owner_keys.clear();
    dd_crypted_address_keys.clear();
    dd_foreign_output_keys.clear();

    // Also clear legacy mock data
    ClearMockData();
}

// =============================================================================
// PHASE 5 VALIDATION AND HELPER FUNCTIONS
// =============================================================================

bool DigiDollarWallet::ValidateMintParams(const CAmount& dd_amount, uint32_t lock_tier) const {
    if (dd_amount <= 0) {
        LogPrintf("DigiDollar: Invalid mint amount: %d\n", dd_amount);
        return false;
    }

    if (lock_tier > 9) {
        LogPrintf("DigiDollar: Invalid lock tier: %d\n", lock_tier);
        return false;
    }

    if (dd_amount > MAX_DIGIDOLLAR) {
        LogPrintf("DigiDollar: Mint amount exceeds maximum: %d > %d\n", dd_amount, MAX_DIGIDOLLAR);
        return false;
    }

    return true;
}

bool DigiDollarWallet::ValidateTransferParams(const CDigiDollarAddress& to, const CAmount& amount) const {
    if (!to.IsValidForCurrentNetwork()) {
        LogPrintf("DigiDollar: Invalid recipient address\n");
        return false;
    }

    if (amount <= 0) {
        LogPrintf("DigiDollar: Invalid transfer amount: %d\n", amount);
        return false;
    }

    if (amount < Params().GetDigiDollarParams().minOutputAmount) {
        LogPrintf("DigiDollar: Transfer amount below minimum DD output: %d < %d\n",
                  amount, Params().GetDigiDollarParams().minOutputAmount);
        return false;
    }

    if (amount > GetTotalDDBalance()) {
        LogPrintf("DigiDollar: Transfer amount exceeds balance: %d > %d\n", amount, GetTotalDDBalance());
        return false;
    }

    return true;
}

bool DigiDollarWallet::ValidateRedeemParams(const uint256& dd_timelock_id, const CAmount& amount) const {
    LOCK(cs_dd_wallet);
    if (dd_timelock_id.IsNull()) {
        LogPrintf("DigiDollar: Invalid position ID\n");
        return false;
    }

    if (amount <= 0) {
        LogPrintf("DigiDollar: Invalid redemption amount: %d\n", amount);
        return false;
    }

    auto it = collateral_positions.find(dd_timelock_id);
    if (it == collateral_positions.end()) {
        LogPrintf("DigiDollar: Position not found: %s\n", dd_timelock_id.ToString());
        return false;
    }

    if (!it->second.is_active) {
        LogPrintf("DigiDollar: Position is not active: %s\n", dd_timelock_id.ToString());
        return false;
    }

    if (amount != it->second.dd_minted) {
        LogPrintf("DigiDollar: EXACT-AMOUNT REDEMPTION ENFORCED - amount must equal dd_minted (provided: %d, required: %d)\n",
                  amount, it->second.dd_minted);
        return false;
    }

    return true;
}

bool DigiDollarWallet::SelectDDCoins(const CAmount& target_amount, std::vector<COutPoint>& selected_utxos, CAmount& selected_total, std::vector<CAmount>* amounts) const {
    auto locks = LockDDWallet();
    // Reset output parameters
    selected_total = 0;
    selected_utxos.clear();
    if (amounts) amounts->clear();

    // Validate target amount
    if (target_amount <= 0) {
        LogPrintf("DigiDollar: SelectDDCoins - Invalid target amount %lld\n", static_cast<long long>(target_amount));
        return false;
    }

    LogPrintf("DigiDollar: SelectDDCoins - target: %lld cents\n", static_cast<long long>(target_amount));

    // Get all spendable DD UTXOs
    std::vector<DDUtxo> available_utxos = GetDDUTXOs();

    if (available_utxos.empty()) {
        LogPrintf("DigiDollar: SelectDDCoins - No DD UTXOs available\n");
        return false;
    }

    // Greedy selection: Sort by amount (smallest first for better privacy)
    std::sort(available_utxos.begin(), available_utxos.end(),
              [](const DDUtxo& a, const DDUtxo& b) {
                  return a.dd_amount < b.dd_amount;
              });

    // DD outputs have a consensus-enforced minimum amount. Since DD conservation
    // is exact, any change output must be either zero (exact spend) or at least
    // minOutputAmount. A naive "first total >= target" selector can pick a UTXO
    // that leaves sub-minimum DD change; the transaction then builds, signs, and
    // only fails at mempool admission with transfer-dd-amount-below-minimum.
    const CAmount min_change = Params().GetDigiDollarParams().minOutputAmount;

    // Select UTXOs until target amount met
    // FIXED: Now selects ALL DD UTXOs (both minted and received)
    // Signing logic properly handles both types:
    //  - Minted DD: Uses custom owner keys from dd_owner_keys map
    //  - Received DD: Uses wallet's regular key management
    for (const auto& utxo : available_utxos) {
        CAmount current_change = selected_total - target_amount;
        if (selected_total >= target_amount && (current_change == 0 || current_change >= min_change)) break;

        selected_utxos.push_back(utxo.outpoint);
        selected_total += utxo.dd_amount;

        // Store individual amounts if requested (CRITICAL FIX #7)
        if (amounts) amounts->push_back(utxo.dd_amount);

        LogPrintf("DigiDollar: SelectDDCoins - Selected UTXO %s:%u (%lld cents, total: %lld)\n",
                  utxo.outpoint.hash.ToString(), utxo.outpoint.n,
                  static_cast<long long>(utxo.dd_amount), static_cast<long long>(selected_total));
    }

    CAmount change = selected_total - target_amount;
    bool success = selected_total >= target_amount && (change == 0 || change >= min_change);

    if (!success) {
        LogPrintf("DigiDollar: SelectDDCoins - FAILED: need %lld, selected %lld, change %lld (minimum DD change is %lld unless exact)\n",
                  static_cast<long long>(target_amount), static_cast<long long>(selected_total),
                  static_cast<long long>(change), static_cast<long long>(min_change));
        selected_utxos.clear();
        selected_total = 0;
        if (amounts) amounts->clear();
    } else {
        LogPrintf("DigiDollar: SelectDDCoins - SUCCESS: selected %lld cents from %zu UTXOs (change: %lld cents)\n",
                  static_cast<long long>(selected_total), selected_utxos.size(), static_cast<long long>(change));
    }

    return success;
}

bool DigiDollarWallet::SelectDDCoins(const CAmount& target_amount,
                                     const std::vector<COutPoint>& preset_inputs,
                                     std::vector<COutPoint>& selected_utxos,
                                     CAmount& selected_total,
                                     std::vector<CAmount>* amounts,
                                     std::string* error) const
{
    auto locks = LockDDWallet();
    selected_utxos.clear();
    selected_total = 0;
    if (amounts) amounts->clear();
    if (error) error->clear();

    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        selected_utxos.clear();
        selected_total = 0;
        if (amounts) amounts->clear();
        return false;
    };

    if (target_amount <= 0) {
        return fail("Amount must be positive");
    }
    if (preset_inputs.empty()) {
        return fail("No selected DD inputs");
    }

    std::set<COutPoint> seen;
    for (const COutPoint& outpoint : preset_inputs) {
        if (!seen.insert(outpoint).second) {
            return fail(strprintf("Duplicate selected DD input: %s:%u", outpoint.hash.ToString(), outpoint.n));
        }

        const auto it = dd_utxos.find(outpoint);
        if (it == dd_utxos.end()) {
            return fail(strprintf("Selected DD input is unknown or not owned: %s:%u", outpoint.hash.ToString(), outpoint.n));
        }
        if (it->second <= 0) {
            return fail(strprintf("Selected DD input has invalid amount: %s:%u",
                                  outpoint.hash.ToString(), outpoint.n));
        }

        if (m_wallet) {
            const wallet::CWalletTx* wtx = m_wallet->GetWalletTx(outpoint.hash);
            if (!wtx || outpoint.n >= wtx->tx->vout.size()) {
                return fail(strprintf("Selected DD input is unknown or not owned by this wallet: %s:%u", outpoint.hash.ToString(), outpoint.n));
            }
            if (!IsStandardDDTokenOutput(wtx->tx->vout[outpoint.n])) {
                return fail(strprintf("Selected DD input is not a standard DigiDollar token output: %s:%u",
                                      outpoint.hash.ToString(), outpoint.n));
            }
            if (m_wallet->IsSpent(outpoint)) {
                return fail(strprintf("Selected DD input is already spent: %s:%u", outpoint.hash.ToString(), outpoint.n));
            }
            if (m_wallet->GetTxDepthInMainChain(*wtx) < 1) {
                return fail(strprintf("Selected DD input is unconfirmed: %s:%u", outpoint.hash.ToString(), outpoint.n));
            }
        }

        if (selected_total > std::numeric_limits<CAmount>::max() - it->second) {
            return fail("Selected DD input amount overflow");
        }
        selected_utxos.push_back(outpoint);
        selected_total += it->second;
        if (amounts) amounts->push_back(it->second);
    }

    if (!DDChangeIsStandard(selected_total, target_amount)) {
        return fail(DDChangePolicyError(selected_total, target_amount));
    }

    return true;
}

bool DigiDollarWallet::PlanDigiDollarTransfer(const std::vector<std::pair<CDigiDollarAddress, CAmount>>& recipients,
                                              DDTransferPlan& plan,
                                              std::string& error,
                                              const std::vector<COutPoint>* preset_dd_inputs) const
{
    auto locks = LockDDWallet();
    plan = DDTransferPlan{};
    error.clear();

    if (recipients.empty()) {
        error = "No recipients specified";
        return false;
    }

    const CAmount min_output = Params().GetDigiDollarParams().minOutputAmount;
    for (const auto& [to, amount] : recipients) {
        if (!to.IsValidForCurrentNetwork()) {
            error = "Invalid recipient address";
            return false;
        }
        if (amount <= 0) {
            error = "Amount must be positive";
            return false;
        }
        if (amount < min_output) {
            error = strprintf("Amount below minimum DigiDollar output. Minimum: %lld cents",
                              static_cast<long long>(min_output));
            return false;
        }
        if (amount > 10000000) {
            error = "Amount exceeds maximum transfer limit ($100,000)";
            return false;
        }
        if (plan.total_amount > std::numeric_limits<CAmount>::max() - amount) {
            error = "Total amount overflow";
            return false;
        }
        plan.total_amount += amount;
        plan.recipients.push_back({to.ToString(), amount});
    }

    if (preset_dd_inputs) {
        if (!SelectDDCoins(plan.total_amount, *preset_dd_inputs, plan.dd_utxos, plan.selected_dd_total, &plan.dd_amounts, &error)) return false;
    } else if (!SelectDDCoins(plan.total_amount, plan.dd_utxos, plan.selected_dd_total, &plan.dd_amounts)) {
        error = "No spendable confirmed DD UTXOs found. Please wait for prior DigiDollar transfer confirmation or confirm a mint before sending again.";
        return false;
    }

    plan.dd_change = plan.selected_dd_total - plan.total_amount;

    DigiDollar::TxBuilderTransferParams params;
    params.recipients = plan.recipients;
    params.ddUtxos = plan.dd_utxos;
    params.ddAmounts = plan.dd_amounts;
    params.feeRate = DigiDollar::MIN_DD_FEE_RATE;
    if (!PreflightDDTransferCapacity(params, plan.selected_dd_total, plan.total_amount, error, &plan.projected_vsize, &plan.estimated_fee)) return false;

    return true;
}

bool DigiDollarWallet::SelectFeeCoins(const CAmount& fee_amount, std::vector<COutPoint>& selected_utxos, CAmount& selected_total, std::vector<CAmount>* selected_amounts, const std::vector<COutPoint>* exclude_utxos, bool minimize_inputs, CAmount fee_rate) const {
    auto locks = LockDDWallet();
    // Reset output parameters
    selected_total = 0;
    selected_utxos.clear();
    if (selected_amounts) selected_amounts->clear();

    // Validate fee amount
    if (fee_amount <= 0) {
        LogPrintf("DigiDollar: SelectFeeCoins - Invalid fee amount %lld\n", static_cast<long long>(fee_amount));
        return false;
    }

    // The fee rate is what makes an input's cost - and therefore its effective
    // value - meaningful, so it has to be a real rate.
    if (fee_rate <= 0) {
        LogPrintf("DigiDollar: SelectFeeCoins - Invalid fee rate %lld sat/kvB\n", static_cast<long long>(fee_rate));
        return false;
    }

    // Check if wallet pointer exists
    if (!m_wallet) {
        LogPrintf("DigiDollar: SelectFeeCoins - No wallet available for UTXO selection\n");
        return false;
    }

    LogPrintf("DigiDollar: SelectFeeCoins - target fee: %lld satoshis\n", static_cast<long long>(fee_amount));
    if (exclude_utxos && !exclude_utxos->empty()) {
        LogPrintf("DigiDollar: SelectFeeCoins - excluding %zu UTXOs from selection\n", exclude_utxos->size());
    }

    // Get available DGB UTXOs from wallet
    std::vector<wallet::COutput> available_coins;

    // Lock wallet and get available coins
    LOCK(m_wallet->cs_wallet);
    wallet::CCoinControl coin_control;
    // Keep the wallet's normal safe coin policy. With Dandelion enabled, DD
    // transactions use confirmed DGB fee inputs only so rapid sends do not
    // build fee-change chains that can be promoted from the stempool out of
    // ancestor order.
    const bool require_confirmed_fee_inputs = gArgs.GetBoolArg("-dandelion", true);

    wallet::CoinFilterParams filter_params;
    filter_params.only_spendable = true;
    filter_params.min_amount = 1;  // Minimum 1 satoshi
    filter_params.include_immature_coinbase = false;  // Exclude immature coinbase

    available_coins = wallet::AvailableCoins(*m_wallet, &coin_control, std::nullopt, filter_params).All();

    if (available_coins.empty()) {
        LogPrintf("DigiDollar: SelectFeeCoins - No DGB UTXOs available\n");
        return false;
    }

    LogPrintf("DigiDollar: SelectFeeCoins - Found %zu available DGB UTXOs before filtering\n", available_coins.size());

    // Adding a fee input is not free: it enlarges the transaction the fee is
    // computed from. Price every candidate by its effective value (value minus
    // the cost of spending it) so the selection still covers the fee once the
    // inputs it chose are paid for.
    const CAmount input_spend_cost = DigiDollar::EstimateInputSpendCost(fee_rate);

    // Sort by amount. Smallest first by default: that is the order DD transfers
    // have always used, and it spends small UTXOs down rather than leaving them
    // behind. It is not the cheapest order - every extra input it takes adds
    // input_spend_cost to the fee actually paid - so callers that care about the
    // fee, or that must converge on an exact one, pass minimize_inputs.
    std::sort(available_coins.begin(), available_coins.end(),
              [minimize_inputs](const wallet::COutput& a, const wallet::COutput& b) {
                  return minimize_inputs ? a.txout.nValue > b.txout.nValue
                                         : a.txout.nValue < b.txout.nValue;
              });

    // Select UTXOs until the fee is covered in effective value, excluding any
    // specified UTXOs
    CAmount selected_effective = 0;
    for (const auto& coin : available_coins) {
        if (selected_effective >= fee_amount) break;
        if (require_confirmed_fee_inputs && coin.depth < 1) {
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: SelectFeeCoins - skipping unconfirmed fee UTXO %s:%u while Dandelion is enabled\n",
                     coin.outpoint.hash.ToString(), coin.outpoint.n);
            continue;
        }

        COutPoint outpoint = coin.outpoint;
        CAmount amount = coin.txout.nValue;

        // CRITICAL: Skip if this UTXO is in the exclude list (collateral or DD UTXOs)
        if (exclude_utxos) {
            bool should_exclude = false;
            for (const auto& exclude : *exclude_utxos) {
                if (outpoint == exclude) {
                    should_exclude = true;
                    LogPrintf("DigiDollar: SelectFeeCoins - EXCLUDING UTXO %s:%d (in exclude list)\n",
                              outpoint.hash.ToString(), outpoint.n);
                    break;
                }
            }
            if (should_exclude) continue;
        }

        // A UTXO worth no more than it costs to spend can never help pay the
        // fee: including it would lower the amount available to pay with.
        if (amount <= input_spend_cost) {
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: SelectFeeCoins - skipping uneconomic fee UTXO %s:%u (%lld sats, costs %lld sats to spend)\n",
                     outpoint.hash.ToString(), outpoint.n,
                     static_cast<long long>(amount), static_cast<long long>(input_spend_cost));
            continue;
        }

        selected_utxos.push_back(outpoint);
        if (selected_amounts) selected_amounts->push_back(amount);
        selected_total += amount;
        selected_effective += amount - input_spend_cost;

        LogPrintf("DigiDollar: SelectFeeCoins - Selected UTXO %s:%u (%lld sats, %lld sats effective)\n",
                  outpoint.hash.ToString(), outpoint.n, static_cast<long long>(amount),
                  static_cast<long long>(amount - input_spend_cost));
    }

    bool success = (selected_effective >= fee_amount);

    if (!success) {
        LogPrintf("DigiDollar: SelectFeeCoins - FAILED: need %lld sats, have %lld sats effective (%lld sats raw from %zu UTXOs)\n",
                  static_cast<long long>(fee_amount), static_cast<long long>(selected_effective),
                  static_cast<long long>(selected_total), selected_utxos.size());
        selected_utxos.clear();
        if (selected_amounts) selected_amounts->clear();
        selected_total = 0;
    } else {
        LogPrintf("DigiDollar: SelectFeeCoins - SUCCESS: selected %lld sats (%lld sats effective) from %zu UTXOs\n",
                  static_cast<long long>(selected_total), static_cast<long long>(selected_effective),
                  selected_utxos.size());
    }

    return success;
}

DDFeeSelectionResult DigiDollarWallet::SelectRedemptionFeeCoins(DigiDollar::TxBuilderRedeemParams& params,
                                                                std::string& error,
                                                                CAmount* projected_fee) const
{
    // Bound the select/re-project loop; each round can only raise the fee
    // target, so a handful of rounds is plenty for any convergent wallet.
    static constexpr int MAX_FEE_SELECTION_ATTEMPTS = 6;

    error.clear();
    params.feeUtxos.clear();
    params.feeAmounts.clear();

    // Never fund the fee from the collateral being released or from the DD
    // tokens being burned.
    std::vector<COutPoint> exclude_utxos;
    exclude_utxos.reserve(1 + params.ddUtxos.size());
    exclude_utxos.push_back(params.collateralOutpoint);
    exclude_utxos.insert(exclude_utxos.end(), params.ddUtxos.begin(), params.ddUtxos.end());

    // Seed the target with the fee of a redemption carrying no fee inputs; each
    // round then re-projects the transaction and raises the target by whatever
    // the inputs it picked added to the fee.
    size_t projected_vsize = 0;
    CAmount required_fee = 0;
    if (!PreflightDDRedemptionCapacity(params, error, &projected_vsize, &required_fee)) {
        return DDFeeSelectionResult::INVALID_TRANSACTION;
    }

    CAmount fee_target = required_fee;
    CAmount selected_total = 0;
    for (int attempt = 0; attempt < MAX_FEE_SELECTION_ATTEMPTS; ++attempt) {
        params.feeUtxos.clear();
        params.feeAmounts.clear();
        selected_total = 0;
        if (!SelectFeeCoins(fee_target, params.feeUtxos, selected_total, &params.feeAmounts,
                            &exclude_utxos, /*minimize_inputs=*/true, /*fee_rate=*/params.feeRate)) {
            error = strprintf("Insufficient spendable DGB for the redemption fee: need %lld sats net of the cost of spending the fee inputs (about %lld sats per input at %lld sat/kvB). Add DGB, or consolidate small DGB UTXOs into fewer, larger ones and retry.",
                              static_cast<long long>(fee_target),
                              static_cast<long long>(DigiDollar::EstimateInputSpendCost(params.feeRate)),
                              static_cast<long long>(params.feeRate));
            return DDFeeSelectionResult::INSUFFICIENT_FUNDS;
        }

        if (!PreflightDDRedemptionCapacity(params, error, &projected_vsize, &required_fee)) {
            params.feeUtxos.clear();
            params.feeAmounts.clear();
            return DDFeeSelectionResult::INVALID_TRANSACTION;
        }

        if (selected_total >= required_fee) {
            if (projected_fee) *projected_fee = required_fee;
            // required_fee is an upper bound: the projection carries a worst-case
            // output set, so the built transaction may owe slightly less.
            LogPrintf("DigiDollar: Redemption fee inputs converged after %d round(s): %zu input(s) totalling %lld sats for a projected %u vB transaction owing at most %lld sats\n",
                      attempt + 1, params.feeUtxos.size(), static_cast<long long>(selected_total),
                      static_cast<unsigned>(projected_vsize), static_cast<long long>(required_fee));
            return DDFeeSelectionResult::OK;
        }

        // Short by exactly this much once the selected inputs are paid for.
        fee_target += required_fee - selected_total;
    }

    error = strprintf("Could not assemble DGB fee inputs for this redemption after %d attempts: last selection was %zu input(s) totalling %lld sats against a projected %lld sat fee. The wallet's DGB is too fragmented - consolidate small DGB UTXOs into fewer, larger ones and retry.",
                      MAX_FEE_SELECTION_ATTEMPTS, params.feeUtxos.size(),
                      static_cast<long long>(selected_total), static_cast<long long>(required_fee));
    params.feeUtxos.clear();
    params.feeAmounts.clear();
    return DDFeeSelectionResult::INSUFFICIENT_FUNDS;
}

CAmount DigiDollarWallet::CalculateTransactionFee(const CMutableTransaction& tx) const {
    // DigiDollar fee constants (match network requirements)
    // DigiByte uses KvB (kilobyte), not vB (virtual bytes)
    // DEFAULT_MIN_RELAY_TX_FEE in policy.h is 100000 satoshis/kB (0.001 DGB/kB)
    static const CAmount MIN_RELAY_FEE_PER_KB = 100000;  // 0.001 DGB/kB (matches network min relay fee)
    // Bug #9 fix: Use the actual DD fee rate (35M sat/kB) instead of the generic 200K sat/kB.
    // DD transactions require higher fees to ensure relay at the DD minimum fee rate.
    static const CAmount DEFAULT_FEE_RATE = DigiDollar::MIN_DD_FEE_RATE;

    // DigiDollar transactions MUST pay at least 0.1 DGB fee to miners
    static const CAmount MIN_DD_TX_FEE = DigiDollar::MIN_DD_TX_FEE;

    // Calculate transaction size
    // NOTE: This is an estimate. Actual size determined after signing
    unsigned int tx_size = GetSerializeSize(tx, PROTOCOL_VERSION);

    // Add estimated witness size for P2TR inputs
    // Each P2TR witness is approximately 65 bytes (1 byte length + 64 byte Schnorr signature)
    size_t num_inputs = tx.vin.size();
    unsigned int estimated_witness_size = num_inputs * 65;
    unsigned int total_size = tx_size + estimated_witness_size;

    // Calculate fee in satoshis based on size
    // Formula: (size_in_bytes / 1000) * fee_rate_per_KB
    CAmount size_based_fee = (total_size * DEFAULT_FEE_RATE) / 1000;

    // Ensure minimum relay fee is met
    CAmount min_relay_fee = (total_size * MIN_RELAY_FEE_PER_KB) / 1000;
    if (size_based_fee < min_relay_fee) {
        size_based_fee = min_relay_fee;
    }

    // DigiDollar transactions must pay at least 0.1 DGB fee
    CAmount fee = std::max(size_based_fee, MIN_DD_TX_FEE);

    LogPrintf("DigiDollar: CalculateTransactionFee - size: %d bytes, size_based_fee: %d sats, final_fee: %d sats (min 0.1 DGB)\n",
              total_size, size_based_fee, fee);

    return fee;
}
// =============================================================================
// PHASE 3.1: P2TR SIGNING FOR DD INPUTS (SCHNORR SIGNATURES)
// =============================================================================

bool DigiDollarWallet::SignDDInputs(CMutableTransaction& tx,
                                     const std::vector<COutPoint>& dd_utxos,
                                     const std::vector<COutPoint>& fee_utxos) {
    auto locks = LockDDWallet();
    if (!m_wallet) {
        LogPrintf("DigiDollar: SignDDInputs - No wallet available\n");
        return false;
    }
    if (m_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        LogPrintf("DigiDollar: SignDDInputs - Private keys are disabled for this wallet\n");
        return false;
    }

    LogPrintf("DigiDollar: SignDDInputs - Signing %d DD inputs and %d fee inputs using wallet's SignTransaction\n",
              dd_utxos.size(), fee_utxos.size());

    LOCK(m_wallet->cs_wallet);

    // Build coins map for ALL inputs (DD + fee)
    // The wallet's SignTransaction needs all inputs to be in the coins map
    std::map<COutPoint, Coin> coins;

    // Add DD UTXOs to coins map
    for (const auto& outpoint : dd_utxos) {
        // Get the transaction from mapWallet
        const auto mi = m_wallet->mapWallet.find(outpoint.hash);
        if (mi == m_wallet->mapWallet.end() || outpoint.n >= mi->second.tx->vout.size()) {
            LogPrintf("DigiDollar: SignDDInputs - Failed to find DD transaction %s in wallet\n",
                      outpoint.hash.ToString());
            return false;
        }

        const wallet::CWalletTx& wtx = mi->second;
        const CTxOut& txout = wtx.tx->vout[outpoint.n];

        // Get block height for the transaction
        int prev_height = wtx.state<wallet::TxStateConfirmed>() ? wtx.state<wallet::TxStateConfirmed>()->confirmed_block_height : 0;

        // Use the actual on-chain value for sighash calculation
        // DD tokens have nValue=0 on-chain - we MUST use 0 here to match network verification
        // The network validates using actual UTXO values from the chain, so we must sign with the same
        coins[outpoint] = Coin(txout, prev_height, wtx.IsCoinBase());

        LogPrintf("DigiDollar: SignDDInputs - Added DD coin for %s:%d at height %d, scriptPubKey size %d\n",
                  outpoint.hash.ToString(), outpoint.n, prev_height, txout.scriptPubKey.size());
    }

    // Add fee UTXOs to coins map
    for (const auto& outpoint : fee_utxos) {
        const auto mi = m_wallet->mapWallet.find(outpoint.hash);
        if (mi == m_wallet->mapWallet.end() || outpoint.n >= mi->second.tx->vout.size()) {
            LogPrintf("DigiDollar: SignDDInputs - Failed to find fee transaction %s in wallet\n",
                      outpoint.hash.ToString());
            return false;
        }

        const wallet::CWalletTx& wtx = mi->second;
        const CTxOut& txout = wtx.tx->vout[outpoint.n];
        int prev_height = wtx.state<wallet::TxStateConfirmed>() ? wtx.state<wallet::TxStateConfirmed>()->confirmed_block_height : 0;

        coins[outpoint] = Coin(txout, prev_height, wtx.IsCoinBase());

        LogPrintf("DigiDollar: SignDDInputs - Added fee coin for %s:%d at height %d, value %d\n",
                  outpoint.hash.ToString(), outpoint.n, prev_height, txout.nValue);
    }

    // IMPORTANT: For Taproot, we must sign fee inputs FIRST, then DD inputs
    // This is because Taproot sighash includes the witness data of other inputs
    // If we sign DD inputs first, the sighash will be different when fee inputs are added later

    // Sign fee inputs using wallet's standard signing
    if (!fee_utxos.empty()) {
        LogPrintf("DigiDollar: SignDDInputs - Signing fee inputs FIRST using wallet's SignTransaction\n");

        // Log DD input witness BEFORE SignTransaction
        for (size_t i = 0; i < dd_utxos.size(); i++) {
            LogPrintf("DigiDollar: SignDDInputs - DD input %d witness BEFORE SignTransaction: %s\n",
                      i, tx.vin[i].scriptWitness.IsNull() ? "NULL" : "NOT NULL");
        }

        // CRITICAL FIX: Wallet's SignTransaction will sign inputs it has keys for
        // This includes:
        //  - ALL fee inputs (DGB UTXOs)
        //  - RECEIVED DD inputs (wallet has keys from address generation)
        // It will NOT sign:
        //  - MINTED DD inputs (use custom owner keys not in wallet descriptors)
        //
        // IMPORTANT: We use the overload that accepts a coins map so that our
        // modified DD coins (with dummy value=1) are used for Taproot sighash.
        // The simple SignTransaction(tx) looks up coins from UTXO set which has value=0.
        std::map<int, bilingual_str> input_errors;
        bool sign_result = m_wallet->SignTransaction(tx, coins, SIGHASH_DEFAULT, input_errors);
        LogPrintf("DigiDollar: SignDDInputs - SignTransaction returned: %s\n", sign_result ? "true" : "false");
        if (!sign_result) {
            for (const auto& [idx, err] : input_errors) {
                LogPrintf("DigiDollar: SignDDInputs - Input %d error: %s\n", idx, err.original);
            }
        }

        // Log DD input witness AFTER SignTransaction
        for (size_t i = 0; i < dd_utxos.size(); i++) {
            LogPrintf("DigiDollar: SignDDInputs - DD input %d witness AFTER SignTransaction: %s\n",
                      i, tx.vin[i].scriptWitness.IsNull() ? "NULL" : "NOT NULL");
        }

        // Verify that fee inputs were actually signed
        bool all_fee_inputs_signed = true;
        for (size_t i = dd_utxos.size(); i < tx.vin.size(); i++) {
            bool has_witness = !tx.vin[i].scriptWitness.IsNull() &&
                             !tx.vin[i].scriptWitness.stack.empty();
            bool has_scriptsig = !tx.vin[i].scriptSig.empty();

            if (!has_witness && !has_scriptsig) {
                all_fee_inputs_signed = false;
                LogPrintf("DigiDollar: SignDDInputs - Fee input %d was NOT signed\n", i);
                return false;
            }

            LogPrintf("DigiDollar: SignDDInputs - Fee input %d signed (witness: %s, scriptSig: %s)\n",
                      i, has_witness ? "yes" : "no", has_scriptsig ? "yes" : "no");
        }

        if (!all_fee_inputs_signed) {
            LogPrintf("DigiDollar: SignDDInputs - Not all fee inputs were signed\n");
            return false;
        }

        LogPrintf("DigiDollar: SignDDInputs - All fee inputs signed successfully\n");
    }

    // Create PrecomputedTransactionData for proper Taproot sighash calculation
    // NOW with fee inputs already signed
    std::vector<CTxOut> prevouts;
    for (size_t idx = 0; idx < tx.vin.size(); idx++) {
        const auto& input = tx.vin[idx];
        const Coin& coin = coins.at(input.prevout);
        prevouts.push_back(coin.out);
        LogPrintf("DigiDollar: SignDDInputs - Prevout %d: amount=%d, scriptPubKey=%s\n",
                  idx, coin.out.nValue, HexStr(coin.out.scriptPubKey));
    }

    PrecomputedTransactionData txdata;
    txdata.Init(tx, std::move(prevouts), /* force=*/ true);

    // NOW manually sign DD inputs that wallet couldn't sign (minted DD with owner keys)
    // RECEIVED DD was already signed by wallet's SignTransaction above
    for (size_t i = 0; i < dd_utxos.size(); i++) {
        // Check if this input is already signed
        bool already_signed = !tx.vin[i].scriptWitness.IsNull() &&
                             !tx.vin[i].scriptWitness.stack.empty();

        if (already_signed) {
            LogPrintf("DigiDollar: SignDDInputs - DD input %d already signed by wallet (received DD)\n", i);
            continue;  // Skip - wallet already signed it
        }

        const COutPoint& outpoint = dd_utxos[i];

        // Get the owner key for this DD UTXO
        // For MINTED DD: owner key is stored in dd_owner_keys
        // For RECEIVED DD: we need to find the internal key in wallet that matches the tweaked output
        CKey ownerKey;
        bool found_key = false;

        // First, extract the output key from scriptPubKey to verify any key we find
        const Coin& coin_for_key = coins.at(outpoint);
        const CTxOut& output_for_key = coin_for_key.out;
        std::vector<unsigned char> target_output_key_verify;
        if (output_for_key.scriptPubKey.size() == 34 && output_for_key.scriptPubKey[0] == OP_1) {
            target_output_key_verify.assign(output_for_key.scriptPubKey.begin() + 2, output_for_key.scriptPubKey.end());
        }

        // FIRST: Try dd_address_keys (for received DD via getdigidollaraddress)
        // This takes priority because dd_owner_keys is indexed by txid which can return wrong key
        // for multi-output transfer transactions
        if (!target_output_key_verify.empty()) {
            XOnlyPubKey xonly_output(target_output_key_verify);
            if (GetAddressKey(xonly_output, ownerKey)) {
                found_key = true;
                LogPrintf("DigiDollar: SignDDInputs - Found key via dd_address_keys for received DD (priority lookup)\n");
            }
        }

        // SECOND: Try dd_owner_keys (for minted DD), but VERIFY the key matches this output
        if (!found_key) {
            CKey candidate_key;
            if (GetOwnerKey(outpoint.hash, candidate_key)) {
                // Verify this key produces the correct tweaked output key
                if (!target_output_key_verify.empty()) {
                    XOnlyPubKey internal_xonly(candidate_key.GetPubKey());
                    auto tweaked = internal_xonly.CreateTapTweak(nullptr);
                    if (tweaked && std::equal(target_output_key_verify.begin(), target_output_key_verify.end(),
                                             tweaked->first.begin())) {
                        ownerKey = candidate_key;
                        found_key = true;
                        LogPrintf("DigiDollar: SignDDInputs - Found verified key via dd_owner_keys for minted DD\n");
                    } else {
                        LogPrintf("DigiDollar: SignDDInputs - Key from dd_owner_keys doesn't match output (wrong key for this outpoint)\n");
                    }
                } else {
                    // Can't verify, use it anyway (legacy behavior)
                    ownerKey = candidate_key;
                    found_key = true;
                    LogPrintf("DigiDollar: SignDDInputs - Using unverified key from dd_owner_keys\n");
                }
            }
        }

        // THIRD: Fall back to wallet keystore search
        if (!found_key) {
            // This might be RECEIVED DD - try to find the internal key in wallet
            // The DD output uses a tweaked P2TR key. We need to find which wallet key
            // when tweaked matches the output key.
            LogPrintf("DigiDollar: SignDDInputs - No owner key in dd_owner_keys, trying wallet keystore for received DD\n");

            // Get the output key from the scriptPubKey
            const Coin& search_coin = coins.at(outpoint);
            const CTxOut& search_output = search_coin.out;

            LogPrintf("DigiDollar: SignDDInputs - scriptPubKey: %s (size=%d)\n",
                      HexStr(search_output.scriptPubKey), search_output.scriptPubKey.size());

            if (search_output.scriptPubKey.size() == 34 && search_output.scriptPubKey[0] == OP_1) {
                std::vector<unsigned char> target_output_key(search_output.scriptPubKey.begin() + 2, search_output.scriptPubKey.end());
                LogPrintf("DigiDollar: SignDDInputs - Target output key (tweaked): %s\n", HexStr(target_output_key));

                // FIRST: Try dd_address_keys map (for received DD tokens via getdigidollaraddress)
                XOnlyPubKey xonly_output_key(target_output_key);
                if (GetAddressKey(xonly_output_key, ownerKey)) {
                    found_key = true;
                    LogPrintf("DigiDollar: SignDDInputs - Found key via dd_address_keys for received DD\n");
                }

                // Helper to get signing provider with private key access for descriptor wallets
                auto getSigningProviderWithKeys = [this](const CScript& script) -> std::unique_ptr<SigningProvider> {
                    const auto& spk_mans = m_wallet->GetScriptPubKeyMans(script);
                    if (!spk_mans.empty()) {
                        wallet::ScriptPubKeyMan* spk_man = *spk_mans.begin();
                        wallet::DescriptorScriptPubKeyMan* desc_spk_man = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(spk_man);
                        if (desc_spk_man) {
                            LogPrintf("DigiDollar: SignDDInputs - Using DescriptorScriptPubKeyMan with private keys\n");
                            return desc_spk_man->GetSigningProviderWithKeys(script);
                        }
                    }
                    // Fallback for legacy wallets
                    return m_wallet->GetSolvingProvider(script);
                };

                // Try to find the internal key by iterating through wallet keys
                // This is the key that when tweaked produces the target output key
                // For descriptor wallets, we need GetSigningProviderWithKeys
                if (!found_key) {
                auto provider = getSigningProviderWithKeys(search_output.scriptPubKey);
                LogPrintf("DigiDollar: SignDDInputs - GetSigningProvider returned: %s\n", provider ? "valid" : "nullptr");

                if (provider) {
                    // The provider knows about this script - try to get the key
                    // For Taproot, the key in WitnessV1Taproot is the tweaked output key
                    // We need the internal key for signing

                    // Extract destination and see if we can get signing info
                    CTxDestination dest;
                    if (ExtractDestination(search_output.scriptPubKey, dest)) {
                        LogPrintf("DigiDollar: SignDDInputs - ExtractDestination succeeded, dest type index: %d\n", dest.index());
                        if (auto* tr = std::get_if<WitnessV1Taproot>(&dest)) {
                            LogPrintf("DigiDollar: SignDDInputs - Destination is WitnessV1Taproot\n");
                            // Check if we have a key for this via TaprootSpendData
                            TaprootSpendData spenddata;
                            if (provider->GetTaprootSpendData(XOnlyPubKey(*tr), spenddata)) {
                                LogPrintf("DigiDollar: SignDDInputs - GetTaprootSpendData succeeded, internal_key valid: %s\n",
                                          spenddata.internal_key.IsFullyValid() ? "yes" : "no");
                                // For key-path spending, the internal key is what we need
                                // Try to get the private key for the internal key
                                if (spenddata.internal_key.IsFullyValid()) {
                                    LogPrintf("DigiDollar: SignDDInputs - Internal key: %s\n",
                                              HexStr(Span<const unsigned char>(spenddata.internal_key.begin(), 32)));
                                    CKeyID keyid = CKeyID(Hash160(std::vector<unsigned char>(
                                        spenddata.internal_key.begin(),
                                        spenddata.internal_key.begin() + 32)));
                                    LogPrintf("DigiDollar: SignDDInputs - Trying GetKey with keyid: %s\n", keyid.ToString());

                                    // Try getting key via FlatSigningProvider
                                    if (provider->GetKey(keyid, ownerKey)) {
                                        LogPrintf("DigiDollar: SignDDInputs - GetKey succeeded!\n");
                                        // Verify this key when tweaked matches the output
                                        XOnlyPubKey internal_xonly(ownerKey.GetPubKey());
                                        auto tweaked = internal_xonly.CreateTapTweak(nullptr);
                                        if (tweaked && std::equal(target_output_key.begin(), target_output_key.end(),
                                                                 tweaked->first.begin())) {
                                            found_key = true;
                                            LogPrintf("DigiDollar: SignDDInputs - Found internal key via GetTaprootSpendData\n");
                                        } else {
                                            LogPrintf("DigiDollar: SignDDInputs - GetKey succeeded but tweak doesn't match target\n");
                                        }
                                    } else {
                                        LogPrintf("DigiDollar: SignDDInputs - GetKey failed for keyid\n");
                                        // Try GetKeyByXOnly instead
                                        if (provider->GetKeyByXOnly(spenddata.internal_key, ownerKey)) {
                                            LogPrintf("DigiDollar: SignDDInputs - GetKeyByXOnly succeeded!\n");
                                            XOnlyPubKey internal_xonly(ownerKey.GetPubKey());
                                            auto tweaked = internal_xonly.CreateTapTweak(nullptr);
                                            if (tweaked && std::equal(target_output_key.begin(), target_output_key.end(),
                                                                     tweaked->first.begin())) {
                                                found_key = true;
                                                LogPrintf("DigiDollar: SignDDInputs - Found internal key via GetKeyByXOnly\n");
                                            } else {
                                                LogPrintf("DigiDollar: SignDDInputs - GetKeyByXOnly succeeded but tweak doesn't match\n");
                                            }
                                        } else {
                                            LogPrintf("DigiDollar: SignDDInputs - GetKeyByXOnly also failed\n");
                                        }
                                    }
                                }
                            } else {
                                LogPrintf("DigiDollar: SignDDInputs - GetTaprootSpendData failed\n");
                            }
                        } else {
                            LogPrintf("DigiDollar: SignDDInputs - Destination is NOT WitnessV1Taproot\n");
                        }
                    } else {
                        LogPrintf("DigiDollar: SignDDInputs - ExtractDestination failed\n");
                    }
                }

                // If still not found, try a brute force search through wallet keys
                if (!found_key) {
                    LogPrintf("DigiDollar: SignDDInputs - Starting brute force wallet key scan (mapWallet size: %d)\n",
                              m_wallet->mapWallet.size());
                    int pkh_count = 0, p2tr_count = 0;
                    // Get all keys from mapWallet transactions and try each
                    for (const auto& [txid, wtx] : m_wallet->mapWallet) {
                        for (size_t n = 0; n < wtx.tx->vout.size(); n++) {
                            CTxDestination out_dest;
                            if (ExtractDestination(wtx.tx->vout[n].scriptPubKey, out_dest)) {
                                // Check if this output belongs to our wallet and has a key (WITH PRIVATE KEY ACCESS)
                                auto out_provider = getSigningProviderWithKeys(wtx.tx->vout[n].scriptPubKey);
                                if (out_provider) {
                                    // Try all key types
                                    if (auto* pkh = std::get_if<PKHash>(&out_dest)) {
                                        pkh_count++;
                                        CKey test_key;
                                        if (out_provider->GetKey(ToKeyID(*pkh), test_key)) {
                                            XOnlyPubKey test_xonly(test_key.GetPubKey());
                                            auto tweaked = test_xonly.CreateTapTweak(nullptr);
                                            if (tweaked && std::equal(target_output_key.begin(), target_output_key.end(),
                                                                     tweaked->first.begin())) {
                                                ownerKey = test_key;
                                                found_key = true;
                                                LogPrintf("DigiDollar: SignDDInputs - Found matching key via wallet PKH scan\n");
                                                break;
                                            }
                                        }
                                    }
                                    // Also try Taproot destinations (WitnessV1Taproot)
                                    // The output key in P2TR is already tweaked, so we need to find
                                    // the internal key that produces this tweaked output
                                    else if (auto* tr = std::get_if<WitnessV1Taproot>(&out_dest)) {
                                        p2tr_count++;
                                        // For our wallet's own P2TR outputs, we can get the internal key
                                        // via GetTaprootSpendData
                                        TaprootSpendData tr_spenddata;
                                        if (out_provider->GetTaprootSpendData(XOnlyPubKey(*tr), tr_spenddata)) {
                                            if (tr_spenddata.internal_key.IsFullyValid()) {
                                                // Try to get the private key for this internal key
                                                CKey test_key;
                                                if (out_provider->GetKeyByXOnly(tr_spenddata.internal_key, test_key)) {
                                                    // Verify this key when tweaked matches our target output
                                                    XOnlyPubKey test_xonly(test_key.GetPubKey());
                                                    auto tweaked = test_xonly.CreateTapTweak(nullptr);
                                                    if (tweaked && std::equal(target_output_key.begin(), target_output_key.end(),
                                                                             tweaked->first.begin())) {
                                                        ownerKey = test_key;
                                                        found_key = true;
                                                        LogPrintf("DigiDollar: SignDDInputs - Found matching key via wallet P2TR scan (internal key)\n");
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (found_key) break;
                    }
                    LogPrintf("DigiDollar: SignDDInputs - Brute force scan complete: checked %d PKH, %d P2TR outputs, found_key=%s\n",
                              pkh_count, p2tr_count, found_key ? "true" : "false");
                }
                } // end if (!found_key) - wallet key lookup
            }
        }

        if (!found_key) {
            LogPrintf("DigiDollar: SignDDInputs - DD input %d not signed and no owner key found for %s\n",
                      i, outpoint.hash.ToString());
            return false;
        }

        CPubKey ownerPubKey = ownerKey.GetPubKey();
        XOnlyPubKey ownerXOnly(ownerPubKey);

        LogPrintf("DigiDollar: SignDDInputs - Owner compressed pubkey: %s\n", HexStr(ownerPubKey));
        LogPrintf("DigiDollar: SignDDInputs - Owner x-only pubkey: %s\n", HexStr(ownerXOnly));

        // Get the CTxOut for signing
        const Coin& coin = coins.at(outpoint);
        const CTxOut& prevOutput = coin.out;

        // Verify it's a valid Taproot output
        if (prevOutput.scriptPubKey.size() != 34 || prevOutput.scriptPubKey[0] != OP_1) {
            LogPrintf("DigiDollar: SignDDInputs - Invalid Taproot output for input %d\n", i);
            return false;
        }

        // Extract the output key from the script (this is the TWEAKED key = internal_key + merkle_root_hash)
        std::vector<unsigned char> outputKeyBytes(prevOutput.scriptPubKey.begin() + 2, prevOutput.scriptPubKey.end());
        LogPrintf("DigiDollar: SignDDInputs - Actual output key in script: %s\n", HexStr(outputKeyBytes));

        // CRITICAL: Check if this is a DD output (vout 1) or collateral output (vout 0)
        // DD outputs are simple P2TR with key-path only
        // Collateral outputs have MAST and require script-path spending
        //
        // To distinguish: first check if there's a collateral position for this outpoint
        // If there IS a collateral position -> script-path signing
        // If there is NO collateral position -> it's a DD token output -> key-path signing

        // First, check if this is a collateral output by looking for a registered position
        WalletCollateralPosition position;
        bool is_collateral = false;
        for (const auto& [pos_id, pos] : collateral_positions) {
            if (pos_id == outpoint.hash) {
                COutPoint collateral_outpoint(pos_id, 0);
                auto tx_it = m_wallet->mapWallet.find(pos_id);
                if (tx_it != m_wallet->mapWallet.end() && tx_it->second.tx) {
                    MintOutputIndexes mint_outputs;
                    if (FindMintOutputIndexes(*tx_it->second.tx, mint_outputs)) {
                        collateral_outpoint = COutPoint(pos_id, mint_outputs.collateral_index);
                    }
                }
                if (outpoint == collateral_outpoint) {
                    position = pos;
                    is_collateral = true;
                    break;
                }
            }
        }

        if (!is_collateral) {
            // This is a DD token output - use KEY-PATH signing
            // DD outputs use standard Taproot P2TR with tweaked key (key-path only, no merkle root)
            LogPrintf("DigiDollar: SignDDInputs - Output %s:%d is DD token (no collateral position), using key-path signing\n",
                      outpoint.hash.ToString(), outpoint.n);

            // Compute the expected tweaked output key (same tweak as CreateDigiDollarP2TR)
            auto tweaked = ownerXOnly.CreateTapTweak(nullptr);  // nullptr = no merkle root
            if (!tweaked) {
                LogPrintf("DigiDollar: SignDDInputs - Failed to create tap tweak for key-path signing\n");
                return false;
            }
            XOnlyPubKey expected_output_key = tweaked->first;

            // Verify the output key matches the tweaked key
            if (outputKeyBytes.size() != 32 ||
                !std::equal(outputKeyBytes.begin(), outputKeyBytes.end(), expected_output_key.begin())) {
                LogPrintf("DigiDollar: SignDDInputs - Output key mismatch for key-path (expected tweaked: %s, got: %s)\n",
                         HexStr(expected_output_key), HexStr(outputKeyBytes));
                return false;
            }

            LogPrintf("DigiDollar: SignDDInputs - Using KEY-PATH signing for DD token (tweaked key)\n");

            // Calculate sighash for Taproot KEY-PATH spending
            uint256 sighash;
            ScriptExecutionData execdata;
            execdata.m_annex_init = true;
            execdata.m_annex_present = false;
            // For key-path: NO tapleaf hash (that's only for script-path)
            execdata.m_tapleaf_hash_init = false;

            if (!SignatureHashSchnorr(sighash, execdata, tx, i, SIGHASH_DEFAULT, SigVersion::TAPROOT, txdata, MissingDataBehavior::FAIL)) {
                LogPrintf("DigiDollar: SignDDInputs - Failed to compute key-path sighash for input %d\n", i);
                return false;
            }

            LogPrintf("DigiDollar: SignDDInputs - KEY-PATH sighash: %s\n", sighash.ToString());

            // Sign for Taproot key-path spending
            // For BIP-341 key-path spending, we need to apply the standard tweak:
            // - Wallet stores the INTERNAL private key
            // - Output uses the TWEAKED public key (from CreateTapTweak(nullptr))
            // - SignSchnorr with empty merkle root applies the same standard tweak
            std::vector<unsigned char> sig(64);
            uint256 aux = GetRandHash();
            uint256 empty_merkle_root;  // Zero hash = standard key-path tweak

            if (!ownerKey.SignSchnorr(sighash, sig, &empty_merkle_root, aux)) {
                LogPrintf("DigiDollar: SignDDInputs - Failed to create key-path signature for input %d\n", i);
                return false;
            }

            LogPrintf("DigiDollar: SignDDInputs - Created key-path signature: %s\n", HexStr(sig));

            // For Taproot KEY-PATH spending, witness stack is: [signature]
            tx.vin[i].scriptWitness.stack.clear();
            tx.vin[i].scriptWitness.stack.push_back(sig);

            LogPrintf("DigiDollar: SignDDInputs - KEY-PATH witness stack: sig (%d bytes)\n", sig.size());
            continue;  // Move to next input
        }

        // This is collateral - position already found above
        // Use the position data to reconstruct MAST tree
        LogPrintf("DigiDollar: SignDDInputs - Output %s:%d is collateral, using script-path signing\n",
                  outpoint.hash.ToString(), outpoint.n);

        // Rebuild the MAST tree using the same parameters as mint
        TaprootBuilder builder;

        // Recreate redemption path scripts (same as CreateCollateralP2TR)
        DigiDollar::MintParams scriptParams;
        scriptParams.ddAmount = position.dd_minted;
        scriptParams.lockHeight = position.unlock_height;
        scriptParams.ownerKey = ownerXOnly;
        scriptParams.internalKey = DigiDollar::GetCollateralNUMSKey();  // Must match mint (txbuilder.cpp:199)
        scriptParams.oracleKeys = DigiDollar::GetOracleKeys(15); // Same as mint

        // Add the 2 redemption paths in the same order as CreateCollateralP2TR in scripts.cpp
        // DigiDollar uses exactly 2 MAST redemption paths:
        // 1. Normal path: CLTV + owner signature (system health >= 100%)
        // 2. ERR path: CLTV + OP_CHECKCOLLATERAL + owner signature (system health < 100%)
        //
        // CRITICAL: Both paths are at depth 1 to match CreateCollateralP2TR
        // NOTE: Partial redemption and Emergency oracle override are NOT supported.
        CScript normalPath = DigiDollar::CreateNormalRedemptionPath(scriptParams);
        if (!normalPath.empty()) {
            builder.Add(1, normalPath, 0xC0);  // Leaf version 0xC0 for Tapscript
        }

        CScript errPath = DigiDollar::CreateERRPath(scriptParams);
        if (!errPath.empty()) {
            builder.Add(1, errPath, 0xC0);  // Leaf version 0xC0 for Tapscript
        }

        // Finalize with the NUMS internal key to match mint (txbuilder.cpp:199)
        builder.Finalize(DigiDollar::GetCollateralNUMSKey());

        if (!builder.IsValid() || !builder.IsComplete()) {
            LogPrintf("DigiDollar: SignDDInputs - Failed to rebuild Taproot tree for signing\n");
            return false;
        }

        // Get the TaprootSpendData which contains the merkle root
        TaprootSpendData spend_data = builder.GetSpendData();

        // Verify the output key matches what we expect
        XOnlyPubKey computed_output_key(outputKeyBytes);
        WitnessV1Taproot expected_output = builder.GetOutput();

        LogPrintf("DigiDollar: SignDDInputs - Reconstructed merkle root: %s\n",
                  spend_data.merkle_root.IsNull() ? "NULL" : HexStr(spend_data.merkle_root));
        LogPrintf("DigiDollar: SignDDInputs - Expected output key from builder: %s\n",
                  HexStr(expected_output));

        // CRITICAL: Use SCRIPT-PATH spending to execute the Normal Redemption Path
        // For redemption, we need to execute the OP_CHECKLOCKTIMEVERIFY script,
        // which requires script-path spending, NOT key-path spending.

        // Get the control block for the normal redemption path from spend_data
        std::pair<CScript, int> script_key = {normalPath, TAPROOT_LEAF_TAPSCRIPT};
        auto it = spend_data.scripts.find(script_key);
        if (it == spend_data.scripts.end() || it->second.empty()) {
            LogPrintf("DigiDollar: SignDDInputs - Control block not found for normal redemption path\n");
            return false;
        }

        // Get the shortest control block (most efficient)
        std::vector<unsigned char> control_block = *it->second.begin();

        LogPrintf("DigiDollar: SignDDInputs - Found control block (%d bytes) for normal redemption\n",
                  control_block.size());

        // Calculate the leaf hash for the normal redemption script
        uint256 leaf_hash = ComputeTapleafHash(TAPROOT_LEAF_TAPSCRIPT, normalPath);

        LogPrintf("DigiDollar: SignDDInputs - Leaf hash: %s\n", leaf_hash.ToString());

        // Create Schnorr signature for Taproot SCRIPT-PATH spending
        std::vector<unsigned char> sig(64); // Schnorr signatures are always 64 bytes

        // Calculate sighash for Taproot script-path spending
        ScriptExecutionData execdata;
        execdata.m_annex_init = true;
        execdata.m_annex_present = false;
        execdata.m_tapleaf_hash = leaf_hash;
        execdata.m_tapleaf_hash_init = true;
        execdata.m_codeseparator_pos_init = true;
        execdata.m_codeseparator_pos = 0xFFFFFFFF; // No OP_CODESEPARATOR in our script

        uint256 sighash;
        if (!SignatureHashSchnorr(sighash, execdata, tx, i, SIGHASH_DEFAULT, SigVersion::TAPSCRIPT, txdata, MissingDataBehavior::FAIL)) {
            LogPrintf("DigiDollar: SignDDInputs - Failed to compute Tapscript sighash for input %d\n", i);
            return false;
        }

        LogPrintf("DigiDollar: SignDDInputs - SCRIPT-PATH SIGNING - input %d, sighash: %s\n",
                  i, sighash.ToString());

        // Generate auxiliary randomness for Schnorr signing
        uint256 aux = GetRandHash();

        // CRITICAL FIX: Sign with UNTWEAKED internal key for script-path spending
        // The leaf hash is already committed in the sighash (via execdata.m_tapleaf_hash)
        // Passing &leaf_hash would TWEAK the key (only correct for key-path spending)
        // For script-path: Sign with untweaked key, verify against pubkey in script
        if (!ownerKey.SignSchnorr(sighash, sig, nullptr, aux)) {
            LogPrintf("DigiDollar: SignDDInputs - Failed to create Schnorr signature for input %d\n", i);
            return false;
        }

        LogPrintf("DigiDollar: SignDDInputs - Created signature: %s\n", HexStr(sig));

        // For Taproot SCRIPT-PATH spending, witness stack is:
        // [signature, script, control_block]
        tx.vin[i].scriptWitness.stack.clear();
        tx.vin[i].scriptWitness.stack.push_back(sig);
        tx.vin[i].scriptWitness.stack.push_back(std::vector<unsigned char>(normalPath.begin(), normalPath.end()));
        tx.vin[i].scriptWitness.stack.push_back(control_block);

        LogPrintf("DigiDollar: SignDDInputs - Witness stack: sig (%d bytes) + script (%d bytes) + control (%d bytes)\n",
                  sig.size(), normalPath.size(), control_block.size());
    }

    LogPrintf("DigiDollar: SignDDInputs - Successfully signed all inputs (%d DD + %d fee)\n",
              dd_utxos.size(), fee_utxos.size());
    return true;
}


// =============================================================================
// PHASE 3.2: FEE INPUT SIGNING IMPLEMENTATION
// =============================================================================

bool DigiDollarWallet::SignFeeInputs(CMutableTransaction& tx,
                                      const std::vector<COutPoint>& fee_utxos,
                                      size_t dd_input_count) {
    auto locks = LockDDWallet();
    // Validate that we have fee inputs to sign
    if (fee_utxos.empty()) {
        LogPrintf("DigiDollar: SignFeeInputs - No fee UTXOs provided\n");
        return true; // No fee inputs to sign is valid (fee-less tx)
    }

    if (!m_wallet) {
        LogPrintf("DigiDollar: SignFeeInputs - No wallet available (test mode)\n");
        return false; // Can't sign without wallet
    }
    if (m_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        LogPrintf("DigiDollar: SignFeeInputs - Private keys are disabled for this wallet\n");
        return false;
    }

    // Fee inputs come after DD inputs in the transaction
    size_t fee_input_start = dd_input_count;

    // Validate transaction structure
    if (tx.vin.size() < fee_input_start + fee_utxos.size()) {
        LogPrintf("DigiDollar: SignFeeInputs - Transaction missing fee inputs (have %d, need %d)\n",
                  tx.vin.size(), fee_input_start + fee_utxos.size());
        return false;
    }

    LogPrintf("DigiDollar: SignFeeInputs - Signing %d fee inputs starting at index %d\n",
              fee_utxos.size(), fee_input_start);

    // Helper to get signing provider with private key access for descriptor wallets
    auto getSigningProviderWithKeys = [this](const CScript& script) -> std::unique_ptr<SigningProvider> {
        const auto& spk_mans = m_wallet->GetScriptPubKeyMans(script);
        if (!spk_mans.empty()) {
            wallet::ScriptPubKeyMan* spk_man = *spk_mans.begin();
            wallet::DescriptorScriptPubKeyMan* desc_spk_man = dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(spk_man);
            if (desc_spk_man) {
                LogPrintf("DigiDollar: SignFeeInputs - Using DescriptorScriptPubKeyMan with private keys\n");
                return desc_spk_man->GetSigningProviderWithKeys(script);
            }
        }
        // Fallback for legacy wallets
        return m_wallet->GetSolvingProvider(script);
    };

    // Lock wallet for thread-safe access
    LOCK(m_wallet->cs_wallet);

    // Sign each fee input
    for (size_t i = 0; i < fee_utxos.size(); i++) {
        size_t input_index = fee_input_start + i;
        const COutPoint& utxo = fee_utxos[i];

        // Get UTXO details from wallet
        const auto& it = m_wallet->mapWallet.find(utxo.hash);
        if (it == m_wallet->mapWallet.end()) {
            LogPrintf("DigiDollar: SignFeeInputs - Fee UTXO %s not found in wallet\n",
                      utxo.ToString());
            return false;
        }

        const wallet::CWalletTx& wtx = it->second;
        if (utxo.n >= wtx.tx->vout.size()) {
            LogPrintf("DigiDollar: SignFeeInputs - Invalid output index %d for UTXO %s\n",
                      utxo.n, utxo.hash.ToString());
            return false;
        }

        const CTxOut& prev_out = wtx.tx->vout[utxo.n];
        CAmount value = prev_out.nValue;
        const CScript& prevScript = prev_out.scriptPubKey;

        LogPrintf("DigiDollar: SignFeeInputs - Signing input %d: UTXO %s:%d, value: %d sats\n",
                  input_index, utxo.hash.ToString(), utxo.n, value);

        // Get the signing provider for this script WITH PRIVATE KEY ACCESS
        std::unique_ptr<SigningProvider> provider = getSigningProviderWithKeys(prevScript);
        if (!provider) {
            LogPrintf("DigiDollar: SignFeeInputs - No signing provider for fee input %d\n", input_index);
            return false;
        }

        // Create signature data structure
        SignatureData sigdata;

        // Sign the input using the signing provider
        // MutableTransactionSignatureCreator handles both P2TR and P2WPKH
        MutableTransactionSignatureCreator creator(tx, input_index, value, SIGHASH_ALL);

        // Use ProduceSignature to create the signature
        // This automatically handles P2TR (Taproot) and P2WPKH (SegWit) inputs
        bool sign_success = ProduceSignature(*provider, creator, prevScript, sigdata);

        if (!sign_success) {
            LogPrintf("DigiDollar: SignFeeInputs - Failed to sign fee input %d (UTXO %s:%d)\n",
                      input_index, utxo.hash.ToString(), utxo.n);
            return false;
        }

        // Update the transaction input with the signature
        UpdateInput(tx.vin[input_index], sigdata);

        LogPrintf("DigiDollar: SignFeeInputs - Successfully signed fee input %d\n", input_index);
    }

    LogPrintf("DigiDollar: SignFeeInputs - Successfully signed all %d fee inputs\n", fee_utxos.size());
    return true;
}

// =============================================================================
// PHASE 3.3: COMPLETE TRANSACTION SIGNING COORDINATION
// =============================================================================

bool DigiDollarWallet::SignTransaction(CMutableTransaction& tx,
                                        const std::vector<COutPoint>& dd_utxos,
                                        const std::vector<COutPoint>& fee_utxos) {
    auto locks = LockDDWallet();
    LogPrintf("DigiDollar: SignTransaction - Signing %d DD inputs and %d fee inputs\n",
              dd_utxos.size(), fee_utxos.size());

    // Sign all inputs together (DD + fee) using wallet's SignTransaction
    // SignDDInputs now handles both DD and fee inputs in one call
    if (!SignDDInputs(tx, dd_utxos, fee_utxos)) {
        LogPrintf("DigiDollar: SignTransaction - Failed to sign inputs\n");
        return false;
    }

    // Verify all inputs are signed (have witness or scriptSig)
    for (size_t i = 0; i < tx.vin.size(); i++) {
        bool has_witness = !tx.vin[i].scriptWitness.IsNull() && !tx.vin[i].scriptWitness.stack.empty();
        bool has_scriptsig = !tx.vin[i].scriptSig.empty();

        if (!has_witness && !has_scriptsig) {
            LogPrintf("DigiDollar: SignTransaction - Input %d not signed (no witness or scriptSig)\n", i);
            return false;
        }
    }

    LogPrintf("DigiDollar: SignTransaction - Successfully signed all inputs\n");
    return true;
}

// =============================================================================
// REDEMPTION-SPECIFIC SIGNING (INCLUDES COLLATERAL INPUT)
// =============================================================================

bool DigiDollarWallet::SignRedemptionTransaction(CMutableTransaction& tx,
                                                  const COutPoint& collateral_outpoint,
                                                  const std::vector<COutPoint>& dd_utxos,
                                                  const std::vector<COutPoint>& fee_utxos,
                                                  const CKey& owner_key) {
    auto locks = LockDDWallet();
    LogPrintf("DigiDollar: SignRedemptionTransaction - Signing collateral + %d DD inputs + %d fee inputs\n",
              dd_utxos.size(), fee_utxos.size());

    if (!m_wallet) {
        LogPrintf("DigiDollar: SignRedemptionTransaction - No wallet available\n");
        return false;
    }
    if (m_wallet->IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        LogPrintf("DigiDollar: SignRedemptionTransaction - Private keys are disabled for this wallet\n");
        return false;
    }
    if (!RefreshPositionMetadataFromMintTx(collateral_outpoint.hash)) {
        LogPrintf("DigiDollar: SignRedemptionTransaction - Mint metadata verification failed for %s\n",
                  collateral_outpoint.hash.ToString());
        return false;
    }
    COutPoint expected_collateral_outpoint;
    if (!GetMintCollateralOutpoint(collateral_outpoint.hash, expected_collateral_outpoint) ||
        expected_collateral_outpoint != collateral_outpoint) {
        LogPrintf("DigiDollar: SignRedemptionTransaction - Collateral outpoint mismatch for %s (got %u, expected %u)\n",
                  collateral_outpoint.hash.ToString(), collateral_outpoint.n,
                  expected_collateral_outpoint.n);
        return false;
    }

    LOCK(m_wallet->cs_wallet);

    // Build coins map for ALL inputs (collateral + DD + fee)
    std::map<COutPoint, Coin> coins;

    // 1. Add COLLATERAL input (index 0) to coins map
    const auto collateral_mi = m_wallet->mapWallet.find(collateral_outpoint.hash);
    if (collateral_mi == m_wallet->mapWallet.end() || collateral_outpoint.n >= collateral_mi->second.tx->vout.size()) {
        LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to find collateral transaction %s in wallet\n",
                  collateral_outpoint.hash.ToString());
        return false;
    }

    const wallet::CWalletTx& collateral_wtx = collateral_mi->second;
    const CTxOut& collateral_txout = collateral_wtx.tx->vout[collateral_outpoint.n];
    int collateral_height = collateral_wtx.state<wallet::TxStateConfirmed>() ?
                           collateral_wtx.state<wallet::TxStateConfirmed>()->confirmed_block_height : 0;

    coins[collateral_outpoint] = Coin(collateral_txout, collateral_height, collateral_wtx.IsCoinBase());

    LogPrintf("DigiDollar: SignRedemptionTransaction - Added collateral coin for %s:%d at height %d, value %d\n",
              collateral_outpoint.hash.ToString(), collateral_outpoint.n, collateral_height, collateral_txout.nValue);

    // 2. Add DD UTXOs to coins map (inputs 1+)
    for (const auto& outpoint : dd_utxos) {
        const auto mi = m_wallet->mapWallet.find(outpoint.hash);
        if (mi == m_wallet->mapWallet.end() || outpoint.n >= mi->second.tx->vout.size()) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to find DD transaction %s in wallet\n",
                      outpoint.hash.ToString());
            return false;
        }

        const wallet::CWalletTx& wtx = mi->second;
        const CTxOut& txout = wtx.tx->vout[outpoint.n];
        int prev_height = wtx.state<wallet::TxStateConfirmed>() ?
                         wtx.state<wallet::TxStateConfirmed>()->confirmed_block_height : 0;

        coins[outpoint] = Coin(txout, prev_height, wtx.IsCoinBase());

        LogPrintf("DigiDollar: SignRedemptionTransaction - Added DD coin for %s:%d at height %d\n",
                  outpoint.hash.ToString(), outpoint.n, prev_height);
    }

    // 3. Add fee UTXOs to coins map
    for (const auto& outpoint : fee_utxos) {
        const auto mi = m_wallet->mapWallet.find(outpoint.hash);
        if (mi == m_wallet->mapWallet.end() || outpoint.n >= mi->second.tx->vout.size()) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to find fee transaction %s in wallet\n",
                      outpoint.hash.ToString());
            return false;
        }

        const wallet::CWalletTx& wtx = mi->second;
        const CTxOut& txout = wtx.tx->vout[outpoint.n];
        int prev_height = wtx.state<wallet::TxStateConfirmed>() ?
                         wtx.state<wallet::TxStateConfirmed>()->confirmed_block_height : 0;

        coins[outpoint] = Coin(txout, prev_height, wtx.IsCoinBase());

        LogPrintf("DigiDollar: SignRedemptionTransaction - Added fee coin for %s:%d at height %d, value %d\n",
                  outpoint.hash.ToString(), outpoint.n, prev_height, txout.nValue);
    }

    // 4. Sign fee inputs FIRST using wallet's standard signing
    if (!fee_utxos.empty()) {
        LogPrintf("DigiDollar: SignRedemptionTransaction - Signing fee inputs FIRST using wallet's SignTransaction\n");

        bool sign_result = m_wallet->SignTransaction(tx);
        LogPrintf("DigiDollar: SignRedemptionTransaction - SignTransaction returned: %s\n", sign_result ? "true" : "false");

        // Verify that fee inputs were actually signed
        // Fee inputs start at index (1 + dd_utxos.size())
        size_t fee_input_start = 1 + dd_utxos.size();
        for (size_t i = fee_input_start; i < tx.vin.size(); i++) {
            bool has_witness = !tx.vin[i].scriptWitness.IsNull() && !tx.vin[i].scriptWitness.stack.empty();
            bool has_scriptsig = !tx.vin[i].scriptSig.empty();

            if (!has_witness && !has_scriptsig) {
                LogPrintf("DigiDollar: SignRedemptionTransaction - Fee input %d was NOT signed\n", i);
                return false;
            }

            LogPrintf("DigiDollar: SignRedemptionTransaction - Fee input %d signed successfully\n", i);
        }
    }

    // 5. Create PrecomputedTransactionData for proper Taproot sighash calculation
    std::vector<CTxOut> prevouts;
    for (size_t idx = 0; idx < tx.vin.size(); idx++) {
        const auto& input = tx.vin[idx];
        const Coin& coin = coins.at(input.prevout);
        prevouts.push_back(coin.out);
        LogPrintf("DigiDollar: SignRedemptionTransaction - Prevout %d: amount=%d, scriptPubKey=%s\n",
                  idx, coin.out.nValue, HexStr(coin.out.scriptPubKey));
    }

    PrecomputedTransactionData txdata;
    txdata.Init(tx, std::move(prevouts), /* force=*/ true);

    // 6. Sign COLLATERAL input at index 0 (script-path spending)
    {
        LogPrintf("DigiDollar: SignRedemptionTransaction - Signing collateral input at index 0\n");

        // Get the position data to reconstruct the MAST tree
        WalletCollateralPosition position;
        bool found_position = false;
        for (const auto& [pos_id, pos] : collateral_positions) {
            if (pos_id == collateral_outpoint.hash) {
                position = pos;
                found_position = true;
                break;
            }
        }

        if (!found_position) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Position not found for %s\n",
                      collateral_outpoint.hash.ToString());
            return false;
        }

        // Use the owner_key parameter - it should be the correct key from RPC
        CPubKey ownerPubKey = owner_key.GetPubKey();
        XOnlyPubKey ownerXOnly(ownerPubKey);

        LogPrintf("DigiDollar: SignRedemptionTransaction - Using owner key: %s\n",
                  HexStr(ownerXOnly));

        // Rebuild the MAST tree using the same parameters as mint
        TaprootBuilder builder;

        DigiDollar::MintParams scriptParams;
        scriptParams.ddAmount = position.dd_minted;
        scriptParams.lockHeight = position.unlock_height;
        scriptParams.ownerKey = ownerXOnly;
        scriptParams.internalKey = DigiDollar::GetCollateralNUMSKey();  // Must match mint (txbuilder.cpp:199)
        scriptParams.oracleKeys = DigiDollar::GetOracleKeys(15);

        // Add the 2 redemption paths in the same order as CreateCollateralP2TR in scripts.cpp
        // DigiDollar uses exactly 2 MAST redemption paths:
        // 1. Normal path: CLTV + owner signature (system health >= 100%)
        // 2. ERR path: CLTV + OP_CHECKCOLLATERAL + owner signature (system health < 100%)
        //
        // CRITICAL: Both paths are at depth 1 to match CreateCollateralP2TR
        // NOTE: Partial redemption and Emergency oracle override are NOT supported.
        CScript normalPath = DigiDollar::CreateNormalRedemptionPath(scriptParams);
        if (!normalPath.empty()) {
            builder.Add(1, normalPath, 0xC0);  // Leaf version 0xC0 for Tapscript
        }

        CScript errPath = DigiDollar::CreateERRPath(scriptParams);
        if (!errPath.empty()) {
            builder.Add(1, errPath, 0xC0);  // Leaf version 0xC0 for Tapscript
        }

        // Finalize with the NUMS internal key to match mint (txbuilder.cpp:199)
        builder.Finalize(DigiDollar::GetCollateralNUMSKey());

        if (!builder.IsValid() || !builder.IsComplete()) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to rebuild Taproot tree for collateral\n");
            return false;
        }

        // Get the TaprootSpendData
        TaprootSpendData spend_data = builder.GetSpendData();

        // Get the control block for the normal redemption path
        std::pair<CScript, int> script_key = {normalPath, TAPROOT_LEAF_TAPSCRIPT};
        auto it = spend_data.scripts.find(script_key);
        if (it == spend_data.scripts.end() || it->second.empty()) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Control block not found for normal redemption path\n");
            return false;
        }

        std::vector<unsigned char> control_block = *it->second.begin();

        LogPrintf("DigiDollar: SignRedemptionTransaction - Found control block (%d bytes) for normal redemption\n",
                  control_block.size());

        // Calculate the leaf hash for the normal redemption script
        uint256 leaf_hash = ComputeTapleafHash(TAPROOT_LEAF_TAPSCRIPT, normalPath);

        LogPrintf("DigiDollar: SignRedemptionTransaction - Leaf hash: %s\n", leaf_hash.ToString());

        // Create Schnorr signature for Taproot SCRIPT-PATH spending
        std::vector<unsigned char> sig(64);

        // Calculate sighash for Taproot script-path spending
        ScriptExecutionData execdata;
        execdata.m_annex_init = true;
        execdata.m_annex_present = false;
        execdata.m_tapleaf_hash = leaf_hash;
        execdata.m_tapleaf_hash_init = true;
        execdata.m_codeseparator_pos_init = true;
        execdata.m_codeseparator_pos = 0xFFFFFFFF;

        uint256 sighash;
        if (!SignatureHashSchnorr(sighash, execdata, tx, 0, SIGHASH_DEFAULT, SigVersion::TAPSCRIPT, txdata, MissingDataBehavior::FAIL)) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to compute Tapscript sighash for collateral input\n");
            return false;
        }

        LogPrintf("DigiDollar: SignRedemptionTransaction - Collateral SCRIPT-PATH sighash: %s\n", sighash.ToString());

        // Generate auxiliary randomness for Schnorr signing
        uint256 aux = GetRandHash();

        // Sign with UNTWEAKED internal key for script-path spending
        if (!owner_key.SignSchnorr(sighash, sig, nullptr, aux)) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to create Schnorr signature for collateral\n");
            return false;
        }

        LogPrintf("DigiDollar: SignRedemptionTransaction - Created collateral signature: %s\n", HexStr(sig));

        // Set witness stack: [signature, script, control_block]
        tx.vin[0].scriptWitness.stack.clear();
        tx.vin[0].scriptWitness.stack.push_back(sig);
        tx.vin[0].scriptWitness.stack.push_back(std::vector<unsigned char>(normalPath.begin(), normalPath.end()));
        tx.vin[0].scriptWitness.stack.push_back(control_block);

        LogPrintf("DigiDollar: SignRedemptionTransaction - Collateral witness stack: sig (%d) + script (%d) + control (%d)\n",
                  sig.size(), normalPath.size(), control_block.size());
    }

    // 7. Sign DD inputs (indices 1, 2, 3...) using KEY-PATH spending
    // DD token outputs are simple P2TR with key-path only (no MAST, no CLTV).
    // Collateral outputs have MAST and use script-path spending.
    for (size_t i = 0; i < dd_utxos.size(); i++) {
        size_t input_index = 1 + i;  // DD inputs start at index 1 (after collateral at index 0)

        // Check if this input is already signed
        bool already_signed = !tx.vin[input_index].scriptWitness.IsNull() &&
                             !tx.vin[input_index].scriptWitness.stack.empty();

        if (already_signed) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - DD input %d already signed by wallet\n", input_index);
            continue;
        }

        const COutPoint& outpoint = dd_utxos[i];

        // Look up the owner key for this specific DD UTXO
        // It could be from:
        // - The same mint tx as collateral (use owner_key)
        // - A different mint tx (use that tx's owner key)
        // - A transfer tx (use that transfer's owner key)
        CKey ddOwnerKey;
        if (!GetOwnerKey(outpoint.hash, ddOwnerKey)) {
            const auto coin_it = coins.find(outpoint);
            if (coin_it != coins.end() && GetDDOutputSpendingKey(coin_it->second.out, ddOwnerKey)) {
                StoreOwnerKey(outpoint.hash, ddOwnerKey);
                LogPrintf("DigiDollar: SignRedemptionTransaction - Recovered owner key from DD output %s:%u\n",
                          outpoint.hash.ToString(), outpoint.n);
            }
            // Fallback to collateral's owner key if same mint tx
            else if (outpoint.hash == collateral_outpoint.hash) {
                ddOwnerKey = owner_key;
                LogPrintf("DigiDollar: SignRedemptionTransaction - Using collateral owner key for DD from same mint tx\n");
            } else {
                LogPrintf("DigiDollar: SignRedemptionTransaction - ERROR: No owner key found for DD UTXO %s\n",
                          outpoint.hash.ToString());
                return false;
            }
        } else {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Found owner key for DD UTXO %s\n",
                      outpoint.hash.ToString());
        }

        CPubKey ddOwnerPubKey = ddOwnerKey.GetPubKey();
        XOnlyPubKey ddOwnerXOnly(ddOwnerPubKey);

        LogPrintf("DigiDollar: SignRedemptionTransaction - DD input %d using owner key: %s\n",
                  input_index, HexStr(ddOwnerXOnly));

        // Get the CTxOut for signing
        const Coin& coin = coins.at(outpoint);
        const CTxOut& prevOutput = coin.out;

        // Verify it's a valid Taproot output
        if (prevOutput.scriptPubKey.size() != 34 || prevOutput.scriptPubKey[0] != OP_1) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Invalid Taproot output for DD input %d\n", input_index);
            return false;
        }

        // Extract the output key from the script (this is the TWEAKED key)
        std::vector<unsigned char> outputKeyBytes(prevOutput.scriptPubKey.begin() + 2, prevOutput.scriptPubKey.end());
        LogPrintf("DigiDollar: SignRedemptionTransaction - Actual output key in script: %s\n", HexStr(outputKeyBytes));

        // CRITICAL: DD token outputs are simple P2TR with key-path only (no MAST, no CLTV)
        // DD can be at any vout index:
        //   - the mint DD token output
        //   - vout[0] for transfer recipient DD
        //   - vout[1+] for transfer change DD
        // All use the same key-path signing approach
        LogPrintf("DigiDollar: SignRedemptionTransaction - Output %s:%d (DD token), using key-path signing\n",
                  outpoint.hash.ToString(), outpoint.n);

        // DD outputs are created with CreateDigiDollarP2TR which applies a Taproot tweak
        // (owner.CreateTapTweak(nullptr) - standard BIP-341 key-path only P2TR)
        // We must compute the tweaked key for verification and sign with the tweaked key
        auto tweaked = ddOwnerXOnly.CreateTapTweak(nullptr);  // nullptr = no merkle root
        if (!tweaked) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to compute tweaked key for DD input\n");
            return false;
        }
        XOnlyPubKey tweakedOutputKey = tweaked->first;

        // Verify the output key matches the TWEAKED pubkey (not raw)
        if (outputKeyBytes.size() != 32 ||
            !std::equal(outputKeyBytes.begin(), outputKeyBytes.end(), tweakedOutputKey.begin())) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Output key mismatch for key-path (expected tweaked: %s, got: %s)\n",
                     HexStr(tweakedOutputKey), HexStr(outputKeyBytes));
            return false;
        }

        LogPrintf("DigiDollar: SignRedemptionTransaction - Using KEY-PATH signing for DD token (tweaked pubkey)\n");

        // Calculate sighash for Taproot KEY-PATH spending
        uint256 dd_sighash;
        ScriptExecutionData dd_execdata;
        dd_execdata.m_annex_init = true;
        dd_execdata.m_annex_present = false;
        // For key-path: NO tapleaf hash (that's only for script-path)
        dd_execdata.m_tapleaf_hash_init = false;

        if (!SignatureHashSchnorr(dd_sighash, dd_execdata, tx, input_index, SIGHASH_DEFAULT, SigVersion::TAPROOT, txdata, MissingDataBehavior::FAIL)) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to compute key-path sighash for input %d\n", input_index);
            return false;
        }

        LogPrintf("DigiDollar: SignRedemptionTransaction - DD input %d KEY-PATH sighash: %s\n",
                  input_index, dd_sighash.ToString());

        // Sign for Taproot key-path spending
        // For BIP-341 key-path spending, we need to apply the standard tweak:
        // - Wallet stores the INTERNAL private key
        // - Output uses the TWEAKED public key (from CreateTapTweak(nullptr))
        // - SignSchnorr with empty merkle root applies the same standard tweak
        std::vector<unsigned char> dd_sig(64);
        uint256 dd_aux = GetRandHash();
        uint256 empty_merkle_root;  // Zero hash = standard key-path tweak

        if (!ddOwnerKey.SignSchnorr(dd_sighash, dd_sig, &empty_merkle_root, dd_aux)) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Failed to create key-path signature for input %d\n", input_index);
            return false;
        }

        LogPrintf("DigiDollar: SignRedemptionTransaction - Created key-path signature (tweaked): %s\n", HexStr(dd_sig));

        // For Taproot KEY-PATH spending, witness stack is: [signature]
        tx.vin[input_index].scriptWitness.stack.clear();
        tx.vin[input_index].scriptWitness.stack.push_back(dd_sig);

        LogPrintf("DigiDollar: SignRedemptionTransaction - DD input %d signed successfully with KEY-PATH (witness: sig %d bytes)\n",
                  input_index, dd_sig.size());
    }

    // 8. Verify all inputs are signed
    for (size_t i = 0; i < tx.vin.size(); i++) {
        bool has_witness = !tx.vin[i].scriptWitness.IsNull() && !tx.vin[i].scriptWitness.stack.empty();
        bool has_scriptsig = !tx.vin[i].scriptSig.empty();

        if (!has_witness && !has_scriptsig) {
            LogPrintf("DigiDollar: SignRedemptionTransaction - Input %d not signed\n", i);
            return false;
        }
    }

    LogPrintf("DigiDollar: SignRedemptionTransaction - Successfully signed all inputs (1 collateral + %d DD + %d fee)\n",
              dd_utxos.size(), fee_utxos.size());
    return true;
}

// =============================================================================
// PHASE 5.2: UTXO SET UPDATE IMPLEMENTATIONS
// =============================================================================

bool DigiDollarWallet::MarkDDUTXOsSpent(const std::vector<COutPoint>& spent_utxos) {
    auto locks = LockDDWallet();
    LogPrintf("DigiDollar: MarkDDUTXOsSpent - Marking %d DD UTXOs as spent\n", spent_utxos.size());

    // Validate input
    if (spent_utxos.empty()) {
        LogPrintf("DigiDollar: MarkDDUTXOsSpent - No UTXOs to mark\n");
        return true; // No UTXOs to mark is valid
    }

    // Get database batch if wallet available (for persistence)
    std::unique_ptr<wallet::WalletBatch> batch;
    if (m_wallet) {
        batch = std::make_unique<wallet::WalletBatch>(m_wallet->GetDatabase());
    }

    // Mark each UTXO as spent
    for (const auto& utxo : spent_utxos) {
        // FIX: Do NOT erase DD UTXOs from dd_utxos at TX creation time!
        // The UTXO stays in the map and is hidden from balance/UTXO queries
        // via IsSpent(). It will be properly erased when the TX confirms in a
        // block (ProcessTransactionForDD from blockConnected) or during rescan.
        // This prevents permanent DD balance loss if the TX is later abandoned.
        auto utxo_it = dd_utxos.find(utxo);
        if (utxo_it == dd_utxos.end()) {
            LogPrintf("DigiDollar: MarkDDUTXOsSpent - Warning: UTXO not found in tracking map: %s:%d\n",
                      utxo.hash.ToString(), utxo.n);
        } else {
            LogPrintf("DigiDollar: DD UTXO %s:%d (%lld DD) pending spend (will be erased on block confirm)\n",
                      utxo.hash.ToString(), utxo.n, static_cast<long long>(utxo_it->second));
        }

        // Check if this is a DDTimeLock position
        if (utxo.n == 1) {
            // Find position in cache
            auto it = collateral_positions.find(utxo.hash);
            if (it != collateral_positions.end()) {
                // Mark as inactive (spent)
                it->second.is_active = false;

                // Persist to database if wallet available
                if (batch) {
                    if (!batch->WriteDDTimeLock(it->second)) {
                        LogPrintf("DigiDollar: MarkDDUTXOsSpent - Failed to persist spent position: %s\n",
                                  utxo.hash.ToString());
                        return false;
                    }
                }

                LogPrintf("DigiDollar: Marked DDTimeLock position as spent: %s:%d (%d DD)\n",
                          utxo.hash.ToString(), utxo.n, it->second.dd_minted);
            }
        }
    }

    LogPrintf("DigiDollar: Successfully marked %d DD UTXOs as spent\n", spent_utxos.size());
    return true;
}

bool DigiDollarWallet::AddDDChangeUTXO(const CTransactionRef& tx, uint32_t change_vout, CAmount dd_amount) {
    LOCK(cs_dd_wallet);
    LogPrintf("DigiDollar: AddDDChangeUTXO - Adding change UTXO at vout[%d] with %d DD\n",
              change_vout, dd_amount);

    // Validate inputs
    if (!tx) {
        LogPrintf("DigiDollar: AddDDChangeUTXO - Null transaction reference\n");
        return false;
    }

    if (dd_amount <= 0) {
        LogPrintf("DigiDollar: AddDDChangeUTXO - Invalid DD amount: %d\n", dd_amount);
        return false;
    }

    if (change_vout >= tx->vout.size()) {
        LogPrintf("DigiDollar: AddDDChangeUTXO - Invalid vout index %d (tx has %d outputs)\n",
                  change_vout, tx->vout.size());
        return false;
    }

    // FIX #1: Add change UTXO to dd_utxos map
    COutPoint change_outpoint(tx->GetHash(), change_vout);
    dd_utxos[change_outpoint] = dd_amount;
    LogPrintf("DigiDollar: Added DD change UTXO to tracking - %s:%d (%d cents)\n",
              change_outpoint.hash.ToString(), change_outpoint.n, dd_amount);

    // Persist DD UTXO to database
    if (m_wallet) {
        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDUTXO(change_outpoint, dd_amount)) {
            LogPrintf("DigiDollar: AddDDChangeUTXO - Failed to persist change UTXO to database\n");
            return false;
        }
    }

    // Legacy: Also create WalletCollateralPosition for compatibility
    // (This can be removed once all code uses dd_utxos instead of collateral_positions)
    WalletCollateralPosition change_position;
    change_position.dd_timelock_id = tx->GetHash();
    change_position.dd_minted = dd_amount;
    change_position.dgb_collateral = 0;      // Change has no collateral
    change_position.lock_tier = 0;           // Not a mint, so no tier
    change_position.unlock_height = 0;       // Not locked
    change_position.is_active = true;        // Spendable immediately

    // Add to in-memory cache
    collateral_positions[tx->GetHash()] = change_position;

    // Persist to database if wallet available
    if (m_wallet) {
        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDTimeLock(change_position)) {
            LogPrintf("DigiDollar: AddDDChangeUTXO - Failed to persist change position\n");
            // Don't fail, we already persisted the UTXO
        }
    }

    LogPrintf("DigiDollar: Successfully added DD change UTXO: %s:%d (%d DD)\n",
              tx->GetHash().ToString(), change_vout, dd_amount);
    return true;
}

bool DigiDollarWallet::UpdateDDUTXOSet(const CTransactionRef& tx,
                                       const std::vector<COutPoint>& input_utxos,
                                       int change_vout,
                                       CAmount change_amount) {
    LOCK(cs_dd_wallet);
    LogPrintf("DigiDollar: UpdateDDUTXOSet - Updating UTXO set for tx %s\n",
              tx ? tx->GetHash().ToString() : "null");

    // Validate transaction
    if (!tx) {
        LogPrintf("DigiDollar: UpdateDDUTXOSet - Null transaction reference\n");
        return false;
    }

    // Step 1: Mark input UTXOs as spent
    if (!input_utxos.empty()) {
        if (!MarkDDUTXOsSpent(input_utxos)) {
            LogPrintf("DigiDollar: UpdateDDUTXOSet - Failed to mark inputs spent\n");
            return false;
        }
    }

    // Step 2: Add change UTXO if present
    if (change_vout >= 0 && change_amount > 0) {
        if (!AddDDChangeUTXO(tx, change_vout, change_amount)) {
            LogPrintf("DigiDollar: UpdateDDUTXOSet - Failed to add change UTXO\n");
            // Note: Inputs already marked as spent at this point
            // This is a consistency error that would need recovery
            return false;
        }
    }

    LogPrintf("DigiDollar: UTXO set updated successfully for tx %s\n",
              tx->GetHash().ToString());
    return true;
}

// =============================================================================
// PHASE 4.1: MEMPOOL SUBMISSION IMPLEMENTATION
// =============================================================================

bool DigiDollarWallet::CommitDDTransaction(const CTransactionRef& tx, std::string& error) {
    auto locks = LockDDWallet();
    // Clear previous error
    error.clear();

    LogPrintf("DigiDollar: CommitDDTransaction - Starting transaction submission\n");

    // Validate transaction reference is not null
    if (!tx) {
        error = "Transaction reference is null";
        LogPrintf("DigiDollar: CommitDDTransaction - ERROR: %s\n", error);
        return false;
    }

    // Validate wallet pointer is set (must check before transaction validation)
    if (!m_wallet) {
        error = "Wallet not initialized";
        LogPrintf("DigiDollar: CommitDDTransaction - ERROR: %s\n", error);
        return false;
    }

    // Validate transaction has inputs and outputs
    if (tx->vin.empty() || tx->vout.empty()) {
        error = "Invalid transaction: no inputs or outputs";
        LogPrintf("DigiDollar: CommitDDTransaction - ERROR: %s\n", error);
        return false;
    }

    // Log transaction details
    uint256 txid = tx->GetHash();
    LogPrintf("DigiDollar: Committing DD transfer transaction %s\n", txid.ToString());
    LogPrintf("DigiDollar: Transaction has %d inputs and %d outputs\n",
              tx->vin.size(), tx->vout.size());

    try {
        // Use CWallet::CommitTransaction to submit to mempool
        // This handles:
        // - Adding tx to wallet
        // - Marking UTXOs as spent
        // - Broadcasting to mempool
        // - Relaying to peers

        wallet::mapValue_t mapValue;  // Empty metadata for DD transfers
        std::vector<std::pair<std::string, std::string>> orderForm; // Empty order form

        std::string commit_error;
        const bool commit_success = m_wallet->CommitTransaction(
            tx, std::move(mapValue), std::move(orderForm), &commit_error);

        if (!commit_success) {
            error = commit_error.empty() ? "Transaction rejected by mempool" : commit_error;
            LogPrintf("DigiDollar: CommitDDTransaction - REJECTED: %s\n", error);
            if (m_wallet->TransactionCanBeAbandoned(txid)) {
                m_wallet->AbandonTransaction(txid);
                LogPrintf("DigiDollar: CommitDDTransaction - Abandoned rejected local transaction %s\n",
                          txid.ToString());
            }
            return false;
        }

        LogPrintf("DigiDollar: Successfully committed transaction %s to mempool\n", txid.ToString());
        return true;

    } catch (const std::exception& e) {
        error = strprintf("Failed to commit transaction: %s", e.what());
        LogPrintf("DigiDollar: CommitDDTransaction - EXCEPTION: %s\n", error);
        return false;
    }
}

// =============================================================================
// PHASE 4.3: CONFIRMATION TRACKING IMPLEMENTATION
// =============================================================================

int DigiDollarWallet::GetDDTransactionConfirmations(const uint256& txid) const {
    if (!m_wallet) {
        LogPrintf("DigiDollar: GetDDTransactionConfirmations - No wallet pointer\n");
        return 0;
    }

    LogPrint(BCLog::WALLETDB, "DigiDollar: GetDDTransactionConfirmations - Checking confirmations for %s\n",
             txid.ToString());

    // Get transaction from wallet
    LOCK(m_wallet->cs_wallet);
    auto it = m_wallet->mapWallet.find(txid);
    if (it == m_wallet->mapWallet.end()) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: Transaction %s not found in wallet\n", txid.ToString());
        return 0;
    }

    const wallet::CWalletTx& wtx = it->second;

    // Get depth in main chain (number of confirmations)
    int depth = m_wallet->GetTxDepthInMainChain(wtx);

    LogPrint(BCLog::WALLETDB, "DigiDollar: Transaction %s has %d confirmations\n",
             txid.ToString(), depth);

    return depth;
}

void DigiDollarWallet::UpdateDDConfirmations(const uint256& block_hash) {
    LOCK(cs_dd_wallet);
    LogPrintf("DigiDollar: UpdateDDConfirmations - Updating confirmations for block %s\n",
              block_hash.ToString());

    // Iterate through all DD transactions and update confirmation counts
    for (auto& ddtx : transaction_history) {
        uint256 txid;
        txid.SetHex(ddtx.txid);

        // Get updated confirmation count
        int new_confirmations = GetDDTransactionConfirmations(txid);

        // Update if changed
        if (ddtx.confirmations != new_confirmations) {
            LogPrint(BCLog::WALLETDB, "DigiDollar: Transaction %s confirmations: %d -> %d\n",
                     ddtx.txid, ddtx.confirmations, new_confirmations);

            ddtx.confirmations = new_confirmations;

            // Persist to database if wallet available
            if (m_wallet) {
                wallet::WalletBatch batch(m_wallet->GetDatabase());
                if (!batch.WriteDDTransaction(ddtx)) {
                    LogPrintf("DigiDollar: WARNING - Failed to write updated confirmations for %s\n",
                              ddtx.txid);
                }
            }
        }
    }

    LogPrintf("DigiDollar: Updated confirmations for %d DD transactions\n",
              transaction_history.size());
}

std::vector<uint256> DigiDollarWallet::GetUnconfirmedDDTransactions() const {
    LOCK(cs_dd_wallet);
    std::vector<uint256> unconfirmed;

    LogPrint(BCLog::WALLETDB, "DigiDollar: GetUnconfirmedDDTransactions - Scanning transaction history\n");

    // Iterate through transaction history
    for (const auto& ddtx : transaction_history) {
        uint256 txid;
        txid.SetHex(ddtx.txid);

        // Check if transaction has 0 confirmations
        int confirmations = GetDDTransactionConfirmations(txid);
        if (confirmations == 0) {
            unconfirmed.push_back(txid);
            LogPrint(BCLog::WALLETDB, "DigiDollar: Found unconfirmed transaction %s\n",
                     ddtx.txid);
        }
    }

    LogPrint(BCLog::WALLETDB, "DigiDollar: Found %d unconfirmed DD transactions\n",
             unconfirmed.size());

    return unconfirmed;
}

void DigiDollarWallet::ProcessIncomingTransaction(const CTransactionRef& tx, const uint256& txid) {
    auto locks = LockDDWallet();
    if (!m_wallet) {
        LogPrintf("DigiDollar: ProcessIncomingTransaction called but no wallet pointer\n");
        return;
    }
    if (!tx || GetDigiDollarTxType(*tx) == DD_TX_NONE) {
        return;
    }

    try {
        std::vector<CAmount> dd_amounts;
        int txType = static_cast<int>(GetDigiDollarTxType(*tx));

        // Skip mint transactions and redemption change for history insertion.
        // Mint history is recorded by AddCollateralPosition(); redemption history by
        // redeemdigidollar (DD change synthesized as redeem_change in
        // GetDDTransactionHistory()). Do this BEFORE parsing OP_RETURN amounts: a MINT's
        // OP_RETURN ends with a 32-byte owner pubkey push, and the amount loop below
        // builds CScriptNum(push, 8) over every trailing push -- 32 > 8 threw
        // "script number overflow" on every mint (caught below, but noisy and fragile).
        if (txType == DD_TX_MINT || txType == DD_TX_REDEEM) {
            return;
        }

        // Extract DD amounts from OP_RETURN (same logic as DetectIncomingDDOutputs)
        for (const auto& vout : tx->vout) {
            if (vout.scriptPubKey.size() > 0 && vout.scriptPubKey[0] == OP_RETURN) {
                CScript::const_iterator pc = vout.scriptPubKey.begin();
                opcodetype opcode;
                std::vector<unsigned char> data;

                // Skip OP_RETURN
                if (!vout.scriptPubKey.GetOp(pc, opcode)) break;

                // Check for "DD" marker
                if (!vout.scriptPubKey.GetOp(pc, opcode, data)) break;
                if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') break;

                // Get transaction type
                if (!vout.scriptPubKey.GetOp(pc, opcode, data)) break;
                CScriptNum txTypeNum(data, true);
                if (txTypeNum.getint() != txType) break;

                // Extract DD amounts. Amount pushes are <= 8 bytes; guard the size so a
                // malformed/oversized push can never throw "script number overflow".
                while (vout.scriptPubKey.GetOp(pc, opcode, data)) {
                    if (data.size() > 0 && data.size() <= 8) {
                        CScriptNum amount(data, true, 8);  // 8-byte max for large DD amounts
                        dd_amounts.push_back(amount.GetInt64());
                    }
                }
                break;
            }
        }

        if (dd_amounts.empty()) {
            return; // No DD amounts in this transaction
        }

        if (pending_outgoing_dd_txs.count(txid) > 0) {
            LogPrintf("DigiDollar: Skipping incoming-history scan for outgoing DD tx %s during wallet commit\n",
                      txid.GetHex());
            return;
        }

        // Now check P2TR outputs we own
        size_t dd_output_index = 0;
        for (size_t n = 0; n < tx->vout.size(); ++n) {
            const CTxOut& txout = tx->vout[n];

            // Skip OP_RETURN and non-zero value outputs
            if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) continue;
            if (txout.nValue != 0) continue;

            // Check if it's a P2TR output (OP_1 + 32 bytes = DD output)
            if (txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1) {
                // Check if we own this output
                LOCK(m_wallet->cs_wallet);
                wallet::isminetype mine = m_wallet->IsMine(txout);
                if (!(mine & wallet::ISMINE_SPENDABLE)) {
                    dd_output_index++;
                    continue; // Not ours
                }

                // Get DD amount for this output
                if (dd_output_index >= dd_amounts.size()) {
                    dd_output_index++;
                    continue;
                }
                CAmount dd_amount = dd_amounts[dd_output_index];
                dd_output_index++;

                LogPrintf("DigiDollar: Detected incoming DD transaction - txid: %s, vout: %d, amount: %d cents\n",
                          txid.GetHex(), n, dd_amount);

                // CRITICAL FIX: Register the DD amount in the global metadata registry
                // This enables validation to find DD amounts for received UTXOs during redemption.
                // Without this, multi-input redemptions fail with "bad-redeem-dd-not-burned"
                // because ExtractDDAmount() can't find amounts for UTXOs received from other wallets.
                DigiDollar::RegisterScriptMetadata(txout.scriptPubKey, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, dd_amount, 0);

                // Check if we already have a transaction with this txid (e.g., from a send)
                // This prevents "receive" for change from overwriting our "send" transaction
                bool txExists = false;
                for (const auto& existing : transaction_history) {
                    if (existing.txid == txid.GetHex()) {
                        txExists = true;
                        LogPrintf("DigiDollar: Skipping duplicate transaction %s (already exists as %s)\n",
                                  txid.GetHex(), existing.category);
                        break;
                    }
                }

                if (txExists) {
                    // Skip - this is likely our change from a send we already recorded
                    continue;
                }

                // Add to transaction history (txType already checked above, only TRANSFER here)
                DDTransaction ddtx;
                ddtx.txid = txid.GetHex();
                ddtx.amount = dd_amount;
                ddtx.timestamp = GetTime();
                ddtx.confirmations = 0; // Will be updated later
                ddtx.incoming = true;
                ddtx.address = ""; // Could extract sender address if needed
                ddtx.category = "receive";

                transaction_history.push_back(ddtx);

                // Persist to database
                if (m_wallet) {
                    wallet::WalletBatch batch(m_wallet->GetDatabase());
                    if (!batch.WriteDDTransaction(ddtx)) {
                        LogPrintf("DigiDollar: WARNING - Failed to persist received transaction to database\n");
                    }
                }

                LogPrintf("DigiDollar: Added receive transaction to history - TxID: %s, Amount: %d cents\n",
                          txid.GetHex(), dd_amount);

                // Only process first DD output (there should only be one per receive anyway)
                break;
            }
        }
    } catch (const std::exception& e) {
        LogPrintf("DigiDollar: ProcessIncomingTransaction exception - %s\n", e.what());
    }
}

// =============================================================================
// PHASE 6: RECEIVE OPERATIONS (Tasks 6.1-6.3)
// =============================================================================

/**
 * Detect if transaction has DD outputs to our wallet (Task 6.1)
 */
bool DigiDollarWallet::DetectIncomingDDOutputs(const CTransactionRef& tx,
                                               std::vector<std::pair<uint32_t, CAmount>>& our_dd_outputs) {
    auto locks = LockDDWallet();
    if (!tx) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: DetectIncomingDDOutputs - null transaction\n");
        return false;
    }

    our_dd_outputs.clear();

    LogPrint(BCLog::WALLETDB, "DigiDollar: DetectIncomingDDOutputs - Checking transaction %s\n",
             tx->GetHash().ToString());

    const DigiDollarTxType versionTxType = GetDigiDollarTxType(*tx);
    if (versionTxType == DD_TX_NONE) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: DetectIncomingDDOutputs - not a DigiDollar versioned transaction\n");
        return false;
    }

    // First, extract DD amounts from OP_RETURN
    // Format: OP_RETURN <"DD"> <txType> <amount1> <amount2> ...
    std::vector<CAmount> dd_amounts;
    for (const auto& txout : tx->vout) {
        if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) {
            // Parse OP_RETURN data
            CScript::const_iterator pc = txout.scriptPubKey.begin();
            opcodetype opcode;
            std::vector<unsigned char> data;

            // Skip OP_RETURN
            if (!txout.scriptPubKey.GetOp(pc, opcode)) continue;

            // Check for "DD" marker
            if (!txout.scriptPubKey.GetOp(pc, opcode, data)) continue;
            if (data.size() != 2 || data[0] != 'D' || data[1] != 'D') continue;

            // Get transaction type
            if (!txout.scriptPubKey.GetOp(pc, opcode, data)) continue;
            CScriptNum txType(data, true);
            int type = txType.getint();
            if (type != static_cast<int>(versionTxType)) continue;
            if (type != 2 && type != 3) continue;  // TRANSFER (2) or REDEEM (3) transactions

            // Extract DD amounts
            while (txout.scriptPubKey.GetOp(pc, opcode, data)) {
                if (data.size() > 0) {
                    CScriptNum amount(data, true, 8);  // 8-byte max for large DD amounts
                    dd_amounts.push_back(amount.GetInt64());
                    LogPrintf("DigiDollar: Found DD amount in OP_RETURN: %lld cents (txType=%d)\n", (long long)amount.GetInt64(), type);
                }
            }
            break;  // Only one OP_RETURN per transaction
        }
    }

    if (dd_amounts.empty()) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: No DD amounts found in OP_RETURN\n");
        return false;
    }

    // Now match P2TR outputs with DD amounts
    size_t dd_output_index = 0;
    for (uint32_t i = 0; i < tx->vout.size(); i++) {
        const CTxOut& txout = tx->vout[i];

        // Skip OP_RETURN and non-zero value outputs
        if (txout.scriptPubKey.size() > 0 && txout.scriptPubKey[0] == OP_RETURN) continue;
        if (txout.nValue != 0) continue;  // DD outputs have 0 DGB value

        // Check if it's a P2TR output (OP_1 + 32 bytes)
        if (txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1) {
            // This is a DD output - check if we own it via dd_address_keys
            // Extract the 32-byte output key from P2TR scriptPubKey (bytes 2-33)
            std::array<unsigned char, 32> output_key;
            std::copy(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.begin() + 34, output_key.begin());

            // Check if we have this key in our dd_address_keys map
            bool have_key = false;
            {
                // Check dd_address_keys for the output key
                auto it = dd_address_keys.find(output_key);
                if (it != dd_address_keys.end()) {
                    have_key = true;
                    LogPrintf("DigiDollar: Found output key in dd_address_keys for vout[%d]\n", i);
                }
            }

            // Also check if wallet recognizes it (for minted DD where we own the key)
            if (!have_key && m_wallet) {
                LOCK(m_wallet->cs_wallet);
                wallet::isminetype mine = m_wallet->IsMine(txout);
                if (mine & wallet::ISMINE_SPENDABLE) {
                    have_key = true;
                    LogPrintf("DigiDollar: Wallet IsMine returned spendable for vout[%d]\n", i);
                }
            }

            if (have_key) {
                if (dd_output_index < dd_amounts.size()) {
                    CAmount dd_amount = dd_amounts[dd_output_index];
                    LogPrintf("DigiDollar: Detected incoming DD output - vout[%d]: %d DD cents\n",
                              i, dd_amount);
                    our_dd_outputs.push_back(std::make_pair(i, dd_amount));
                }
            } else {
                LogPrint(BCLog::WALLETDB, "DigiDollar: Output %d is DD P2TR but not ours (no key found)\n", i);
            }
            dd_output_index++;
        }
    }

    bool found = !our_dd_outputs.empty();
    LogPrint(BCLog::WALLETDB, "DigiDollar: DetectIncomingDDOutputs - Found %d output(s) for our wallet\n",
             our_dd_outputs.size());

    return found;
}

/**
 * Add received DD UTXO to spendable set (Task 6.3)
 */
bool DigiDollarWallet::AddReceivedDDUTXO(const CTransactionRef& tx,
                                         uint32_t vout_index,
                                         CAmount dd_amount) {
    auto locks = LockDDWallet();
    if (!tx) {
        LogPrintf("DigiDollar: AddReceivedDDUTXO - null transaction\n");
        return false;
    }

    if (dd_amount <= 0) {
        LogPrintf("DigiDollar: AddReceivedDDUTXO - invalid DD amount: %d\n", dd_amount);
        return false;
    }

    uint256 txid = tx->GetHash();

    // Check for duplicate processing using dd_utxos (NOT collateral_positions)
    // Received DD is tracked in dd_utxos only, NOT in collateral_positions
    // collateral_positions should ONLY contain actual minted collateral outputs
    COutPoint received_utxo(txid, vout_index);
    if (dd_utxos.find(received_utxo) != dd_utxos.end()) {
        LogPrint(BCLog::WALLETDB,
                 "DigiDollar: AddReceivedDDUTXO - UTXO %s:%d already exists, skipping\n",
                 txid.ToString(), vout_index);
        return true;  // Not an error, just already processed
    }

    LogPrintf("DigiDollar: AddReceivedDDUTXO - Adding received DD UTXO: %s:%d (%d DD cents)\n",
              txid.ToString(), vout_index, dd_amount);

    // Look up the owner key from dd_address_keys and store in dd_owner_keys
    // This allows GetOwnerKey to find the key by txid for spending
    const CTxOut& txout = tx->vout[vout_index];
    if (txout.scriptPubKey.size() == 34 && txout.scriptPubKey[0] == OP_1) {
        // Extract the output key (tweaked pubkey) from the scriptPubKey
        // dd_address_keys is indexed by std::array<unsigned char, 32>
        std::array<unsigned char, 32> output_key_array;
        std::copy(txout.scriptPubKey.begin() + 2, txout.scriptPubKey.end(), output_key_array.begin());

        CKey output_key;
        if (GetDDOutputSpendingKey(txout, output_key)) {
            // Found the key. Store it under the txid so GetOwnerKey can find it.
            StoreOwnerKey(txid, output_key);
            LogPrintf("DigiDollar: AddReceivedDDUTXO - Stored owner key for received DD tx %s\n",
                      txid.ToString());
        } else {
            LogPrintf("DigiDollar: AddReceivedDDUTXO - WARNING: No spendable key found for output %s\n",
                      HexStr(output_key_array));
        }
    }

    // Add to DD UTXO tracking (primary balance source)
    // This is the ONLY tracking needed for received DD
    // Note: DO NOT add to collateral_positions - that would cause script-path signing
    // for DD token outputs, which is incorrect (they need key-path signing)
    dd_utxos[received_utxo] = dd_amount;

    // Persist DD UTXO to database
    if (m_wallet) {
        wallet::WalletBatch batch(m_wallet->GetDatabase());
        if (!batch.WriteDDUTXO(received_utxo, dd_amount)) {
            LogPrintf("DigiDollar: AddReceivedDDUTXO - Failed to persist DD UTXO to database\n");
            dd_utxos.erase(received_utxo);
            return false;
        }
        LogPrint(BCLog::WALLETDB, "DigiDollar: Persisted received DD UTXO to wallet.dat\n");
    }

    // Balance updates automatically (UTXO-derived approach from Phase 5.1)
    CAmount new_balance = GetTotalDDBalance();
    LogPrintf("DigiDollar: Balance after receive: %d DD cents\n", new_balance);

    return true;
}

/**
 * Process incoming DD transaction (Tasks 6.1-6.3 combined)
 */
bool DigiDollarWallet::ProcessIncomingDDTransaction(const CTransactionRef& tx) {
    auto locks = LockDDWallet();
    if (!tx) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: ProcessIncomingDDTransaction - null transaction\n");
        return false;
    }

    if (!m_wallet) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: ProcessIncomingDDTransaction - no wallet pointer\n");
        return false;
    }
    const DigiDollarTxType tx_type = GetDigiDollarTxType(*tx);
    if (tx_type == DD_TX_NONE) {
        LogPrint(BCLog::WALLETDB, "DigiDollar: ProcessIncomingDDTransaction - not a DigiDollar versioned transaction\n");
        return true;
    }

    LogPrint(BCLog::WALLETDB, "DigiDollar: ProcessIncomingDDTransaction - Processing tx %s\n",
             tx->GetHash().ToString());

    if (tx_type == DD_TX_REDEEM) {
        bool position_updated = false;
        for (const CTxIn& txin : tx->vin) {
            if (txin.prevout.n != 0) continue;

            auto it = collateral_positions.find(txin.prevout.hash);
            if (it == collateral_positions.end() || !it->second.is_active) continue;

            it->second.is_active = false;
            position_updated = true;
            if (m_wallet) {
                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.WriteDDTimeLock(it->second);
            }
            LogPrintf("DigiDollar: Pending redeem %s deactivated position %s\n",
                      tx->GetHash().ToString(), txin.prevout.hash.ToString());
        }

        if (position_updated) {
            CAmount total_locked = 0;
            for (const auto& [id, pos] : collateral_positions) {
                if (pos.is_active) {
                    total_locked += pos.dgb_collateral;
                }
            }
            locked_collateral = total_locked;
            if (m_wallet) {
                wallet::WalletBatch batch(m_wallet->GetDatabase());
                batch.WriteDDMetadata("locked_collateral", std::to_string(total_locked));
            }
        }
    }

    // Task 6.1: Detect incoming DD outputs
    std::vector<std::pair<uint32_t, CAmount>> our_dd_outputs;
    if (!DetectIncomingDDOutputs(tx, our_dd_outputs)) {
        // No DD outputs for us in this transaction - this is normal, not an error
        LogPrint(BCLog::WALLETDB, "DigiDollar: No DD outputs for our wallet in tx %s\n",
                 tx->GetHash().ToString());
        return true;  // Not an error, just nothing for us
    }

    LogPrintf("DigiDollar: Found %d DD output(s) for our wallet in tx %s\n",
              our_dd_outputs.size(), tx->GetHash().ToString());

    // Task 6.3: Add each received DD UTXO to spendable set
    for (const auto& [vout_index, dd_amount] : our_dd_outputs) {
        if (!AddReceivedDDUTXO(tx, vout_index, dd_amount)) {
            LogPrintf("DigiDollar: Failed to add received UTXO vout[%d] from tx %s\n",
                      vout_index, tx->GetHash().ToString());
            return false;
        }

        // Task 6.2: Balance updates automatically (UTXO-derived)
        // GetTotalDDBalance() will now include this new position
        LogPrint(BCLog::WALLETDB, "DigiDollar: Added received UTXO vout[%d]: %d DD cents\n",
                 vout_index, dd_amount);
    }

    // Log final balance after receive
    CAmount new_balance = GetTotalDDBalance();
    LogPrintf("DigiDollar: Receive complete - New balance: %d DD cents (%.2f DD)\n",
              new_balance, new_balance / 100.0);

    return true;
}

// =============================================================================
// UTILITY FUNCTION IMPLEMENTATIONS
// =============================================================================

namespace DigiDollarWalletUtils {

int GetLockDaysForTier(uint32_t tier) {
    // Lock tiers must match consensus/digidollar.h collateralRatios (10 tiers, 0-9):
    // Tier 0: 1 hour, Tier 1: 30 days, Tier 2: 90 days, Tier 3: 180 days,
    // Tier 4: 1 year, Tier 5: 2 years, Tier 6: 3 years, Tier 7: 5 years,
    // Tier 8: 7 years, Tier 9: 10 years
    //
    // DD-FA-FUNC-026 (Wave 17 Agent A): tier 0 returns 0 to match the
    // RPC-side helper at src/rpc/digidollar.cpp:64. The consensus
    // converter `DigiDollar::LockDaysToBlocks` treats days==0 as the
    // canonical 1-hour testing tier and emits 240 blocks. Returning 1
    // here previously emitted 5760 blocks (1 day) and was rejected by
    // consensus with `bad-mint-lock-tier-duration` if the helper ever
    // leaked into a tx-builder path. Pinned by
    // wallet/test/digidollar_wave17_helper_asymmetry_tests.cpp.
    switch (tier) {
        case 0: return 0;     // 1 hour testing tier; LockDaysToBlocks(0) = 240
        case 1: return 30;    // 30 days
        case 2: return 90;    // 90 days (3 months)
        case 3: return 180;   // 180 days (6 months)
        case 4: return 365;   // 1 year
        case 5: return 730;   // 2 years (2 * 365)
        case 6: return 1095;  // 3 years (3 * 365)
        case 7: return 1825;  // 5 years (5 * 365)
        case 8: return 2555;  // 7 years (7 * 365)
        case 9: return 3650;  // 10 years (10 * 365)
        default: return 30;   // Default to tier 1
    }
}

int GetMinCollateralRatio(uint32_t tier) {
    // Matches consensus/digidollar.h collateralRatios
    // Longer locks require less collateral
    switch (tier) {
        case 0: return 1000; // 1000% for 1 hour (testing only)
        case 1: return 500;  // 500% for 30 days
        case 2: return 400;  // 400% for 90 days
        case 3: return 350;  // 350% for 180 days
        case 4: return 300;  // 300% for 1 year
        case 5: return 275;  // 275% for 2 years
        case 6: return 250;  // 250% for 3 years
        case 7: return 225;  // 225% for 5 years
        case 8: return 212;  // 212% for 7 years
        case 9: return 200;  // 200% for 10 years
        default: return 500; // Default to tier 1
    }
}

bool IsValidDDAddress(const std::string& address) {
    return CDigiDollarAddress::IsValidDigiDollarAddress(address);
}

} // namespace DigiDollarWalletUtils
