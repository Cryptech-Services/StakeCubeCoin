// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2014-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include "consensus/merkle.h"
#include <consensus/validation.h>
#include <core_io.h>
#include "crypto/progpow.h"
#include "crypto/progpow/helpers.hpp"
#include <key_io.h>
#include <miner.h>
#include <net.h>
#include <policy/fees.h>
#include <pow.h>
#include <rpc/blockchain.h>
#include <rpc/mining.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <shutdown.h>
#include <txmempool.h>
#include <util/fees.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/validation.h>
#include <validation.h>
#include <validationinterface.h>
#include <versionbitsinfo.h>
#include <warnings.h>

#include <governance/classes.h>
#include <governance/governance.h>
#include <masternode/sync.h>

#include <utility>      // std::pair
#include <memory>
#include <stdint.h>

/** ProgPow **/
std::map<std::string, CBlock> mapPPBlockTemplates;

/**
 * Return average network hashes per second based on the last 'lookup' blocks,
 * or from the last difficulty change if 'lookup' is nonpositive.
 * If 'height' is nonnegative, compute the estimate at the time when a given block was found.
 */
static UniValue GetNetworkHashPS(int lookup, int height) {
    CBlockIndex *pb = ::ChainActive().Tip();

    if (height >= 0 && height < ::ChainActive().Height())
        pb = ::ChainActive()[height];

    if (pb == nullptr || !pb->nHeight)
        return 0;

    // If lookup is -1, then use blocks since last difficulty change.
    if (lookup <= 0)
        lookup = pb->nHeight % Params().GetConsensus().DifficultyAdjustmentInterval() + 1;

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->nHeight)
        lookup = pb->nHeight;

    CBlockIndex *pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->pprev;
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    int64_t timeDiff = maxTime - minTime;

    return workDiff.getdouble() / timeDiff;
}

static UniValue getnetworkhashps(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{"getnetworkhashps",
                "\nReturns the estimated network hashes per second based on the last n blocks.\n"
                "Pass in [blocks] to override # of blocks, -1 specifies since last difficulty change.\n"
                "Pass in [height] to estimate the network speed at the time when a certain block was found.\n",
                {
                    {"nblocks", RPCArg::Type::NUM, /* default */ "120", "The number of blocks, or -1 for blocks since last difficulty change."},
                    {"height", RPCArg::Type::NUM, /* default */ "-1", "To estimate at the time of the given height."},
                },
                RPCResult{
            "x             (numeric) Hashes per second estimated\n"
                },
                RPCExamples{
                    HelpExampleCli("getnetworkhashps", "")
            + HelpExampleRpc("getnetworkhashps", "")
                },
            }.ToString());

    LOCK(cs_main);
    return GetNetworkHashPS(!request.params[0].isNull() ? request.params[0].get_int() : 120, !request.params[1].isNull() ? request.params[1].get_int() : -1);
}

#if ENABLE_MINER
UniValue generateBlocks(std::shared_ptr<CReserveScript> coinbaseScript, int nGenerate, uint64_t nMaxTries, bool keepScript)
{
    static const int nInnerLoopCount = 0x10000;
    int nHeightEnd = 0;
    int nHeight = 0;

    {   // Don't keep cs_main locked
        LOCK(cs_main);
        nHeight = ::ChainActive().Height();
        nHeightEnd = nHeight+nGenerate;
    }
    unsigned int nExtraNonce = 0;
    UniValue blockHashes(UniValue::VARR);
    uint256 thash;
    while (nHeight < nHeightEnd && !ShutdownRequested())
    {
        std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(Params()).CreateNewBlock(coinbaseScript->reserveScript));
        if (!pblocktemplate.get())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Couldn't create new block");
        CBlock *pblock = &pblocktemplate->block;
        {
            LOCK(cs_main);
            IncrementExtraNonce(pblock, ::ChainActive().Tip(), nExtraNonce);
        }
        if (pblock->IsProgPow()) {
            while (nMaxTries > 0 && pblock->nNonce64 < nInnerLoopCount) {
                uint256 mix_hash;
                auto final_hash{progpow_hash_full(pblock->GetProgPowHeader(), mix_hash)};
                if (CheckProofOfWork(final_hash, pblock->nBits, Params().GetConsensus()))
                {
                    pblock->mix_hash = mix_hash;
                    break;
                }
                ++pblock->nNonce64;
                --nMaxTries;
            }
        while (nMaxTries > 0 && pblock->nNonce < std::numeric_limits<uint32_t>::max() && !CheckProofOfWork(pblock->GetHash(), pblock->nBits, Params().GetConsensus()) && !ShutdownRequested()) {
            ++pblock->nNonce;
            --nMaxTries;
        }
        if (nMaxTries == 0 || ShutdownRequested()) {
            break;
        }
        if (pblock->nNonce == nInnerLoopCount || pblock->nNonce64 == nInnerLoopCount) {
            continue;
        }
        std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
        if (!ProcessNewBlock(Params(), shared_pblock, true, nullptr))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");
        ++nHeight;
        blockHashes.push_back(pblock->GetHash().GetHex());

        //mark script as important because it was used at least for one coinbase output if the script came from the wallet
        if (keepScript)
        {
            LOCK(cs_main);
            coinbaseScript->KeepScript();
        }
    }
    return blockHashes;
}

static UniValue generatetoaddress(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw std::runtime_error(
            RPCHelpMan{"generatetoaddress",
                "\nMine blocks immediately to a specified address (before the RPC call returns)\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated immediately."},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address to send the newly generated SCC to."},
                    {"maxtries", RPCArg::Type::NUM, /* default */ "1000000", "How many iterations to try."},
                },
                RPCResult{
            "[ blockhashes ]     (array) hashes of blocks generated\n"
                },
                RPCExamples{
            "\nGenerate 11 blocks to myaddress\n"
            + HelpExampleCli("generatetoaddress", "11 \"myaddress\"")
                },
            }.ToString());

    int nGenerate = request.params[0].get_int();
    uint64_t nMaxTries = 1000000;
    if (!request.params[2].isNull()) {
        nMaxTries = request.params[2].get_int();
    }

    CTxDestination destination = DecodeDestination(request.params[1].get_str());
    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    }

    std::shared_ptr<CReserveScript> coinbaseScript = std::make_shared<CReserveScript>();
    coinbaseScript->reserveScript = GetScriptForDestination(destination);

    return generateBlocks(coinbaseScript, nGenerate, nMaxTries, false);
}
#else
static UniValue generatetoaddress(const JSONRPCRequest& request)
{
    throw JSONRPCError(RPC_METHOD_NOT_FOUND, "This call is not available because RPC miner isn't compiled");
}
#endif // ENABLE_MINER

static UniValue getmininginfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            RPCHelpMan{"getmininginfo",
                "\nReturns a json object containing mining-related information.",
                {},
                RPCResult{
            "{\n"
            "  \"blocks\": nnn,             (numeric) The current block\n"
            "  \"currentblocksize\": nnn,   (numeric) The last block size\n"
            "  \"currentblocktx\": nnn,     (numeric) The last block transaction\n"
            "  \"difficulty\": xxx.xxxxx    (numeric) The current difficulty\n"
            "  \"networkhashps\": nnn,      (numeric) The network hashes per second\n"
            "  \"pooledtx\": n              (numeric) The size of the mempool\n"
            "  \"chain\": \"xxxx\",           (string) current network name as defined in BIP70 (main, test, regtest)\n"
            "  \"warnings\": \"...\"          (string) any network and blockchain warnings\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getmininginfo", "")
            + HelpExampleRpc("getmininginfo", "")
                },
            }.ToString());


    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("blocks",           (int)::ChainActive().Height());
    obj.pushKV("currentblocksize", (uint64_t)nLastBlockSize);
    obj.pushKV("currentblocktx",   (uint64_t)nLastBlockTx);
    obj.pushKV("difficulty",       (double)GetDifficulty(::ChainActive().Tip()));
    obj.pushKV("networkhashps",    getnetworkhashps(request));
    obj.pushKV("pooledtx",         (uint64_t)mempool.size());
    obj.pushKV("chain",            Params().NetworkIDString());
    obj.pushKV("warnings",         GetWarnings("statusbar"));
    return obj;
}


// NOTE: Unlike wallet RPC (which use BTC values), mining RPCs follow GBT (BIP 22) in using satoshi amounts
static UniValue prioritisetransaction(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            RPCHelpMan{"prioritisetransaction",
                "Accepts the transaction into mined blocks at a higher (or lower) priority\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id."},
                    {"fee_delta", RPCArg::Type::NUM, RPCArg::Optional::NO, "The fee value (in olegs) to add (or subtract, if negative).\n"
            "                  Note, that this value is not a fee rate. It is a value to modify absolute fee of the TX.\n"
            "                  The fee is not actually paid, only the algorithm for selecting transactions into a block\n"
            "                  considers the transaction as it would have paid a higher (or lower) fee."},
                },
                RPCResult{
            "true              (boolean) Returns true\n"
                },
                RPCExamples{
                    HelpExampleCli("prioritisetransaction", "\"txid\" 10000")
            + HelpExampleRpc("prioritisetransaction", "\"txid\", 10000")
                },
            }.ToString());

    LOCK(cs_main);

    uint256 hash = ParseHashV(request.params[0].get_str(), "txid");
    CAmount nAmount = request.params[1].get_int64();

    mempool.PrioritiseTransaction(hash, nAmount);
    return true;
}


// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const CValidationState& state)
{
    if (state.IsValid())
        return NullUniValue;

    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, FormatStateMessage(state));
    if (state.IsInvalid())
    {
        std::string strRejectReason = state.GetRejectReason();
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

static std::string gbt_vb_name(const Consensus::DeploymentPos pos) {
    const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
    std::string s = vbinfo.name;
    if (!vbinfo.gbt_force) {
        s.insert(s.begin(), '!');
    }
    return s;
}

static UniValue getblocktemplate(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{"getblocktemplate",
                "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
                "It returns data needed to construct a block to work on.\n"
                "For full specification, see BIPs 22, 23, and 9:\n"
                "    https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki\n"
                "    https://github.com/bitcoin/bips/blob/master/bip-0023.mediawiki\n"
                "    https://github.com/bitcoin/bips/blob/master/bip-0009.mediawiki#getblocktemplate_changes\n",
                {
                    {"template_request", RPCArg::Type::OBJ, /* default_val */ "", "A json object in the following spec",
                        {
                            {"mode", RPCArg::Type::STR, /* treat as named arg */ RPCArg::Optional::OMITTED_NAMED_ARG, "This must be set to \"template\", \"proposal\" (see BIP 23), or omitted"},
                            {"capabilities", RPCArg::Type::ARR, /* treat as named arg */ RPCArg::Optional::OMITTED_NAMED_ARG, "A list of strings",
                                {
                                    {"support", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "client side supported feature, 'longpoll', 'coinbasetxn', 'coinbasevalue', 'proposal', 'serverlist', 'workid'"},
                                },
                                },
                            {"rules", RPCArg::Type::ARR, /* default_val */ "", "A list of strings",
                                {
                                    {"support", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "client side supported softfork deployment"},
                                },
                                },
                        },
                        "\"template_request\""},
                    {"reward_address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "address for reward in coinbase (meaningful only if block solution is later submitter with pprpcsb)\n"},
                },
                RPCResult{
            "{\n"
            "  \"capabilities\" : [ \"capability\", ... ],    (array of strings) specific client side supported features\n"
            "  \"version\" : n,                    (numeric) The preferred block version\n"
            "  \"rules\" : [ \"rulename\", ... ],    (array of strings) specific block rules that are to be enforced\n"
            "  \"vbavailable\" : {                 (json object) set of pending, supported versionbit (BIP 9) softfork deployments\n"
            "      \"rulename\" : bitnumber          (numeric) identifies the bit number as indicating acceptance and readiness for the named softfork rule\n"
            "      ,...\n"
            "  },\n"
            "  \"vbrequired\" : n,                 (numeric) bit mask of versionbits the server requires set in submissions\n"
            "  \"previousblockhash\" : \"xxxx\",     (string) The hash of current highest block\n"
            "  \"transactions\" : [                (array) contents of non-coinbase transactions that should be included in the next block\n"
            "      {\n"
            "         \"data\" : \"xxxx\",             (string) transaction data encoded in hexadecimal (byte-for-byte)\n"
            "         \"hash\" : \"xxxx\",             (string) hash/id encoded in little-endian hexadecimal\n"
            "         \"depends\" : [                (array) array of numbers \n"
            "             n                          (numeric) transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is\n"
            "             ,...\n"
            "         ],\n"
            "         \"fee\": n,                    (numeric) difference in value between transaction inputs and outputs (in olegs); for coinbase transactions, this is a negative Number of the total collected block fees (ie, not including the block subsidy); if key is not present, fee is unknown and clients MUST NOT assume there isn't one\n"
            "         \"sigops\" : n,                (numeric) total number of SigOps, as counted for purposes of block limits; if key is not present, sigop count is unknown and clients MUST NOT assume there aren't any\n"
            "      }\n"
            "      ,...\n"
            "  ],\n"
            "  \"coinbaseaux\" : { ... },            (json object) data that should be included in the coinbase's scriptSig content\n"
            "  \"coinbasevalue\" : n,              (numeric) maximum allowable input to coinbase transaction, including the generation award and transaction fees (in olegs)\n"
            "  \"coinbasetxn\" : { ... },          (json object) information for coinbase transaction\n"
            "  \"target\" : \"xxxx\",                (string) The hash target\n"
            "  \"mintime\" : xxx,                  (numeric) The minimum timestamp appropriate for next block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mutable\" : [                     (array of string) list of ways the block template may be changed \n"
            "     \"value\"                          (string) A way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'\n"
            "     ,...\n"
            "  ],\n"
            "  \"noncerange\" : \"00000000ffffffff\",(string) A range of valid nonces\n"
            "  \"sigoplimit\" : n,                 (numeric) limit of sigops in blocks\n"
            "  \"sizelimit\" : n,                  (numeric) limit of block size\n"
            "  \"curtime\" : ttt,                  (numeric) current timestamp in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"bits\" : \"xxxxxxxx\",              (string) compressed target of next block\n"
            "  \"previousbits\" : \"xxxxxxxx\",      (string) compressed target of current highest block\n"
            "  \"height\" : n                      (numeric) The height of the next block\n"
            "  \"masternode\" : [                  (array) required masternode payments that must be included in the next block\n"
            "      {\n"
            "         \"payee\" : \"xxxx\",          (string) payee address\n"
            "         \"script\" : \"xxxx\",         (string) payee scriptPubKey\n"
            "         \"amount\": n                (numeric) required amount to pay\n"
            "      }\n"
            "  },\n"
            "  \"masternode_payments_started\" :  true|false, (boolean) true, if masternode payments started\n"
            "  \"masternode_payments_enforced\" : true|false, (boolean) true, if masternode payments are enforced\n"
            "  \"superblock\" : [                  (array) required superblock payees that must be included in the next block\n"
            "      {\n"
            "         \"payee\" : \"xxxx\",          (string) payee address\n"
            "         \"script\" : \"xxxx\",         (string) payee scriptPubKey\n"
            "         \"amount\": n                (numeric) required amount to pay\n"
            "      }\n"
            "      ,...\n"
            "  ],\n"
            "  \"superblocks_started\" : true|false, (boolean) true, if superblock payments started\n"
            "  \"superblocks_enabled\" : true|false, (boolean) true, if superblock payments are enabled\n"
            "  \"coinbase_payload\" : \"xxxxxxxx\"    (string) coinbase transaction payload data encoded in hexadecimal\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getblocktemplate", "")
            + HelpExampleRpc("getblocktemplate", "")
                },
            }.ToString());

    LOCK(cs_main);

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    std::set<std::string> setClientRules;
    int64_t nMaxVersionPreVB = -1;
    if (!request.params[0].isNull())
    {
        const UniValue& oparam = request.params[0].get_obj();
        const UniValue& modeval = find_value(oparam, "mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull())
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = find_value(oparam, "longpollid");

        if (strMode == "proposal")
        {
            const UniValue& dataval = find_value(oparam, "data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHash();
            const CBlockIndex* pindex = LookupBlockIndex(hash);
            if (pindex) {
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                return "duplicate-inconclusive";
            }

            CBlockIndex* const pindexPrev = ::ChainActive().Tip();
            // TestBlockValidity only supports blocks built on the current Tip
            if (block.hashPrevBlock != pindexPrev->GetBlockHash())
                return "inconclusive-not-best-prevblk";
            CValidationState state;
            TestBlockValidity(state, Params(), block, pindexPrev, false, true);
            return BIP22ValidationResult(state);
        }

        const UniValue& aClientRules = find_value(oparam, "rules");
        if (aClientRules.isArray()) {
            for (unsigned int i = 0; i < aClientRules.size(); ++i) {
                const UniValue& v = aClientRules[i];
                setClientRules.insert(v.get_str());
            }
        } else {
            // NOTE: It is important that this NOT be read if versionbits is supported
            const UniValue& uvMaxVersion = find_value(oparam, "maxversion");
            if (uvMaxVersion.isNum()) {
                nMaxVersionPreVB = uvMaxVersion.get_int64();
            }
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if(!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    if (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, PACKAGE_NAME " is not connected!");

    if (::ChainstateActive().IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, PACKAGE_NAME " is in initial sync and waiting for blocks...");

    // next bock is a superblock and we need governance info to correctly construct it
    if (AreSuperblocksEnabled()
        && !masternodeSync.IsSynced()
        && CSuperblock::IsValidBlockHeight(::ChainActive().Height() + 1))
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, PACKAGE_NAME "is syncing with network...");

    static unsigned int nTransactionsUpdatedLast;

    if (!lpval.isNull())
    {
        // Wait to respond until either the best block changes, OR a minute has passed and there are more transactions
        uint256 hashWatchedChain;
        std::chrono::steady_clock::time_point checktxtime;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr())
        {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            std::string lpstr = lpval.get_str();

            hashWatchedChain.SetHex(lpstr.substr(0, 64));
            nTransactionsUpdatedLastLP = atoi64(lpstr.substr(64));
        }
        else
        {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = ::ChainActive().Tip()->GetBlockHash();
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        // Release the wallet and main lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            checktxtime = std::chrono::steady_clock::now() + std::chrono::minutes(1);

            WAIT_LOCK(g_best_block_mutex, lock);
            while (g_best_block == hashWatchedChain && IsRPCRunning())
            {
                if (g_best_block_cv.wait_until(lock, checktxtime) == std::cv_status::timeout)
                {
                    // Timeout: Check transactions for update
                    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP)
                        break;
                    checktxtime += std::chrono::seconds(10);
                }
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }

    // Update block
    static CBlockIndex* pindexPrev;
    static int64_t nStart;
    static std::unique_ptr<CBlockTemplate> pblocktemplate;
    if (pindexPrev != ::ChainActive().Tip() ||
        (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = nullptr;
        mapPPBlockTemplates.clear();

        // Store the ::ChainActive().Tip() used before CreateNewBlock, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = ::ChainActive().Tip();
        nStart = GetTime();

        // Create new block
        CScript scriptDummy = CScript() << OP_TRUE;
        pblocktemplate = BlockAssembler(Params()).CreateNewBlock(scriptDummy);
        if (!pblocktemplate)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");

        // Need to update only after we know CreateNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }
    assert(pindexPrev);
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience
    const Consensus::Params& consensusParams = Params().GetConsensus();

    // Update nTime
    UpdateTime(pblock, consensusParams, pindexPrev);
    pblock->nNonce = 0;

    // Update coinbase reward address
    bool fRewardAddressSet = false;
    if (request.params.size() >= 2) {
        /* TODO!!
        * Setup bech32 address reading, 
        * and normal to handle the reward 
        * address param for progpow.
        * Subsequent code is from firo 
        * and needs rebasing
        */
        CTxDestination rewardAddress = DecodeDestination(request.params[1].get_str());

        if (!(IsValidDestination(rewardAddress)))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Incorrect reward address");

        CMutableTransaction coinbaseTx = *pblock->vtx[0];
        coinbaseTx.vout[0].scriptPubKey = GetScriptForDestination(rewardAddress);
        pblock->vtx[0] = MakeTransactionRef(CTransaction(coinbaseTx));

        fRewardAddressSet = true;
    }

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue transactions(UniValue::VARR);
    std::map<uint256, int64_t> setTxIndex;
    int i = 0;
    for (const auto& it : pblock->vtx) {
        const CTransaction& tx = *it;
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase())
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.pushKV("data", EncodeHexTx(tx));

        entry.pushKV("hash", txHash.GetHex());

        UniValue deps(UniValue::VARR);
        for (const CTxIn &in : tx.vin)
        {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.pushKV("depends", deps);

        int index_in_template = i - 1;
        entry.pushKV("fee", pblocktemplate->vTxFees[index_in_template]);
        entry.pushKV("sigops", pblocktemplate->vTxSigOps[index_in_template]);

        transactions.push_back(entry);
    }

    UniValue aux(UniValue::VOBJ);

    arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

    UniValue aMutable(UniValue::VARR);
    aMutable.push_back("time");
    aMutable.push_back("transactions");
    aMutable.push_back("prevblock");

    UniValue result(UniValue::VOBJ);
    result.pushKV("capabilities", aCaps);

    UniValue aRules(UniValue::VARR);
    UniValue vbavailable(UniValue::VOBJ);
    for (int j = 0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
        Consensus::DeploymentPos pos = Consensus::DeploymentPos(j);
        ThresholdState state = VersionBitsState(pindexPrev, consensusParams, pos, versionbitscache);
        switch (state) {
            case ThresholdState::DEFINED:
            case ThresholdState::FAILED:
                // Not exposed to GBT at all
                break;
            case ThresholdState::LOCKED_IN:
                // Ensure bit is set in block version
                pblock->nVersion |= VersionBitsMask(consensusParams, pos);
                // FALL THROUGH to get vbavailable set...
            case ThresholdState::STARTED:
            {
                const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
                vbavailable.pushKV(gbt_vb_name(pos), consensusParams.vDeployments[pos].bit);
                if (setClientRules.find(vbinfo.name) == setClientRules.end()) {
                    if (!vbinfo.gbt_force) {
                        // If the client doesn't support this, don't indicate it in the [default] version
                        pblock->nVersion &= ~VersionBitsMask(consensusParams, pos);
                    }
                }
                break;
            }
            case ThresholdState::ACTIVE:
            {
                // Add to rules only
                const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
                aRules.push_back(gbt_vb_name(pos));
                if (setClientRules.find(vbinfo.name) == setClientRules.end()) {
                    // Not supported by the client; make sure it's safe to proceed
                    if (!vbinfo.gbt_force) {
                        // If we do anything other than throw an exception here, be sure version/force isn't sent to old clients
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Support for '%s' rule requires explicit client support", vbinfo.name));
                    }
                }
                break;
            }
        }
    }
    result.pushKV("version", pblock->nVersion);
    result.pushKV("rules", aRules);
    result.pushKV("vbavailable", vbavailable);
    result.pushKV("vbrequired", int(0));

    if (nMaxVersionPreVB >= 2) {
        // If VB is supported by the client, nMaxVersionPreVB is -1, so we won't get here
        // Because BIP 34 changed how the generation transaction is serialized, we can only use version/force back to v2 blocks
        // This is safe to do [otherwise-]unconditionally only because we are throwing an exception above if a non-force deployment gets activated
        // Note that this can probably also be removed entirely after the first BIP9 non-force deployment (ie, probably segwit) gets activated
        aMutable.push_back("version/force");
    }

    result.pushKV("previousblockhash", pblock->hashPrevBlock.GetHex());
    result.pushKV("transactions", transactions);
    result.pushKV("coinbaseaux", aux);
    result.pushKV("coinbasevalue", (int64_t)pblock->vtx[0]->GetValueOut());
    result.pushKV("longpollid", ::ChainActive().Tip()->GetBlockHash().GetHex() + i64tostr(nTransactionsUpdatedLast));
    result.pushKV("target", hashTarget.GetHex());
    result.pushKV("mintime", (int64_t)pindexPrev->GetMedianTimePast()+1);
    result.pushKV("mutable", aMutable);
    result.pushKV("noncerange", "00000000ffffffff");
    result.pushKV("sigoplimit", (int64_t)MaxBlockSigOps(fDIP0001ActiveAtTip));
    result.pushKV("sizelimit", (int64_t)MaxBlockSize(fDIP0001ActiveAtTip));
    result.pushKV("curtime", pblock->GetBlockTime());
    result.pushKV("bits", strprintf("%08x", pblock->nBits));
    result.pushKV("previousbits", strprintf("%08x", pblocktemplate->nPrevBits));
    result.pushKV("height", (int64_t)(pindexPrev->nHeight+1));

    UniValue masternodeObj(UniValue::VARR);
    for (const auto& txout : pblocktemplate->voutMasternodePayments) {
        CTxDestination dest;
        ExtractDestination(txout.scriptPubKey, dest);

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("payee", EncodeDestination(dest).c_str());
        obj.pushKV("script", HexStr(txout.scriptPubKey));
        obj.pushKV("amount", txout.nValue);
        masternodeObj.push_back(obj);
    }

    result.pushKV("masternode", masternodeObj);
    result.pushKV("masternode_payments_started", pindexPrev->nHeight + 1 > consensusParams.nMasternodePaymentsStartBlock);
    result.pushKV("masternode_payments_enforced", true);

    UniValue superblockObjArray(UniValue::VARR);
    if(pblocktemplate->voutSuperblockPayments.size()) {
        for (const auto& txout : pblocktemplate->voutSuperblockPayments) {
            UniValue entry(UniValue::VOBJ);
            CTxDestination dest;
            ExtractDestination(txout.scriptPubKey, dest);
            entry.pushKV("payee", EncodeDestination(dest).c_str());
            entry.pushKV("script", HexStr(txout.scriptPubKey));
            entry.pushKV("amount", txout.nValue);
            superblockObjArray.push_back(entry);
        }
    }
    result.pushKV("superblock", superblockObjArray);
    result.pushKV("superblocks_started", pindexPrev->nHeight + 1 > consensusParams.nSuperblockStartBlock);
    result.pushKV("superblocks_enabled", AreSuperblocksEnabled());

    result.pushKV("coinbase_payload", HexStr(pblock->vtx[0]->vExtraPayload));

    if (pblock->IsProgPow()) {
        static std::string lastHeader{};
        if (mapPPBlockTemplates.count(lastHeader) && ((pblock->nTime - 30) < mapPPBlockTemplates.at(lastHeader).nTime))
        {
            result.pushKV("pprpcheader", lastHeader);
            result.pushKV("pprpcepoch", ethash::get_epoch_number(pblock->nHeight));
            return result;
        }
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
        lastHeader = pblock->GetProgPowHeaderHash().GetHex();
        result.pushKV("pprpcheader", lastHeader);
        result.pushKV("pprpcepoch", ethash::get_epoch_number(pblock->nHeight));
        if (fRewardAddressSet)
            // don't bother to save block unless reward address is set
            mapPPBlockTemplates[lastHeader] = *pblock;
    }
    return result;
}

class submitblock_StateCatcher : public CValidationInterface
{
public:
    uint256 hash;
    bool found;
    CValidationState state;

    explicit submitblock_StateCatcher(const uint256 &hashIn) : hash(hashIn), found(false), state() {}

protected:
    void BlockChecked(const CBlock& block, const CValidationState& stateIn) override {
        if (block.GetHash() != hash)
            return;
        found = true;
        state = stateIn;
    }
};

UniValue pprpcsb(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 3) {
        throw std::runtime_error(
                "pprpcsb \"header_hash\" \"mix_hash\" \"nonce\"\n"
                "\nAttempts to submit a progpow solution to block via rpc.\n"

                "\nArguments\n"
                "1. \"header_hash\"        (string, required) the prow_pow header hash that was given to the gpu miner from this rpc client\n"
                "2. \"mix_hash\"           (string, required) the mix hash that was mined by the gpu miner via rpc\n"
                "3. \"nonce\"              (string, required) the nonce of the block that hashed the valid block\n"
                "\nResult:\n"
                "\nExamples:\n"
                + HelpExampleCli("pprpcsb", "\"header_hash\" \"mix_hash\" 100000")
                + HelpExampleRpc("pprpcsb", "\"header_hash\" \"mix_hash\" 100000")
        );
    }

    std::string header_hex = request.params[0].get_str();
    std::string mix_hex = request.params[1].get_str();
    std::string nonce_hex = request.params[2].get_str();

    // Parse nonce
    uint64_t nonce{0};
    try
    {
        nonce = std::stoull(nonce_hex, nullptr, 0);
    }
    catch(...)
    {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid hex nonce");
    }

    // Check provided header_hash is in cache
    if (!mapPPBlockTemplates.count(header_hex))
    {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Job not found");
    }

    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    *blockptr = mapPPBlockTemplates.at(header_hex);
    blockptr->nNonce64 = nonce;

    // Check provided solution is formally valid
    uint256 act_mix_hash = uint256S(mix_hex);
    uint256 exp_mix_hash{};
    uint256 final_hash = blockptr->GetProgPowHashFull(exp_mix_hash);
    if (act_mix_hash != exp_mix_hash)
    {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Bad solution : mismatching mix_hash");
    }

    // Check provided solution honors boundaries
    if (!CheckProofOfWork(final_hash, blockptr->nBits, Params().GetConsensus()))
    {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Bad solution : not below target");
    }

    // Store mix_hash
    blockptr->mix_hash = exp_mix_hash;
    uint256 blockHash = blockptr->GetHash();

    bool fBlockPresent{false};
    {
        LOCK(cs_main);
        BlockMap::iterator mi{mapBlockIndex.find(blockHash)};
        if (mi != mapBlockIndex.end())
        {
            CBlockIndex* indexptr{mi->second};
            if (indexptr->IsValid(BLOCK_VALID_SCRIPTS))
            {
                return "duplicate";
            }
            if (indexptr->nStatus & BLOCK_FAILED_MASK)
            {
                return "duplicate-invalid";
            }

        }
        // Otherwise, we might only have the header - process the block before returning
        fBlockPresent = true;

        mi = mapBlockIndex.find(blockptr->hashPrevBlock);
        if (mi != mapBlockIndex.end()) {
            UpdateUncommittedBlockStructures(*blockptr, mi->second, Params().GetConsensus());
        }
    }

    // Process block
    submitblock_StateCatcher sc(blockHash);
    RegisterValidationInterface(&sc);
    bool fAccepted = ProcessNewBlock(Params(), blockptr, true, nullptr);
    UnregisterValidationInterface(&sc);
    if (fBlockPresent)
    {
        if (fAccepted && !sc.found)
        {
            return "duplicate-inconclusive";
        }
        return "duplicate";
    }
    if (!sc.found)
    {
        return "inconclusive";
    }

    return BIP22ValidationResult(sc.state);
}

static UniValue submitblock(const JSONRPCRequest& request)
{
    // We allow 2 arguments for compliance with BIP22. Argument 2 is ignored.
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            RPCHelpMan{"submitblock",
                "\nAttempts to submit new block to network.\n"
                "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n",
                {
                    {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block data to submit"},
                    {"dummy", RPCArg::Type::STR, /* default */ "ignored", "dummy value, for compatibility with BIP22. This value is ignored."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("submitblock", "\"mydata\"")
            + HelpExampleRpc("submitblock", "\"mydata\"")
                },
            }.ToString());
    }

    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock& block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block does not start with a coinbase");
    }

    uint256 hash = block.GetHash();
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        if (pindex) {
            if (pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
                return "duplicate";
            }
            if (pindex->nStatus & BLOCK_FAILED_MASK) {
                return "duplicate-invalid";
            }
        }
    }

    bool new_block;
    submitblock_StateCatcher sc(block.GetHash());
    RegisterValidationInterface(&sc);
    bool accepted = ProcessNewBlock(Params(), blockptr, /* fForceProcessing */ true, /* fNewBlock */ &new_block);
    UnregisterValidationInterface(&sc);
    if (!new_block && accepted) {
        return "duplicate";
    }
    if (!sc.found) {
        return "inconclusive";
    }
    return BIP22ValidationResult(sc.state);
}

static UniValue submitheader(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            RPCHelpMan{"submitheader",
                "\nDecode the given hexdata as a header and submit it as a candidate chain tip if valid."
                "\nThrows when the header is invalid.\n",
                {
                    {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block header data"},
                },
                RPCResult{
            "None"
                },
                RPCExamples{
                    HelpExampleCli("submitheader", "\"aabbcc\"") +
                    HelpExampleRpc("submitheader", "\"aabbcc\"")
                },
            }.ToString());
    }

    CBlockHeader h;
    if (!DecodeHexBlockHeader(h, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block header decode failed");
    }
    {
        LOCK(cs_main);
        if (!LookupBlockIndex(h.hashPrevBlock)) {
            throw JSONRPCError(RPC_VERIFY_ERROR, "Must submit previous header (" + h.hashPrevBlock.GetHex() + ") first");
        }
    }

    CValidationState state;
    ProcessNewBlockHeaders({h}, state, Params(), /* ppindex */ nullptr, /* first_invalid */ nullptr);
    if (state.IsValid()) return NullUniValue;
    if (state.IsError()) {
        throw JSONRPCError(RPC_VERIFY_ERROR, FormatStateMessage(state));
    }
    throw JSONRPCError(RPC_VERIFY_ERROR, state.GetRejectReason());
}

static UniValue estimatesmartfee(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{"estimatesmartfee",
                "\nEstimates the approximate fee per kilobyte needed for a transaction to begin\n"
                "confirmation within conf_target blocks if possible and return the number of blocks\n"
                "for which the estimate is valid.\n",
                {
                    {"conf_target", RPCArg::Type::NUM, RPCArg::Optional::NO, "Confirmation target in blocks (1 - 1008)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "CONSERVATIVE", "The fee estimate mode.\n"
            "                   Whether to return a more conservative estimate which also satisfies\n"
            "                   a longer history. A conservative estimate potentially returns a\n"
            "                   higher feerate and is more likely to be sufficient for the desired\n"
            "                   target, but is not as responsive to short term drops in the\n"
            "                   prevailing fee market.  Must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\""},
                },
                RPCResult{
            "{\n"
            "  \"feerate\" : x.x,     (numeric, optional) estimate fee rate in " + CURRENCY_UNIT + "/kB\n"
            "  \"errors\": [ str... ] (json array of strings, optional) Errors encountered during processing\n"
            "  \"blocks\" : n         (numeric) block number where estimate was found\n"
            "}\n"
            "\n"
            "The request target will be clamped between 2 and the highest target\n"
            "fee estimation is able to return based on how long it has been running.\n"
            "An error is returned if not enough transactions and blocks\n"
            "have been observed to make an estimate for any number of blocks.\n"
                },
                RPCExamples{
                    HelpExampleCli("estimatesmartfee", "6")
                },
            }.ToString());

    RPCTypeCheck(request.params, {UniValue::VNUM, UniValue::VSTR});
    RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
    unsigned int max_target = ::feeEstimator.HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
    unsigned int conf_target = ParseConfirmTarget(request.params[0], max_target);
    bool conservative = true;
    if (!request.params[1].isNull()) {
        FeeEstimateMode fee_mode;
        if (!FeeModeFromString(request.params[1].get_str(), fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
        if (fee_mode == FeeEstimateMode::ECONOMICAL) conservative = false;
    }

    UniValue result(UniValue::VOBJ);
    UniValue errors(UniValue::VARR);
    FeeCalculation feeCalc;
    CFeeRate feeRate = ::feeEstimator.estimateSmartFee(conf_target, &feeCalc, conservative);
    if (feeRate != CFeeRate(0)) {
        result.pushKV("feerate", ValueFromAmount(feeRate.GetFeePerK()));
    } else {
        errors.push_back("Insufficient data or no feerate found");
        result.pushKV("errors", errors);
    }
    result.pushKV("blocks", feeCalc.returnedTarget);
    return result;
}

static UniValue estimaterawfee(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{"estimaterawfee",
                "\nWARNING: This interface is unstable and may disappear or change!\n"
                "\nWARNING: This is an advanced API call that is tightly coupled to the specific\n"
                "         implementation of fee estimation. The parameters it can be called with\n"
                "         and the results it returns will change if the internal implementation changes.\n"
                "\nEstimates the approximate fee per kilobyte needed for a transaction to begin\n"
                "confirmation within conf_target blocks if possible.\n",
                {
                    {"conf_target", RPCArg::Type::NUM, RPCArg::Optional::NO, "Confirmation target in blocks (1 - 1008)"},
                    {"threshold", RPCArg::Type::NUM, /* default */ "0.95", "The proportion of transactions in a given feerate range that must have been\n"
            "               confirmed within conf_target in order to consider those feerates as high enough and proceed to check\n"
            "               lower buckets."},
                },
                RPCResult{
            "{\n"
            "  \"short\" : {            (json object, optional) estimate for short time horizon\n"
            "      \"feerate\" : x.x,        (numeric, optional) estimate fee rate in " + CURRENCY_UNIT + "/kB\n"
            "      \"decay\" : x.x,          (numeric) exponential decay (per block) for historical moving average of confirmation data\n"
            "      \"scale\" : x,            (numeric) The resolution of confirmation targets at this time horizon\n"
            "      \"pass\" : {              (json object, optional) information about the lowest range of feerates to succeed in meeting the threshold\n"
            "          \"startrange\" : x.x,     (numeric) start of feerate range\n"
            "          \"endrange\" : x.x,       (numeric) end of feerate range\n"
            "          \"withintarget\" : x.x,   (numeric) number of txs over history horizon in the feerate range that were confirmed within target\n"
            "          \"totalconfirmed\" : x.x, (numeric) number of txs over history horizon in the feerate range that were confirmed at any point\n"
            "          \"inmempool\" : x.x,      (numeric) current number of txs in mempool in the feerate range unconfirmed for at least target blocks\n"
            "          \"leftmempool\" : x.x,    (numeric) number of txs over history horizon in the feerate range that left mempool unconfirmed after target\n"
            "      },\n"
            "      \"fail\" : { ... },       (json object, optional) information about the highest range of feerates to fail to meet the threshold\n"
            "      \"errors\":  [ str... ]   (json array of strings, optional) Errors encountered during processing\n"
            "  },\n"
            "  \"medium\" : { ... },    (json object, optional) estimate for medium time horizon\n"
            "  \"long\" : { ... }       (json object) estimate for long time horizon\n"
            "}\n"
            "\n"
            "Results are returned for any horizon which tracks blocks up to the confirmation target.\n"
                },
                RPCExamples{
                    HelpExampleCli("estimaterawfee", "6 0.9")
                },
            }.ToString());

    RPCTypeCheck(request.params, {UniValue::VNUM, UniValue::VNUM}, true);
    RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
    unsigned int max_target = ::feeEstimator.HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
    unsigned int conf_target = ParseConfirmTarget(request.params[0], max_target);
    double threshold = 0.95;
    if (!request.params[1].isNull()) {
        threshold = request.params[1].get_real();
    }
    if (threshold < 0 || threshold > 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid threshold");
    }

    UniValue result(UniValue::VOBJ);

    for (const FeeEstimateHorizon horizon : {FeeEstimateHorizon::SHORT_HALFLIFE, FeeEstimateHorizon::MED_HALFLIFE, FeeEstimateHorizon::LONG_HALFLIFE}) {
        CFeeRate feeRate;
        EstimationResult buckets;

        // Only output results for horizons which track the target
        if (conf_target > ::feeEstimator.HighestTargetTracked(horizon)) continue;

        feeRate = ::feeEstimator.estimateRawFee(conf_target, threshold, horizon, &buckets);
        UniValue horizon_result(UniValue::VOBJ);
        UniValue errors(UniValue::VARR);
        UniValue passbucket(UniValue::VOBJ);
        passbucket.pushKV("startrange", round(buckets.pass.start));
        passbucket.pushKV("endrange", round(buckets.pass.end));
        passbucket.pushKV("withintarget", round(buckets.pass.withinTarget * 100.0) / 100.0);
        passbucket.pushKV("totalconfirmed", round(buckets.pass.totalConfirmed * 100.0) / 100.0);
        passbucket.pushKV("inmempool", round(buckets.pass.inMempool * 100.0) / 100.0);
        passbucket.pushKV("leftmempool", round(buckets.pass.leftMempool * 100.0) / 100.0);
        UniValue failbucket(UniValue::VOBJ);
        failbucket.pushKV("startrange", round(buckets.fail.start));
        failbucket.pushKV("endrange", round(buckets.fail.end));
        failbucket.pushKV("withintarget", round(buckets.fail.withinTarget * 100.0) / 100.0);
        failbucket.pushKV("totalconfirmed", round(buckets.fail.totalConfirmed * 100.0) / 100.0);
        failbucket.pushKV("inmempool", round(buckets.fail.inMempool * 100.0) / 100.0);
        failbucket.pushKV("leftmempool", round(buckets.fail.leftMempool * 100.0) / 100.0);

        // CFeeRate(0) is used to indicate error as a return value from estimateRawFee
        if (feeRate != CFeeRate(0)) {
            horizon_result.pushKV("feerate", ValueFromAmount(feeRate.GetFeePerK()));
            horizon_result.pushKV("decay", buckets.decay);
            horizon_result.pushKV("scale", (int)buckets.scale);
            horizon_result.pushKV("pass", passbucket);
            // buckets.fail.start == -1 indicates that all buckets passed, there is no fail bucket to output
            if (buckets.fail.start != -1) horizon_result.pushKV("fail", failbucket);
        } else {
            // Output only information that is still meaningful in the event of error
            horizon_result.pushKV("decay", buckets.decay);
            horizon_result.pushKV("scale", (int)buckets.scale);
            horizon_result.pushKV("fail", failbucket);
            errors.push_back("Insufficient data or no feerate found which meets threshold");
            horizon_result.pushKV("errors",errors);
        }
        result.pushKV(StringForFeeEstimateHorizon(horizon), horizon_result);
    }
    return result;
}

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    { "mining",             "getnetworkhashps",       &getnetworkhashps,       {"nblocks","height"} },
    { "mining",             "getmininginfo",          &getmininginfo,          {} },
    { "mining",             "prioritisetransaction",  &prioritisetransaction,  {"txid","fee_delta"} },
    { "mining",             "getblocktemplate",       &getblocktemplate,       {"template_request"} },
    { "mining",             "pprpcsb",                &pprpcsb,                {"header_hash","mix_hash", "nonce"} },
    { "mining",             "submitblock",            &submitblock,            {"hexdata","dummy"} },
    { "mining",             "submitheader",           &submitheader,           {"hexdata"} },

#if ENABLE_MINER
    { "generating",         "generatetoaddress",      &generatetoaddress,      {"nblocks","address","maxtries"} },
#else
    { "hidden",             "generatetoaddress",      &generatetoaddress,      {"nblocks","address","maxtries"} }, // Hidden as it isn't functional, just an error to let people know if miner isn't compiled
#endif // ENABLE_MINER

    { "util",               "estimatesmartfee",       &estimatesmartfee,       {"conf_target", "estimate_mode"} },

    { "hidden",             "estimaterawfee",         &estimaterawfee,         {"conf_target", "threshold"} },
};
// clang-format on

void RegisterMiningRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
