// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <chain.h>
#include <chainparams.h>
#include <validation.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/time.h>
#include <logging.h>

namespace {
class LocalBlockHeader final : public BlockHeaderSource {
    const BlockHeaderData m_data;
public:
    explicit LocalBlockHeader(const BlockHeaderData& data) : m_data{data} {}
    BlockHeaderData Read(const uint256&) const override { return m_data; }
    bool IsShared() const override { return false; }
};
} // namespace

BlockHeaderData CBlockIndex::GetHeaderData() const
{
    if (!m_header_source) return {};
    return m_header_source->Read(m_header_source->IsShared() ? GetBlockHash() : uint256{});
}

void CBlockIndex::SetHeaderData(const BlockHeaderData& data)
{
    auto replacement = std::make_unique<LocalBlockHeader>(data);
    if (m_header_source && !m_header_source->IsShared()) delete m_header_source;
    m_header_source = replacement.release();
}

void CBlockIndex::UseHeaderSource(const BlockHeaderSource& source)
{
    assert(source.IsShared());
    if (m_header_source && !m_header_source->IsShared()) delete m_header_source;
    m_header_source = &source;
}

CBlockIndex::~CBlockIndex()
{
    if (m_header_source && !m_header_source->IsShared()) delete m_header_source;
}

CBlockIndex::CBlockIndex(const CBlockIndex& other)
    : m_chain_work{other.m_chain_work},
      phashBlock{other.phashBlock}, pprev{other.pprev}, pskip{other.pskip},
      nHeight{other.nHeight}, nFile{other.nFile}, nDataPos{other.nDataPos}, nUndoPos{other.nUndoPos},
      nTx{other.nTx}, nChainTx{other.nChainTx}, nStatus{other.nStatus},
      nVersion{other.nVersion}, nTime{other.nTime}, nBits{other.nBits},
      nSequenceId{other.nSequenceId}, nTimeMax{other.nTimeMax}
{
    SetHeaderData(other.GetHeaderData());
}

/**
 * CBlockIndex constructor that copies from a block header.
 * We can safely call LogPrintf here because we are in a .cpp file that includes logging.
 */
CBlockIndex::CBlockIndex(const CBlockHeader& block)
    : nVersion(block.nVersion),
      nTime(block.nTime),
      nBits(block.nBits)
{
    SetHeaderData({block.hashMerkleRoot, block.nNonce});
    // A block header names its mining algorithm in its version bits. Say so
    // once here if the bits are not one of DigiByte's five algorithms; GetAlgo
    // below then treats the block as Scrypt.
    if (block.GetAlgo() == ALGO_UNKNOWN) {
        LogPrintf("CBlockIndex ctor: ALGO_UNKNOWN in block version=0x%08x\n", block.nVersion);
    }
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, FormatISO8601Date(nTimeFirst), FormatISO8601Date(nTimeLast));
}

std::string CBlockIndex::ToString() const
{
    return strprintf("CBlockIndex(pprev=%p, nHeight=%d, merkle=%s, hashBlock=%s)",
                     pprev, nHeight, GetHeaderData().merkle_root.ToString(), GetBlockHash().ToString());
}

void CChain::SetTip(CBlockIndex& block) {
    CBlockIndex* pindex = &block;
    vChain.resize(pindex->nHeight + 1);
    while (pindex && vChain[pindex->nHeight] != pindex) {
        vChain[pindex->nHeight] = pindex;
        pindex = pindex->pprev;
    }
}

std::vector<uint256> LocatorEntries(const CBlockIndex* index)
{
    int step = 1;
    std::vector<uint256> have;
    if (index == nullptr) return have;

    have.reserve(32);
    while (index) {
        have.emplace_back(index->GetBlockHash());
        if (index->nHeight == 0) break;
        // Exponentially larger steps back, plus the genesis block.
        int height = std::max(index->nHeight - step, 0);
        // Use skiplist.
        index = index->GetAncestor(height);
        if (have.size() > 10) step *= 2;
    }
    return have;
}

CBlockLocator GetLocator(const CBlockIndex* index)
{
    return CBlockLocator{LocatorEntries(index)};
}

CBlockLocator CChain::GetLocator() const
{
    return ::GetLocator(Tip());
}

const CBlockIndex *CChain::FindFork(const CBlockIndex *pindex) const {
    if (pindex == nullptr) {
        return nullptr;
    }
    if (pindex->nHeight > Height())
        pindex = pindex->GetAncestor(Height());
    while (pindex && !Contains(pindex))
        pindex = pindex->pprev;
    return pindex;
}

CBlockIndex* CChain::FindEarliestAtLeast(int64_t nTime, int height) const
{
    std::pair<int64_t, int> blockparams = std::make_pair(nTime, height);
    std::vector<CBlockIndex*>::const_iterator lower = std::lower_bound(vChain.begin(), vChain.end(), blockparams,
        [](CBlockIndex* pBlock, const std::pair<int64_t, int>& blockparams) -> bool { return pBlock->GetBlockTimeMax() < blockparams.first || pBlock->nHeight < blockparams.second; });
    return (lower == vChain.end() ? nullptr : *lower);
}

/**
 * Return the mining algorithm named in this block's version bits. If the bits
 * name none of them, log a warning and call it Scrypt.
 */
int CBlockIndex::GetAlgo() const
{
    // Parse algorithm from version bits.
    // Pre-multi-algo blocks (mainnet <145000, testnet <100) have version bits
    // that naturally map to BLOCK_VERSION_SCRYPT (algo bits = 0x0000), so no
    // special height check is needed.
    //
    // This used to force Scrypt for every block below mainnet height 145,000.
    // That broke testnet and regtest, where multi-algorithm mining starts much
    // earlier, at block 100: every earlier block looked like a Scrypt block,
    // the difficulty rules could not find a previous block of the other
    // algorithms, and DigiShield V4 fell back to the easiest possible
    // difficulty every time. Difficulty never adjusted there for Qubit,
    // Skein, SHA256D or Odocrypt.
    switch (nVersion & BLOCK_VERSION_ALGO) {
        case BLOCK_VERSION_SCRYPT:   return ALGO_SCRYPT;
        case BLOCK_VERSION_SHA256D:  return ALGO_SHA256D;
        case BLOCK_VERSION_GROESTL:  return ALGO_GROESTL;
        case BLOCK_VERSION_SKEIN:    return ALGO_SKEIN;
        case BLOCK_VERSION_QUBIT:    return ALGO_QUBIT;
        case BLOCK_VERSION_ODO:      return ALGO_ODO;
    }

    // Unrecognized algo bits — should not happen for valid blocks.
    // Default to Scrypt as it was the original algorithm on all networks.
    LogPrintf("Warning: block at height=%d has unrecognized algo bits in nVersion=0x%08x, defaulting to Scrypt\n", nHeight, nVersion);
    return ALGO_SCRYPT;
}

// Helper function that uses consensus parameters to determine algorithm correctly for any chain
int GetAlgoForBlockIndex(const CBlockIndex* blockindex, const Consensus::Params& consensus)
{
    if (!blockindex) {
        return ALGO_SCRYPT;
    }
    
    // For blocks below the multi-algo height, always return ALGO_SCRYPT
    if (blockindex->nHeight < consensus.multiAlgoDiffChangeTarget) {
        return ALGO_SCRYPT;
    }

    // Otherwise, parse from version bits:
    switch (blockindex->nVersion & BLOCK_VERSION_ALGO) {
        case BLOCK_VERSION_SCRYPT:   return ALGO_SCRYPT;
        case BLOCK_VERSION_SHA256D:  return ALGO_SHA256D;
        case BLOCK_VERSION_GROESTL:  return ALGO_GROESTL;
        case BLOCK_VERSION_SKEIN:    return ALGO_SKEIN;
        case BLOCK_VERSION_QUBIT:    return ALGO_QUBIT;
        case BLOCK_VERSION_ODO:      return ALGO_ODO;
    }
    // If still not recognized:
    LogPrintf("Warning: block at height=%d has unrecognized nVersion=0x%08x\n", blockindex->nHeight, blockindex->nVersion);
    return ALGO_UNKNOWN;
}


/** Turn the lowest '1' bit in the binary representation of a number into '0'. */
int static inline InvertLowestOne(int n) { return n & (n - 1); }

/** Compute what height to jump back to with the CBlockIndex::pskip pointer. */
int static inline GetSkipHeight(int height) {
    if (height < 2)
        return 0;

    return (height & 1)
         ? InvertLowestOne(InvertLowestOne(height - 1)) + 1
         : InvertLowestOne(height);
}

const CBlockIndex* CBlockIndex::GetAncestor(int height) const
{
    if (height > nHeight || height < 0) {
        return nullptr;
    }

    const CBlockIndex* pindexWalk = this;
    int heightWalk = nHeight;
    while (heightWalk > height) {
        int heightSkip = GetSkipHeight(heightWalk);
        int heightSkipPrev = GetSkipHeight(heightWalk - 1);
        if (pindexWalk->pskip != nullptr &&
            (heightSkip == height ||
             (heightSkip > height && !(heightSkipPrev < heightSkip - 2 &&
                                       heightSkipPrev >= height)))) {
            // Only follow pskip if pprev->pskip isn't better than pskip->pprev.
            pindexWalk = pindexWalk->pskip;
            heightWalk = heightSkip;
        } else {
            assert(pindexWalk->pprev);
            pindexWalk = pindexWalk->pprev;
            heightWalk--;
        }
    }
    return pindexWalk;
}

CBlockIndex* CBlockIndex::GetAncestor(int height)
{
    return const_cast<CBlockIndex*>(static_cast<const CBlockIndex*>(this)->GetAncestor(height));
}

void CBlockIndex::BuildSkip()
{
    if (pprev)
        pskip = pprev->GetAncestor(GetSkipHeight(nHeight));
}

int GetAlgoWorkFactor(int nHeight, int algo) 
{
    if (nHeight < Params().GetConsensus().multiAlgoDiffChangeTarget) {
        return 1;
    }

    switch (algo)
    {
        case ALGO_SHA256D:
            return 1;
        case ALGO_SCRYPT:
            return 1024 * 4; // etc...
        case ALGO_GROESTL:
            return 64 * 8;
        case ALGO_SKEIN:
            return 4 * 6;
        case ALGO_QUBIT:
            return 128 * 8;
        default:
            return 1;
    }
}

arith_uint256 GetBlockProofBase(const CBlockIndex& block)
{
    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;
    bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0)
        return 0;

    // 2**256 / (bnTarget+1)
    return (~bnTarget / (bnTarget + 1)) + 1;
}

arith_uint256 GetBlockProof(const CBlockIndex& block)
{
    // Work uses the header version and time, which remain in memory. Preserve
    // CBlockHeader's algorithm decoding, including unknown version bits.
    CBlockHeader header;
    header.nVersion = block.nVersion;
    header.nTime = block.nTime;
    int nHeight = block.nHeight;
    const Consensus::Params& params = Params().GetConsensus();

    if (nHeight < params.workComputationChangeTarget) {
        arith_uint256 bnBlockWork = GetBlockProofBase(block);
        uint32_t nAlgoWork = GetAlgoWorkFactor(nHeight, header.GetAlgo());
        return bnBlockWork * nAlgoWork;
    } else {
        // Compute the geometric mean across all active algos
        arith_uint256 bnAvgTarget(1);

        for (int i = 0; i < NUM_ALGOS_IMPL; i++) {
            if (!IsAlgoActive(block.pprev, params, i))
                continue;
            unsigned int nBits = GetNextWorkRequired(block.pprev, &header, params, i);
            arith_uint256 bnTarget;
            bool fNegative;
            bool fOverflow;
            bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
            if (fNegative || fOverflow || bnTarget == 0)
                return 0;
            // Instead of multiplying them all together and then taking the
            // nth root at the end, take the roots individually then multiply so
            // that all intermediate values fit in 256-bit integers.
            bnAvgTarget *= bnTarget.ApproxNthRoot(NUM_ALGOS);
        }
        arith_uint256 bnRes = (~bnAvgTarget / (bnAvgTarget + 1)) + 1;
        // scale
        bnRes <<= 7;
        return bnRes;
    }
}

arith_uint256 GetBlockProof(const CBlockIndex& block, int algo)
{
    CBlockHeader header;
    header.nVersion = block.nVersion;
    header.nTime = block.nTime;
    int nHeight = block.nHeight;
    const Consensus::Params& params = Params().GetConsensus();

    if (nHeight < params.workComputationChangeTarget) {
        arith_uint256 bnBlockWork = GetBlockProofBase(block);
        uint32_t nAlgoWork = GetAlgoWorkFactor(nHeight, header.GetAlgo());
        return bnBlockWork * nAlgoWork;
    } else {
        if (!IsAlgoActive(block.pprev, params, algo))
            return 0;
        unsigned int nBits = GetNextWorkRequired(block.pprev, &header, params, algo);
        arith_uint256 bnTarget;
        bool fNegative;
        bool fOverflow;
        bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
        if (fNegative || fOverflow || bnTarget == 0)
            return 0;
        return (~bnTarget / (bnTarget + 1)) + 1;
    }
}

int64_t GetBlockProofEquivalentTime(const CBlockIndex& to, const CBlockIndex& from,
                                    const CBlockIndex& tip, const Consensus::Params& params)
{
    arith_uint256 r;
    int sign = 1;
    if (to.GetChainWork() > from.GetChainWork()) {
        r = to.GetChainWork() - from.GetChainWork();
    } else {
        r = from.GetChainWork() - to.GetChainWork();
        sign = -1;
    }
    r = r * arith_uint256(params.nPowTargetSpacing) / GetBlockProof(tip);
    if (r.bits() > 63) {
        return sign * std::numeric_limits<int64_t>::max();
    }
    return sign * int64_t(r.GetLow64());
}

const CBlockIndex* LastCommonAncestor(const CBlockIndex* pa, const CBlockIndex* pb)
{
    if (pa->nHeight > pb->nHeight) {
        pa = pa->GetAncestor(pb->nHeight);
    } else if (pb->nHeight > pa->nHeight) {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb) {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}
