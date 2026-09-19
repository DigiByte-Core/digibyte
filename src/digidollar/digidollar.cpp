// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/int128.h>
#include <digidollar/digidollar.h>

#include <consensus/amount.h>
#include <util/strencodings.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

// =====================================
// CDigiDollarOutput Implementation
// =====================================

CDigiDollarOutput::CDigiDollarOutput()
    : nDDAmount(0), nLockTime(0)
{
    collateralId.SetNull();
    internalKey = XOnlyPubKey{};
    taprootMerkleRoot.SetNull();
}

CDigiDollarOutput::CDigiDollarOutput(CAmount nDDAmountIn, const uint256& collateralIdIn, int64_t nLockTimeIn)
    : nDDAmount(nDDAmountIn), collateralId(collateralIdIn), nLockTime(nLockTimeIn)
{
    internalKey = XOnlyPubKey{};
    taprootMerkleRoot.SetNull();
}

bool CDigiDollarOutput::IsValid() const
{
    // Check amount is positive and within limits
    if (nDDAmount <= 0) return false;
    if (nDDAmount > MAX_DIGIDOLLAR) return false;

    // Check lock time is non-negative
    if (nLockTime < 0) return false;

    // Additional validation could include:
    // - Checking that collateralId is not null for certain types
    // - Validating internal key format
    // For now, basic validation is sufficient

    return true;
}

bool operator==(const CDigiDollarOutput& a, const CDigiDollarOutput& b)
{
    return (a.nDDAmount == b.nDDAmount &&
            a.collateralId == b.collateralId &&
            a.nLockTime == b.nLockTime &&
            a.internalKey == b.internalKey &&
            a.taprootMerkleRoot == b.taprootMerkleRoot);
}

bool operator!=(const CDigiDollarOutput& a, const CDigiDollarOutput& b)
{
    return !(a == b);
}

// =====================================
// CCollateralPosition Implementation
// =====================================

CCollateralPosition::CCollateralPosition()
    : dgbLocked(0), ddMinted(0), unlockHeight(0), collateralRatio(0)
{
    outpoint.SetNull();
}

CCollateralPosition::CCollateralPosition(const COutPoint& outpointIn, CAmount dgbLockedIn,
                                       CAmount ddMintedIn, int64_t unlockHeightIn, int collateralRatioIn)
    : outpoint(outpointIn), dgbLocked(dgbLockedIn), ddMinted(ddMintedIn),
      unlockHeight(unlockHeightIn), collateralRatio(collateralRatioIn)
{
}

CAmount CCollateralPosition::GetCurrentCollateralRatio(CAmount currentPrice) const
{
    // dgbLocked is in satoshis. currentPrice is cents per DGB, so 100 means
    // $1.00 per DGB. ddMinted is in cents. The answer is a percentage, so 200
    // means the locked DGB is worth twice the DigiDollar minted against it.

    // With nothing minted there is no ratio to report. A negative amount or a
    // negative price can only come from damaged data, and there is no honest
    // ratio for those either, so report nothing rather than a made-up number.
    if (ddMinted <= 0 || dgbLocked <= 0 || currentPrice <= 0) {
        return 0;
    }

    // Work out what the locked DGB is worth, then the ratio. Both steps are
    // done in a 128-bit type on purpose. A large position at a high price does
    // not fit in a 64-bit money amount, and in 64 bits the multiply would wrap
    // round and report a small or negative ratio for a position that is in
    // fact hugely over-collateralised.
    const util::int128_t valueInCents = (static_cast<util::int128_t>(dgbLocked) * currentPrice) / COIN;
    if (valueInCents <= 0) {
        return 0;
    }

    // When the true ratio is larger than a money amount can hold, report the
    // largest amount. That is a ceiling, not the exact figure, and it is the
    // closest true statement available: the position is over-collateralised
    // beyond anything this type can express.
    const util::int128_t ratio = (valueInCents * 100) / ddMinted;
    if (ratio > std::numeric_limits<CAmount>::max()) {
        return std::numeric_limits<CAmount>::max();
    }
    return static_cast<CAmount>(ratio);
}

bool CCollateralPosition::IsHealthy(CAmount currentPrice) const
{
    CAmount ratio = GetCurrentCollateralRatio(currentPrice);
    return ratio >= 100; // Healthy if collateral ratio >= 100%
}

CAmount CCollateralPosition::GetRequiredDDForRedemption(int systemCollateral) const
{
    if (systemCollateral <= 0) {
        throw std::runtime_error("System collateral cannot be zero or negative");
    }

    if (systemCollateral >= 100) {
        // Normal case: full redemption at 1:1 ratio
        return ddMinted;
    } else {
        // Emergency Redemption Ratio (ERR) case
        // Required DD = ddMinted * (100 / systemCollateral)
        // Check for overflow
        if (ddMinted > (std::numeric_limits<CAmount>::max() / 100)) {
            throw std::runtime_error("Calculation would overflow");
        }

        return (ddMinted * 100) / systemCollateral;
    }
}

void CCollateralPosition::AddRedemptionPath(RedemptionPath path)
{
    // Only add if not already present
    if (std::find(availablePaths.begin(), availablePaths.end(), path) == availablePaths.end()) {
        availablePaths.push_back(path);
    }
}

bool CCollateralPosition::HasRedemptionPath(RedemptionPath path) const
{
    return std::find(availablePaths.begin(), availablePaths.end(), path) != availablePaths.end();
}

bool operator==(const CCollateralPosition& a, const CCollateralPosition& b)
{
    return (a.outpoint == b.outpoint &&
            a.dgbLocked == b.dgbLocked &&
            a.ddMinted == b.ddMinted &&
            a.unlockHeight == b.unlockHeight &&
            a.collateralRatio == b.collateralRatio &&
            a.availablePaths == b.availablePaths);
}

bool operator!=(const CCollateralPosition& a, const CCollateralPosition& b)
{
    return !(a == b);
}

// =====================================
// DigiDollar Activation Functions
// =====================================

#include <consensus/params.h>
#include <deploymentstatus.h>
#include <validation.h>

namespace DigiDollar {

bool IsDigiDollarEnabled(const CBlockIndex* pindexPrev, const ChainstateManager& chainman)
{
    return DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_DIGIDOLLAR);
}

bool IsDigiDollarEnabled(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    // DigiDollar is a buried deployment (BIP90): activation is a fixed height
    // in chainparams, so both overloads reduce to the same O(1) height
    // comparison — no versionbits cache involved. This mirrors the buried
    // DeploymentActiveAfter semantics in deploymentstatus.h exactly.
    return (pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1) >=
           params.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);
}

bool IsThawDayScheduled(const Consensus::Params& params)
{
    return params.nDDThawDayHeight != std::numeric_limits<int>::max();
}

bool IsThawDayActive(const Consensus::Params& params, int candidate_height)
{
    // Only ever compare against the configured height; never add to it,
    // because the "not scheduled" value is the largest int and would overflow.
    if (candidate_height < 0) return false;
    if (!IsThawDayScheduled(params)) return false;
    // A network with the DigiDollar deployment switched off can never have
    // Thaw Day rules, at any height. Checking this explicitly keeps the
    // predicate false even at the largest int height, where a bare height
    // compare against the "disabled" value (also the largest int) would
    // otherwise read as active.
    if (!DeploymentEnabled(params, Consensus::DEPLOYMENT_DIGIDOLLAR)) return false;
    // The Thaw Day rules are DigiDollar rules, so they cannot apply to a block
    // where DigiDollar itself is not yet active. This is the same buried
    // height that IsDigiDollarEnabled() compares against.
    if (candidate_height < params.DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR)) return false;
    return candidate_height >= params.nDDThawDayHeight;
}

} // namespace DigiDollar