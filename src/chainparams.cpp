// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>

#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/params.h>
#include <deploymentinfo.h>
#include <logging.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

void ReadSigNetArgs(const ArgsManager& args, CChainParams::SigNetOptions& options)
{
    if (args.IsArgSet("-signetseednode")) {
        options.seeds.emplace(args.GetArgs("-signetseednode"));
    }
    if (args.IsArgSet("-signetchallenge")) {
        const auto signet_challenge = args.GetArgs("-signetchallenge");
        if (signet_challenge.size() != 1) {
            throw std::runtime_error("-signetchallenge cannot be multiple values.");
        }
        const auto val{TryParseHex<uint8_t>(signet_challenge[0])};
        if (!val) {
            throw std::runtime_error(strprintf("-signetchallenge must be hex, not '%s'.", signet_challenge[0]));
        }
        options.challenge.emplace(*val);
    }
}

void ReadRegTestArgs(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    if (auto value = args.GetBoolArg("-fastprune")) options.fastprune = *value;

    for (const std::string& arg : args.GetArgs("-testactivationheight")) {
        const auto found{arg.find('@')};
        if (found == std::string::npos) {
            throw std::runtime_error(strprintf("Invalid format (%s) for -testactivationheight=name@height.", arg));
        }

        const auto value{arg.substr(found + 1)};
        int32_t height;
        if (!ParseInt32(value, &height) || height < 0 || height >= std::numeric_limits<int>::max()) {
            throw std::runtime_error(strprintf("Invalid height value (%s) for -testactivationheight=name@height.", arg));
        }

        const auto deployment_name{arg.substr(0, found)};
        if (const auto buried_deployment = GetBuriedDeployment(deployment_name)) {
            options.activation_heights[*buried_deployment] = height;
        } else {
            throw std::runtime_error(strprintf("Invalid name (%s) for -testactivationheight=name@height.", arg));
        }
    }

    for (const std::string& strDeployment : args.GetArgs("-vbparams")) {
        std::vector<std::string> vDeploymentParams = SplitString(strDeployment, ':');
        if (vDeploymentParams.size() < 3 || 4 < vDeploymentParams.size()) {
            throw std::runtime_error("Version bits parameters malformed, expecting deployment:start:end[:min_activation_height]");
        }
        CChainParams::VersionBitsParameters vbparams{};
        if (!ParseInt64(vDeploymentParams[1], &vbparams.start_time)) {
            throw std::runtime_error(strprintf("Invalid nStartTime (%s)", vDeploymentParams[1]));
        }
        if (!ParseInt64(vDeploymentParams[2], &vbparams.timeout)) {
            throw std::runtime_error(strprintf("Invalid nTimeout (%s)", vDeploymentParams[2]));
        }
        if (vDeploymentParams.size() >= 4) {
            if (!ParseInt32(vDeploymentParams[3], &vbparams.min_activation_height)) {
                throw std::runtime_error(strprintf("Invalid min_activation_height (%s)", vDeploymentParams[3]));
            }
        } else {
            vbparams.min_activation_height = 0;
        }
        bool found = false;
        for (int j=0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
            if (vDeploymentParams[0] == VersionBitsDeploymentInfo[j].name) {
                options.version_bits_parameters[Consensus::DeploymentPos(j)] = vbparams;
                found = true;
                LogPrintf("Setting version bits activation parameters for %s to start=%ld, timeout=%ld, min_activation_height=%d\n", vDeploymentParams[0], vbparams.start_time, vbparams.timeout, vbparams.min_activation_height);
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(strprintf("Invalid deployment (%s)", vDeploymentParams[0]));
        }
    }

    // Handle DigiDollar specific activation height for regtest.
    // DigiDollar is a buried deployment (BIP90): the knob retargets the buried
    // activation height together with the static DD/oracle/MuSig2 height gates,
    // so DigiDollar activates at exactly this height. (Pre-burial this knob ran
    // the real BIP9 state machine, which activated at the first 144-block
    // window boundary >= max(432, N).)
    if (auto digidollar_height = args.GetIntArg("-digidollaractivationheight")) {
        if (*digidollar_height < 0 || *digidollar_height >= std::numeric_limits<int>::max()) {
            throw std::runtime_error(strprintf("Invalid height value (%d) for -digidollaractivationheight.", *digidollar_height));
        }
        options.digidollar_activation_height = static_cast<int>(*digidollar_height);
        LogPrintf("Setting DigiDollar activation height for regtest to %d (buried deployment)\n", *digidollar_height);
    }

    // Thaw Day height for regtest: the one height at which every consensus
    // change of the DigiDollar Thaw Day release takes effect. Independent of
    // -digidollaractivationheight (it may lie before, at, or after DigiDollar
    // activation; DigiDollar::IsThawDayActive handles the order). Parsed
    // strictly: anything that is not a whole number in [0, int max) is a
    // startup error rather than a silent zero.
    if (const auto thaw_day_height = args.GetArg("-ddthawdayheight")) {
        int32_t height;
        if (!ParseInt32(*thaw_day_height, &height) || height < 0 || height >= std::numeric_limits<int>::max()) {
            throw std::runtime_error(strprintf("Invalid height value (%s) for -ddthawdayheight: expected a whole number from 0 up to, but not including, %d.", *thaw_day_height, std::numeric_limits<int>::max()));
        }
        options.dd_thaw_day_height = int{height};
        LogPrintf("Setting DigiDollar Thaw Day height for regtest to %d\n", height);
    }
}

static std::unique_ptr<const CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, const ChainType chain)
{
    // The Thaw Day height of a public network is fixed in the release and
    // cannot be moved from the command line or the config file; only regtest
    // accepts the knob. Refusing to start is safer than quietly ignoring the
    // option, which could leave an operator believing a different height is
    // in force.
    if (chain != ChainType::REGTEST && args.IsArgSet("-ddthawdayheight")) {
        throw std::runtime_error(strprintf("-ddthawdayheight is only accepted on regtest; the Thaw Day height of the %s network is fixed in the release.", ChainTypeToString(chain)));
    }

    switch (chain) {
    case ChainType::MAIN:
        return CChainParams::Main();
    case ChainType::TESTNET: {
        auto opts = CChainParams::TestNetOptions{};
        if (auto value = args.GetBoolArg("-easypow")) opts.easy_pow = *value;
        return CChainParams::TestNet(opts);
    }
    case ChainType::SIGNET: {
        auto opts = CChainParams::SigNetOptions{};
        ReadSigNetArgs(args, opts);
        return CChainParams::SigNet(opts);
    }
    case ChainType::REGTEST: {
        auto opts = CChainParams::RegTestOptions{};
        ReadRegTestArgs(args, opts);
        return CChainParams::RegTest(opts);
    }
    }
    assert(false);
}

void SelectParams(const ChainType chain)
{
    SelectBaseParams(chain);
    globalChainParams = CreateChainParams(gArgs, chain);
}
