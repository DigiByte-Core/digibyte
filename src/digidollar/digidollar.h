// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_DIGIDOLLAR_DIGIDOLLAR_H
#define DIGIBYTE_DIGIDOLLAR_DIGIDOLLAR_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <vector>

/** Maximum DigiDollar amount (21 billion DGB equivalent in cents) */
static const CAmount MAX_DIGIDOLLAR = 21000000000LL * 100; // 21B dollars in cents

/**
 * DigiDollar Output Structure
 * Represents a DigiDollar UTXO with Taproot-based redemption paths
 */
class CDigiDollarOutput
{
public:
    CAmount nDDAmount;          //!< DigiDollar amount in cents (100 = $1)
    uint256 collateralId;       //!< Links to specific collateral UTXO
    int64_t nLockTime;          //!< Time-lock period in blocks

    // P2TR specific
    XOnlyPubKey internalKey;    //!< Taproot internal key
    uint256 taprootMerkleRoot;  //!< MAST root for redemption paths

    //! Constructors
    CDigiDollarOutput();
    CDigiDollarOutput(CAmount nDDAmountIn, const uint256& collateralIdIn, int64_t nLockTimeIn);

    //! Serialization
    SERIALIZE_METHODS(CDigiDollarOutput, obj)
    {
        READWRITE(obj.nDDAmount);
        READWRITE(obj.collateralId);
        READWRITE(obj.nLockTime);
        READWRITE(obj.internalKey);
        READWRITE(obj.taprootMerkleRoot);
    }

    //! Validation
    bool IsValid() const;

    //! Get USD value (same as nDDAmount since stored in cents)
    CAmount GetUSDValue() const { return nDDAmount; }

    //! Equality operators
    friend bool operator==(const CDigiDollarOutput& a, const CDigiDollarOutput& b);
    friend bool operator!=(const CDigiDollarOutput& a, const CDigiDollarOutput& b);
};

/**
 * Collateral Position Structure
 * Represents a locked DGB position backing DigiDollar issuance
 */
class CCollateralPosition
{
public:
    //! Taproot redemption paths (only 2 exist)
    //! NOTE: NO partial redemption, NO emergency oracle override
    enum RedemptionPath {
        PATH_NORMAL = 0,        //!< Standard timelock expiry (health >= 100%)
        PATH_ERR = 1            //!< Emergency Redemption Ratio (health < 100%)
    };

    COutPoint outpoint;         //!< The locked DGB UTXO
    CAmount dgbLocked;          //!< Amount of DGB locked
    CAmount ddMinted;           //!< Amount of DD created (in cents)
    int64_t unlockHeight;       //!< Block height when redeemable
    int collateralRatio;        //!< Initial collateral ratio used (percentage)

    //! Available redemption paths for this position
    std::vector<RedemptionPath> availablePaths;

    //! Constructors
    CCollateralPosition();
    CCollateralPosition(const COutPoint& outpointIn, CAmount dgbLockedIn,
                       CAmount ddMintedIn, int64_t unlockHeightIn, int collateralRatioIn);

    //! Serialization
    SERIALIZE_METHODS(CCollateralPosition, obj)
    {
        READWRITE(obj.outpoint);
        READWRITE(obj.dgbLocked);
        READWRITE(obj.ddMinted);
        READWRITE(obj.unlockHeight);
        READWRITE(obj.collateralRatio);
        READWRITE(obj.availablePaths);
    }

    //! System health tracking
    CAmount GetCurrentCollateralRatio(CAmount currentPrice) const;
    bool IsHealthy(CAmount currentPrice) const;
    CAmount GetRequiredDDForRedemption(int systemCollateral) const;

    //! Path management
    void AddRedemptionPath(RedemptionPath path);
    bool HasRedemptionPath(RedemptionPath path) const;

    //! Equality operators
    friend bool operator==(const CCollateralPosition& a, const CCollateralPosition& b);
    friend bool operator!=(const CCollateralPosition& a, const CCollateralPosition& b);
};

// Forward declarations
class CBlockIndex;
class ChainstateManager;
namespace Consensus {
    struct Params;
}

namespace DigiDollar {
    /**
     * Check if DigiDollar is enabled using BIP9 deployment status.
     * This is the proper way to check DigiDollar activation.
     */
    bool IsDigiDollarEnabled(const CBlockIndex* pindexPrev, const ChainstateManager& chainman);
    bool IsDigiDollarEnabled(const CBlockIndex* pindexPrev, const Consensus::Params& params);

    /**
     * Is Thaw Day scheduled on this network at all? False while
     * Consensus::Params::nDDThawDayHeight holds its "not scheduled" value.
     */
    bool IsThawDayScheduled(const Consensus::Params& params);

    /**
     * Do the Thaw Day rules apply to a block at this height? True only when
     * Thaw Day is scheduled, DigiDollar is active at that height
     * (candidate_height >= params.DigiDollarHeight), and
     * candidate_height >= params.nDDThawDayHeight.
     *
     * Callers pass the height of the block being checked (validation,
     * replay), the intended height (mining: parent height + 1), or the next
     * block height on the active chain (mempool, wallet). A negative height
     * is never active. Pure: no chain state, no globals.
     */
    bool IsThawDayActive(const Consensus::Params& params, int candidate_height);
}

#endif // DIGIBYTE_DIGIDOLLAR_DIGIDOLLAR_H