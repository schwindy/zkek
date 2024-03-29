// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "amount.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "hash.h"
#include "main.h"
#include "net.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "random.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#ifdef ENABLE_WALLET
#include "crypto/equihash.h"
#include "wallet/wallet.h"
#include <functional>
#endif

#include "sodium.h"

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#include <mutex>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.
// The COrphan class keeps track of these 'temporary orphans' while
// CreateBlock is figuring out which transactions to include.
//
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    CFeeRate feeRate;
    double dPriority;

    COrphan(const CTransaction* ptxIn) : ptx(ptxIn), feeRate(0), dPriority(0)
    {
    }
};

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

// We want to sort transactions by priority and fee rate, so:
typedef boost::tuple<double, CFeeRate, const CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;

    public:TxPriorityCompare(bool _byFee) : byFee(_byFee) { }

    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if(byFee)
        {
            if(a.get<1>() == b.get<1>())return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }

        if(a.get<0>() == b.get<0>())return a.get<1>() < b.get<1>();
        return a.get<0>() < b.get<0>();
    }
};

void UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    pblock->nTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if(consensusParams.fPowAllowMinDifficultyBlocks)pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
}

CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn)
{
    const CChainParams& chainparams = Params();
    // Create new block
    unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if(Params().MineBlocksOnDemand())pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);

    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)250, std::min((unsigned int)(MAX_BLOCK_SIZE-250), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CAmount nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = chainActive.Tip();
        const int nHeight = pindexPrev->nHeight + 1;
        pblock->nTime = GetAdjustedTime();
        const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();
        CCoinsViewCache view(pcoinsTip);

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority", false);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for(map<uint256, CTxMemPoolEntry>::iterator mi = mempool.mapTx.begin();mi != mempool.mapTx.end();++mi)
        {
            const CTransaction& tx = mi->second.GetTx();

            int64_t nLockTimeCutoff;
            if(STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)nLockTimeCutoff = nMedianTimePast;
            else nLockTimeCutoff = pblock->GetBlockTime();

            if(tx.IsCoinBase() || !IsFinalTx(tx, nHeight, nLockTimeCutoff))continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            CAmount nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                if(!view.HaveCoins(txin.prevout.hash))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if(!mempool.mapTx.count(txin.prevout.hash))
                    {
                        LogPrintf("ERROR: mempool transaction missing input\n");
                        if(fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if(porphan)vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if(!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }

                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].GetTx().vout[txin.prevout.n].nValue;
                    continue;
                }

                const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                assert(coins);

                CAmount nValueIn = coins->vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = nHeight - coins->nHeight;
                dPriority += (double)nValueIn * nConf;
            }

            if(fMissingInputs)continue;

            // Priority is sum(valuein * age) / modified_txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority = tx.ComputePriority(dPriority, nTxSize);

            uint256 hash = tx.GetHash();
            mempool.ApplyDeltas(hash, dPriority, nTotalIn);

            CFeeRate feeRate(nTotalIn-tx.GetValueOut(), nTxSize);

            if(!porphan)vecPriority.push_back(TxPriority(dPriority, feeRate, &mi->second.GetTx()));
            else
            {
                porphan->dPriority = dPriority;
                porphan->feeRate = feeRate;
            }
        }

        // Collect transactions into block
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while(!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            CFeeRate feeRate = vecPriority.front().get<1>();
            const CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if(nBlockSize + nTxSize >= nBlockMaxSize)continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if(nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)continue;

            // Skip free transactions if we're past the minimum block size:
            const uint256& hash = tx.GetHash();
            double dPriorityDelta = 0;
            CAmount nFeeDelta = 0;
            mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);

            if(fSortedByFee && (dPriorityDelta <= 0) && (nFeeDelta <= 0) && (feeRate < ::minRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))continue;

            // Prioritise by fee once past the priority size or we run out of high-priority
            // transactions:
            if(!fSortedByFee && ((nBlockSize + nTxSize >= nBlockPrioritySize) || !AllowFree(dPriority)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if(!view.HaveInputs(tx))continue;

            CAmount nTxFees = view.GetValueIn(tx)-tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if(nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)continue;

            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            CValidationState state;
            if(!ContextualCheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, Params().GetConsensus()))continue;

            UpdateCoins(tx, state, view, nHeight);

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if(fPrintPriority)
            {
                LogPrintf
                (
                    "priority %.1f fee %s txid %s\n",
                    dPriority,
                    feeRate.ToString(),
                    tx.GetHash().ToString()
                );
            }

            // Add transactions that depend on this one to the priority queue
            if(mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if(!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if(porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->feeRate, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        LogPrintf("Zkek-Miner::CreateNewBlock(): total size %u\n", nBlockSize);

        // Create coinbase tx
        CMutableTransaction txNew;
        txNew.vin.resize(1);
        txNew.vin[0].prevout.SetNull();
        txNew.vout.resize(1);
        txNew.vout[0].scriptPubKey = scriptPubKeyIn;
        txNew.vout[0].nValue = GetBlockSubsidy(nHeight, chainparams.GetConsensus());

        if((nHeight > 0) && (nHeight <= chainparams.GetConsensus().GetLastFoundersRewardBlockHeight()))
        {
            // Founders reward is 20% of the block subsidy
            auto vFoundersReward = txNew.vout[0].nValue / 5;
            // Take some reward away from us
            txNew.vout[0].nValue -= vFoundersReward;

            // And give it to the founders
            txNew.vout.push_back(CTxOut(vFoundersReward, chainparams.GetFoundersRewardScriptAtHeight(nHeight)));
        }

        // Add fees
        txNew.vout[0].nValue += nFees;
        txNew.vin[0].scriptSig = CScript() << nHeight << OP_0;

        pblock->vtx[0] = txNew;
        pblocktemplate->vTxFees[0] = -nFees;

        // Randomise nonce
        arith_uint256 nonce = UintToArith256(GetRandHash());
        // Clear the top and bottom 16 bits (for local use as thread flags and counters)
        nonce <<= 32;
        nonce >>= 16;
        pblock->nNonce = ArithToUint256(nonce);

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        pblock->hashReserved   = uint256();
        UpdateTime(pblock, Params().GetConsensus(), pindexPrev);
        pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());
        pblock->nSolution.clear();
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

        CValidationState state;
        if(!TestBlockValidity(state, *pblock, pindexPrev, false, false))throw std::runtime_error("CreateNewBlock(): TestBlockValidity failed");
    }

    return pblocktemplate.release();
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if(hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }

    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}

//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey)
{
    CPubKey pubkey;
    if(!reservekey.GetReservedKey(pubkey))return NULL;

    CScript scriptPubKey = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
    return CreateNewBlock(scriptPubKey);
}

static bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if(pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())return error("Zkek-Miner: generated block is stale");
    }

    // Remove key from key pool
    reservekey.KeepKey();

    // Track how many getdata requests this block gets
    {
        LOCK(wallet.cs_wallet);
        wallet.mapRequestCount[pblock->GetHash()] = 0;
    }

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if(!ProcessNewBlock(state, NULL, pblock, true, NULL))return error("Zkek-Miner: ProcessNewBlock, block not accepted");
    return true;
}

void static BitcoinMiner(CWallet *pwallet)
{
    LogPrintf("Zkek-Miner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("zcash-miner");
    const CChainParams& chainparams = Params();

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    unsigned int n = chainparams.EquihashN();
    unsigned int k = chainparams.EquihashK();

    std::mutex m_cs;
    bool cancelSolver = false;
    boost::signals2::connection c = uiInterface.NotifyBlockTip.connect
    (
        [&m_cs, &cancelSolver](const uint256& hashNewTip) mutable
        {
            std::lock_guard<std::mutex> lock{m_cs};
            cancelSolver = true;
        }
    );

    try
    {
        while(true)
        {
            if(chainparams.MiningRequiresPeers())
            {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do
                {
                    bool fvNodesEmpty;
                    {
                        LOCK(cs_vNodes);
                        fvNodesEmpty = vNodes.empty();
                    }

                    if(!fvNodesEmpty && !IsInitialBlockDownload())break;
                    MilliSleep(1000);
                }
                while(true);
            }

            // Create new block
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev = chainActive.Tip();

            unique_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey));
            if(!pblocktemplate.get())
            {
                LogPrintf("Error in Zkek-Miner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                return;
            }

            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            LogPrintf
            (
                "Miner::BitcoinMiner(): Hashing %u transactions in block (%u bytes)\n",
                pblock->vtx.size(),
                ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION)
            );

            // Search
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

            while(true)
            {
                // Hash state
                crypto_generichash_blake2b_state state;
                EhInitialiseState(n, k, state);

                // I = the block header minus nonce and solution.
                CEquihashInput I{*pblock};
                CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                ss << I;

                // @TODO: Fix this
//                std::array<string, 5> const args =
//                {
//                    {0, pblock->nNonce.ToString()},
//                    {1, I},
//                };
//                std::string const command = std::string("/home/azureuser/zcash-tromp/equi1 -n ") + args[0];
//                system(command.c_str());

                // H(I||...
                crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());

                // H(I||V||...
                crypto_generichash_blake2b_state curr_state;
                curr_state = state;
                crypto_generichash_blake2b_update(&curr_state, pblock->nNonce.begin(), pblock->nNonce.size());

                // (x_1, x_2, ...) = A(I, V, n, k)
                LogPrintf
                (
                    "Miner::BitcoinMiner(): Running Equihash solver with nNonce = %s\n",
                    pblock->nNonce.ToString()
                );

                std::function<bool(std::vector<unsigned char>)>
                    validBlock = [&pblock, &hashTarget, &pwallet, &reservekey, &m_cs, &cancelSolver, &chainparams](std::vector<unsigned char> soln)
                    {
                        // Write the solution to the hash and compute the result.
                        LogPrintf("Miner::BitcoinMiner(): Solution against target..\n");
                        pblock->nSolution = soln;

                        if(UintToArith256(pblock->GetHash()) > hashTarget)return false;

                        // Found a solution
                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("Zkek-Miner:\n");
                        LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", pblock->GetHash().GetHex(), hashTarget.GetHex());
                        if(ProcessBlockFound(pblock, *pwallet, reservekey))
                        {
                            // Ignore chain updates caused by us
                            std::lock_guard<std::mutex> lock{m_cs};
                            cancelSolver = false;
                        }

                        SetThreadPriority(THREAD_PRIORITY_LOWEST);

                        // In regression test mode, stop mining after a block is found.
                        if(chainparams.MineBlocksOnDemand())throw boost::thread_interrupted();
                        return true;
                    };

                std::function<bool(EhSolverCancelCheck)>
                    cancelled = [&m_cs, &cancelSolver](EhSolverCancelCheck pos)
                    {
                        std::lock_guard<std::mutex> lock{m_cs};
                        return cancelSolver;
                    };

                try
                {
                    // If we find a valid block, we rebuild
                    if(EhOptimisedSolve(n, k, curr_state, validBlock, cancelled))break;
                }
                catch(EhSolverCancelledException&)
                {
                    LogPrint("pow", "Equihash solver cancelled\n");
                    std::lock_guard<std::mutex> lock{m_cs};
                    cancelSolver = false;
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();

                // Regtest mode doesn't require peers
                if(vNodes.empty() && chainparams.MiningRequiresPeers())break;
                if((UintToArith256(pblock->nNonce) & 0xffff) == 0xffff)break;
                if(mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)break;
                if(pindexPrev != chainActive.Tip())break;

                // Update nNonce and nTime
                pblock->nNonce = ArithToUint256(UintToArith256(pblock->nNonce) + 1);
                UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);

                if(chainparams.GetConsensus().fPowAllowMinDifficultyBlocks)
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch(const boost::thread_interrupted&)
    {
        LogPrintf("ZcashMiner terminated\n");
        throw;
    }
    catch(const std::runtime_error &e)
    {
        LogPrintf("ZcashMiner runtime error: %s\n", e.what());
        return;
    }

    c.disconnect();
}

void GenerateBitcoins(bool fGenerate, CWallet* pwallet, int nThreads)
{
    static boost::thread_group* minerThreads = NULL;

    if(nThreads < 0)
    {
        // In regtest threads defaults to 1
        if(Params().DefaultMinerThreads())nThreads = Params().DefaultMinerThreads();
        else nThreads = boost::thread::hardware_concurrency();
    }

    if(minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if(nThreads == 0 || !fGenerate)return;

    minerThreads = new boost::thread_group();
    for(int i = 0; i < nThreads; i++)minerThreads->create_thread(boost::bind(&BitcoinMiner, pwallet));
}