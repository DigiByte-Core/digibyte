// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <wallet/load.h>

#include <common/args.h>
#include <interfaces/chain.h>
#include <paymaster/manager.h>
#include <scheduler.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/context.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <univalue.h>

#include <system_error>

namespace wallet {
namespace {
constexpr auto PAYMASTER_MAINTENANCE_INTERVAL{std::chrono::seconds{30}};
constexpr auto PAYMASTER_PROVIDER_SERVICE_INTERVAL{std::chrono::seconds{1}};
} // namespace

bool VerifyWallets(WalletContext& context)
{
    interfaces::Chain& chain = *context.chain;
    ArgsManager& args = *Assert(context.args);

    if (args.IsArgSet("-walletdir")) {
        const fs::path wallet_dir{args.GetPathArg("-walletdir")};
        std::error_code error;
        // The canonical path cleans the path, preventing >1 Berkeley environment instances for the same directory
        // It also lets the fs::exists and fs::is_directory checks below pass on windows, since they return false
        // if a path has trailing slashes, and it strips trailing slashes.
        fs::path canonical_wallet_dir = fs::canonical(wallet_dir, error);
        if (error || !fs::exists(canonical_wallet_dir)) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" does not exist"), fs::PathToString(wallet_dir)));
            return false;
        } else if (!fs::is_directory(canonical_wallet_dir)) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" is not a directory"), fs::PathToString(wallet_dir)));
            return false;
            // The canonical path transforms relative paths into absolute ones, so we check the non-canonical version
        } else if (!wallet_dir.is_absolute()) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" is a relative path"), fs::PathToString(wallet_dir)));
            return false;
        }
        args.ForceSetArg("-walletdir", fs::PathToString(canonical_wallet_dir));
    }

    LogPrintf("Using wallet directory %s\n", fs::PathToString(GetWalletDir()));

    chain.initMessage(_("Verifying wallet(s)…").translated);

    // For backwards compatibility if an unnamed top level wallet exists in the
    // wallets directory, include it in the default list of wallets to load.
    if (!args.IsArgSet("wallet")) {
        DatabaseOptions options;
        DatabaseStatus status;
        ReadDatabaseArgs(args, options);
        bilingual_str error_string;
        options.require_existing = true;
        options.verify = false;
        if (MakeWalletDatabase("", options, status, error_string)) {
            common::SettingsValue wallets(common::SettingsValue::VARR);
            wallets.push_back(""); // Default wallet name is ""
            // Pass write=false because no need to write file and probably
            // better not to. If unnamed wallet needs to be added next startup
            // and the setting is empty, this code will just run again.
            chain.updateRwSetting("wallet", wallets, /* write= */ false);
        }
    }

    // Keep track of each wallet absolute path to detect duplicates.
    std::set<fs::path> wallet_paths;

    for (const auto& wallet : chain.getSettingsList("wallet")) {
        const auto& wallet_file = wallet.get_str();
        const fs::path path = fsbridge::AbsPathJoin(GetWalletDir(), fs::PathFromString(wallet_file));

        if (!wallet_paths.insert(path).second) {
            chain.initWarning(strprintf(_("Ignoring duplicate -wallet %s."), wallet_file));
            continue;
        }

        DatabaseOptions options;
        DatabaseStatus status;
        ReadDatabaseArgs(args, options);
        options.require_existing = true;
        options.verify = true;
        bilingual_str error_string;
        if (!MakeWalletDatabase(wallet_file, options, status, error_string)) {
            if (status == DatabaseStatus::FAILED_NOT_FOUND) {
                chain.initWarning(Untranslated(strprintf("Skipping -wallet path that doesn't exist. %s", error_string.original)));
            } else {
                chain.initError(error_string);
                return false;
            }
        }
    }

    return true;
}

bool LoadWallets(WalletContext& context)
{
    interfaces::Chain& chain = *context.chain;
    try {
        std::set<fs::path> wallet_paths;
        for (const auto& wallet : chain.getSettingsList("wallet")) {
            const auto& name = wallet.get_str();
            if (!wallet_paths.insert(fs::PathFromString(name)).second) {
                continue;
            }
            DatabaseOptions options;
            DatabaseStatus status;
            ReadDatabaseArgs(*context.args, options);
            options.require_existing = true;
            options.verify = false; // No need to verify, assuming verified earlier in VerifyWallets()
            bilingual_str error;
            std::vector<bilingual_str> warnings;
            std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
            if (!database && status == DatabaseStatus::FAILED_NOT_FOUND) {
                continue;
            }
            chain.initMessage(_("Loading wallet…").translated);
            std::shared_ptr<CWallet> pwallet = database ? CWallet::Create(context, name, std::move(database), options.create_flags, error, warnings) : nullptr;
            if (!warnings.empty()) chain.initWarning(Join(warnings, Untranslated("\n")));
            if (!pwallet) {
                chain.initError(error);
                return false;
            }

            NotifyWalletLoaded(context, pwallet);
            AddWallet(context, pwallet);
        }
        return true;
    } catch (const std::runtime_error& e) {
        chain.initError(Untranslated(e.what()));
        return false;
    }
}

void StartWallets(WalletContext& context, CScheduler& scheduler)
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets(context)) {
        pwallet->postInitProcess();
        // Older Paymaster clients could persist only the wallet-owned DD
        // change row because collaborative sends bypassed the ordinary direct
        // send history path. Rebuild those display rows from successful,
        // durable sessions without changing transaction authority or funds.
        std::string client_history_error;
        if (!ReconcilePaymasterClientHistory(*pwallet,
                                             client_history_error)) {
            pwallet->WalletLogPrintf(
                "Paymaster client-history startup reconciliation failed: %s\n",
                client_history_error);
        }
        // Reconstruct free, wallet-owned successor liquidity before automatic
        // service starts. This is idempotent and creates no transaction; paid
        // replenishment remains gated by the persisted maintenance policy.
        size_t recovered_successors{0};
        std::string successor_error;
        if (!PaymasterStore{*pwallet}.ReconcileProviderPoolSuccessors(
                recovered_successors, successor_error)) {
            pwallet->WalletLogPrintf(
                "Paymaster pool-successor startup reconciliation failed: %s\n",
                successor_error);
        } else if (recovered_successors != 0) {
            pwallet->WalletLogPrintf(
                "Recovered %u historical Paymaster pool successor(s) at wallet startup\n",
                static_cast<unsigned int>(recovered_successors));
        }
        size_t recovered_maintenance{0};
        std::string maintenance_error;
        if (!ReconcilePaymasterProviderMaintenance(
                *pwallet, recovered_maintenance, maintenance_error)) {
            pwallet->WalletLogPrintf(
                "Paymaster maintenance startup reconciliation failed: %s\n",
                maintenance_error);
        }
    }

    // Schedule periodic wallet flushes, tx rebroadcasts, and Paymaster cleanup.
    if (context.args->GetBoolArg("-flushwallet", DEFAULT_FLUSHWALLET)) {
        scheduler.scheduleEvery([&context] { MaybeCompactWalletDB(context); }, std::chrono::milliseconds{500});
    }
    scheduler.scheduleEvery([&context] { MaybeResendWalletTxs(context); }, 1min);
    SchedulePeriodicPaymasterMaintenance(context, scheduler);
    SchedulePaymasterProviderServices(context, scheduler);
}

void RunPeriodicPaymasterMaintenance(WalletContext& context, int64_t now)
{
    // Reconcile durable observations and continue explicitly authorized finite
    // setup. Elapsed time never authorizes discarding a signature, releasing an
    // ambiguous reservation, or exceeding an approved fee limit.
    bool all_equivocation_scans_succeeded{true};
    for (const std::shared_ptr<CWallet>& wallet : GetWallets(context)) {
        std::string error;
        if (!DrainPaymasterEquivocationInbox(context, *wallet, error)) {
            all_equivocation_scans_succeeded = false;
            wallet->WalletLogPrintf(
                "Paymaster equivocation-inbox maintenance failed: %s\n",
                error);
            error.clear();
        }
        if (!PaymasterStore{*wallet}.ReconcileFinalSessionsAtTip(now, error)) {
            wallet->WalletLogPrintf("Paymaster periodic maintenance failed: %s\n",
                                    error);
        }
        size_t recovered_maintenance{0};
        error.clear();
        if (!ReconcilePaymasterProviderMaintenance(
                *wallet, recovered_maintenance, error)) {
            wallet->WalletLogPrintf(
                "Paymaster liquidity maintenance reconciliation failed: %s\n",
                error);
        } else {
            RunPaymasterPoolPreparation(context, *wallet);
        }
    }
    // The manager is shared by all wallets. Expired signed artifacts can be
    // discarded only after every loaded wallet had an opportunity to persist
    // evidence; pruning inside the per-wallet loop could lose a conflict that
    // belongs to a later wallet.
    if (all_equivocation_scans_succeeded && context.paymaster) {
        context.paymaster->PruneDirectMessages(now);
    }
}

void SchedulePeriodicPaymasterMaintenance(WalletContext& context,
                                          CScheduler& scheduler)
{
    scheduler.scheduleEvery(
        [&context] { RunPeriodicPaymasterMaintenance(context, GetTime()); },
        PAYMASTER_MAINTENANCE_INTERVAL);
}

void RunPaymasterProviderServices(WalletContext& context)
{
    // Each wallet owns independent identity, liquidity, and ledgers. Iterating
    // loaded wallets here must not merge provider state merely because several
    // wallets share one node-level Paymaster transport manager.
    for (const std::shared_ptr<CWallet>& wallet : GetWallets(context)) {
        RunPaymasterProviderServiceCycle(context, *wallet);
    }
}

void SchedulePaymasterProviderServices(WalletContext& context,
                                       CScheduler& scheduler)
{
    scheduler.scheduleEvery(
        [&context] { RunPaymasterProviderServices(context); },
        PAYMASTER_PROVIDER_SERVICE_INTERVAL);
}

void FlushWallets(WalletContext& context)
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets(context)) {
        pwallet->Flush();
    }
}

void StopWallets(WalletContext& context)
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets(context)) {
        pwallet->Close();
    }
}

void UnloadWallets(WalletContext& context)
{
    auto wallets = GetWallets(context);
    while (!wallets.empty()) {
        auto wallet = wallets.back();
        wallets.pop_back();
        std::vector<bilingual_str> warnings;
        RemoveWallet(context, wallet, /* load_on_start= */ std::nullopt, warnings);
        UnloadWallet(std::move(wallet));
    }
}
} // namespace wallet
