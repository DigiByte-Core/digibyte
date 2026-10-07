#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <headerssync.h>
#include <pow.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <deque>
#include <iterator>
#include <vector>

static void initialize_headers_sync_state_fuzz()
{
    static const auto testing_setup = MakeNoLogFileContext<>(
        /*chain_type=*/ChainType::MAIN);
}

void MakeHeadersContinuous(
    const CBlockHeader& genesis_header,
    const std::vector<CBlockHeader>& all_headers,
    std::vector<CBlockHeader>& new_headers)
{
    Assume(!new_headers.empty());

    const CBlockHeader* prev_header{
        all_headers.empty() ? &genesis_header : &all_headers.back()};

    for (auto& header : new_headers) {
        header.hashPrevBlock = prev_header->GetHash();

        prev_header = &header;
    }
}

class FuzzedHeadersSyncState : public HeadersSyncState
{
public:
    FuzzedHeadersSyncState(const unsigned commit_offset, const CBlockIndex* chain_start, const arith_uint256& minimum_required_work)
        : HeadersSyncState(/*id=*/0, Params().GetConsensus(), chain_start, minimum_required_work)
    {
        const_cast<unsigned&>(m_commit_offset) = commit_offset;
    }
};

FUZZ_TARGET(headers_sync_state, .init = initialize_headers_sync_state_fuzz)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    auto mock_time{ConsumeTime(fuzzed_data_provider)};

    CBlockHeader genesis_header{Params().GenesisBlock()};
    CBlockIndex start_index(genesis_header);

    if (mock_time < start_index.GetMedianTimePast()) return;
    SetMockTime(mock_time);

    const uint256 genesis_hash = genesis_header.GetHash();
    start_index.phashBlock = &genesis_hash;

    arith_uint256 min_work{UintToArith256(ConsumeUInt256(fuzzed_data_provider))};
    FuzzedHeadersSyncState headers_sync(
        /*commit_offset=*/fuzzed_data_provider.ConsumeIntegralInRange<unsigned>(1, 1024),
        /*chain_start=*/&start_index,
        /*minimum_required_work=*/min_work);

    // Store headers for potential redownload phase.
    std::vector<CBlockHeader> all_headers;
    std::vector<CBlockHeader>::const_iterator redownloaded_it;
    bool presync{true};
    bool requested_more{true};

    while (requested_more) {
        std::vector<CBlockHeader> headers;

        // Consume headers from fuzzer or maybe replay headers if we got to the
        // redownload phase.
        if (presync || fuzzed_data_provider.ConsumeBool()) {
            auto deser_headers = ConsumeDeserializable<std::vector<CBlockHeader>>(fuzzed_data_provider);
            if (!deser_headers || deser_headers->empty()) return;

            if (fuzzed_data_provider.ConsumeBool()) {
                MakeHeadersContinuous(genesis_header, all_headers, *deser_headers);
            }

            headers.swap(*deser_headers);
        } else if (auto num_headers_left{std::distance(redownloaded_it, all_headers.cend())}; num_headers_left > 0) {
            // Consume some headers from the redownload buffer (At least one
            // header is consumed).
            auto begin_it{redownloaded_it};
            std::advance(redownloaded_it, fuzzed_data_provider.ConsumeIntegralInRange<int>(1, num_headers_left));
            headers.insert(headers.cend(), begin_it, redownloaded_it);
        }

        if (headers.empty()) return;
        auto result = headers_sync.ProcessNextHeaders(headers, fuzzed_data_provider.ConsumeBool());
        requested_more = result.request_more;

        if (result.request_more) {
            if (presync) {
                all_headers.insert(all_headers.cend(), headers.cbegin(), headers.cend());

                if (headers_sync.GetState() == HeadersSyncState::State::REDOWNLOAD) {
                    presync = false;
                    redownloaded_it = all_headers.cbegin();

                    // If we get to redownloading, the presynced headers need
                    // to have the min amount of work on them.
                    const auto work = CalculateHeadersWork(all_headers, start_index);
                    assert(work && *work >= min_work);
                }
            }

            (void)headers_sync.NextHeadersRequestLocator();
        }
    }
}

FUZZ_TARGET(headers_work, .init = initialize_headers_sync_state_fuzz)
{
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    const auto& params = Params().GetConsensus();
    constexpr std::array<int, 6> algorithms{ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT, ALGO_ODO};
    const std::array<int, 3> event_heights{
        static_cast<int>(params.workComputationChangeTarget),
        std::max(static_cast<int>(params.algoSwapChangeTarget + 1), params.DeploymentHeight(Consensus::DEPLOYMENT_ODO)),
        static_cast<int>(params.nGroestlDeactivationHeight),
    };
    const int next_height = event_heights[provider.ConsumeIntegralInRange<size_t>(0, event_heights.size() - 1)] +
        provider.ConsumeIntegralInRange<int>(-2, 2);
    const size_t count = provider.ConsumeIntegralInRange<size_t>(1, 300);
    const bool only_scrypt = provider.ConsumeBool();

    // The reference keeps every record. The download tracker must agree while
    // discarding old records, including when an algorithm disappears for a while.
    std::deque<CBlockIndex> full_chain;
    for (int i = 0; i < 100; ++i) {
        auto* previous = full_chain.empty() ? nullptr : &full_chain.back();
        full_chain.emplace_back();
        auto& index = full_chain.back();
        index.pprev = previous;
        index.nHeight = next_height - 100 + i;
        index.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algorithms[i % algorithms.size()]);
        index.nTime = (previous ? previous->nTime : 1500000000U) + provider.ConsumeIntegralInRange<uint32_t>(1, 240);
        arith_uint256 target = UintToArith256(params.powLimit);
        target >>= provider.ConsumeIntegralInRange<unsigned>(4, 64);
        index.nBits = target.GetCompact();
    }
    HeadersWorkState work_state(params, full_chain.back());
    const size_t history_limit = NUM_ALGOS * params.nAveragingInterval + CBlockIndex::nMedianTimeSpan;

    for (size_t i = 0; i < count; ++i) {
        auto* previous = &full_chain.back();
        const int height = previous->nHeight + 1;
        const bool algo_lock = height >= params.DeploymentHeight(Consensus::DEPLOYMENT_ALGOLOCK) ||
            height >= params.nGroestlDeactivationHeight;
        int algo = only_scrypt ? ALGO_SCRYPT : algorithms[provider.ConsumeIntegralInRange<size_t>(0, algorithms.size() - 1)];
        if (algo_lock && !IsAlgoActive(previous, params, algo)) algo = ALGO_SCRYPT;
        CBlockHeader header;
        header.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(algo);
        header.nTime = previous->nTime + provider.ConsumeIntegralInRange<uint32_t>(1, 240);
        header.nBits = GetNextWorkRequired(previous, &header, params, algo);

        const size_t history_size = work_state.GetHistorySize();
        if (height >= params.workComputationChangeTarget && provider.ConsumeBool()) {
            CBlockHeader invalid = header;
            invalid.nBits ^= 1;
            assert(!work_state.AddHeader(invalid));
            assert(work_state.GetHistorySize() == history_size);
        }
        if (algo_lock && provider.ConsumeBool()) {
            // Correct difficulty cannot make a retired algorithm valid again.
            assert(!IsAlgoActive(previous, params, ALGO_GROESTL));
            CBlockHeader invalid = header;
            invalid.nVersion = BLOCK_VERSION_DEFAULT | GetVersionForAlgo(ALGO_GROESTL);
            invalid.nBits = GetNextWorkRequired(previous, &invalid, params, ALGO_GROESTL);
            assert(!work_state.AddHeader(invalid));
            assert(work_state.GetHistorySize() == history_size);
        }

        const auto work = work_state.AddHeader(header);
        assert(work);
        full_chain.emplace_back();
        auto& index = full_chain.back();
        index.pprev = previous;
        index.nHeight = height;
        index.nVersion = header.nVersion;
        index.nTime = header.nTime;
        index.nBits = header.nBits;
        assert(*work == GetBlockProof(index));
        assert(work_state.GetHistorySize() <= history_limit);
    }
}
