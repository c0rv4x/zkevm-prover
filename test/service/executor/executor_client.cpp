
#include <nlohmann/json.hpp>
#include "executor_client.hpp"
#include "hashdb_singleton.hpp"
#include "zkmax.hpp"
#include "check_tree.hpp"
#include "state_manager_64.hpp"

using namespace std;
using json = nlohmann::json;

ExecutorClient::ExecutorClient (Goldilocks &fr, const Config &config) :
    fr(fr),
    config(config)
{
    // Set channel option to receive large messages
    grpc::ChannelArguments channelArguments;
    channelArguments.SetMaxReceiveMessageSize(1024*1024*1024);

    // Create channel
    std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(config.executorClientHost + ":" + to_string(config.executorClientPort), grpc::InsecureChannelCredentials(), channelArguments);

    // Create stub (i.e. client)
    stub = new executor::v1::ExecutorService::Stub(channel);
}

ExecutorClient::~ExecutorClient()
{
    delete stub;
}

void ExecutorClient::runThread (void)
{
    // Allow service to initialize
    sleep(1);

    pthread_create(&t, NULL, executorClientThread, this);
}

void ExecutorClient::waitForThread (void)
{
    pthread_join(t, NULL);
}

void ExecutorClient::runThreads (void)
{
    // Allow service to initialize
    sleep(1);

    for (uint64_t i=0; i<EXECUTOR_CLIENT_MULTITHREAD_N_THREADS; i++)
    {
        pthread_create(&threads[i], NULL, executorClientThreads, this);
    }
}

void ExecutorClient::waitForThreads (void)
{
    for (uint64_t i=0; i<EXECUTOR_CLIENT_MULTITHREAD_N_THREADS; i++)
    {
        pthread_join(threads[i], NULL);
    }
}

bool ExecutorClient::ProcessBatch (const string &inputFile)
{
    // Get a  HashDB interface
    HashDBInterface* pHashDB = HashDBClientFactory::createHashDBClient(fr, config);
    zkassertpermanent(pHashDB != NULL);

    TimerStart(EXECUTOR_CLIENT_PROCESS_BATCH);

    if (inputFile.size() == 0)
    {
        cerr << "Error: ExecutorClient::ProcessBatch() found inputFile empty" << endl;
        exit(-1);
    }

    Input input(fr);
    json inputJson;
    file2json(inputFile, inputJson);
    zkresult zkResult = input.load(inputJson);
    if (zkResult != ZKR_SUCCESS)
    {
        cerr << "Error: ProverClient::GenProof() failed calling input.load() zkResult=" << zkResult << "=" << zkresult2string(zkResult) << endl;
        exit(-1);
    }

    // Flags
    bool update_merkle_tree = true;
    bool get_keys = false;

    // Resulting new state root
    string newStateRoot;

    if (input.publicInputsExtended.publicInputs.forkID <= 6)
    {
        ::executor::v1::ProcessBatchRequest request;
        request.set_coinbase(Add0xIfMissing(input.publicInputsExtended.publicInputs.sequencerAddr.get_str(16)));
        request.set_batch_l2_data(input.publicInputsExtended.publicInputs.batchL2Data);
        request.set_old_state_root(scalar2ba(input.publicInputsExtended.publicInputs.oldStateRoot));
        request.set_old_acc_input_hash(scalar2ba(input.publicInputsExtended.publicInputs.oldAccInputHash));
        request.set_global_exit_root(scalar2ba(input.publicInputsExtended.publicInputs.globalExitRoot));
        request.set_eth_timestamp(input.publicInputsExtended.publicInputs.timestamp);
        request.set_update_merkle_tree(update_merkle_tree);
        request.set_chain_id(input.publicInputsExtended.publicInputs.chainID);
        request.set_fork_id(input.publicInputsExtended.publicInputs.forkID);
        request.set_from(input.from);
        request.set_no_counters(input.bNoCounters);
        if (input.traceConfig.bEnabled)
        {
            executor::v1::TraceConfig * pTraceConfig = request.mutable_trace_config();
            pTraceConfig->set_disable_storage(input.traceConfig.bDisableStorage);
            pTraceConfig->set_disable_stack(input.traceConfig.bDisableStack);
            pTraceConfig->set_enable_memory(input.traceConfig.bEnableMemory);
            pTraceConfig->set_enable_return_data(input.traceConfig.bEnableReturnData);
            pTraceConfig->set_tx_hash_to_generate_full_trace(string2ba(input.traceConfig.txHashToGenerateFullTrace));
        }
        request.set_old_batch_num(input.publicInputsExtended.publicInputs.oldBatchNum);

        // Parse keys map
        DatabaseMap::MTMap::const_iterator it;
        for (it=input.db.begin(); it!=input.db.end(); it++)
        {
            string key = NormalizeToNFormat(it->first, 64);
            string value;
            vector<Goldilocks::Element> dbValue = it->second;
            for (uint64_t i=0; i<dbValue.size(); i++)
            {
                value += NormalizeToNFormat(fr.toString(dbValue[i], 16), 16);
            }
            (*request.mutable_db())[key] = value;
        }

        // Parse contracts data
        DatabaseMap::ProgramMap::const_iterator itp;
        for (itp=input.contractsBytecode.begin(); itp!=input.contractsBytecode.end(); itp++)
        {
            string key = NormalizeToNFormat(itp->first, 64);
            string value;
            vector<uint8_t> contractValue = itp->second;
            for (uint64_t i=0; i<contractValue.size(); i++)
            {
                value += byte2string(contractValue[i]);
            }
            (*request.mutable_contracts_bytecode())[key] = value;
        }

        ::executor::v1::ProcessBatchResponse processBatchResponse;
        for (uint64_t i=0; i<config.executorClientLoops; i++)
        {
            if (i == 1)
            {
                request.clear_db();
                request.clear_contracts_bytecode();
            }
            ::grpc::ClientContext context;
            ::grpc::Status grpcStatus = stub->ProcessBatch(&context, request, &processBatchResponse);
            if (grpcStatus.error_code() != grpc::StatusCode::OK)
            {
                cerr << "Error: ExecutorClient::ProcessBatch() failed calling server i=" << i << " error=" << grpcStatus.error_code() << "=" << grpcStatus.error_message() << endl;
                return false;
            }
            if (processBatchResponse.error() != executor::v1::EXECUTOR_ERROR_NO_ERROR)
            {
                cerr << "Error: ExecutorClient::ProcessBatch() failed i=" << i << " error=" << processBatchResponse.error() << endl;
                return false;
            }
            newStateRoot = ba2string(processBatchResponse.new_state_root());

    #ifdef LOG_SERVICE
            cout << "ExecutorClient::ProcessBatch() got:\n" << response.DebugString() << endl;
    #endif
        }

        if (processBatchResponse.stored_flush_id() != processBatchResponse.flush_id())
        {
            executor::v1::GetFlushStatusResponse getFlushStatusResponse;
            do
            {
                usleep(10000);
                google::protobuf::Empty request;
                ::grpc::ClientContext context;
                ::grpc::Status grpcStatus = stub->GetFlushStatus(&context, request, &getFlushStatusResponse);
                if (grpcStatus.error_code() != grpc::StatusCode::OK)
                {
                    cerr << "Error: ExecutorClient::ProcessBatch() failed calling GetFlushStatus()" << endl;
                    break;
                }
            } while (getFlushStatusResponse.stored_flush_id() < processBatchResponse.flush_id());
            zklog.info("ExecutorClient::ProcessBatch() successfully stored returned flush id=" + to_string(processBatchResponse.flush_id()));
        }
    }
    else
    {
        ::executor::v1::ProcessBatchRequestV2 request;
        request.set_coinbase(Add0xIfMissing(input.publicInputsExtended.publicInputs.sequencerAddr.get_str(16)));
        request.set_batch_l2_data(input.publicInputsExtended.publicInputs.batchL2Data);
        request.set_old_state_root(scalar2ba(input.publicInputsExtended.publicInputs.oldStateRoot));
        request.set_old_acc_input_hash(scalar2ba(input.publicInputsExtended.publicInputs.oldAccInputHash));
        request.set_l1_info_root(scalar2ba(input.publicInputsExtended.publicInputs.l1InfoRoot));
        request.set_timestamp_limit(input.publicInputsExtended.publicInputs.timestampLimit);
        request.set_forced_blockhash_l1(scalar2ba(input.publicInputsExtended.publicInputs.forcedBlockHashL1));
        request.set_update_merkle_tree(update_merkle_tree);
        request.set_no_counters(input.bNoCounters);
        request.set_get_keys(get_keys);
        request.set_skip_verify_l1_info_root(input.bSkipVerifyL1InfoRoot);
        request.set_skip_first_change_l2_block(input.bSkipFirstChangeL2Block);
        request.set_chain_id(input.publicInputsExtended.publicInputs.chainID);
        request.set_fork_id(input.publicInputsExtended.publicInputs.forkID);
        request.set_from(input.from);
        request.set_skip_verify_l1_info_root(input.bSkipFirstChangeL2Block);
        unordered_map<uint64_t, L1Data>::const_iterator itL1Data;
        for (itL1Data = input.l1InfoTreeData.begin(); itL1Data != input.l1InfoTreeData.end(); itL1Data++)
        {
            executor::v1::L1DataV2 l1Data;
            l1Data.set_global_exit_root(string2ba(itL1Data->second.globalExitRoot.get_str(16)));
            l1Data.set_block_hash_l1(string2ba(itL1Data->second.blockHashL1.get_str(16)));
            l1Data.set_min_timestamp(itL1Data->second.minTimestamp);
            for (uint64_t i=0; i<itL1Data->second.smtProof.size(); i++)
            {
                l1Data.add_smt_proof(string2ba(itL1Data->second.smtProof[i].get_str(16)));
            }
            (*request.mutable_l1_info_tree_data())[itL1Data->first] = l1Data;
        }
        if (input.traceConfig.bEnabled)
        {
            executor::v1::TraceConfigV2 * pTraceConfig = request.mutable_trace_config();
            pTraceConfig->set_disable_storage(input.traceConfig.bDisableStorage);
            pTraceConfig->set_disable_stack(input.traceConfig.bDisableStack);
            pTraceConfig->set_enable_memory(input.traceConfig.bEnableMemory);
            pTraceConfig->set_enable_return_data(input.traceConfig.bEnableReturnData);
            pTraceConfig->set_tx_hash_to_generate_full_trace(string2ba(input.traceConfig.txHashToGenerateFullTrace));
        }
        request.set_old_batch_num(input.publicInputsExtended.publicInputs.oldBatchNum);

        // Parse keys map
        DatabaseMap::MTMap::const_iterator it;
        for (it=input.db.begin(); it!=input.db.end(); it++)
        {
            string key = NormalizeToNFormat(it->first, 64);
            string value;
            vector<Goldilocks::Element> dbValue = it->second;
            for (uint64_t i=0; i<dbValue.size(); i++)
            {
                value += NormalizeToNFormat(fr.toString(dbValue[i], 16), 16);
            }
            (*request.mutable_db())[key] = value;
        }

        // Parse contracts data
        DatabaseMap::ProgramMap::const_iterator itp;
        for (itp=input.contractsBytecode.begin(); itp!=input.contractsBytecode.end(); itp++)
        {
            string key = NormalizeToNFormat(itp->first, 64);
            string value;
            vector<uint8_t> contractValue = itp->second;
            for (uint64_t i=0; i<contractValue.size(); i++)
            {
                value += byte2string(contractValue[i]);
            }
            (*request.mutable_contracts_bytecode())[key] = value;
        }

        ::executor::v1::ProcessBatchResponseV2 processBatchResponse;
        for (uint64_t i=0; i<config.executorClientLoops; i++)
        {
            if (i == 1)
            {
                request.clear_db();
                request.clear_contracts_bytecode();
            }
            ::grpc::ClientContext context;
            ::grpc::Status grpcStatus = stub->ProcessBatchV2(&context, request, &processBatchResponse);
            if (grpcStatus.error_code() != grpc::StatusCode::OK)
            {
                cerr << "Error: ExecutorClient::ProcessBatch() failed calling server i=" << i << " error=" << grpcStatus.error_code() << "=" << grpcStatus.error_message() << endl;
                break;
            }
            if (processBatchResponse.error() != executor::v1::EXECUTOR_ERROR_NO_ERROR)
            {
                cerr << "Error: ExecutorClient::ProcessBatch() failed i=" << i << " error=" << processBatchResponse.error() << endl;
                return false;
            }
            newStateRoot = ba2string(processBatchResponse.new_state_root());

    #ifdef LOG_SERVICE
            cout << "ExecutorClient::ProcessBatch() got:\n" << response.DebugString() << endl;
    #endif
        }

        if (processBatchResponse.stored_flush_id() != processBatchResponse.flush_id())
        {
            executor::v1::GetFlushStatusResponse getFlushStatusResponse;
            do
            {
                usleep(10000);
                google::protobuf::Empty request;
                ::grpc::ClientContext context;
                ::grpc::Status grpcStatus = stub->GetFlushStatus(&context, request, &getFlushStatusResponse);
                if (grpcStatus.error_code() != grpc::StatusCode::OK)
                {
                    cerr << "Error: ExecutorClient::ProcessBatch() failed calling GetFlushStatus()" << endl;
                    break;
                }
            } while (getFlushStatusResponse.stored_flush_id() < processBatchResponse.flush_id());
            zklog.info("ExecutorClient::ProcessBatch() successfully stored returned flush id=" + to_string(processBatchResponse.flush_id()));
        }
    }

    if (input.publicInputsExtended.newStateRoot != 0)
    {
        mpz_class newStateRootScalar;
        newStateRootScalar.set_str(Remove0xIfPresent(newStateRoot), 16);
        if (input.publicInputsExtended.newStateRoot != newStateRootScalar)
        {
            zklog.error("ExecutorClient::ProcessBatch() returned newStateRoot=" + newStateRoot + " != input.publicInputsExtended.newStateRoot=" + input.publicInputsExtended.newStateRoot.get_str(16) + " inputFile=" + inputFile);
            return false;
        }
    }

    if (config.executorClientCheckNewStateRoot)
    {
        if (config.hashDB64)
        {            
            //if (StateManager64::isVirtualStateRoot(newStateRoot))
            {
                TimerStart(CONSOLIDATE_STATE);

                Goldilocks::Element virtualStateRoot[4];
                string2fea(fr, newStateRoot, virtualStateRoot);
                Goldilocks::Element consolidatedStateRoot[4];
                uint64_t flushId, storedFlushId;
                zkresult zkr = pHashDB->consolidateState(virtualStateRoot, update_merkle_tree ? PERSISTENCE_DATABASE : PERSISTENCE_CACHE, consolidatedStateRoot, flushId, storedFlushId);
                if (zkr != ZKR_SUCCESS)
                {
                    zklog.error("ExecutorClient::ProcessBatch() failed calling pHashDB->consolidateState() result=" + zkresult2string(zkr));
                    return false;
                }
                newStateRoot = fea2string(fr, consolidatedStateRoot);

                TimerStopAndLog(CONSOLIDATE_STATE);
            }
        }

        TimerStart(CHECK_NEW_STATE_ROOT);

        if (newStateRoot.size() == 0)
        {
            zklog.error("ExecutorClient::ProcessBatch() found newStateRoot emty");
            return false;
        }

        HashDB &hashDB = *hashDBSingleton.get();

        if (config.hashDB64)
        {
            Database64 &db = hashDB.db64;
            zkresult zkr = db.PrintTree(newStateRoot);
            if (zkr != ZKR_SUCCESS)
            {
                zklog.error("ExecutorClient::ProcessBatch() failed calling db.PrintTree() result=" + zkresult2string(zkr));
                return false;
            }
        }
        else
        {
            Database &db = hashDB.db;
            db.clearCache();

            CheckTreeCounters checkTreeCounters;

            zkresult result = CheckTree(db, newStateRoot, 0, checkTreeCounters, "");
            if (result != ZKR_SUCCESS)
            {
                zklog.error("ExecutorClient::ProcessBatch() failed calling CheckTree() result=" + zkresult2string(result));
                return false;
            }

            zklog.info("intermediateNodes=" + to_string(checkTreeCounters.intermediateNodes));
            zklog.info("leafNodes=" + to_string(checkTreeCounters.leafNodes));
            zklog.info("values=" + to_string(checkTreeCounters.values));
            zklog.info("maxLevel=" + to_string(checkTreeCounters.maxLevel));
        }

        TimerStopAndLog(CHECK_NEW_STATE_ROOT);

    }

    TimerStopAndLog(EXECUTOR_CLIENT_PROCESS_BATCH);

    return true;
}

bool ProcessDirectory (ExecutorClient *pClient, const string &directoryName)
{
    // Get files sorted alphabetically from the folder
    vector<string> files = getFolderFiles(directoryName, true);

    // Process each input file in order
    for (size_t i = 0; i < files.size(); i++)
    {
        string inputFile = directoryName + files[i];

        // Skip some files that we know are failing
        if ( false 
                //|| (files[i].find("pre-") == 0) // Arith equation fails
                //|| (files[i].find("ecpairing_one_point_not_in_subgroup_0.json") == 0) // Number of parameters 4 (should be 2) eval_ARITH_BN254_ADDFP2_X()
                // testvectors/inputs-executor/ethereum-tests/GeneralStateTests/stZeroKnowledge/ecpairing_one_point_not_in_subgroup_0.json
                || (files[i].find("test-length-data_1.json") == 0) // OOCB => new state root does not match
                || (files[i].find("header_timestamp_") == 0) // Index is "1" but rom index differs
                || (files[i].find("txs-different-batch_") == 0) // Index is "1" but rom index differs
                || (files[i].find("txs-same-batch.json") == 0) // Index is "1" but rom index differs
                || (files[i].find("ignore") != string::npos) // Ignore tests masked as such
                || (files[i].find("performanceTester_1.json") == 0) // SystemManager missing substate
                //|| (files[i].find("CallcodeToPrecompileFromCalledContract-custom.json") == 0)
                //|| (files[i].find("CallcodeToPrecompileFromTransaction-custom.json") == 0)
                //|| (files[i].find("DelegatecallToPrecompileFromCalledContract-custom.json") == 0)
                //|| (files[i].find("DelegatecallToPrecompileFromTransaction-custom.json") == 0)
                // testvectors/inputs-executor/ethereum-tests/GeneralStateTests/stStaticFlagEnabled/CallcodeToPrecompileFromCalledContract-custom.json  RR-1->RR leaves -1 (underflow)                
                // testvectors/inputs-executor/ethereum-tests/GeneralStateTests/stStaticFlagEnabled/CallcodeToPrecompileFromTransaction-custom.json
                // testvectors/inputs-executor/ethereum-tests/GeneralStateTests/stStaticFlagEnabled/DelegatecallToPrecompileFromCalledContract-custom.json
                // testvectors/inputs-executor/ethereum-tests/GeneralStateTests/stStaticFlagEnabled/DelegatecallToPrecompileFromTransaction-custom.json
            )
        {
            zklog.error("ProcessDirectory() skipping file i=" + to_string(i) + " file=" + inputFile);
            continue;
        }

        // Check file existence
        if (!fileExists(inputFile))
        {
            zklog.error("ProcessDirectory() found invalid file or directory with name=" + inputFile);
            exitProcess();
        }

        // If file is a directory, call recursively
        if (fileIsDirectory(inputFile))
        {
            bool bResult = ProcessDirectory(pClient, inputFile + "/");
            if (bResult == false)
            {
                return false;
            }
            continue;
        }

        // File exists and it is not a directory
        zklog.info("ProcessDirectory() i=" + to_string(i) + " inputFile=" + inputFile);
        bool bResult = pClient->ProcessBatch(inputFile);
        if (!bResult)
        {
            zklog.error("ProcessDirectory() failed i=" + to_string(i) + " inputFile=" + inputFile);
            return false;
        }
    }
    return true;
}

void* executorClientThread (void* arg)
{
    cout << "executorClientThread() started" << endl;
    string uuid;
    ExecutorClient *pClient = (ExecutorClient *)arg;
    
    // Execute should block and succeed
    cout << "executorClientThread() calling pClient->ProcessBatch()" << endl;

    if (config.inputFile.back() == '/')
    {
        ProcessDirectory(pClient, config.inputFile);
    }
    else
    {
        pClient->ProcessBatch(config.inputFile);
    }
    
    return NULL;
}

void* executorClientThreads (void* arg)
{
    //cout << "executorClientThreads() started" << endl;
    string uuid;
    ExecutorClient *pClient = (ExecutorClient *)arg;

    // Execute should block and succeed
    //cout << "executorClientThreads() calling pClient->ProcessBatch()" << endl;
    for(uint64_t i=0; i<EXECUTOR_CLIENT_MULTITHREAD_N_FILES; i++)
    {        
        if (config.inputFile.back() == '/')
        {
            // Get files sorted alphabetically from the folder
            vector<string> files = getFolderFiles(config.inputFile, true);
            // Process each input file in order
            for (size_t i = 0; i < files.size(); i++)
            {
                string inputFile = config.inputFile + files[i];
                zklog.info("executorClientThreads() inputFile=" + inputFile);
                bool bResult = pClient->ProcessBatch(inputFile);
                if (!bResult)
                {
                    zklog.error("executorClientThreads() failed i=" + to_string(i) + " inputFile=" + inputFile);
                    break;
                }
            }
        }
        else
        {
            pClient->ProcessBatch(config.inputFile);
        }
    }

    return NULL;
}