// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_POW_H
#define DIGIBYTE_POW_H

#include <consensus/params.h>
#include <primitives/block.h>

#include <array>
#include <stdint.h>

class CBlockHeader;
class CBlockIndex;
class uint256;

/** The latest eligible ancestor of each algorithm, or nullptr if none exists. */
using PreviousAlgoBlocks = std::array<const CBlockIndex*, NUM_ALGOS_IMPL>;

unsigned int InitialDifficulty(const Consensus::Params& params, int algo);
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&, int algo);
/**
 * Use supplied ancestor lookups for V3 and V4. Each entry must match
 * GetLastBlockIndexForAlgo on the full chain. The linked history must still
 * contain the averaging window and its median-time ancestors. Earlier
 * difficulty rules continue to require their full linked history.
 */
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params&, int algo, const PreviousAlgoBlocks& previous_algos);
unsigned int GetNextWorkRequiredv1(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&, int algo);
unsigned int GetNextWorkRequiredv2(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&, int algo);
unsigned int GetNextWorkRequiredv3(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&, int algo);
unsigned int GetNextWorkRequiredv4(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&, int algo);
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params&);

/** Check whether a block hash satisfies the proof-of-work requirement specified by nBits */
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params&);
const CBlockIndex* GetLastBlockIndexForAlgo(const CBlockIndex* pindex, const Consensus::Params&, int algo);
uint256 GetPoWAlgoHash(const CBlockHeader& block);

/**
 * Return false if the proof-of-work requirement specified by new_nbits at a
 * given height is not possible, given the proof-of-work on the prior block as
 * specified by old_nbits.
 *
 * This function only checks that the new value is within a factor of 4 of the
 * old value for blocks at the difficulty adjustment interval, and otherwise
 * requires the values to be the same.
 *
 * Always returns true on networks where min difficulty blocks are allowed,
 * such as regtest/testnet.
 */
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits);

#endif // DIGIBYTE_POW_H
