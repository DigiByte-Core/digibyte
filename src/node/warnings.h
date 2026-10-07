// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_NODE_WARNINGS_H
#define DIGIBYTE_NODE_WARNINGS_H

#include <versionbits.h>

class ChainstateManager;

/** Detect unknown version bits without changing deployment activation. */
class WarningBitsConditionChecker : public AbstractThresholdConditionChecker
{
private:
    const ChainstateManager& m_chainman;
    const int m_bit;

public:
    explicit WarningBitsConditionChecker(const ChainstateManager& chainman, int bit);

    int64_t BeginTime(const Consensus::Params& params) const override;
    int64_t EndTime(const Consensus::Params& params) const override;
    int Period(const Consensus::Params& params) const override;
    int Threshold(const Consensus::Params& params) const override;
    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const override;

    /** Compute warnings without counting blocks that are below the warning floor. */
    ThresholdState GetStateFor(const CBlockIndex* pindexPrev, const Consensus::Params& params,
                               ThresholdConditionCache& cache) const;
};

#endif // DIGIBYTE_NODE_WARNINGS_H
