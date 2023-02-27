#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <vector>
#include <map>
#include <pqxx/pqxx>
#include "goldilocks_base_field.hpp"
#include "compare_fe.hpp"
#include "config.hpp"
#include <semaphore.h>
#include "zkresult.hpp"
#include "database_map.hpp"
#include "database_cache.hpp"

using namespace std;

class DatabaseMap;

class Database
{
private:
    Goldilocks &fr;
    const Config &config;
#ifdef DATABASE_COMMIT
    bool autoCommit = true;
#endif
    bool asyncWrite = false;
    bool bInitialized = false;
    bool useRemoteDB = false;
    bool useDBMTCache = false;
    bool useDBProgramCache = false;
    pthread_t writeThread;
    vector<string> writeQueue;
    pthread_mutex_t writeQueueMutex; // Mutex to protect writeQueue list
    pthread_cond_t writeQueueCond; // Cond to signal when queue has new items (no empty)
    pthread_cond_t emptyWriteQueueCond; // Cond to signal when queue is empty
    pqxx::connection * pConnectionWrite = NULL;
    pqxx::connection * pConnectionRead = NULL;
    pqxx::work* transaction = NULL;

    // Multi write attributes
    pthread_mutex_t mutex; // Mutex to protect the multi write queues
    void lock(void) { pthread_mutex_lock(&mutex); };
    void unlock(void) { pthread_mutex_unlock(&mutex); };
    string multiWriteProgram;
    string multiWriteNodes;

private:
    // Remote database based on Postgres (PostgreSQL)
    void initRemote(void);
    zkresult readRemote(bool bProgram, const string &key, string &value);
    zkresult writeRemote(bool bProgram, const string &key, const string &value);
    void addWriteQueue(const string sqlWrite);
    void signalEmptyWriteQueue() {};

public:
    static DatabaseMTCache dbMTCache;
    static DatabaseProgramCache dbProgramCache;

    Database(Goldilocks &fr, const Config &config) : fr(fr), config(config) {};
    ~Database();
    void init(void);
    zkresult read(const string &_key, vector<Goldilocks::Element> &value, DatabaseMap *dbReadLog);
    zkresult write(const string &_key, const vector<Goldilocks::Element> &value, const bool persistent);
    zkresult getProgram(const string &_key, vector<uint8_t> &value, DatabaseMap *dbReadLog);
    zkresult setProgram(const string &_key, const vector<uint8_t> &value, const bool persistent);
    void processWriteQueue();
#ifdef DATABASE_COMMIT
    void setAutoCommit(const bool autoCommit);
    void commit();
#endif
    void flush();
    void printTree(const string &root, string prefix = "");
};

void loadDb2MemCache(const Config config);

#endif