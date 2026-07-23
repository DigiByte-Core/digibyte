// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <kernel/chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/oracle.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <arith_uint256.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <type_traits>

// Helper function to parse public key from hex string
static CPubKey ParsePubKey(const std::string& hex) {
    std::vector<unsigned char> data = ParseHex(hex);
    CPubKey pubkey(data);
    if (!pubkey.IsValid()) {
        throw std::runtime_error("Invalid oracle public key: " + hex);
    }
    return pubkey;
}

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "USA Today: 10/Jan/2014, Target: Data stolen from up to 110M customers";
    const CScript genesisOutputScript = CScript() << 0x0 << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

/**
 * Main network on which people trade goods and services.
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        m_chain_type = ChainType::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 8409600; // DigiByte halving interval
        // BIP16 and Taproot exceptions (script validation rules)
        consensus.script_flag_exceptions.emplace( // BIP16 exception
            uint256S("0x00000000000002dc756eebf4f49723ed8d30cc28a5f108eb94b1ba88ac4f9c22"), SCRIPT_VERIFY_NONE);
        consensus.script_flag_exceptions.emplace( // Taproot exception
            uint256S("0x0000000000000000000f14c35b2d841e986ab5441de8c585d5ffe55ea1e395ad"), SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS);

        // BIP34, BIP65 and BIP66, CSV and Segwit were activated simultaneously
        // DEPLOYMENT_NVERSIONBIPS, DEPLOYMENT_CSV, DEPLOYMENT_SEGWIT
        consensus.BIP34Hash = uint256S("0xadd8ca420f557f62377ec2be6e6f47b96cf2e68160d58aeb7b73433de834cca0");
        consensus.BIP34Height = consensus.BIP65Height = consensus.BIP66Height = 4394880; // add8ca420f557f62377ec2be6e6f47b96cf2e68160d58aeb7b73433de834cca0
        consensus.CSVHeight = consensus.SegwitHeight = 4394880;
        consensus.MinBIP9WarningHeight = 483840; // segwit activation height + miner confirmation window
        consensus.powLimit = ArithToUint256(~arith_uint256(0) >> 20);
        consensus.initialTarget[ALGO_ODO] = ArithToUint256(~arith_uint256(0) >> 40); // 256 difficulty
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 60 / 4; // 15 seconds
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fEasyPow = false;
        consensus.fPowNoRetargeting = false;
        consensus.fRbfEnabled = false;

        // DigiByte Specific Consensus Code
        consensus.nOdoShapechangeInterval = 10*24*60*60; // 10 days
        consensus.nRuleChangeActivationThreshold = 28224; // 28224 - 70% of 40320 blocks
        consensus.nMinerConfirmationWindow = 40320; // nPowTargetTimespan / nPowTargetSpacing 40320 blocks main net - 1 week

        // Ignore versionbits warnings below the most recent buried activation
        // (DigiDollar/AlgoLock at 23,869,440): the historical bit-2/23/0
        // signaling periods must not trigger "unknown new rules" warnings now
        // that those deployments are buried. Also covers Odo and Segwit.
        consensus.MinBIP9WarningHeight = 23909760; // DigiDollar/AlgoLock activation height + miner confirmation window

        // DigiByte Hard Fork Block Heights
        consensus.multiAlgoDiffChangeTarget = 145000; // Block 145,000 MultiAlgo Hard Fork
        consensus.alwaysUpdateDiffChangeTarget = 400000; // Block 400,000 MultiShield Hard Fork
        consensus.workComputationChangeTarget = 1430000; // Block 1,430,000 DigiSpeed Hard Fork
        consensus.algoSwapChangeTarget = 9100000; // Block 9,100,000 Odo PoW Hard Fork
        consensus.OdoHeight = 9112320; // 906b712a7b1f54f10b0faf86111e832ddb7b8ce86ac71a4edd2c61e5ccfe9428
        consensus.nGroestlDeactivationHeight = 23808000; // v9.26.2 activation (~7 days for miner upgrade): reject reactivated Groestl / unknown algos
        consensus.ReserveAlgoBitsHeight = 8547840; // d2c03966aeef35f739b222c8332b68df2676204d49c390b3a2544b967c46163f

        // DigiByte-specific difficulty adjustment parameters
        consensus.nTargetTimespan = 0.10 * 24 * 60 * 60; // 2.4 hours
        consensus.nTargetSpacing = 60; // 60 seconds
        consensus.nInterval = consensus.nTargetTimespan / consensus.nTargetSpacing;
        consensus.nDiffChangeTarget = 67200; // DigiShield Hard Fork Block BIP34Height 67,200

        // Old 1% monthly DGB Reward before 15 second block change
        consensus.patchBlockRewardDuration = 10080; //10080; - No longer used
        //4 blocks per min, x60 minutes x 24hours x 14 days = 80,160 blocks for 0.5% reduction in DGB reward supply - No longer used
        consensus.patchBlockRewardDuration2 = 80160; //80160;
        consensus.nTargetTimespanRe = 1*60; // 60 Seconds
        consensus.nTargetSpacingRe = 1*60; // 60 seconds
        consensus.nIntervalRe = consensus.nTargetTimespanRe / consensus.nTargetSpacingRe; // 1 block

        consensus.nAveragingInterval = 10; // 10 blocks
        consensus.multiAlgoTargetSpacing = 30*5; // NUM_ALGOS * 30 seconds
        consensus.multiAlgoTargetSpacingV4 = 15*5; // NUM_ALGOS * 15 seconds
        consensus.nAveragingTargetTimespan = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacing; // 10* NUM_ALGOS * 30
        consensus.nAveragingTargetTimespanV4 = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacingV4; // 10 * NUM_ALGOS * 15

        consensus.nMaxAdjustDown = 40; // 40% adjustment down
        consensus.nMaxAdjustUp = 20; // 20% adjustment up
        consensus.nMaxAdjustDownV3 = 16; // 16% adjustment down
        consensus.nMaxAdjustUpV3 = 8; // 8% adjustment up
        consensus.nMaxAdjustDownV4 = 16;
        consensus.nMaxAdjustUpV4 = 8;

        consensus.nMinActualTimespan = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUp) / 100;
        consensus.nMaxActualTimespan = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustDown) / 100;
        consensus.nMinActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUpV3) / 100;
        consensus.nMaxActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustDownV3) / 100;
        consensus.nMinActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 - consensus.nMaxAdjustUpV4) / 100;
        consensus.nMaxActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 + consensus.nMaxAdjustDownV4) / 100;

        consensus.nLocalTargetAdjustment = 4; //target adjustment per algo
        consensus.nLocalDifficultyAdjustment = 4; //local difficulty adjustment
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 27;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Buried deployments (BIP90). All three activated via BIP9 and are now
        // hardcoded at their actual on-chain activation ('since') heights:
        // - Taproot (BIPs 340-342): BIP9 bit 2, start 2025-01-10, activated at
        //   21,168,000 (= 525 * 40320).
        // - DigiDollar: BIP9 bit 23, start 2026-06-01, min_activation_height
        //   floor 23,627,520; locked in and activated at 23,869,440
        //   (= 592 * 40320). The static nDDActivationHeight/
        //   nOracleActivationHeight gates below stay at the 23,627,520 floor.
        // - AlgoLock (reject reactivated Groestl / unknown-algo blocks): BIP9
        //   bit 0, start 2026-06-29, activated at 23,869,440 alongside
        //   DigiDollar; nGroestlDeactivationHeight (23,808,000) remains the
        //   unconditional static backstop.
        consensus.TaprootHeight = 21168000;
        consensus.DigiDollarHeight = 23869440;
        consensus.AlgoLockHeight = 23869440;

        // The best chain should have at least this much work.
        // NOTE: must stay reachable by the headers pre-sync, which measures *contextless*
        // work (~28% of real chainwork in DigiByte's multi-algo model). The 2026-06-26
        // refresh set this to a real-chainwork value (~98.68% of tip), which pre-sync can
        // never reach, breaking fresh-node sync (headers reset loop). Reverted to 0x00
        // (mainnet's historical value) until pre-sync computes contextual work. See
        // HEADERS_SYNC_FIX_PLAN.md.
        consensus.nMinimumChainWork = uint256S("0x00");

        // By default assume that the signatures in ancestors of this block are valid block 23,500,000.
        consensus.defaultAssumeValid = uint256S("0xade47d5ccbb92cb1d965b97a187bdbf65bf74be6a3709cb6a01339f8c2856deb"); // Block 23,500,000

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xc3;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xda;
        nDefaultPort = 12024;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 32;
        m_assumed_chain_state_size = 1;

        genesis = CreateGenesisBlock(1389388394, 2447652, 0x1e0ffff0, 1, 8000);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x7497ea1b465eb39f1c8f507bc877078fe016d6fcb6dfad3a64c98dcc6e1e8496"));
        assert(genesis.hashMerkleRoot == uint256S("0x72ddd9496b004221ed0557358846d9248ecd4c440ebd28ed901efc18757d0fad"));

        // The current status of the DigiByte DNS Seed Servers can be checked here: http://digibyteseed.com/
        // If you notice a problem with an exiting Seed Server, please contact the DigiByte Critical Infrastructure team (DGBCIT)
        // via the #DGBCIT channel on the DigiByte Discord server: https://discord.com/channels/878200503815782400/1133815334013509764
        // Alternatively, create an issue ticket here: https://github.com/DigiByte-Core/digibyte/issues

        // When adding a new MAINNET Seed Server URL below, please include the name of the person in charge of it
        // and their Github handle so they can be contacted in an emergency.

        // DigiByte MAINNET DNS Seed Server:
        vSeeds.emplace_back("seed.digibyte.io"); // Jared Tate @JaredTate
        vSeeds.emplace_back("seed.diginode.tools"); // Olly Stedall @saltedlolly
        vSeeds.emplace_back("seed.digibyte.link"); // Bastian Driessen @bastiandriessen
        vSeeds.emplace_back("seed.aroundtheblock.app"); // Mark McNiel @JohnnyLawDGB
        vSeeds.emplace_back("seed.tuyul.cc"); // Mbah Jambon @mbah_jambon

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,30);
        base58Prefixes[SCRIPT_ADDRESS_OLD] = std::vector<unsigned char>(1,5);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,63);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,128);
        base58Prefixes[SECRET_KEY_OLD] = std::vector<unsigned char>(1,158);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "dgb";

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_main), std::end(chainparams_seed_main));

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                {     0, uint256S("0x7497ea1b465eb39f1c8f507bc877078fe016d6fcb6dfad3a64c98dcc6e1e8496")},
                {  5000, uint256S("0x95753d284404118788a799ac754a3fdb5d817f5bd73a78697dfe40985c085596")},
                { 10000, uint256S("0x12f90b8744f3b965e107ad9fd8b33ba6d95a91882fbc4b5f8588d70d494bed88")},
                { 12000, uint256S("0xa1266acba91dc3d5737d9e8c6e21b7a91901f7f4c48082ce3d84dd394a13e415")},
                { 14300, uint256S("0x24f665d71b0c6c88f6f72a863e9f1ba8e835cc52d13ad895dc5426021c7d2c48")},
                { 30000, uint256S("0x17c69ef6b403571b1bd333c91fbe116e451ba8281be12aa6bafb0486764bb315")},
                { 60000, uint256S("0x57b2c612b60462a3d6c388c8b30a68cb6f7e2034eea962b12b7ef506454fa2c1")},
                {110000, uint256S("0xab2da24656493015f2fd288994661e1cc657d90aa34c755514af044aaaf1569d")},
                {141100, uint256S("0x145c2cb5239a4e019c730ce8468d927a3955529c2bae077850783da97ddbca05")},
                {141656, uint256S("0x683d27720429f28bcfa22d8385b7a06f307c8fd918d49215148fbd41a0dda595")},
                {245000, uint256S("0x852c475c605e1f20bbe60219c811abaeef08bf0d4ff87eef59200fd7a7567fa7")},
                {302000, uint256S("0xfb6d14ac5e0208f00d941db1fcbfe050f093cfd0c05ed151c809e4428bc14286")},
                {331000, uint256S("0xbd1a1d002750e1648746eb29c78d30fa1043c8b6f89d82924c4488be06fa3d19")},
                {360000, uint256S("0x8fee7e3f6c38dccd3047a3e4667c63406f835c2890024030a2ab2dc6dba7c912")},
                {400100, uint256S("0x82325a97cd97ac14b0a57408f881b1a9fc40174f8430a4580429499ac5d153c8")},
                {521000, uint256S("0xd23fd1e1f994c0586d761b71bb3530e9ab45bd0fabda3a5a2e394f3dc4d9bb04")},
                {1380000, uint256S("0x00000000000001969b1e5836dd8bf6a001d96f4a16d336e09405b62b29feead6")},
                {2000000, uint256S("0x10f522ec60d8af2e2cbd9e2268260c33fb8bbf9cd9f176b4fddcae7493c6791d")},
                {3500000, uint256S("0xbece76f2a3f53637e2ea84837a45a6ffdc0c86372ab4701c3146094f65832c80")},
                {5000000, uint256S("0x1dd2fdf6416343688eed463a7bc70b298a4f872e941e36f85cda0915d6488e25")},
                {6500000, uint256S("0xb168b7f70cbfd2e5fea07da55d9fa90dc7c65599ceb2700efe04ee6c45692e52")},
                {8000000, uint256S("0x1af919cb004bb05c369a862cb5ded70aaa123d0eac2432ceec859f6f42880660")},
                {9500000, uint256S("0x5b0351361414e520e9132ba6c5c4926d6f9ee55c41b77fffce3a16ea15d4a1be")},
                {11000000, uint256S("0x0f4ad10ae49b504246c0175f6cbab9b0f91b6568a88931e6341a83a731701054")},
                {12500000, uint256S("0x697a015b62140c9549fbc8d8b3c1d027626b2f94d337db32115e429fbf233ed7")},
                {14000000, uint256S("0xa33861c857eed46191cf6cdaf81693e0dfcd00b3a11133821b0c73fe1d7769d9")},
                {15500000, uint256S("0x000000000000000439d5c66b2fb3ec50f50a68b65f5790d338150b63488de645")},
                {17000000, uint256S("0xf167688cc0102743b135499ed9f9eff9c5bad096203150e438be0a6e783d5587")},
                {18500000, uint256S("0x745dc7b89208de482071a3a8d13eb5596d55bedc4f5ba2fa74cbea9ecf91169e")},
                {20000000, uint256S("0xf530a66ba6fe93e647f7d88a9b3f22bfe8c2c2ab1ec1b0286286f86b82d6a10f")},
                {21000000, uint256S("0x0000000000000001cb40d3be76bf601d98555a069669d963060d633ea3a140e8")},
                {21700000, uint256S("0x457f6864b52e5076a433afe3c28e3ae0bbeeaba9036a782ddb691242326fcb80")},
                {23500000, uint256S("0xade47d5ccbb92cb1d965b97a187bdbf65bf74be6a3709cb6a01339f8c2856deb")},
            }
        };

        // No mainnet AssumeUTXO snapshot is committed for v9.26.2. Keep this
        // empty until a real serialized UTXO hash and transaction count are
        // generated for a release snapshot.
        m_assumeutxo_data.clear();

        chainTxData = ChainTxData{
            // DigiByte: Data from DigiByte blockchain
            // DigiByte has ~15 second blocks vs Bitcoin's ~10 minutes (40x faster)
            // As of block 23,500,000.
            .nTime    = 1778906996,
            .nTxCount = 53488713,
            .dTxRate  = 0.09148870167189133,
        };

        // DigiDollar consensus parameters (mainnet)
        digidollarParams = DigiDollar::ConsensusParams();
        digidollarParams.oracleThreshold = 7;   // 7 signatures required
        digidollarParams.activeOracles = 35;    // 35 active oracle keys
        digidollarParams.oracleCount = 35;      // 35 configured oracle slots

        // Initialize DigiDollar Oracle Nodes (35 active slots)
        InitializeOracleNodes();

        // Public mainnet peers that oracle operators can use to bootstrap
        // DigiDollar P2P connectivity. These are operational seed peers, not
        // consensus trust anchors; oracle security comes from the hardcoded
        // public keys and 7-of-35 MuSig2 validation.
        vOracleSeedPeers = {
            "oracle1.digibyte.io:12024",       // DigiByte.io / Jared Tate
            "digihash.digibyte.io:12024",     // DigiHash Mining Pool
            "oracleseed.digibyte.link:12024", // Bastian Driessen
            "digiscope.me:12024",             // DigiScope / JohnnyLawDGB
            "oracle.dgbmaxi.com:12024",       // digibyte-maxi / Ycagel
        };

        // Mainnet-specific oracle and activation settings
        consensus.nDDOracleEpochBlocks = 40;       // Rotate oracle signing epochs every 40 blocks (~10 minutes)
        consensus.nDDOracleUpdateInterval = 4;      // Update price every 4 blocks (~1 minute)
        consensus.nDDActivationHeight = 23627520;   // DigiDollar activation height — aligned with BIP9 min_activation_height
        // Enforce the $100 minimum on mainnet as soon as DigiDollar activates.
        digidollarParams.minMintAmountActivationHeight = consensus.nDDActivationHeight;

        // Oracle system — DigiDollar V1 launches with MuSig2 bundles only.
        consensus.nOracleActivationHeight = consensus.nDDActivationHeight;
        consensus.nOracleEpochLength = 40;            // 10 minutes (40 blocks * 15 seconds)
        consensus.nOracleRequiredMessages = 7;        // 7 signatures required before aggregation
        consensus.nOracleTotalOracles = 35;           // 35 configured oracle bitmap slots
        consensus.nDigiDollarMuSig2Height = consensus.nDDActivationHeight;  // MuSig2 activates alongside DigiDollar

        // DD mint volatility anchor fix: concrete flag-day height to be chosen
        // at release; the oracle watchdog protects minting until then.
        consensus.nDDVolatilityFixHeight = std::numeric_limits<int>::max();

        // MuSig2 oracle configuration — 7 signatures from the active key roster.
        // Same oracle operator set as testnet. To add/replace active operators:
        //   1. Generate keypair via `digibyte-cli generateoraclekey <id>`
        //   2. Add x-only pubkey to vOraclePublicKeys in slot order
        //   3. Add OracleNodeInfo to vOracleNodes (same slot order)
        //   4. Update nOraclePubkeyCount/nOracleConsensusRequired
        //   5. Deploy via coordinated software upgrade
        consensus.nOraclePubkeyCount = 35;
        consensus.nOracleConsensusRequired = 7;

        // Oracle public keys (x-only, 32 bytes) — ordered by active oracle slot (0-34).
        // All 35 RC44 operator keys are configured for the launch roster.
        consensus.vOraclePublicKeys.clear();
        consensus.vOraclePublicKeys.push_back("45c1c7aeb4559f1b79629e0de36fb101cf279a07c5853e305240233e6945882e");  // 0: DigiByte.Io Oracle (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("615653c7883eadb97f5625b0e7fecc960094b9d33a1e281c3a6dfbaa47d529ef");  // 1: Green Candle (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("842d9481a07d949a60b526cea8d315db24f467f33901c1e73875f378bb40c75d");  // 2: Bastian (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("ddb1e57cc9691d4f4aef18b7b46a7240170f73307037822444b4a76327f6d27e");  // 3: DanGB (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("d4199fb00150d02ffae0b1b3fed207a08029c91a1d044fbc27125ac8f6d45261");  // 4: Shenger (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("5e80419393fad8ff0426bd32bde8ccc6570b024a9138660f3fae5ecd60bc195a");  // 5: Ycagel (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("b9a11dde09f69e864223615daecf4e0829b6e2f5e417f50f8cfd74d0a6181488");  // 6: Aussie (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("d15eb49e30bedecbf37f5863d266ea6ca19e81863f4df268d717c62013741f79");  // 7: LookInto (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("99a8442a35abaac5b9c4922e0754d0de6c8ed9d946a1ad9690b62f89a791b374");  // 8: JohnnyLawDGB (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("20bcc8be5f739f826c771f86f45eda0b5b50d11d8f8832c92ed626a65c44c4f2");  // 9: Ogilvie (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("b710dfc3a74137a0d9f3e26d3fd5091ba9ce29b990e12920ac1101b001e23858");  // 10: ChopperBrian (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("b9ca28f68bbe4f0b6f93c08b184e81e11b754906f2426739a1cfee48508276ca");  // 11: Crypto Corner Shop (mainnet RC46 key)
        // Slot 12: DaPunzy real key — operator-supplied and live at oracle_id=12.
        consensus.vOraclePublicKeys.push_back("2c4422fb8e5eac5d27423272d35abe6cff0acb19ab051e51204e3392365c18f3");  // 12: DaPunzy (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("58889fb092fdf366004032d3af7cc35fddd951eb8413409699e879f795111a0e");  // 13: DigiByte_FORCE (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("4b013d2203e06461187604992333b367db165f0e79905c0974ba8a554bc69fec");  // 14: Neel (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("0f809ccbeea32bcca9c817dbf674e38e2fbc234f4462b88612876287489197a9");  // 15: DigiSwarm (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("943744e1635c2871db786b2ef98d3d5007b2d1f8bb622b44a99b31fbe63cc5bf");  // 16: GTO90 (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("9d83219cf9056854da1f56fbf1d792ef14b1853ec027cd9c0a51465e2ec2afaa");  // 17: digibyte-maxi (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("e2fa8be3480929a59fb93b284e49f4b74bfb52ffed3934de751bdb1c33c54e6e");  // 18: Anthony (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("507a43d15e0b6c149dd1835347879a1b723bdbecb4cc8afe124c52df37212295");  // 19: mbah_jambon (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("1bf934b18dd8bab94cec0dfc127f293afd542b8042df60a439affd5e64831285");  // 20: Camden (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("75481d5d543b27e298ba726674730d11d5bc1139f13205957b20d9936225ea38");  // 21: Twoface123 (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("deec78492920e9010c3015c50bacbcc158272a941220bdf74208f4796d962542");  // 22: LivingTheLife (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("5b25677c412923376670284021ed9e99b53ebee6f4e3ef6123e742fb020e1f48");  // 23: ChozenOne43 (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("39543917295ddbba5bf3fa1198f3d7f63321364192f5839e9699a6f50572b68a");  // 24: ckunchained (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("d476d0565e20ca0901c1b61a0aee36e3a6544346df48bdf45447ea051827ddee");  // 25: JMag (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("36031b96a287f671172106f2d62cbeec0e547d31b591169ebbe9ddef7277e5b9");  // 26: HashedMax (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("80f5bdf4f359c7daccfa8156d9569f8b48e36619feadb38adb5292a93c877563");  // 27: DennisPitallano (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("810ab54165a93b0d219494da9a7830ef245f49696662ddc49644d25b081c79d3");  // 28: DigiHash Mining Pool (mainnet key)
        consensus.vOraclePublicKeys.push_back("1ff6510c5c34604fdde76b866656f40a966e04fbabf27589155b754033139d3d");  // 29: medgboracle3452 (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("17aaa874bc5d71a1f9f2e0b54ca9188cabd732fe9776530aa79e7ac2dc0f0b67");  // 30: DigibyteDaily (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("a59c6d65b529fe956704ea6199a55dc1363f4fd4b79d56903f2d1737c7b09284");  // 31: Peer2Peer / DigiRoos (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("451f9d309a35d1eeefdc2bdacadb0d3dd88c50c51dce54317de6ae6bceb49370");  // 32: 3DogsKanab (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("2218c8607635c1f4d71d5497a9d395404fe3c01adf58f92432fbc84b3849534c");  // 33: LiberatedLark (mainnet RC46 key)
        consensus.vOraclePublicKeys.push_back("8e6bc5e7addbf15369c87cd194cc63590c710b8d5c9e592d52ea13ce03927446");  // 34: Manu_DGB_oracle (mainnet RC46 key)

        LogPrintf("Oracle: Mainnet MuSig2 oracle bundles active at block %d, %d-of-%d quorum\n",
                 consensus.nDigiDollarMuSig2Height, consensus.nOracleConsensusRequired,
                 consensus.nOraclePubkeyCount);
    }

private:
    void InitializeOracleNodes() {
        // DigiDollar Oracle Nodes - 35 active slots
        // These use compressed public keys (33 bytes) and unique endpoints
        vOracleNodes = {
            // Oracle 0-9: Active MuSig2 operator set. Slots 0-16 must stay aligned
            // with consensus.vOraclePublicKeys for MuSig2 bitmap/quorum validation.
            {0,  ParsePubKey("0345c1c7aeb4559f1b79629e0de36fb101cf279a07c5853e305240233e6945882e"), "oracle1.digibyte.io:12024", true},  // DigiByte.Io Oracle (mainnet RC46 key)
            {1,  ParsePubKey("02615653c7883eadb97f5625b0e7fecc960094b9d33a1e281c3a6dfbaa47d529ef"), "oracle2.digidollar.org:12024", true},  // Green Candle (mainnet RC46 key)
            {2,  ParsePubKey("02842d9481a07d949a60b526cea8d315db24f467f33901c1e73875f378bb40c75d"), "oracle3.digidollar.org:12024", true},  // Bastian (mainnet RC46 key)
            {3,  ParsePubKey("03ddb1e57cc9691d4f4aef18b7b46a7240170f73307037822444b4a76327f6d27e"), "oracle4.digidollar.org:12024", true},  // DanGB (mainnet RC46 key)
            {4,  ParsePubKey("02d4199fb00150d02ffae0b1b3fed207a08029c91a1d044fbc27125ac8f6d45261"), "oracle5.digidollar.org:12024", true},  // Shenger (mainnet RC46 key)
            {5,  ParsePubKey("035e80419393fad8ff0426bd32bde8ccc6570b024a9138660f3fae5ecd60bc195a"), "oracle6.digidollar.org:12024", true},  // Ycagel (mainnet RC46 key)
            {6,  ParsePubKey("03b9a11dde09f69e864223615daecf4e0829b6e2f5e417f50f8cfd74d0a6181488"), "oracle7.digidollar.org:12024", true},  // Aussie (mainnet RC46 key)
            {7,  ParsePubKey("03d15eb49e30bedecbf37f5863d266ea6ca19e81863f4df268d717c62013741f79"), "oracle8.digidollar.org:12024", true},  // LookInto (mainnet RC46 key)
            {8,  ParsePubKey("0299a8442a35abaac5b9c4922e0754d0de6c8ed9d946a1ad9690b62f89a791b374"), "oracle9.digidollar.org:12024", true},  // JohnnyLawDGB (mainnet RC46 key)
            {9,  ParsePubKey("0220bcc8be5f739f826c771f86f45eda0b5b50d11d8f8832c92ed626a65c44c4f2"), "oracle10.digidollar.org:12024", true},  // Ogilvie (mainnet RC46 key)

            // Oracle 10-16: Keep the first 17 slots aligned with the active roster.
            {10, ParsePubKey("02b710dfc3a74137a0d9f3e26d3fd5091ba9ce29b990e12920ac1101b001e23858"), "oracle11.digidollar.org:12024", true},  // ChopperBrian (mainnet RC46 key)
            {11, ParsePubKey("03b9ca28f68bbe4f0b6f93c08b184e81e11b754906f2426739a1cfee48508276ca"), "oracle12.digidollar.org:12024", true},  // Crypto Corner Shop (mainnet RC46 key)
            {12, ParsePubKey("032c4422fb8e5eac5d27423272d35abe6cff0acb19ab051e51204e3392365c18f3"), "oracle13.digidollar.org:12024", true},  // DaPunzy (mainnet RC46 key)
            {13, ParsePubKey("0258889fb092fdf366004032d3af7cc35fddd951eb8413409699e879f795111a0e"), "oracle14.digidollar.org:12024", true},  // DigiByte_FORCE (mainnet RC46 key)
            {14, ParsePubKey("034b013d2203e06461187604992333b367db165f0e79905c0974ba8a554bc69fec"), "oracle15.digidollar.org:12024", true},  // Neel (mainnet RC46 key)
            {15, ParsePubKey("030f809ccbeea32bcca9c817dbf674e38e2fbc234f4462b88612876287489197a9"), "oracle16.digidollar.org:12024", true},  // DigiSwarm (mainnet RC46 key)
            {16, ParsePubKey("03943744e1635c2871db786b2ef98d3d5007b2d1f8bb622b44a99b31fbe63cc5bf"), "oracle17.digidollar.org:12024", true},  // GTO90 (mainnet RC46 key)

            // Oracle 17-34: Active launch roster additions.
            {17, ParsePubKey("039d83219cf9056854da1f56fbf1d792ef14b1853ec027cd9c0a51465e2ec2afaa"), "oracle18.digidollar.org:12024", true},  // digibyte-maxi (mainnet RC46 key)
            {18, ParsePubKey("03e2fa8be3480929a59fb93b284e49f4b74bfb52ffed3934de751bdb1c33c54e6e"), "oracle19.digidollar.org:12024", true},  // Anthony (mainnet RC46 key)
            {19, ParsePubKey("02507a43d15e0b6c149dd1835347879a1b723bdbecb4cc8afe124c52df37212295"), "oracle20.digidollar.org:12024", true},  // mbah_jambon (mainnet RC46 key)
            {20, ParsePubKey("021bf934b18dd8bab94cec0dfc127f293afd542b8042df60a439affd5e64831285"), "oracle21.digidollar.org:12024", true},  // Camden (mainnet RC46 key)
            {21,  ParsePubKey("0275481d5d543b27e298ba726674730d11d5bc1139f13205957b20d9936225ea38"), "oracle22.digidollar.org:12024", true},  // Twoface123 (mainnet RC46 key)
            {22,  ParsePubKey("02deec78492920e9010c3015c50bacbcc158272a941220bdf74208f4796d962542"), "oracle23.digidollar.org:12024", true},  // LivingTheLife (mainnet RC46 key)
            {23,  ParsePubKey("025b25677c412923376670284021ed9e99b53ebee6f4e3ef6123e742fb020e1f48"), "oracle24.digidollar.org:12024", true},  // ChozenOne43 (mainnet RC46 key)
            {24,  ParsePubKey("0339543917295ddbba5bf3fa1198f3d7f63321364192f5839e9699a6f50572b68a"), "oracle25.digidollar.org:12024", true},  // ckunchained (mainnet RC46 key)
            {25,  ParsePubKey("03d476d0565e20ca0901c1b61a0aee36e3a6544346df48bdf45447ea051827ddee"), "oracle26.digidollar.org:12024", true},  // JMag (mainnet RC46 key)

            {26,  ParsePubKey("0336031b96a287f671172106f2d62cbeec0e547d31b591169ebbe9ddef7277e5b9"), "oracle27.digidollar.org:12024", true},   // HashedMax (mainnet RC46 key)
            {27,  ParsePubKey("0380f5bdf4f359c7daccfa8156d9569f8b48e36619feadb38adb5292a93c877563"), "oracle28.digidollar.org:12024", true},   // DennisPitallano (mainnet RC46 key)
            {28,  ParsePubKey("03810ab54165a93b0d219494da9a7830ef245f49696662ddc49644d25b081c79d3"), "digihash.digibyte.io:12024", true},   // DigiHash Mining Pool (mainnet key)
            {29,  ParsePubKey("021ff6510c5c34604fdde76b866656f40a966e04fbabf27589155b754033139d3d"), "oracle30.digidollar.org:12024", true},   // medgboracle3452 (mainnet RC46 key)
            {30,  ParsePubKey("0317aaa874bc5d71a1f9f2e0b54ca9188cabd732fe9776530aa79e7ac2dc0f0b67"), "oracle31.digidollar.org:12024", true},   // DigibyteDaily (mainnet RC46 key)
            {31,  ParsePubKey("03a59c6d65b529fe956704ea6199a55dc1363f4fd4b79d56903f2d1737c7b09284"), "oracle32.digidollar.org:12024", true},   // Peer2Peer / DigiRoos (mainnet RC46 key)
            {32,  ParsePubKey("03451f9d309a35d1eeefdc2bdacadb0d3dd88c50c51dce54317de6ae6bceb49370"), "oracle33.digidollar.org:12024", true},   // 3DogsKanab (mainnet RC46 key)
            {33,  ParsePubKey("032218c8607635c1f4d71d5497a9d395404fe3c01adf58f92432fbc84b3849534c"), "oracle34.digidollar.org:12024", true},   // LiberatedLark (mainnet RC46 key)
            {34,  ParsePubKey("038e6bc5e7addbf15369c87cd194cc63590c710b8d5c9e592d52ea13ce03927446"), "oracle35.digidollar.org:12024", true}    // Manu_DGB_oracle (mainnet RC46 key)
        };
    }
};

/**
 * Testnet (v3): public test network which is reset from time to time.
 */
class CTestNetParams : public CChainParams {
public:
    explicit CTestNetParams(const CChainParams::TestNetOptions& options) {
        m_chain_type = ChainType::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 300;
        consensus.script_flag_exceptions.emplace( // BIP16 exception
            uint256S("0x00000000dd30457c001f4095d208cc1296b0eed002427aa599874af7a432b105"), SCRIPT_VERIFY_NONE);
        consensus.BIP34Height = 1; // BIP34 activated on testnet (Testnet reset 2025)
        consensus.BIP34Hash = uint256S("0x0");
        consensus.BIP65Height = 1; // BIP65 activated on testnet (Testnet reset 2025)
        consensus.BIP66Height = 1; // BIP66 activated on testnet (Testnet reset 2025)
        consensus.CSVHeight = 1; // CSV activated on testnet (Used in rpc activation tests)
        consensus.SegwitHeight = 0; // SEGWIT is always activated on testnet unless overridden
        // Ignore versionbits warnings below the buried DigiDollar activation
        // (600) + one confirmation window (200): bit 23 was really signaled in
        // blocks ~200-599 on testnet26 and must not warn post-burial.
        consensus.MinBIP9WarningHeight = 800;
        consensus.powLimit = ArithToUint256(~arith_uint256(0) >> 20);

        consensus.initialTarget[ALGO_SHA256D] = ArithToUint256(~arith_uint256(0) >> 31); // production testnet (2048x powLimit)

        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 60 / 4;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fEasyPow = false;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 140; // 140 - 70% of 200
        consensus.nMinerConfirmationWindow = 200; // 200 blocks for fast BIP9 testing
        consensus.fRbfEnabled = false;
        
        // DigiByte Specific Consensus Code from v8.22.2
        consensus.nTargetTimespan =  0.10 * 24 * 60 * 60; // 2.4 hours
        consensus.nTargetSpacing = 60; // 60 seconds
        consensus.nInterval = consensus.nTargetTimespan / consensus.nTargetSpacing;
        // Emission schedule heights (compressed for testnet, similar to regtest)
        // These control BOTH DigiShield activation AND emission period boundaries
        consensus.nDiffChangeTarget = 67; // DigiShield + Period III/IV boundary (mainnet: 67200)
        consensus.patchBlockRewardDuration = 10; // Weekly reward decay blocks (same as regtest)
        consensus.patchBlockRewardDuration2 = 80; // Monthly reward decay blocks (same as regtest)
        consensus.nTargetTimespanRe = 1*60; // 60 Seconds
        consensus.nTargetSpacingRe = 1*60; // 60 seconds
        consensus.nIntervalRe = consensus.nTargetTimespanRe / consensus.nTargetSpacingRe; // 1 block
        consensus.nAveragingInterval = 10; // 10 blocks
        consensus.multiAlgoTargetSpacing = 30*5; // NUM_ALGOS * 30 seconds
        consensus.multiAlgoTargetSpacingV4 = 15*5; // NUM_ALGOS * 15 seconds
        consensus.nAveragingTargetTimespan = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacing;
        consensus.nAveragingTargetTimespanV4 = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacingV4;
        consensus.nMaxAdjustDown = 40; // 40% adjustment down
        consensus.nMaxAdjustUp = 20; // 20% adjustment up
        consensus.nMaxAdjustDownV3 = 16; // 16% adjustment down
        consensus.nMaxAdjustUpV3 = 8; // 8% adjustment up
        consensus.nMaxAdjustDownV4 = 16;
        consensus.nMaxAdjustUpV4 = 8;
        consensus.nMinActualTimespan = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUp) / 100;
        consensus.nMaxActualTimespan = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustDown) / 100;
        consensus.nMinActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUpV3) / 100;
        consensus.nMaxActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustUpV3) / 100;
        consensus.nMinActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 - consensus.nMaxAdjustUpV4) / 100;
        consensus.nMaxActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 + consensus.nMaxAdjustUpV4) / 100;
        consensus.nLocalTargetAdjustment = 4; // target adjustment per algo
        consensus.nLocalDifficultyAdjustment = 4; // difficulty adjustment per algo

        // DigiByte Hard Fork Block Heights for testnet (compressed like regtest)
        // Multi-algo is enabled immediately on testnet26 so automated mining can
        // bootstrap the chain quickly from genesis.
        consensus.multiAlgoDiffChangeTarget = 0; // Height 0 MultiAlgo activation
        consensus.alwaysUpdateDiffChangeTarget = 200; // Block 200 MultiShield Hard Fork
        consensus.workComputationChangeTarget = 400; // Block 400 DigiSpeed Hard Fork
        consensus.algoSwapChangeTarget = 500; // Block 500 Odo PoW Hard Fork
        consensus.OdoHeight = 500; // Odocrypt activation at height 500
        consensus.ReserveAlgoBitsHeight = 0;
        consensus.nOdoShapechangeInterval = 1*24*60*60; // 1 day
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 27;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Buried deployments (BIP90). Taproot and AlgoLock were BIP9
        // ALWAYS_ACTIVE on testnet26 (buried height 0). DigiDollar ran real
        // BIP9 signaling on bit 23 (DEFINED 0-199, STARTED 200-399, LOCKED_IN
        // 400-599) and activated at block 600 — verified against the live
        // testnet26 chain via getdeploymentinfo (bip9.since = 600).
        consensus.TaprootHeight = 0;
        consensus.DigiDollarHeight = 600;
        consensus.AlgoLockHeight = 0;

        consensus.nMinimumChainWork = uint256S("0x00");
        consensus.defaultAssumeValid = uint256S("0x00"); //1079274

        // TESTNET26 MAGIC BYTES (2026) - DigiDollar Reset
        pchMessageStart[0] = 0xfe;
        pchMessageStart[1] = 0xc6;
        pchMessageStart[2] = 0xb9;
        pchMessageStart[3] = 0xe7;
        nDefaultPort = 12033;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 40;
        m_assumed_chain_state_size = 2;

        // DigiDollar testnet26 genesis (RC44, 2026) - 7-signature / 35-slot oracle reset
        const char* pszTimestamp = "DigiDollar Testnet26: RC44 7-of-35 Oracle Reset on DigiByte";
        const CScript genesisOutputScript = CScript() << 0x0 << OP_CHECKSIG;
        genesis = CreateGenesisBlock(pszTimestamp, genesisOutputScript, 1780156800, 1283721, 0x1e0ffff0, 1, 8000 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x0c9af936f28f7bd0e90c8f6235399063a026ed267bb53da398313b5d7aa55d82"));
        assert(genesis.hashMerkleRoot == uint256S("0x76eca59f6a477206c602b72682a901fe87c48d1a6335ec3d094d5837c508c197"));

        vFixedSeeds.clear();
        vSeeds.clear();

        // DigiByte TESTNET DNS Seed Servers:
        vSeeds.emplace_back("testnetseed.digibyte.io"); // Jared Tate @JaredTate
        vSeeds.emplace_back("testnetseed.digibyte.link"); // Bastian Driessen @bastiandriessen
        vSeeds.emplace_back("testnetseed.digibyte.services"); // Craig Donnachie @cdonnachie

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,126);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,140);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,254);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "dgbt";

        vFixedSeeds.clear();

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                // testnet26: fresh chain, no checkpoints yet
            }
        };

        m_assumeutxo_data = {
            // TODO to be specified in a future patch.
        };

        chainTxData = ChainTxData{
            // testnet26 starts from a fresh RC44 reset genesis
            .nTime    = 1780156800,
            .nTxCount = 1,
            .dTxRate  = 0.0,
        };

        // DigiDollar consensus parameters (testnet)
        digidollarParams = DigiDollar::ConsensusParams();
        // Testnet specific adjustments for easier testing
        // NOTE: DD amounts are in CENTS, not satoshis. 100 cents = $1.00
        digidollarParams.minMintAmount = 10000;            // 10,000 cents = $100 minimum
        digidollarParams.minMintAmountActivationHeight = 1;  // Activate $100 min at height 150000
        digidollarParams.maxMintAmount = 10000000;         // 10,000,000 cents = $100k maximum
        digidollarParams.oracleThreshold = 7;                         // 7 signatures required
        digidollarParams.activeOracles = 35;                          // 35 active oracle keys
        digidollarParams.oracleCount = 35;                            // 35 configured oracle slots

        // Initialize DigiDollar Oracle Nodes (same as mainnet for compatibility)
        InitializeOracleNodes();

        // Oracle system parameters — Testnet 7 signatures from the active key roster.
        // Activation heights are synchronized after nDDActivationHeight is assigned below.
        consensus.nOracleActivationHeight = 600;
        consensus.nOracleEpochLength = 40;            // 10 minutes (40 blocks * 15 seconds)
        consensus.nOracleRequiredMessages = 7;        // 7 signatures required before aggregation
        consensus.nOracleTotalOracles = 35;           // 35 configured oracle bitmap slots

        // MuSig2 oracle configuration — 7 signatures from the active key roster.
        consensus.nOraclePubkeyCount = 35;
        consensus.nOracleConsensusRequired = 7;

        // Testnet oracle public keys (x-only, 32 bytes) — ordered by active oracle slot (0-34)
        // for deterministic MuSig2 key aggregation.
        //
        // Production testnet oracle public keys:
        // 35 assigned operators for the RC44 launch roster.
        consensus.vOraclePublicKeys.clear();
        consensus.vOraclePublicKeys.push_back("e1dce189a530c1fb39dcd9282cf5f9de0e4eb257344be9fd94ce27c06005e8c7");  // 0: Jared
        consensus.vOraclePublicKeys.push_back("3dfb7a36ab40fa6fbc69b4b499eaa17bfa1958aa89ec248efc24b4c18694f990");  // 1: Green Candle
        consensus.vOraclePublicKeys.push_back("172755a320cec96c981d46c86d79a03578d73406a25e89d8edc616a8f361cb5c");  // 2: Bastian
        consensus.vOraclePublicKeys.push_back("546c07ee9d21640c4b4e96e6954bd49c3ab5bcf36c6a512603ebf75f8609da0c");  // 3: DanGB
        consensus.vOraclePublicKeys.push_back("9cef021f841794c1afc4e84d678f3c70dbe3a972330b2b6329852898443deb4f");  // 4: Shenger
        consensus.vOraclePublicKeys.push_back("85016758856ed27388501a54031fa3a678df705bf811fb8bc9abd2d7cfb6d9f7");  // 5: Ycagel
        consensus.vOraclePublicKeys.push_back("7a858e055099e4a9cf8273e9171da148d4fd00afd4376b60dc1cd09974731b51");  // 6: Aussie
        consensus.vOraclePublicKeys.push_back("2d8c9f054d7087e263016c0800ad1c2f8106859e772766b9f8179042d1792a09");  // 7: LookInto
        consensus.vOraclePublicKeys.push_back("89d5c588c8e0d311028f2f7e0db6df1a9fb0319c5e3b2cfc32efaee86538d250");  // 8: JohnnyLawDGB
        consensus.vOraclePublicKeys.push_back("d2f9b0e00ed2fb0a93d04f12eb250b4adf2e4ac8335692c7942e4cba6e462484");  // 9: Ogilvie
        consensus.vOraclePublicKeys.push_back("028a52c7a3e8f22c44e356dcda43a0e24ed5e8e284c53c902599f0947763113c");  // 10: ChopperBrian
        consensus.vOraclePublicKeys.push_back("4ef063a67b35295e9eaaa9251bc7f0effbceaedc8e9bc92504b0da832744ca08");  // 11: hallvardo (RC31 rotated key)
        consensus.vOraclePublicKeys.push_back("75d7494b55ebc3874450954ac5caca402bbd068467a89d48fe877f3e2d09eda9");  // 12: DaPunzy (RC31 rotated key)
        consensus.vOraclePublicKeys.push_back("4770d416e5f751ff310fa8e35cd48d9709ee539cbaa07c67dbf52503518cc573");  // 13: DigiByteForce (RC31 rotated key)
        consensus.vOraclePublicKeys.push_back("f3ab098eb0ceff8259c280cf5a3682e78b298dc300e26cdb4694d8efcd6a11a2");  // 14: Neel (RC30)
        consensus.vOraclePublicKeys.push_back("447153bcec341f2dad94541104f4e8c0a8b19e342dada1b2204ae56cf2b960a6");  // 15: DigiSwarm (RC30)
        consensus.vOraclePublicKeys.push_back("83b9c6d229a6347370517fc11329abebeae511055702b0408158f2205035adc7");  // 16: GTO90 (RC30)
        consensus.vOraclePublicKeys.push_back("649d750bcad5b42b3dd0f11c8d98d62ed5afd515cd986663f81c35f086e58d47");  // 17: digibyte-maxi (RC42)
        consensus.vOraclePublicKeys.push_back("45f8cb22dfde6aff8f18552c338256e0df551ca2df007f6449d6da1dbb7f4d89");  // 18: Anthony
        consensus.vOraclePublicKeys.push_back("1758a6d7f1f87c95d1a4a38415608d41463a504ea28da7c6129e2a9d654add42");  // 19: mbah_jambon
        consensus.vOraclePublicKeys.push_back("018c81746d6ddc326c993d9f2e7f2015554e97a261f3b5fe637ac5098f421a4c");  // 20: Camden
        consensus.vOraclePublicKeys.push_back("d8165aa05b045de2a9b979b23a63cca1fec865784d12ab6f3f1bca8a90f3dd86");  // 21: Twoface123
        consensus.vOraclePublicKeys.push_back("9241688b464c3f03f957cd85a3d1d6a760963be3707a16805b8064a8740e07ef");  // 22: LivingTheLife
        consensus.vOraclePublicKeys.push_back("b6302e3cc8ee6d474c3c0078c25b87ce708757e2a81e8f4f01975dc4b25e0f6d");  // 23: ChozenOne43
        consensus.vOraclePublicKeys.push_back("926ed40635d294a554ec046a96d3fa58587521385c7df58ff21ede12a31add0e");  // 24: ckunchained
        consensus.vOraclePublicKeys.push_back("4103ed4168d11dcaafa96494d5b3dd37247fa6deefa08d47f7004568792b1672");  // 25: JMag
        consensus.vOraclePublicKeys.push_back("8adf7df5fcd114178643f16aa0e3be8fa1e221ca421479e48c0bd04f2561d3a8");  // 26: HashedMax
        consensus.vOraclePublicKeys.push_back("557029e2419af54984f3f2fb600004c0a6f8573dac5730cfaab2048c80ba6894");  // 27: DennisPitallano
        consensus.vOraclePublicKeys.push_back("532efd6277226f38903401ec8317ba7cc8f13eb48dc8cb1e102fdc23488d7cef");  // 28: DigiHash Mining Pool
        consensus.vOraclePublicKeys.push_back("d566a244719aa577d828da31ad9863f94686f710ac1f0638914eff5692ec7d58");  // 29: Michael E / medgboracle3452
        consensus.vOraclePublicKeys.push_back("603a0175197a1fe28859c71c69fd0081710c5569fceceaa96ce7de386d3ebf61");  // 30: Scott K / DigibyteDaily
        consensus.vOraclePublicKeys.push_back("e96759ba5c67df6fc5cfc86977389d08fa31b4b297586f13d43bdd1569b459ba");  // 31: Peer2Peer / DigiRoos
        consensus.vOraclePublicKeys.push_back("5878eb72be3710d8c3f9585e27374011a476378f5ea7f3ab2f2830ab052ec5f8");  // 32: 3DogsKanab
        consensus.vOraclePublicKeys.push_back("5b3729a255bfbcff1bfd5b72fdb1a971b098545db7e874751179bc22c7f7f0b0");  // 33: LiberatedLark
        consensus.vOraclePublicKeys.push_back("d8fe0cd773604fa78fb1cef7d1e88a05c2473961178e40a4ac3c9aeeb4cbca87");  // 34: Manu_DGB_oracle
        //
        // LOCAL MINI-TESTNET TESTING (disabled; retained for future use).
        // Test keys derived from SHA256("digibyte_testnet_oracle_N"), N=0..23.
        // Enable this block (and the matching vOracleNodes block in
        // InitializeOracleNodes()) when running test_multi_oracle_testnet.sh.
        //
        // consensus.vOraclePublicKeys.clear();
        // consensus.vOraclePublicKeys.push_back("a69d02b2e39684deacead23e3b34191af99a124411465b19bc0a6e1384f70dee");  //  0: test oracle 0
        // consensus.vOraclePublicKeys.push_back("56baa94b6a787502146177802820189113029d31c4d99b0a5ad59bbb776efc88");  //  1: test oracle 1
        // consensus.vOraclePublicKeys.push_back("df3f298148c79840218722a4dfc81413d3dd4597a1aa3b97f12b3810bb21e4e9");  //  2: test oracle 2
        // consensus.vOraclePublicKeys.push_back("692857a4e171ce477d16afc8c4f225d479f139eaa4baee796f195f7d67927d81");  //  3: test oracle 3
        // consensus.vOraclePublicKeys.push_back("977e8e7355bf6718adf79e3e0cc874f9c98893fa450bfe9d433ae7643b6f6440");  //  4: test oracle 4
        // consensus.vOraclePublicKeys.push_back("ccdca14f48d2e2c7225e378d3d0b40f6aebbc0aef783a6971479cae4d105fc5e");  //  5: test oracle 5
        // consensus.vOraclePublicKeys.push_back("f3c51b026ce02416d806c76e046fb8c32dce4e7ab82a1bb163a1a852f521a7ac");  //  6: test oracle 6
        // consensus.vOraclePublicKeys.push_back("b003867d046e3ea6876e1d33fca9018fd9b3f130f9fcb8a0904d9f58e8cf6141");  //  7: test oracle 7
        // consensus.vOraclePublicKeys.push_back("75a81280ef4e7d977a3234ca9c6860e15243accb7565ff133fb02eda174df34e");  //  8: test oracle 8
        // consensus.vOraclePublicKeys.push_back("a31a0d23ab9d2b2e9fdea406ccc902ff6812b2455985edd54ddba37402283cbb");  //  9: test oracle 9
        // consensus.vOraclePublicKeys.push_back("9f802e732aa1b71e6ebf2385a21d70945c04d7a79adaaf1a72b8358e7d9c8466");  // 10: test oracle 10
        // consensus.vOraclePublicKeys.push_back("ab2ca9a4d43ea036eab82375b3eebe6a4422cb8c327bcdd144813f0d111603fd");  // 11: test oracle 11
        // consensus.vOraclePublicKeys.push_back("bc7117d979782809de3012f2946a91a38af36176c171c1f214282c6958387990");  // 12: test oracle 12
        // consensus.vOraclePublicKeys.push_back("954543733113170ff2fafad02b372ed14ed1692640e0299d7c5d2701f21c5d32");  // 13: test oracle 13
        // consensus.vOraclePublicKeys.push_back("07ad71f90ef3eac0f599a1e233c2b1cb79f10d547ef5615c73935a7e25a30c27");  // 14: test oracle 14
        // consensus.vOraclePublicKeys.push_back("c8a52a79f57a6682396f8cc2589f8cd639434128a96e0bf245987d12e8896016");  // 15: test oracle 15
        // consensus.vOraclePublicKeys.push_back("b2f5c1f7dba8484c08a1b6d8a1b47320586edfdfbd53aedb59522f8154ed6f89");  // 16: test oracle 16
        // consensus.vOraclePublicKeys.push_back("190b0b263098ed4584c32a3f13dcd29eba860b651b987768dc0d2748e4e3d76d");  // 17: test oracle 17
        // consensus.vOraclePublicKeys.push_back("b2b6b92fb80bbc2646990982f5860116e427a25b784661a3e1d6cd4f988afe5a");  // 18: test oracle 18
        // consensus.vOraclePublicKeys.push_back("5c771dd86c9a08f1ee8f6b27ad937edab441e72ad63cc26e07ef7f09ae525589");  // 19: test oracle 19
        // consensus.vOraclePublicKeys.push_back("0923a87f7e4d3c4f972883a5045156f9598576f79f8441f35c1c7ed27842efb8");  // 20: test oracle 20
        // consensus.vOraclePublicKeys.push_back("b103e6fe28c03edbb31f1df7beb143d57d2ed443174bc8f140ea9870db155fe9");  // 21: test oracle 21
        // consensus.vOraclePublicKeys.push_back("d1a62f9b7c0911e8e0755b7f6c510716761b569b473e5262e78f2d40fe226356");  // 22: test oracle 22
        // consensus.vOraclePublicKeys.push_back("f51b13bd59d3a2bbb93e8123f0ea2ea10a85029c4440add35cdf4afa14f9a460");  // 23: test oracle 23

        if (options.easy_pow) {
            EnableLocalMiniTestnetMode();
        }

        // Testnet-specific oracle and activation settings
        consensus.nDDOracleEpochBlocks = 40;       // Rotate oracle signing epochs every 40 blocks (~10 minutes)
        consensus.nDDOracleUpdateInterval = 2;     // Update price every 2 blocks (~30 seconds)
        consensus.nDDActivationHeight = 600;       // DigiDollar BIP9 activation at block 600 (DEFINED→STARTED→LOCKED_IN→ACTIVE)
        consensus.nOracleActivationHeight = consensus.nDDActivationHeight;
        consensus.nDigiDollarMuSig2Height = consensus.nDDActivationHeight;

        // DD mint volatility anchor fix. PLACEHOLDER — must be re-confirmed
        // >= the testnet26 tip at release tagging; activation is the unfreeze
        // event for the incident chain.
        consensus.nDDVolatilityFixHeight = 190000;

        LogPrintf("Oracle: Testnet oracle activation height: %d\n", consensus.nOracleActivationHeight);
        LogPrintf("Oracle: %d oracles configured, %d-of-%d MuSig2 quorum, V1 activation height %d\n",
                 (int)consensus.vOraclePublicKeys.size(), consensus.nOracleRequiredMessages,
                 consensus.nOracleTotalOracles, consensus.nDDActivationHeight);
    }

private:
    void EnableLocalMiniTestnetMode() {
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fEasyPow = true;
        digidollarParams.activeOracles = 24;
        consensus.nOraclePubkeyCount = 24;

        consensus.vOraclePublicKeys.clear();
        consensus.vOraclePublicKeys.push_back("a69d02b2e39684deacead23e3b34191af99a124411465b19bc0a6e1384f70dee");  //  0: test oracle 0
        consensus.vOraclePublicKeys.push_back("56baa94b6a787502146177802820189113029d31c4d99b0a5ad59bbb776efc88");  //  1: test oracle 1
        consensus.vOraclePublicKeys.push_back("df3f298148c79840218722a4dfc81413d3dd4597a1aa3b97f12b3810bb21e4e9");  //  2: test oracle 2
        consensus.vOraclePublicKeys.push_back("692857a4e171ce477d16afc8c4f225d479f139eaa4baee796f195f7d67927d81");  //  3: test oracle 3
        consensus.vOraclePublicKeys.push_back("977e8e7355bf6718adf79e3e0cc874f9c98893fa450bfe9d433ae7643b6f6440");  //  4: test oracle 4
        consensus.vOraclePublicKeys.push_back("ccdca14f48d2e2c7225e378d3d0b40f6aebbc0aef783a6971479cae4d105fc5e");  //  5: test oracle 5
        consensus.vOraclePublicKeys.push_back("f3c51b026ce02416d806c76e046fb8c32dce4e7ab82a1bb163a1a852f521a7ac");  //  6: test oracle 6
        consensus.vOraclePublicKeys.push_back("b003867d046e3ea6876e1d33fca9018fd9b3f130f9fcb8a0904d9f58e8cf6141");  //  7: test oracle 7
        consensus.vOraclePublicKeys.push_back("75a81280ef4e7d977a3234ca9c6860e15243accb7565ff133fb02eda174df34e");  //  8: test oracle 8
        consensus.vOraclePublicKeys.push_back("a31a0d23ab9d2b2e9fdea406ccc902ff6812b2455985edd54ddba37402283cbb");  //  9: test oracle 9
        consensus.vOraclePublicKeys.push_back("9f802e732aa1b71e6ebf2385a21d70945c04d7a79adaaf1a72b8358e7d9c8466");  // 10: test oracle 10
        consensus.vOraclePublicKeys.push_back("ab2ca9a4d43ea036eab82375b3eebe6a4422cb8c327bcdd144813f0d111603fd");  // 11: test oracle 11
        consensus.vOraclePublicKeys.push_back("bc7117d979782809de3012f2946a91a38af36176c171c1f214282c6958387990");  // 12: test oracle 12
        consensus.vOraclePublicKeys.push_back("954543733113170ff2fafad02b372ed14ed1692640e0299d7c5d2701f21c5d32");  // 13: test oracle 13
        consensus.vOraclePublicKeys.push_back("07ad71f90ef3eac0f599a1e233c2b1cb79f10d547ef5615c73935a7e25a30c27");  // 14: test oracle 14
        consensus.vOraclePublicKeys.push_back("c8a52a79f57a6682396f8cc2589f8cd639434128a96e0bf245987d12e8896016");  // 15: test oracle 15
        consensus.vOraclePublicKeys.push_back("b2f5c1f7dba8484c08a1b6d8a1b47320586edfdfbd53aedb59522f8154ed6f89");  // 16: test oracle 16
        consensus.vOraclePublicKeys.push_back("190b0b263098ed4584c32a3f13dcd29eba860b651b987768dc0d2748e4e3d76d");  // 17: test oracle 17
        consensus.vOraclePublicKeys.push_back("b2b6b92fb80bbc2646990982f5860116e427a25b784661a3e1d6cd4f988afe5a");  // 18: test oracle 18
        consensus.vOraclePublicKeys.push_back("5c771dd86c9a08f1ee8f6b27ad937edab441e72ad63cc26e07ef7f09ae525589");  // 19: test oracle 19
        consensus.vOraclePublicKeys.push_back("0923a87f7e4d3c4f972883a5045156f9598576f79f8441f35c1c7ed27842efb8");  // 20: test oracle 20
        consensus.vOraclePublicKeys.push_back("b103e6fe28c03edbb31f1df7beb143d57d2ed443174bc8f140ea9870db155fe9");  // 21: test oracle 21
        consensus.vOraclePublicKeys.push_back("d1a62f9b7c0911e8e0755b7f6c510716761b569b473e5262e78f2d40fe226356");  // 22: test oracle 22
        consensus.vOraclePublicKeys.push_back("f51b13bd59d3a2bbb93e8123f0ea2ea10a85029c4440add35cdf4afa14f9a460");  // 23: test oracle 23

        vOracleNodes = {
            { 0, ParsePubKey("03a69d02b2e39684deacead23e3b34191af99a124411465b19bc0a6e1384f70dee"), "localhost:9001", true},
            { 1, ParsePubKey("0356baa94b6a787502146177802820189113029d31c4d99b0a5ad59bbb776efc88"), "localhost:9002", true},
            { 2, ParsePubKey("03df3f298148c79840218722a4dfc81413d3dd4597a1aa3b97f12b3810bb21e4e9"), "localhost:9003", true},
            { 3, ParsePubKey("03692857a4e171ce477d16afc8c4f225d479f139eaa4baee796f195f7d67927d81"), "localhost:9004", true},
            { 4, ParsePubKey("03977e8e7355bf6718adf79e3e0cc874f9c98893fa450bfe9d433ae7643b6f6440"), "localhost:9005", true},
            { 5, ParsePubKey("02ccdca14f48d2e2c7225e378d3d0b40f6aebbc0aef783a6971479cae4d105fc5e"), "localhost:9006", true},
            { 6, ParsePubKey("02f3c51b026ce02416d806c76e046fb8c32dce4e7ab82a1bb163a1a852f521a7ac"), "localhost:9007", true},
            { 7, ParsePubKey("03b003867d046e3ea6876e1d33fca9018fd9b3f130f9fcb8a0904d9f58e8cf6141"), "localhost:9008", true},
            { 8, ParsePubKey("0275a81280ef4e7d977a3234ca9c6860e15243accb7565ff133fb02eda174df34e"), "localhost:9009", true},
            { 9, ParsePubKey("02a31a0d23ab9d2b2e9fdea406ccc902ff6812b2455985edd54ddba37402283cbb"), "localhost:9010", true},
            {10, ParsePubKey("039f802e732aa1b71e6ebf2385a21d70945c04d7a79adaaf1a72b8358e7d9c8466"), "localhost:9011", true},
            {11, ParsePubKey("02ab2ca9a4d43ea036eab82375b3eebe6a4422cb8c327bcdd144813f0d111603fd"), "localhost:9012", true},
            {12, ParsePubKey("02bc7117d979782809de3012f2946a91a38af36176c171c1f214282c6958387990"), "localhost:9013", true},
            {13, ParsePubKey("02954543733113170ff2fafad02b372ed14ed1692640e0299d7c5d2701f21c5d32"), "localhost:9014", true},
            {14, ParsePubKey("0207ad71f90ef3eac0f599a1e233c2b1cb79f10d547ef5615c73935a7e25a30c27"), "localhost:9015", true},
            {15, ParsePubKey("02c8a52a79f57a6682396f8cc2589f8cd639434128a96e0bf245987d12e8896016"), "localhost:9016", true},
            {16, ParsePubKey("02b2f5c1f7dba8484c08a1b6d8a1b47320586edfdfbd53aedb59522f8154ed6f89"), "localhost:9017", true},
            {17, ParsePubKey("02190b0b263098ed4584c32a3f13dcd29eba860b651b987768dc0d2748e4e3d76d"), "localhost:9018", true},
            {18, ParsePubKey("03b2b6b92fb80bbc2646990982f5860116e427a25b784661a3e1d6cd4f988afe5a"), "localhost:9019", true},
            {19, ParsePubKey("035c771dd86c9a08f1ee8f6b27ad937edab441e72ad63cc26e07ef7f09ae525589"), "localhost:9020", true},
            {20, ParsePubKey("030923a87f7e4d3c4f972883a5045156f9598576f79f8441f35c1c7ed27842efb8"), "localhost:9021", true},
            {21, ParsePubKey("02b103e6fe28c03edbb31f1df7beb143d57d2ed443174bc8f140ea9870db155fe9"), "localhost:9022", true},
            {22, ParsePubKey("03d1a62f9b7c0911e8e0755b7f6c510716761b569b473e5262e78f2d40fe226356"), "localhost:9023", true},
            {23, ParsePubKey("02f51b13bd59d3a2bbb93e8123f0ea2ea10a85029c4440add35cdf4afa14f9a460"), "localhost:9024", true},
            {24, ParsePubKey("0277de68daecd823babbb58edb1c8e14d7106e83bb6d6ba7c1c6e0e476c6de5273"), "localhost:9025", false},
            {25, ParsePubKey("031b6453892473a467d07372d45eb05abc20316478a8f89a0e74023c3c05a8e1e7"), "localhost:9026", false},
            {26, ParsePubKey("02ac3478d69a3c81fa62e60f5c3696165a4e5e6ac4cda4d41d92ee258b91e23e38"), "localhost:9027", false},
            {27, ParsePubKey("03c1dfd96eea8cc2b62785275bca38ac261256e27864cf4f74db1c3bb5d8d08c42"), "localhost:9028", false},
            {28, ParsePubKey("03532efd6277226f38903401ec8317ba7cc8f13eb48dc8cb1e102fdc23488d7cef"), "localhost:9029", false},
            {29, ParsePubKey("03fe5dbbcea5ce7e2988b8c69bcfdfde8904aabc1f79c7b2bf6cd5dbec0d93c26c"), "localhost:9030", false},
            {30, ParsePubKey("03c7481c9305df583bf505a7cf9148d6aaaf0bb351d75a945efe57a13c82187480"), "localhost:9031", false},
            {31, ParsePubKey("02e96759ba5c67df6fc5cfc86977389d08fa31b4b297586f13d43bdd1569b459ba"), "localhost:9032", false},
            {32, ParsePubKey("035878eb72be3710d8c3f9585e27374011a476378f5ea7f3ab2f2830ab052ec5f8"), "localhost:9033", false},
            {33, ParsePubKey("035b3729a255bfbcff1bfd5b72fdb1a971b098545db7e874751179bc22c7f7f0b0"), "localhost:9034", false},
            {34, ParsePubKey("03d8fe0cd773604fa78fb1cef7d1e88a05c2473961178e40a4ac3c9aeeb4cbca87"), "localhost:9035", false},
        };

        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 0;

        LogPrintf("Testnet local mini-testnet mode enabled via -easypow: easy PoW + local oracle set\n");
    }

    void InitializeOracleNodes() {
        // DigiDollar Oracle Nodes - Testnet
        // 35 active oracle keys for the RC44 roster.
        //
        // Production testnet oracle nodes.
        vOracleNodes = {
            { 0, ParsePubKey("03e1dce189a530c1fb39dcd9282cf5f9de0e4eb257344be9fd94ce27c06005e8c7"), "oracle1.digibyte.io:12033",  true},  // Jared
            { 1, ParsePubKey("033dfb7a36ab40fa6fbc69b4b499eaa17bfa1958aa89ec248efc24b4c18694f990"), "oracle2.digibyte.io:12033",  true},  // Green Candle
            { 2, ParsePubKey("03172755a320cec96c981d46c86d79a03578d73406a25e89d8edc616a8f361cb5c"), "oracle3.digibyte.io:12033",  true},  // Bastian
            { 3, ParsePubKey("03546c07ee9d21640c4b4e96e6954bd49c3ab5bcf36c6a512603ebf75f8609da0c"), "oracle4.digibyte.io:12033",  true},  // DanGB
            { 4, ParsePubKey("039cef021f841794c1afc4e84d678f3c70dbe3a972330b2b6329852898443deb4f"), "oracle5.digibyte.io:12033",  true},  // Shenger
            { 5, ParsePubKey("0285016758856ed27388501a54031fa3a678df705bf811fb8bc9abd2d7cfb6d9f7"), "oracle6.digibyte.io:12033",  true},  // Ycagel
            { 6, ParsePubKey("037a858e055099e4a9cf8273e9171da148d4fd00afd4376b60dc1cd09974731b51"), "oracle7.digibyte.io:12033",  true},  // Aussie
            { 7, ParsePubKey("032d8c9f054d7087e263016c0800ad1c2f8106859e772766b9f8179042d1792a09"), "oracle8.digibyte.io:12033",  true},  // LookInto
            { 8, ParsePubKey("0389d5c588c8e0d311028f2f7e0db6df1a9fb0319c5e3b2cfc32efaee86538d250"), "oracle9.digibyte.io:12033",  true},  // JohnnyLawDGB
            { 9, ParsePubKey("02d2f9b0e00ed2fb0a93d04f12eb250b4adf2e4ac8335692c7942e4cba6e462484"), "129.212.182.152:12033",      true},  // Ogilvie
            {10, ParsePubKey("02028a52c7a3e8f22c44e356dcda43a0e24ed5e8e284c53c902599f0947763113c"), "oracle11.digibyte.io:12033", true},  // ChopperBrian
            {11, ParsePubKey("024ef063a67b35295e9eaaa9251bc7f0effbceaedc8e9bc92504b0da832744ca08"), "oracle12.digibyte.io:12033", true},  // hallvardo (key rotated for RC31)
            {12, ParsePubKey("0375d7494b55ebc3874450954ac5caca402bbd068467a89d48fe877f3e2d09eda9"), "oracle13.digibyte.io:12033", true},  // DaPunzy (key rotated for RC31)
            {13, ParsePubKey("024770d416e5f751ff310fa8e35cd48d9709ee539cbaa07c67dbf52503518cc573"), "oracle14.digibyte.io:12033", true},  // DigiByteForce (key rotated for RC31)
            {14, ParsePubKey("03f3ab098eb0ceff8259c280cf5a3682e78b298dc300e26cdb4694d8efcd6a11a2"), "oracle15.digibyte.io:12033", true},  // Neel (RC30)
            {15, ParsePubKey("03447153bcec341f2dad94541104f4e8c0a8b19e342dada1b2204ae56cf2b960a6"), "oracle16.digibyte.io:12033", true},  // DigiSwarm (RC30)
            {16, ParsePubKey("0283b9c6d229a6347370517fc11329abebeae511055702b0408158f2205035adc7"), "oracle17.digibyte.io:12033", true},  // GTO90 (RC30)
            {17, ParsePubKey("03649d750bcad5b42b3dd0f11c8d98d62ed5afd515cd986663f81c35f086e58d47"), "oracle18.digibyte.io:12033", true},  // digibyte-maxi (RC42)
            {18, ParsePubKey("0345f8cb22dfde6aff8f18552c338256e0df551ca2df007f6449d6da1dbb7f4d89"), "oracle19.digibyte.io:12033", true},  // Anthony
            {19, ParsePubKey("031758a6d7f1f87c95d1a4a38415608d41463a504ea28da7c6129e2a9d654add42"), "oracle20.digibyte.io:12033", true},  // mbah_jambon
            {20, ParsePubKey("03018c81746d6ddc326c993d9f2e7f2015554e97a261f3b5fe637ac5098f421a4c"), "oracle21.digibyte.io:12033", true},  // Camden
            {21, ParsePubKey("03d8165aa05b045de2a9b979b23a63cca1fec865784d12ab6f3f1bca8a90f3dd86"), "oracle22.digibyte.io:12033", true},  // Twoface123
            {22, ParsePubKey("039241688b464c3f03f957cd85a3d1d6a760963be3707a16805b8064a8740e07ef"), "oracle23.digibyte.io:12033", true},  // LivingTheLife
            {23, ParsePubKey("03b6302e3cc8ee6d474c3c0078c25b87ce708757e2a81e8f4f01975dc4b25e0f6d"), "oracle24.digibyte.io:12033", true},  // ChozenOne43
            {24, ParsePubKey("03926ed40635d294a554ec046a96d3fa58587521385c7df58ff21ede12a31add0e"), "oracle25.digibyte.io:12033", true},  // ckunchained
            {25, ParsePubKey("034103ed4168d11dcaafa96494d5b3dd37247fa6deefa08d47f7004568792b1672"), "oracle26.digibyte.io:12033", true},  // JMag
            {26, ParsePubKey("038adf7df5fcd114178643f16aa0e3be8fa1e221ca421479e48c0bd04f2561d3a8"), "oracle27.digibyte.io:12033", true},   // HashedMax
            {27, ParsePubKey("02557029e2419af54984f3f2fb600004c0a6f8573dac5730cfaab2048c80ba6894"), "oracle28.digibyte.io:12033", true},   // DennisPitallano
            {28, ParsePubKey("03532efd6277226f38903401ec8317ba7cc8f13eb48dc8cb1e102fdc23488d7cef"), "oracle29.digibyte.io:12033", true},   // DigiHash Mining Pool
            {29, ParsePubKey("03d566a244719aa577d828da31ad9863f94686f710ac1f0638914eff5692ec7d58"), "85.239.234.52:12033", true},           // Michael E / medgboracle3452
            {30, ParsePubKey("03603a0175197a1fe28859c71c69fd0081710c5569fceceaa96ce7de386d3ebf61"), "oracle31.digibyte.io:12033", true},   // Scott K / DigibyteDaily
            {31, ParsePubKey("02e96759ba5c67df6fc5cfc86977389d08fa31b4b297586f13d43bdd1569b459ba"), "oracle32.digibyte.io:12033", true},   // Peer2Peer / DigiRoos
            {32, ParsePubKey("035878eb72be3710d8c3f9585e27374011a476378f5ea7f3ab2f2830ab052ec5f8"), "oracle33.digibyte.io:12033", true},   // 3DogsKanab
            {33, ParsePubKey("035b3729a255bfbcff1bfd5b72fdb1a971b098545db7e874751179bc22c7f7f0b0"), "oracle34.digibyte.io:12033", true},   // LiberatedLark
            {34, ParsePubKey("03d8fe0cd773604fa78fb1cef7d1e88a05c2473961178e40a4ac3c9aeeb4cbca87"), "oracle35.digibyte.io:12033", true},   // Manu_DGB_oracle
        };
        //
        // LOCAL MINI-TESTNET TESTING (disabled; retained for future use).
        // Test keys derived from SHA256("digibyte_testnet_oracle_N"), N=0..23.
        // Enable this block (and the matching vOraclePublicKeys block in CTestNetParams)
        // when running test_multi_oracle_testnet.sh.
        //
        // vOracleNodes = {
        //     { 0, ParsePubKey("03a69d02b2e39684deacead23e3b34191af99a124411465b19bc0a6e1384f70dee"), "localhost:9001", true},   // test oracle 0
        //     { 1, ParsePubKey("0356baa94b6a787502146177802820189113029d31c4d99b0a5ad59bbb776efc88"), "localhost:9002", true},   // test oracle 1
        //     { 2, ParsePubKey("03df3f298148c79840218722a4dfc81413d3dd4597a1aa3b97f12b3810bb21e4e9"), "localhost:9003", true},   // test oracle 2
        //     { 3, ParsePubKey("03692857a4e171ce477d16afc8c4f225d479f139eaa4baee796f195f7d67927d81"), "localhost:9004", true},   // test oracle 3
        //     { 4, ParsePubKey("03977e8e7355bf6718adf79e3e0cc874f9c98893fa450bfe9d433ae7643b6f6440"), "localhost:9005", true},   // test oracle 4
        //     { 5, ParsePubKey("02ccdca14f48d2e2c7225e378d3d0b40f6aebbc0aef783a6971479cae4d105fc5e"), "localhost:9006", true},   // test oracle 5
        //     { 6, ParsePubKey("02f3c51b026ce02416d806c76e046fb8c32dce4e7ab82a1bb163a1a852f521a7ac"), "localhost:9007", true},   // test oracle 6
        //     { 7, ParsePubKey("03b003867d046e3ea6876e1d33fca9018fd9b3f130f9fcb8a0904d9f58e8cf6141"), "localhost:9008", true},   // test oracle 7
        //     { 8, ParsePubKey("0275a81280ef4e7d977a3234ca9c6860e15243accb7565ff133fb02eda174df34e"), "localhost:9009", true},   // test oracle 8
        //     { 9, ParsePubKey("02a31a0d23ab9d2b2e9fdea406ccc902ff6812b2455985edd54ddba37402283cbb"), "localhost:9010", true},   // test oracle 9
        //     {10, ParsePubKey("039f802e732aa1b71e6ebf2385a21d70945c04d7a79adaaf1a72b8358e7d9c8466"), "localhost:9011", true},   // test oracle 10
        //     {11, ParsePubKey("02ab2ca9a4d43ea036eab82375b3eebe6a4422cb8c327bcdd144813f0d111603fd"), "localhost:9012", true},   // test oracle 11
        //     {12, ParsePubKey("02bc7117d979782809de3012f2946a91a38af36176c171c1f214282c6958387990"), "localhost:9013", true},   // test oracle 12
        //     {13, ParsePubKey("02954543733113170ff2fafad02b372ed14ed1692640e0299d7c5d2701f21c5d32"), "localhost:9014", true},   // test oracle 13
        //     {14, ParsePubKey("0207ad71f90ef3eac0f599a1e233c2b1cb79f10d547ef5615c73935a7e25a30c27"), "localhost:9015", true},   // test oracle 14
        //     {15, ParsePubKey("02c8a52a79f57a6682396f8cc2589f8cd639434128a96e0bf245987d12e8896016"), "localhost:9016", true},   // test oracle 15
        //     {16, ParsePubKey("02b2f5c1f7dba8484c08a1b6d8a1b47320586edfdfbd53aedb59522f8154ed6f89"), "localhost:9017", true},   // test oracle 16
        //     {17, ParsePubKey("02190b0b263098ed4584c32a3f13dcd29eba860b651b987768dc0d2748e4e3d76d"), "localhost:9018", true},   // test oracle 17
        //     {18, ParsePubKey("03b2b6b92fb80bbc2646990982f5860116e427a25b784661a3e1d6cd4f988afe5a"), "localhost:9019", true},   // test oracle 18
        //     {19, ParsePubKey("035c771dd86c9a08f1ee8f6b27ad937edab441e72ad63cc26e07ef7f09ae525589"), "localhost:9020", true},   // test oracle 19
        //     {20, ParsePubKey("030923a87f7e4d3c4f972883a5045156f9598576f79f8441f35c1c7ed27842efb8"), "localhost:9021", true},   // test oracle 20
        //     {21, ParsePubKey("02b103e6fe28c03edbb31f1df7beb143d57d2ed443174bc8f140ea9870db155fe9"), "localhost:9022", true},   // test oracle 21
        //     {22, ParsePubKey("03d1a62f9b7c0911e8e0755b7f6c510716761b569b473e5262e78f2d40fe226356"), "localhost:9023", true},   // test oracle 22
        //     {23, ParsePubKey("02f51b13bd59d3a2bbb93e8123f0ea2ea10a85029c4440add35cdf4afa14f9a460"), "localhost:9024", true},   // test oracle 23
        // };
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vSeeds.clear();

        if (!options.challenge) {
            bin = ParseHex("512103ad5e0edad18cb1f0fc0d28a3d4f1f3e445640337489abb10404f2d1e086be430210359ef5021964fe22d6f8e05b2463c9540ce96883fe3b278760f048f5189f2e6c452ae");
            vSeeds.emplace_back("seed.signet.digibyte.sprovoost.nl.");

            // Hardcoded nodes can be removed once there are more DNS seeds
            vSeeds.emplace_back("178.128.221.177");
            vSeeds.emplace_back("v7ajjeirttkbnt32wpy3c6w3emwnfr3fkla7hpxcfokr3ysd3kqtzmqd.onion:38333");

            consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000000001ad46be4862");
            consensus.defaultAssumeValid = uint256S("0x0000013d778ba3f914530f11f6b69869c9fab54acff85acd7b8201d111f19b7f"); // 150000
            m_assumed_blockchain_size = 1;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                // Data from RPC: getchaintxstats 4096 0000013d778ba3f914530f11f6b69869c9fab54acff85acd7b8201d111f19b7f
                .nTime    = 1688366339,
                .nTxCount = 2262750,
                .dTxRate  = 0.003414084572046456,
            };
        } else {
            bin = *options.challenge;
            consensus.nMinimumChainWork = uint256{};
            consensus.defaultAssumeValid = uint256{};
            m_assumed_blockchain_size = 0;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                0,
                0,
                0,
            };
            LogPrintf("Signet with challenge %s\n", HexStr(bin));
        }

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.nSubsidyHalvingInterval = 300; // DigiByte halving interval for signet (same as testnet)
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 60 / 4; // 15 seconds (DigiByte)
        consensus.fPowAllowMinDifficultyBlocks = true; // DigiByte allows min difficulty blocks
        consensus.fEasyPow = false; // DigiByte setting
        consensus.fPowNoRetargeting = false;
        consensus.fRbfEnabled = false; // DigiByte RBF disabled
        
        // DigiByte Specific Consensus Code for signet (same as testnet)
        consensus.nOdoShapechangeInterval = 1*24*60*60; // 1 day
        consensus.nTargetTimespan =  0.10 * 24 * 60 * 60; // 2.4 hours
        consensus.nTargetSpacing = 60; // 60 seconds
        consensus.nInterval = consensus.nTargetTimespan / consensus.nTargetSpacing;
        consensus.nDiffChangeTarget = 67; // DigiShield Hard Fork Block
        consensus.patchBlockRewardDuration = 10; 
        consensus.patchBlockRewardDuration2 = 80;
        consensus.nTargetTimespanRe = 1*60; // 60 Seconds
        consensus.nTargetSpacingRe = 1*60; // 60 seconds
        consensus.nIntervalRe = consensus.nTargetTimespanRe / consensus.nTargetSpacingRe; // 1 block
        consensus.nAveragingInterval = 10; // 10 blocks
        consensus.multiAlgoTargetSpacing = 30*5; // NUM_ALGOS * 30 seconds
        consensus.multiAlgoTargetSpacingV4 = 15*5; // NUM_ALGOS * 15 seconds
        consensus.nAveragingTargetTimespan = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacing;
        consensus.nAveragingTargetTimespanV4 = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacingV4;
        consensus.nMaxAdjustDown = 40; // 40% adjustment down
        consensus.nMaxAdjustUp = 20; // 20% adjustment up
        consensus.nMaxAdjustDownV3 = 16; // 16% adjustment down
        consensus.nMaxAdjustUpV3 = 8; // 8% adjustment up
        consensus.nMaxAdjustDownV4 = 16;
        consensus.nMaxAdjustUpV4 = 8;
        consensus.nMinActualTimespan = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUp) / 100;
        consensus.nMaxActualTimespan = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustDown) / 100;
        consensus.nMinActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUpV3) / 100;
        consensus.nMaxActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustUpV3) / 100;
        consensus.nMinActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 - consensus.nMaxAdjustUpV4) / 100;
        consensus.nMaxActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 + consensus.nMaxAdjustUpV4) / 100;
        consensus.nLocalTargetAdjustment = 4; // target adjustment per algo
        consensus.nLocalDifficultyAdjustment = 4; // difficulty adjustment per algo
        
        // DigiByte Hard Fork Block Heights for signet (same as testnet)
        consensus.multiAlgoDiffChangeTarget = 100; // Block 100 MultiAlgo Hard Fork
        consensus.alwaysUpdateDiffChangeTarget = 400; // Block 400 MultiShield Hard Fork
        consensus.workComputationChangeTarget = 1430; // Block 1,430 DigiSpeed Hard Fork
        consensus.algoSwapChangeTarget = 20000; // Block 20,000 Odo PoW Hard Fork
        consensus.OdoHeight = 600;
        consensus.ReserveAlgoBitsHeight = 0;
        consensus.initialTarget[ALGO_ODO] = ArithToUint256(~arith_uint256(0) >> 20); // Same as other algos
        
        consensus.nRuleChangeActivationThreshold = 1815; // 90% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256S("00000377ae000000000000000000000000000000000000000000000000000000");
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 27;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Buried deployments (BIP90): all were BIP9 ALWAYS_ACTIVE on signet,
        // equivalent to a buried height of 0.
        consensus.TaprootHeight = 0;
        consensus.DigiDollarHeight = 0;
        consensus.AlgoLockHeight = 0;

        // DD mint volatility anchor fix: active from genesis on signet.
        consensus.nDDVolatilityFixHeight = 0;

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 38443;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(1598918400, 52613770, 0x1e0377ae, 1, 8000);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x9cf8c097b1afc5a37d2d050d2b423b052c2da2856acf0e41c40af1da334fcbf7"));
        assert(genesis.hashMerkleRoot == uint256S("0x72ddd9496b004221ed0557358846d9248ecd4c440ebd28ed901efc18757d0fad"));

        vFixedSeeds.clear();

        m_assumeutxo_data = {
            {
                .height = 160'000,
                .hash_serialized = AssumeutxoHash{uint256S("0xfe0a44309b74d6b5883d246cb419c6221bcccf0b308c9b59b7d70783dbdf928a")},
                .nChainTx = 2289496,
                .blockhash = uint256S("0x0000003ca3c99aff040f2563c2ad8f8ec88bd0fd6b8f0895cfaf1ef90353a62c")
            }
        };

        // DigiByte: Use same prefixes as testnet for signet
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,126);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,140);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,254);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "dgbt";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 300; // DigiByte halving interval for regtest
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.ReserveAlgoBitsHeight = 0; // DigiByte ReserveAlgoBits
        consensus.OdoHeight = 600; // DigiByte Odocrypt height
        consensus.nGroestlDeactivationHeight = 0; // enforce deactivated-algo rejection from genesis on regtest
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        // Set initial targets for all algorithms (easy difficulty for regtest)
        consensus.initialTarget[ALGO_SHA256D] = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.initialTarget[ALGO_SCRYPT] = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.initialTarget[ALGO_GROESTL] = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.initialTarget[ALGO_SKEIN] = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.initialTarget[ALGO_QUBIT] = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.initialTarget[ALGO_ODO] = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); // Odocrypt initial target
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 60 / 4; // 15 seconds (DigiByte)
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fEasyPow = true; // DigiByte setting - allow easy pow for regtest
        consensus.fPowNoRetargeting = true; // No retargeting in regtest for faster mining
        consensus.fRbfEnabled = false; // DigiByte RBF disabled
        
        // DigiByte Specific Consensus Code
        consensus.nOdoShapechangeInterval = 10*24*60*60; // 10 days
        consensus.nTargetTimespan = 0.10 * 48 * 60 * 60; // 4.8 hours
        consensus.nTargetSpacing = 60; // 60 seconds
        consensus.nInterval = consensus.nTargetTimespan / consensus.nTargetSpacing;
        consensus.nDiffChangeTarget = 334; // DigiShield Hard Fork Block
        consensus.patchBlockRewardDuration = 10; // DigiByte reward duration
        consensus.patchBlockRewardDuration2 = 80; // DigiByte reward duration 2
        consensus.nTargetTimespanRe = 1*60; // 60 Seconds
        consensus.nTargetSpacingRe = 1*60; // 60 seconds
        consensus.nIntervalRe = consensus.nTargetTimespanRe / consensus.nTargetSpacingRe; // 1 block
        consensus.nAveragingInterval = 10; // 10 blocks
        consensus.multiAlgoTargetSpacing = 30*5; // NUM_ALGOS * 30 seconds
        consensus.multiAlgoTargetSpacingV4 = 15*5; // NUM_ALGOS * 15 seconds
        consensus.nAveragingTargetTimespan = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacing; // 10* NUM_ALGOS * 30
        consensus.nAveragingTargetTimespanV4 = consensus.nAveragingInterval * consensus.multiAlgoTargetSpacingV4; // 10 * NUM_ALGOS * 15
        consensus.nMaxAdjustDown = 40; // 40% adjustment down
        consensus.nMaxAdjustUp = 20; // 20% adjustment up
        consensus.nMaxAdjustDownV3 = 16; // 16% adjustment down
        consensus.nMaxAdjustUpV3 = 8; // 8% adjustment up
        consensus.nMaxAdjustDownV4 = 16;
        consensus.nMaxAdjustUpV4 = 8;
        consensus.nMinActualTimespan = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUp) / 100;
        consensus.nMaxActualTimespan = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustDown) / 100;
        consensus.nMinActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 - consensus.nMaxAdjustUpV3) / 100;
        consensus.nMaxActualTimespanV3 = consensus.nAveragingTargetTimespan * (100 + consensus.nMaxAdjustUpV3) / 100;
        consensus.nMinActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 - consensus.nMaxAdjustUpV4) / 100;
        consensus.nMaxActualTimespanV4 = consensus.nAveragingTargetTimespanV4 * (100 + consensus.nMaxAdjustUpV4) / 100;
        consensus.nLocalTargetAdjustment = 4; // target adjustment per algo
        consensus.nLocalDifficultyAdjustment = 4; // difficulty adjustment per algo
        
        // DigiByte Hard Fork Block Heights for regtest
        consensus.multiAlgoDiffChangeTarget = 100; // Block 100 MultiAlgo Hard Fork
        consensus.alwaysUpdateDiffChangeTarget = 200; // Block 200 MultiShield Hard Fork
        consensus.workComputationChangeTarget = 400; // Block 400 DigiSpeed Hard Fork
        consensus.algoSwapChangeTarget = 600; // Block 600 Odo PoW Hard Fork
        consensus.nRuleChangeActivationThreshold = 108; // 75% for testchains
        consensus.nMinerConfirmationWindow = 144; // Faster than normal for regtest (144 instead of 2016)

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 27;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Buried deployments (BIP90): all were BIP9 ALWAYS_ACTIVE on regtest,
        // equivalent to a buried height of 0. Overridable via
        // -testactivationheight=taproot|digidollar|algolock@HEIGHT below;
        // -digidollaractivationheight=N additionally retargets the static
        // DD/oracle/MuSig2 height gates and takes precedence for DigiDollar.
        consensus.TaprootHeight = 0;
        consensus.DigiDollarHeight = 0;
        consensus.AlgoLockHeight = 0;

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;
        nDefaultPort = 18444;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        for (const auto& [dep, height] : opts.activation_heights) {
            switch (dep) {
            case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
                consensus.SegwitHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
                consensus.BIP34Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
                consensus.BIP66Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
                consensus.BIP65Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
                consensus.CSVHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_NVERSIONBIPS:
                // Handle NVERSIONBIPS deployment
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_RESERVEALGO:
                // Handle ReserveAlgo deployment
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_ODO:
                // Handle Odo deployment
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_TAPROOT:
                consensus.TaprootHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_DIGIDOLLAR:
                // Moves the buried deployment height; the static DD/oracle
                // gates keep their defaults, but nDigiDollarMuSig2Height is
                // derived below as min(nDDActivationHeight, DigiDollarHeight)
                // and so follows this override. Use -digidollaractivationheight
                // to move everything together (applied later, takes precedence).
                consensus.DigiDollarHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_ALGOLOCK:
                consensus.AlgoLockHeight = int{height};
                break;
            }
        }

        for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
            consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
            consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
            consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
        }

        genesis = CreateGenesisBlock(1519460922, 4, 0x207fffff, 1, 8000);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x4598a0f2b823aaf9e77ee6d5e46f1edb824191dcd48b08437b7cec17e6ae6e26"));
        assert(genesis.hashMerkleRoot == uint256S("0x72ddd9496b004221ed0557358846d9248ecd4c440ebd28ed901efc18757d0fad"));

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        checkpointData = {
            {
                {0, uint256S("4598a0f2b823aaf9e77ee6d5e46f1edb824191dcd48b08437b7cec17e6ae6e26")},
            }
        };

        m_assumeutxo_data = {
            {
                // DigiByte regtest values at height 110
                .height = 110,
                .hash_serialized = AssumeutxoHash{uint256S("0x2da005f8e675e4c37ea7d7266d11d2d9c2485c095fff21692ef299fbba42f87c")}, // TODO: Generate actual UTXO hash
                .nChainTx = 111,
                .blockhash = uint256S("0x56b2d1cd24ac6d9c74d3f06867eb2d1b1ca1d455f635dac3e1a450da96ed6374")
            },
            {
                // For use by test/functional/feature_assumeutxo.py
                // DigiByte regtest values at height 299
                .height = 299,
                .hash_serialized = AssumeutxoHash{uint256S("0x0c3eb8c1b150495afa0aa96879243937ae989b45b9f8cd14947f5eec8ba7a103")},
                .nChainTx = 300,
                .blockhash = uint256S("0x2819b3447826b563a8b7cae4e4bcc0c35845149fcd2b99089c0e664c07bc6cfe")
            },
        };

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,126);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,140);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,254);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "dgbrt";

        // DigiDollar consensus parameters (regtest)
        digidollarParams = DigiDollar::ConsensusParams();
        // Regtest specific adjustments for rapid testing
        // NOTE: DD amounts are in CENTS, not satoshis. 100 cents = $1.00
        digidollarParams.minMintAmount = 1;              // 1 cent = $0.01 minimum for testing
        digidollarParams.maxMintAmount = 100000;         // 100,000 cents = $1000 maximum for testing
        digidollarParams.oracleThreshold = 1;                             // 1-of-1 consensus for testing
        digidollarParams.activeOracles = 1;                               // Only 1 active oracle for testing
        digidollarParams.oracleCount = 1;                                 // Total 1 oracle for testing
        digidollarParams.priceValidBlocks = 5;                            // Price valid for 5 blocks only

        // Initialize DigiDollar Oracle Nodes (minimal set for regtest)
        InitializeOracleNodes();

        // Regtest-specific oracle and activation settings
        consensus.nDDOracleEpochBlocks = 40;       // Rotate oracle signing epochs every 40 blocks (~10 minutes)
        consensus.nDDOracleUpdateInterval = 1;     // Update price every block
        consensus.nDDActivationHeight = 650;       // DigiDollar active from height 650 (after Odocrypt at 600)

        // Oracle system parameters (RegTest uses MockOracleManager)
        consensus.nOracleActivationHeight = 650;   // Same as nDDActivationHeight — everything activates together
        if (opts.digidollar_activation_height) {
            consensus.nDDActivationHeight = *opts.digidollar_activation_height;
            consensus.nOracleActivationHeight = *opts.digidollar_activation_height;
            // The knob retargets the buried deployment height too, so BIP9-era
            // semantics ("everything activates together at N") are preserved.
            // Takes precedence over -testactivationheight=digidollar@H.
            consensus.DigiDollarHeight = *opts.digidollar_activation_height;
        }
        consensus.nOracleEpochLength = 40;         // 10 minutes (40 blocks * 15 seconds)
        consensus.nOracleRequiredMessages = 4;     // 4-of-7 off-chain price quorum (matches testnet)
        consensus.nOracleTotalOracles = 7;         // 7 active oracles (matches testnet)
        // MuSig2 must follow the effective DigiDollar activation boundary.
        // Default regtest has the buried DigiDollar deployment active from
        // genesis (DigiDollarHeight=0), while nDDActivationHeight remains 650
        // for height-gated P2P/oracle tests. Using the raw height here leaves
        // DD active before v0x03 quotes validate, breaking every regtest DD
        // mint path.
        consensus.nDigiDollarMuSig2Height = std::min(
            consensus.nDDActivationHeight,
            consensus.DigiDollarHeight);

        // DD mint volatility anchor fix: active from genesis; fast anchor
        // geometry for functional tests (mainnet: lag 240, window 1440).
        // -ddvolatilityfixheight=N moves only the fix height, letting tests
        // exercise the legacy deque rule below the gate.
        consensus.nDDVolatilityFixHeight = 0;
        consensus.nDDVolAnchorLag = 24;
        consensus.nDDVolAnchorWindow = 144;
        if (opts.dd_volatility_fix_height) {
            consensus.nDDVolatilityFixHeight = *opts.dd_volatility_fix_height;
        }

        // MuSig2 oracle configuration — 4-of-7 quorum (lower for testing)
        consensus.nOraclePubkeyCount = 7;
        consensus.nOracleConsensusRequired = 4;

        // RegTest: Oracle public keys for all 7 oracles — ordered by oracle ID.
        // Must match vOracleNodes[].id ordering so ValidateOracleNodeAlignment()
        // succeeds at common::InitConfig; otherwise every regtest digibyted
        // aborts on startup.
        // Deterministic keys derived from SHA256("digibyte_regtest_oracle_N").
        // Matching private keys are in MockOracleManager for test signing.
        consensus.vOraclePublicKeys.clear();
        consensus.vOraclePublicKeys.push_back("8849d466503c5bb3875ada6bff3d6eec6299265b7b0a4e31bb58b7cecf6dd08f");  // oracle 0
        consensus.vOraclePublicKeys.push_back("9991f9e0c61dfe10896ee797f271f2f6d4e2545daf9a22732622c02ce39f43f0");  // oracle 1
        consensus.vOraclePublicKeys.push_back("d2292678e5c549e90420dc4c6311ef01bf014fb0f4b34d07e02b7b98bbc35e9a");  // oracle 2
        consensus.vOraclePublicKeys.push_back("f24df57d6ab29241f0f00dd87d7ce1418852684dc41d186650a452179618234f");  // oracle 3
        consensus.vOraclePublicKeys.push_back("aa1ebe314382eb820a040acf604a6617db3d063e9ee0640780774650649aaf47");  // oracle 4
        consensus.vOraclePublicKeys.push_back("be6ad50e0bf26af3798af09a2824142fa79ff37f8730b7a54ba34c9fe6f1e8ec");  // oracle 5
        consensus.vOraclePublicKeys.push_back("584d30f3650d998b0b5f8be52f46164aee5f10d332421e63f070da8a38693a96");  // oracle 6

        LogPrintf("Oracle: RegTest MuSig2 oracle bundles active at block %d, %d-of-%d quorum\n",
                 consensus.nDigiDollarMuSig2Height, consensus.nOracleConsensusRequired,
                 consensus.nOraclePubkeyCount);
    }

private:
    void InitializeOracleNodes() {
        // DigiDollar Oracle Nodes - 7 oracles for regtest (matches testnet 4-of-7)
        // Deterministic keys from SHA256("digibyte_regtest_oracle_N") - matching privkeys in MockOracleManager
        vOracleNodes = {
            {0, ParsePubKey("038849d466503c5bb3875ada6bff3d6eec6299265b7b0a4e31bb58b7cecf6dd08f"), "localhost:9001", true},
            {1, ParsePubKey("029991f9e0c61dfe10896ee797f271f2f6d4e2545daf9a22732622c02ce39f43f0"), "localhost:9002", true},
            {2, ParsePubKey("02d2292678e5c549e90420dc4c6311ef01bf014fb0f4b34d07e02b7b98bbc35e9a"), "localhost:9003", true},
            {3, ParsePubKey("02f24df57d6ab29241f0f00dd87d7ce1418852684dc41d186650a452179618234f"), "localhost:9004", true},
            {4, ParsePubKey("02aa1ebe314382eb820a040acf604a6617db3d063e9ee0640780774650649aaf47"), "localhost:9005", true},
            {5, ParsePubKey("03be6ad50e0bf26af3798af09a2824142fa79ff37f8730b7a54ba34c9fe6f1e8ec"), "localhost:9006", true},
            {6, ParsePubKey("03584d30f3650d998b0b5f8be52f46164aee5f10d332421e63f070da8a38693a96"), "localhost:9007", true}
        };
    }
};

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main()
{
    return std::make_unique<const CMainParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet()
{
    return TestNet(TestNetOptions{});
}

std::unique_ptr<const CChainParams> CChainParams::TestNet(const TestNetOptions& options)
{
    return std::make_unique<const CTestNetParams>(options);
}

// Helper function implementations
const OracleNodeInfo* CChainParams::GetOracleNode(uint32_t id) const {
    for (const auto& oracle : vOracleNodes) {
        if (oracle.id == id) return &oracle;
    }
    return nullptr;
}

bool CChainParams::ValidateOracleNodeAlignment() const {
    const auto& consensus_params = GetConsensus();
    if (consensus_params.nOraclePubkeyCount < 0) return false;
    if (vOracleNodes.size() < static_cast<size_t>(consensus_params.nOraclePubkeyCount)) return false;

    for (int slot = 0; slot < consensus_params.nOraclePubkeyCount; ++slot) {
        const auto& oracle = vOracleNodes[slot];
        if (oracle.id != static_cast<uint32_t>(slot)) return false;
        if (!oracle.pubkey.IsValid() || oracle.pubkey.size() != CPubKey::COMPRESSED_SIZE) return false;

        const std::string expected_xonly = ToLower(consensus_params.vOraclePublicKeys[slot]);
        const std::string actual_xonly = ToLower(HexStr(Span<const unsigned char>(
            oracle.pubkey.data() + 1, oracle.pubkey.size() - 1)));
        if (actual_xonly != expected_xonly) return false;
    }

    return true;
}

uint32_t CChainParams::GetActiveOracleCount() const {
    // Return network-specific active oracle count based on digidollarParams
    return digidollarParams.activeOracles;
}
