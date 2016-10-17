// Copyright (c) 2016 Jack Grigg <jack@z.cash>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ZcashStratum.h"

#include "chainparams.h"
#include "clientversion.h"
#include "crypto/equihash.h"
#include "streams.h"
#include "version.h"
#include "../json/json_spirit.h"
#include "../primitives/block.h"
#include "../version.h"
#include "../utiltime.h"
#include "../arith_uint256.h"
#include "../serialize.h"
#include "../streams.h"
#include "../crypto/equihash.h"
#include "../uint256.h"
#include "../util.h"
#include "../chainparams.h"
#include "../utilstrencodings.h"

#include <atomic>
inline const char * const BoolToString(bool b)
{
    return b ? "true" : "false";
}

std::string string_to_hex(const std::string& input)
{
    static const char* const lut = "0123456789ABCDEF";
    size_t len = input.length();

    std::string output;
    output.reserve(2 * len);
    for (size_t i = 0; i < len; ++i)
    {
        const unsigned char c = input[i];
        output.push_back(lut[c >> 4]);
        output.push_back(lut[c & 15]);
    }
    return output;
}

void static ZcashMinerThread(ZcashMiner* miner, int size, int pos)
{
    LogPrintf("ZcashMinerThread started\n");
    RenameThread("zcash-miner");

    unsigned int n = Params().EquihashN();
    unsigned int k = Params().EquihashK();

    std::shared_ptr<std::mutex> m_zmt(new std::mutex);
    CBlockHeader header;
    arith_uint256 space;
    size_t offset;
    arith_uint256 inc;
    arith_uint256 target;
    std::atomic_bool workReady {false};
    std::atomic_bool cancelSolver {false};

    miner->NewJob.connect(NewJob_t::slot_type(
        [&m_zmt, &header, &space, &offset, &inc, &target, &workReady, &cancelSolver](const ZcashJob* job) mutable
        {
            std::lock_guard<std::mutex> lock{*m_zmt.get()};
            if(job)
            {
                header = job->header;
                space = job->nonce2Space;
                offset = job->nonce1Size * 4; // Hex length to bit length
                inc = job->nonce2Inc;
                target = job->serverTarget;
                workReady.store(true);
                if(job->clean)cancelSolver.store(true);
            }
            else
            {
                workReady.store(false);
                cancelSolver.store(true);
            }
        }
    ).track_foreign(m_zmt)); // So the signal disconnects when the mining thread exits

    try
    {
        while(true)
        {
            // Wait for work
            bool expected;
            do
            {
                expected = true;
                boost::this_thread::interruption_point();
                MilliSleep(1000);
            }
            while(!workReady.compare_exchange_weak(expected, false));

            // TODO change atomically with workReady
            cancelSolver.store(false);

            // Calculate nonce limits
            arith_uint256 nonce;
            arith_uint256 nonceEnd;
            {
                std::lock_guard<std::mutex> lock{*m_zmt.get()};
                arith_uint256 baseNonce = UintToArith256(header.nNonce);
                nonce = baseNonce + ((space/size)*pos << offset);
                nonceEnd = baseNonce + ((space/size)*(pos+1) << offset);
            }

            // Hash state
            crypto_generichash_blake2b_state state;
            EhInitialiseState(n, k, state);

            // I = the block header minus nonce and solution.
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            {
                std::lock_guard<std::mutex> lock{*m_zmt.get()};
                CEquihashInput I{header};
                ss << I;
            }

            // H(I||...
            crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());

            // Start working
            while(true)
            {
                // H(I||V||...
                crypto_generichash_blake2b_state curr_state;
                curr_state = state;
                auto bNonce = ArithToUint256(nonce);
                crypto_generichash_blake2b_update(&curr_state, bNonce.begin(), bNonce.size());

                // (x_1, x_2, ...) = A(I, V, n, k)
                LogPrint("pow", "Running Equihash solver with nNonce = %s\n", nonce.ToString());

                std::function<bool(std::vector<unsigned char>)>
                validBlock = [&m_zmt, &header, &bNonce, &target, &miner](std::vector<unsigned char> soln)
                {
                    std::lock_guard<std::mutex> lock{*m_zmt.get()};
                    // Write the solution to the hash and compute the result.
                    LogPrint("pow", "- Checking solution against target...");
                    header.nNonce = bNonce;
                    header.nSolution = soln;

                    if(UintToArith256(header.GetHash()) > target)
                    {
                        LogPrint("pow", " too large.\n");
                        return false;
                    }

                    // Found a solution
                    LogPrintf("Found solution satisfying the server target\n");
                    EquihashSolution solution {bNonce, soln};
                    solution.time = header.nTime;
                    miner->submitSolution(solution);

                    // We're a pooled miner, so try all solutions
                    return false;
                };

                std::function<bool(EhSolverCancelCheck)> cancelled = [&cancelSolver](EhSolverCancelCheck pos)
                {
                    boost::this_thread::interruption_point();
                    return cancelSolver.load();
                };

                try
                {
                    // If we find a valid block, we get more work
                    if(EhOptimisedSolve(n, k, curr_state, validBlock, cancelled))break;
                }
                catch(EhSolverCancelledException&)
                {
                    LogPrint("pow", "Equihash solver cancelled\n");
                    cancelSolver.store(false);
                    break;
                }

                // Check for stop
                boost::this_thread::interruption_point();
                if(nonce == nonceEnd)break;

                // Check for new work
                if(workReady.load())
                {
                    LogPrint("pow", "New work received, dropping current work\n");
                    break;
                }

                // Update nonce
                nonce += inc;
            }
        }
    }
    catch(const boost::thread_interrupted&)
    {
        LogPrintf("ZcashMinerThread terminated\n");
        throw;
    }
    catch(const std::runtime_error &e)
    {
        LogPrintf("ZcashMinerThread runtime error: %s\n", e.what());
        return;
    }
}

ZcashJob* ZcashJob::clone() const
{
    ZcashJob* ret = new ZcashJob();
    ret->job = job;
    ret->header = header;
    ret->time = time;
    ret->nonce1Size = nonce1Size;
    ret->nonce2Space = nonce2Space;
    ret->nonce2Inc = nonce2Inc;
    ret->serverTarget = serverTarget;
    ret->clean = clean;
    return ret;
}

ZcashJob* ZcashMiner::parseJob(const Array& params)
{
    if(params.size() < 2)throw std::logic_error("Invalid job params");

    ZcashJob* ret = new ZcashJob();
    ret->job = params[0].get_str();

    // Output Parameters
    std::cout << "KekMiner::parseJob(): Parameters (" << sizeof(params) << " Total)\n";
    std::cout << "Block Header Version: " << ret->header.nVersion << "\n";
    std::cout << "Block Header Bits: " << ret->header.nBits << "\n";
    for(int i = 0;i < 8;i++)
    {
        if(i == 7)
        {
            std::cout << "param[" << i << "](" << typeid(params[i]).name() << "):" << BoolToString(params[i].get_bool()) << "\n";
            continue;
        }
        std::string val = params[i].get_str().c_str();
        val = val.data();
        std::cout << "param[" << i << "](" << typeid(val).name() << "):" << val << "\n";
    }

    std::cout << "Parameter Output Complete\n";

    uint32_t version;
    sscanf(params[1].get_str().c_str(), "%x", &version);
    std::cout << "Block Header Version (Scanned): " << version << "\n";

    uint32_t time;
    sscanf(params[5].get_str().c_str(), "%x", &time);
    std::cout << "Block Header Time (Scanned): " << time << "\n";

    ret->header.nTime = (uint32_t)GetTime();
    ret->header.nVersion = version;

    if(ret->header.nVersion == 4)
    {
        if(params.size() < 8)throw std::logic_error("Invalid job params");

        std::stringstream ssHeader;
        ssHeader << params[1].get_str()
                 << params[2].get_str()
                 << params[3].get_str()
                 << params[4].get_str()
                 << params[5].get_str()
                 << params[6].get_str()
                 // Empty nonce
                 << "0000000000000000000000000000000000000000000000000000000000000000"
                 << "00"; // Empty solution

        std::cout << "ssHeader:stringstream assigned...\n";

        auto strHexHeader = ssHeader.str();
        std::vector<unsigned char> headerData(ParseHex(strHexHeader));
        CDataStream ss(headerData, SER_NETWORK, PROTOCOL_VERSION);

        std::cout << "ss:CDataStream assigned...\n";
        try
        {
            ss >> ret->header;
        }
        catch(const std::ios_base::failure& _e)
        {
            throw std::logic_error("KekMiner::parseJob(): Invalid block header parameters");
        }

        ret->time = params[5].get_str().c_str();
        ret->time_raw = params[5].get_str();
        std::cout << "Block->time assigned...\n";

        ret->clean = params[7].get_bool();
        std::cout << "Block->clean assigned...\n";
    }
    else throw std::logic_error("KekMiner::parseJob(): Invalid or unsupported block header version");

    ret->header.nNonce = nonce1;
    ret->nonce1Size = nonce1Size;
    ret->nonce2Space = nonce2Space;
    ret->nonce2Inc = nonce2Inc;
    return ret;
}

void ZcashMiner::start()
{
    if(minerThreads)stop();
    if(nThreads == 0)return;

    minerThreads = new boost::thread_group();
    for(int i = 0; i < nThreads; i++)minerThreads->create_thread(boost::bind(&ZcashMinerThread, this, nThreads, i));
}

void ZcashMiner::stop()
{
    if(minerThreads)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = nullptr;
    }
}

void ZcashMiner::submitSolution(const EquihashSolution& solution)
{
    solutionFoundCallback(solution);
}

void ZcashMiner::setServerNonce(const Array& params)
{
    auto n1str = params[1].get_str();
    auto n2size = params[2].get_int();
    std::vector<unsigned char> nonceData(ParseHex(n1str));
    while(nonceData.size() < 32)nonceData.push_back(0);
    CDataStream ss(nonceData, SER_NETWORK, PROTOCOL_VERSION);
    ss >> nonce1;

    nonce1Size = n1str.size();
    size_t nonce1Bits = nonce1Size * 4; // Hex length to bit length
    size_t nonce2Bits = 256 - nonce1Bits;

    nonce2Space = 1;
    nonce2Space <<= nonce2Bits;
    nonce2Space -= 1;

    nonce2Inc = 1;
    nonce2Inc <<= nonce1Bits;
}

void ZcashJob::setTarget(std::string target)
{
    if(target.size() > 0)serverTarget = UintToArith256(uint256S(target));
    else
    {
        LogPrint("stratum", "New job but no server target, assuming powLimit\n");
        serverTarget = UintToArith256(Params().GetConsensus().powLimit);
    }
}

bool ZcashJob::evalSolution(const EquihashSolution* solution)
{
    unsigned int n = Params().EquihashN();
    unsigned int k = Params().EquihashK();

    // Hash state
    crypto_generichash_blake2b_state state;
    EhInitialiseState(n, k, state);

    // I = the block header minus nonce and solution.
    CEquihashInput I{header};
    // I||V
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << I;
    ss << solution->nonce;

    // H(I||V||...
    crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());

    bool isValid;
    EhIsValidSolution(n, k, state, solution->solution, isValid);
    return isValid;
}

std::string ZcashJob::getSubmission(const EquihashSolution* solution)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << solution->nonce;
    ss << solution->solution;
    std::string strHex = HexStr(ss.begin(), ss.end());

    std::stringstream s;
    s << time_raw;

    std::stringstream stream;
    stream << "\"" << job;
    stream << "\",\"" << std::hex << strHex.substr(nonce1Size, 16-nonce1Size);
    stream << "\",\"" << string_to_hex(s.str());
    stream << "\",\"" << std::hex << strHex.substr(0, 8-nonce1Size);
    stream << "\"";
    return stream.str();
}

ZcashMiner::ZcashMiner(int threads):nThreads{threads}, minerThreads{nullptr}
{
    if(nThreads < 0)nThreads = boost::thread::hardware_concurrency();
}

void ZcashMiner::setJob(ZcashJob* job)
{
    NewJob(job);
}

void ZcashMiner::onSolutionFound(const std::function<bool(const EquihashSolution&)> callback)
{
    solutionFoundCallback = callback;
}

void ZcashMiner::acceptedSolution(bool stale)
{
}

void ZcashMiner::rejectedSolution(bool stale)
{
}

void ZcashMiner::failedSolution()
{
}

std::string ZcashMiner::userAgent()
{
    return FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<std::string>());
}