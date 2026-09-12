// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_CONSENSUS_DIGIDOLLAR_STATE_H
#define DIGIBYTE_CONSENSUS_DIGIDOLLAR_STATE_H

#include <consensus/amount.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <limits>

namespace DigiDollar {

/** Derived health inputs belonging to exactly one UTXO state.
 * A missing record is distinct from a verified system with no open vaults.
 * Only validation of the activated history may establish history_checked.
 */
struct ChainstateHealth {
    static constexpr uint32_t FORMAT_VERSION{1};
    static constexpr uint32_t RULES_VERSION{1};

    uint32_t format_version{FORMAT_VERSION};
    uint256 genesis_hash;
    uint32_t rules_version{RULES_VERSION};
    int32_t activation_height{std::numeric_limits<int32_t>::max()};
    int32_t digidollar_height{std::numeric_limits<int32_t>::max()};
    uint256 best_block;
    CAmount open_vault_principal{0};
    CAmount collateral{0};
    uint64_t active_vaults{0};
    bool history_checked{false};

    SERIALIZE_METHODS(ChainstateHealth, obj)
    {
        READWRITE(obj.format_version, obj.genesis_hash, obj.rules_version, obj.activation_height, obj.digidollar_height,
                  obj.best_block, obj.open_vault_principal, obj.collateral,
                  obj.active_vaults, obj.history_checked);
    }

    bool IsValid() const
    {
        return format_version == FORMAT_VERSION && rules_version == RULES_VERSION &&
               activation_height >= 0 && digidollar_height >= 0 && !genesis_hash.IsNull() && !best_block.IsNull() &&
               open_vault_principal >= 0 && MoneyRange(collateral) &&
               (active_vaults == 0 ? open_vault_principal == 0 && collateral == 0 :
                                    open_vault_principal > 0 && collateral > 0);
    }

    bool Matches(const uint256& genesis, const uint256& block) const
    {
        return IsValid() && genesis_hash == genesis && best_block == block;
    }

    bool Matches(const uint256& genesis, const uint256& block, int activation, int dd_activation) const
    {
        return Matches(genesis, block) && activation_height == activation && digidollar_height == dd_activation;
    }

    /** Checked, transactional arithmetic; a failure leaves this record unchanged. */
    bool AddVault(CAmount principal, CAmount value)
    {
        if (principal <= 0 || value <= 0 || !MoneyRange(value) ||
            open_vault_principal < 0 || collateral < 0 ||
            principal > std::numeric_limits<CAmount>::max() - open_vault_principal ||
            collateral > MAX_MONEY - value ||
            active_vaults == std::numeric_limits<uint64_t>::max()) return false;
        open_vault_principal += principal;
        collateral += value;
        ++active_vaults;
        return true;
    }

    bool RemoveVault(CAmount principal, CAmount value)
    {
        if (principal <= 0 || value <= 0 || active_vaults == 0 ||
            principal > open_vault_principal || value > collateral) return false;
        const auto remaining = active_vaults - 1;
        if (remaining == 0 ? principal != open_vault_principal || value != collateral :
                             principal == open_vault_principal || value == collateral) return false;
        open_vault_principal -= principal;
        collateral -= value;
        active_vaults = remaining;
        return true;
    }

    friend bool operator==(const ChainstateHealth& a, const ChainstateHealth& b)
    {
        return a.format_version == b.format_version && a.genesis_hash == b.genesis_hash &&
               a.rules_version == b.rules_version && a.activation_height == b.activation_height && a.digidollar_height == b.digidollar_height && a.best_block == b.best_block &&
               a.open_vault_principal == b.open_vault_principal && a.collateral == b.collateral &&
               a.active_vaults == b.active_vaults && a.history_checked == b.history_checked;
    }
    friend bool operator!=(const ChainstateHealth& a, const ChainstateHealth& b) { return !(a == b); }
};

} // namespace DigiDollar
#endif // DIGIBYTE_CONSENSUS_DIGIDOLLAR_STATE_H
