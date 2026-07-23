// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_CONSENSUS_PARAMS_H
#define DIGIBYTE_CONSENSUS_PARAMS_H

#include <uint256.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Consensus {

/**
 * A buried deployment is one where the height of the activation has been hardcoded into
 * the client implementation long after the consensus change has activated. See BIP 90.
 */
enum BuriedDeployment : int16_t {
    // buried deployments get negative values to avoid overlap with DeploymentPos
    DEPLOYMENT_HEIGHTINCB = std::numeric_limits<int16_t>::min(),
    DEPLOYMENT_CLTV,
    DEPLOYMENT_DERSIG,
    DEPLOYMENT_CSV, // Deployment of BIP68, BIP112, and BIP113.
    DEPLOYMENT_SEGWIT, // Deployment of BIP141, BIP143, and BIP147.
    DEPLOYMENT_NVERSIONBIPS, // Deployment of BIP34, BIP65, and BIP66.
    DEPLOYMENT_RESERVEALGO,  // Reservation of version bits for future algos
    DEPLOYMENT_ODO, // Odo hard fork
    //DEPLOYMENT_EQUIHASH, // Equihash algo swap
    //DEPLOYMENT_ETHASH, // Ethash algo swap
    DEPLOYMENT_TAPROOT, // Deployment of Schnorr/Taproot (BIPs 340-342); buried after BIP9 bit-2 activation
    DEPLOYMENT_DIGIDOLLAR, // Deployment of DigiDollar stablecoin features; buried after BIP9 bit-23 activation
    DEPLOYMENT_ALGOLOCK, // Reject deactivated (e.g. retired Groestl) or unknown-algo blocks; buried after BIP9 bit-0 activation
};
constexpr bool ValidDeployment(BuriedDeployment dep) { return dep <= DEPLOYMENT_ALGOLOCK; }

enum DeploymentPos : uint16_t {
    DEPLOYMENT_TESTDUMMY,
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in deploymentinfo.cpp
    MAX_VERSION_BITS_DEPLOYMENTS
};
constexpr bool ValidDeployment(DeploymentPos dep) { return dep < MAX_VERSION_BITS_DEPLOYMENTS; }

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit{28};
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime{NEVER_ACTIVE};
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout{NEVER_ACTIVE};
    /** If lock in occurs, delay activation until at least this block
     *  height.  Note that activation will only occur on a retarget
     *  boundary.
     */
    int min_activation_height{0};

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;

    /** Special value for nStartTime indicating that the deployment is never active.
     *  This is useful for integrating the code changes for a new feature
     *  prior to deploying it on some or all networks. */
    static constexpr int64_t NEVER_ACTIVE = -2;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    int nSubsidyHalvingInterval;
    /**
     * Hashes of blocks that
     * - are known to be consensus valid, and
     * - buried in the chain, and
     * - fail if the default script verify flags are applied.
     */
    std::map<uint256, uint32_t> script_flag_exceptions;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    /** Block height at which CSV (BIP68, BIP112 and BIP113) becomes active */
    int CSVHeight;
    /** Block height at which Segwit (BIP141, BIP143 and BIP147) becomes active.
     * Note that segwit v0 script rules are enforced on all blocks except the
     * BIP 16 exception blocks. */
    int SegwitHeight;
    /**
     * Block height at which version bits were set to resolved
     * to support future algo changes */
    int ReserveAlgoBitsHeight;
    /**
     * Block height at which Odocrypt got activated */
    int OdoHeight;
    /**
     * Block height at which blocks using a deactivated mining algorithm
     * (e.g. the retired Groestl) or an unknown algorithm are rejected. */
    int nGroestlDeactivationHeight{std::numeric_limits<int>::max()};
    /** Block height at which Taproot (BIPs 340-342) becomes active.
     * Buried BIP9 deployment (bit 2); mainnet activated at the 'since'
     * height reported by getdeploymentinfo before burial. */
    int TaprootHeight{std::numeric_limits<int>::max()};
    /** Block height at which DigiDollar becomes active. Buried BIP9
     * deployment (bit 23). NOTE: distinct from the static
     * nDDActivationHeight/nOracleActivationHeight floor gates below, which
     * remain at the pre-burial min_activation_height floor. */
    int DigiDollarHeight{std::numeric_limits<int>::max()};
    /** Block height at which AlgoLock (reject retired-Groestl/unknown-algo
     * blocks) becomes active. Buried BIP9 deployment (bit 0); enforcement is
     * OR'd with the static nGroestlDeactivationHeight backstop. */
    int AlgoLockHeight{std::numeric_limits<int>::max()};
    /** Don't warn about unknown BIP 9 activations below this height.
     * This prevents us from warning about the CSV and segwit activations. */
    int MinBIP9WarningHeight;
    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * (nPowTargetTimespan / nPowTargetSpacing) which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t nRuleChangeActivationThreshold;
    uint32_t nMinerConfirmationWindow;
    BIP9Deployment vDeployments[MAX_VERSION_BITS_DEPLOYMENTS];
    /** Proof of work parameters */
    uint256 powLimit;
    std::map<int, uint256> initialTarget;
    bool fPowAllowMinDifficultyBlocks;
    bool fEasyPow;
    bool fPowNoRetargeting;
    bool fRbfEnabled;
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    std::chrono::seconds PowTargetSpacing() const
    {
        return std::chrono::seconds{nPowTargetSpacing};
    }
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    
    // DigiByte-specific difficulty adjustment parameters
    int64_t nTargetTimespan;
    int64_t nTargetSpacing;
    int64_t nInterval;
    int64_t nDiffChangeTarget;
    int64_t nTargetTimespanRe;
    int64_t nTargetSpacingRe;
    int64_t nIntervalRe;
    int64_t patchBlockRewardDuration;
    int64_t patchBlockRewardDuration2;

    int64_t nAveragingInterval;
    int64_t multiAlgoTargetSpacing;
    int64_t multiAlgoTargetSpacingV4;
    int64_t nAveragingTargetTimespan;
    int64_t nAveragingTargetTimespanV4;

    int64_t nMaxAdjustDown;
    int64_t nMaxAdjustUp;
    int64_t nMaxAdjustDownV3;
    int64_t nMaxAdjustUpV3;
    int64_t nMaxAdjustDownV4;
    int64_t nMaxAdjustUpV4;

    int64_t nMinActualTimespan;
    int64_t nMaxActualTimespan;
    int64_t nMinActualTimespanV3;
    int64_t nMaxActualTimespanV3;
    int64_t nMinActualTimespanV4;
    int64_t nMaxActualTimespanV4;

    int64_t nLocalTargetAdjustment;
    int64_t nLocalDifficultyAdjustment;

    int64_t multiAlgoDiffChangeTarget;
    int64_t alwaysUpdateDiffChangeTarget;
    int64_t workComputationChangeTarget;
    int64_t algoSwapChangeTarget;

    uint32_t nOdoShapechangeInterval;
    /** The best chain should have at least this much work */
    uint256 nMinimumChainWork;
    /** By default assume that the signatures in ancestors of this block are valid */
    uint256 defaultAssumeValid;

    // DigiDollar specific consensus fields
    int nDDOracleEpochBlocks{100};      // Blocks per oracle rotation epoch
    int nDDOracleUpdateInterval{4};     // Blocks between price updates
    int nDDActivationHeight{0};         // Height at which DigiDollar activates

    /** DD mint volatility gate fix (chain-derived lagged-median anchor).
     *  From nDDVolatilityFixHeight the mint freeze compares the candidate
     *  bundle price against the lower median of up to nDDVolAnchorMaxSamples
     *  committed v0x03 bundle prices found in ancestor blocks at heights
     *  [H - nDDVolAnchorWindow, H - nDDVolAnchorLag] (H = candidate height),
     *  collected newest-first, never below DigiDollarHeight. Zero in-window
     *  samples pass the gate (bootstrap / oracle-drought self-expiry). Below
     *  the fix height the legacy process-local deque check applies. */
    int nDDVolatilityFixHeight{std::numeric_limits<int>::max()};
    int nDDVolAnchorLag{240};        // Anchor lag in blocks (~1 hour of 15s blocks)
    int nDDVolAnchorWindow{1440};    // Anchor window depth in blocks (~6 hours)
    int nDDVolAnchorMaxSamples{15};  // Max bundle samples for the lagged median

    /** Oracle system parameters. DigiDollar V1 block data uses MuSig2 bundles only. */
    int nOracleActivationHeight{std::numeric_limits<int>::max()};  // Height when oracle system activates
    int nOracleEpochLength{1440};               // Blocks per oracle epoch (default: 1440 = 24 hours)
    int nOracleRequiredMessages{1};             // Off-chain signed price messages required before MuSig2 aggregation
    int nOracleTotalOracles{1};                 // Total active oracle operators
    std::vector<std::string> vOraclePublicKeys; // Hardcoded oracle public keys (hex encoded XOnlyPubKey, sorted)
    int nDigiDollarMuSig2Height{std::numeric_limits<int>::max()};  // Height when MuSig2 aggregate signatures are valid

    /** MuSig2 oracle configuration */
    int nOraclePubkeyCount{0};                  // Number of oracle pubkeys for MuSig2
    int nOracleConsensusRequired{0};            // Minimum oracles required for MuSig2 aggregate signature

    /** Check if MuSig2 aggregate oracle signatures are active at given height */
    bool IsMuSig2OracleActive(int32_t block_height) const {
        return block_height >= nDigiDollarMuSig2Height;
    }

    /**
     * If true, witness commitments contain a payload equal to a DigiByte Script solution
     * to the signet challenge. See BIP325.
     */
    bool signet_blocks{false};
    std::vector<uint8_t> signet_challenge;

    bool EnableRBF() const { return fRbfEnabled; }
    int DeploymentHeight(BuriedDeployment dep) const
    {
        switch (dep) {
        case DEPLOYMENT_HEIGHTINCB:
            return BIP34Height;
        case DEPLOYMENT_CLTV:
            return BIP65Height;
        case DEPLOYMENT_DERSIG:
            return BIP66Height;
        case DEPLOYMENT_CSV:
            return CSVHeight;
        case DEPLOYMENT_SEGWIT:
            return SegwitHeight;
        case DEPLOYMENT_NVERSIONBIPS:
            // In DigiByte DEPLOYMENT_CSV and DEPLOYMENT_NVERSIONBIPS
            // got activated simultaneously
            return CSVHeight;
        case DEPLOYMENT_RESERVEALGO:
            return ReserveAlgoBitsHeight;
        case DEPLOYMENT_ODO:
            return OdoHeight;
        case DEPLOYMENT_TAPROOT:
            return TaprootHeight;
        case DEPLOYMENT_DIGIDOLLAR:
            return DigiDollarHeight;
        case DEPLOYMENT_ALGOLOCK:
            return AlgoLockHeight;
        } // no default case, so the compiler can warn about missing cases
        return std::numeric_limits<int>::max();
    }
};

/**
 * Check if oracle system is active at given height
 */
inline bool IsOracleActive(const Params& params, int nHeight) {
    return nHeight >= params.nOracleActivationHeight;
}

/**
     * Check if MuSig2 is active at given height
 */
inline bool IsMuSig2Active(const Params& params, int nHeight) {
    return params.IsMuSig2OracleActive(nHeight);
}

/**
 * Validate MuSig2 oracle configuration.
 */
inline bool ValidateOracleConfiguration(const Params& params) {
    if (params.nOracleTotalOracles < 0) return false;
    if (params.nOraclePubkeyCount < 0) return false;
    if (params.nOracleConsensusRequired < 0) return false;
    if (static_cast<int>(params.vOraclePublicKeys.size()) != params.nOraclePubkeyCount) return false;
    if (params.nOraclePubkeyCount > params.nOracleTotalOracles) return false;
    if (params.nOraclePubkeyCount > 0 && params.nOracleConsensusRequired == 0) return false;
    if (params.nOracleConsensusRequired > params.nOraclePubkeyCount) return false;
    std::set<std::string> pubkey_set;
    for (const auto& pk : params.vOraclePublicKeys) {
        if (!pubkey_set.insert(pk).second) return false;
    }
    for (const auto& pk : params.vOraclePublicKeys) {
        if (pk.size() != 64) return false;
        for (char c : pk) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
    }
    // Note: vOraclePublicKeys is ordered by oracle slot (0..N-1), matching
    // vOracleNodes and the MuSig2 participation bitmap. MuSig2 key aggregation
    // sorts internally per BIP-327, so consensus pubkey ordering is slot-based,
    // not lexicographic. We require uniqueness (checked above) but not sort order.
    return true;
}

} // namespace Consensus

#endif // DIGIBYTE_CONSENSUS_PARAMS_H
