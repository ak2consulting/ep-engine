/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 NorthScale, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#ifndef EP_HH
#define EP_HH 1

#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdexcept>
#include <iostream>
#include <queue>
#include <limits>
#include <unistd.h>

#include <set>
#include <list>
#include <queue>
#include <algorithm>

#include <memcached/engine.h>

extern EXTENSION_LOGGER_DESCRIPTOR *getLogger(void);

#include "queueditem.hh"
#include "stats.hh"
#include "locks.hh"
#include "sqlite-kvstore.hh"
#include "stored-value.hh"
#include "atomic.hh"
#include "dispatcher.hh"
#include "vbucket.hh"

#define DEFAULT_TXN_SIZE 250000
#define MAX_TXN_SIZE 10000000

#define MAX_DATA_AGE_PARAM 86400
#define MAX_BG_FETCH_DELAY 900

/**
 * vbucket-aware hashtable visitor.
 */
class VBucketVisitor : public HashTableVisitor {
public:

    VBucketVisitor() : HashTableVisitor() { }

    /**
     * Begin visiting a bucket.
     *
     * @param vbid the vbucket we are beginning to visit
     *
     * @return true iff we want to walk the hashtable in this vbucket
     */
    virtual bool visitBucket(RCPtr<VBucket> vb) {
        currentBucket = vb;
        return true;
    }

    // This is unused in all implementations so far.
    void visit(StoredValue* v) {
        (void)v;
        abort();
    }

protected:
    RCPtr<VBucket> currentBucket;
};

// Forward declaration
class Flusher;
class TapBGFetchCallback;
class EventuallyPersistentStore;

/**
 * Helper class used to insert items into the storage by using
 * the KVStore::dump method to load items from the database
 */
class LoadStorageKVPairCallback : public Callback<GetValue> {
public:
    LoadStorageKVPairCallback(VBucketMap &vb, EPStats &st,
                              EventuallyPersistentStore *ep)
        : vbuckets(vb), stats(st), epstore(ep), hasPurged(false) {
        assert(epstore);
    }

    void initVBucket(uint16_t vbid, uint16_t vb_version, vbucket_state_t state = dead);
    void callback(GetValue &val);

private:

    bool shouldBeResident() {
        return StoredValue::getCurrentSize(stats) < stats.mem_low_wat;
    }

    void purge();

    VBucketMap &vbuckets;
    EPStats    &stats;
    EventuallyPersistentStore *epstore;
    bool        hasPurged;
};

/**
 * Maintains scope of a underlying storage transaction, being useful
 * and what not.
 */
class TransactionContext {
public:

    TransactionContext(EPStats &st, StrategicSqlite3 *ss)
        : stats(st), underlying(ss), _remaining(0), intxn(false) {}

    /**
     * Call this whenever entering a transaction.
     *
     * This will (when necessary) begin the tranasaction and reset the
     * counter of remaining items for a transaction.
     *
     * @return true if we're in a transaction
     */
    bool enter();

    /**
     * Called whenever leaving, having completed the given number of
     * updates.
     *
     * When the number of updates completed exceeds the number
     * permitted per transaction, a transaction will be closed and
     * reopened.
     */
    void leave(int completed);

    /**
     * Explicitly commit a transaction.
     *
     * This will reset the remaining counter and begin a new
     * transaction for the next batch.
     */
    void commit();

    /**
     * Get the number of updates permitted by this transaction.
     */
    size_t remaining() {
        return _remaining;
    }

    /**
     * Request a commit occur at the next opportunity.
     */
    void commitSoon() {
        _remaining = 0;
    }

    /**
     * Get the current number of updates permitted per transaction.
     */
    int getTxnSize() {
        return txnSize.get();
    }

    /**
     * Set the current number of updates permitted per transaction.
     */
    void setTxnSize(int to) {
        txnSize.set(to);
    }

private:
    EPStats          &stats;
    StrategicSqlite3 *underlying;
    int               _remaining;
    Atomic<int>       txnSize;
    bool              intxn;
};

class EventuallyPersistentEngine;

class EventuallyPersistentStore {
public:

    EventuallyPersistentStore(EventuallyPersistentEngine &theEngine,
                              StrategicSqlite3 *t, bool startVb0);

    ~EventuallyPersistentStore();

    ENGINE_ERROR_CODE set(const Item &item,
                          const void *cookie,
                          bool force=false);

    ENGINE_ERROR_CODE add(const Item &item, const void *cookie);

    /**
     * Retrieve a value.
     *
     * @param key the key to fetch
     * @param vbucket the vbucket from which to retrieve the key
     * @param cookie the connection cookie
     * @param core the server API
     * @param queueBG if true, automatically queue a background fetch if necessary
     * @param honorStates if false, fetch a result regardless of state
     *
     * @return a GetValue representing the result of the request
     */
    GetValue get(const std::string &key, uint16_t vbucket,
                 const void *cookie, bool queueBG=true,
                 bool honorStates=true);

    /**
     * Retrieve an item from the disk for vkey stats
     *
     * @param key the key to fetch
     * @param vbucket the vbucket from which to retrieve the key
     * @param cookie the connection cookie
     * @param cb callback to return an item fetched from the disk
     *
     * @return a status resulting form executing the method
     */
    ENGINE_ERROR_CODE getFromUnderlying(const std::string &key,
                                        uint16_t vbucket,
                                        const void *cookie,
                                        shared_ptr<Callback<GetValue> > cb);

    protocol_binary_response_status evictKey(const std::string &key,
                                             uint16_t vbucket,
                                             const char **msg);

    ENGINE_ERROR_CODE del(const std::string &key, uint16_t vbucket,
                          const void *cookie);

    void reset();

    void setMinDataAge(int to);

    /**
     * Set the background fetch delay.
     *
     * This exists for debugging and testing purposes.  It
     * artificially injects delays into background fetches that are
     * performed when the user requests an item whose value is not
     * currently resident.
     *
     * @param to how long to delay before performing a bg fetch
     */
    void setBGFetchDelay(uint32_t to) {
        bgFetchDelay = to;
    }

    void setQueueAgeCap(int to);

    void startDispatcher(void);

    void startNonIODispatcher(void);

    /**
     * Get the current dispatcher.
     *
     * You can use this to queue io related jobs.  Don't do stupid things with
     * it.
     */
    Dispatcher* getDispatcher(void) {
        assert(dispatcher);
        return dispatcher;
    }

    /**
     * Get the current non-io dispatcher.
     *
     * Use this dispatcher to queue non-io jobs.
     */
    Dispatcher* getNonIODispatcher(void) {
        assert(nonIODispatcher);
        return nonIODispatcher;
    }

    void stopFlusher(void);

    void startFlusher(void);

    bool pauseFlusher(void);
    bool resumeFlusher(void);

    /**
     * Enqueue a background fetch for a key.
     *
     * @param the key to be bg fetched
     * @param vbucket the vbucket in which the key lives
     * @param cookie the cookie of the requestor
     */
    void bgFetch(const std::string &key,
                 uint16_t vbucket,
                 uint64_t rowid,
                 const void *cookie);

    /**
     * Complete a background fetch.
     *
     * @param key the key that was fetched
     * @param vbucket the vbucket in which the key lived
     * @param gv the result
     */
    void completeBGFetch(const std::string &key,
                         uint16_t vbucket,
                         uint64_t rowid,
                         const void *cookie,
                         hrtime_t init, hrtime_t start);

    RCPtr<VBucket> getVBucket(uint16_t vbid);

    void snapshotVBuckets(const Priority &priority);
    void setVBucketState(uint16_t vbid,
                         vbucket_state_t state);

    vbucket_del_result completeVBucketDeletion(uint16_t vbid, uint16_t vb_version,
                                               std::pair<int64_t, int64_t> row_range,
                                               bool isLastChunk);
    bool deleteVBucket(uint16_t vbid);

    void visit(VBucketVisitor &visitor) {
        size_t maxSize = vbuckets.getSize();
        for (size_t i = 0; i <= maxSize; ++i) {
            assert(i <= std::numeric_limits<uint16_t>::max());
            uint16_t vbid = static_cast<uint16_t>(i);
            RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
            if (vb) {
                bool wantData = visitor.visitBucket(vb);
                // We could've lost this along the way.
                if (wantData) {
                    vb->ht.visit(visitor);
                }
            }
        }
    }

    void warmup() {
        LoadStorageKVPairCallback cb(vbuckets, stats, this);
        std::map<std::pair<uint16_t, uint16_t>, std::string> state =
            underlying->listPersistedVbuckets();
        std::map<std::pair<uint16_t, uint16_t>, std::string>::iterator it;
        for (it = state.begin(); it != state.end(); ++it) {
            std::pair<uint16_t, uint16_t> vbp = it->first;
            getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                             "Reloading vbucket %d - was in %s state\n",
                             vbp.first, it->second.c_str());
            cb.initVBucket(vbp.first, vbp.second);
        }
        underlying->dump(cb);
    }

    int getTxnSize() {
        return tctx.getTxnSize();
    }

    void setTxnSize(int to) {
        tctx.setTxnSize(to);
    }

    const Flusher* getFlusher();

    bool getKeyStats(const std::string &key, uint16_t vbucket,
                     key_stats &kstats);

    bool getLocked(const std::string &key, uint16_t vbucket,
                   Callback<GetValue> &cb,
                   rel_time_t currentTime, uint32_t lockTimeout);

    StrategicSqlite3* getUnderlying() {
        // This method might also be called leakAbstraction()
        return underlying;
    }

    void deleteMany(std::list<std::pair<uint16_t, std::string> > &);

private:

    void scheduleVBSnapshot(const Priority &priority);
    void scheduleVBDeletion(RCPtr<VBucket> vb, uint16_t vb_version, double delay);

    RCPtr<VBucket> getVBucket(uint16_t vbid, vbucket_state_t wanted_state);

    /* Queue an item to be written to persistent layer. */
    void queueDirty(const std::string &key, uint16_t vbid, enum queue_operation op);

    /**
     * Retrieve a StoredValue and invoke a method on it.
     *
     * Note that because of complications with void/non-void methods
     * and potentially missing StoredValues along with the way I
     * actually intend to use this, I don't return any values from
     * this.
     *
     * @param key the item's key to retrieve
     * @param vbid the vbucket containing the item
     * @param f the method to invoke on the item (see std::mem_fun)
     * @param arg the argument to supply to the method f
     *
     * @return true if the object was found and method was invoked
     */
    template<typename A>
    bool invokeOnLockedStoredValue(const std::string &key, uint16_t vbid,
                                   std::mem_fun1_t<void, StoredValue, A> f,
                                   A arg) {
        RCPtr<VBucket> vb = getVBucket(vbid);
        if (!vb) {
            return false;
        }

        int bucket_num = vb->ht.bucket(key);
        LockHolder lh(vb->ht.getMutex(bucket_num));
        StoredValue *v = vb->ht.unlocked_find(key, bucket_num, true);

        if (v) {
            f(v, arg);
        }
        return v != NULL;
    }

    std::queue<QueuedItem> *beginFlush();
    void completeFlush(std::queue<QueuedItem> *rejects,
                       rel_time_t flush_start);

    int flushSome(std::queue<QueuedItem> *q,
                  std::queue<QueuedItem> *rejectQueue);
    int flushOne(std::queue<QueuedItem> *q,
                 std::queue<QueuedItem> *rejectQueue);
    int flushOneDeleteAll(void);
    int flushOneDelOrSet(QueuedItem &qi, std::queue<QueuedItem> *rejectQueue);

    StoredValue *fetchValidValue(RCPtr<VBucket> vb, const std::string &key,
                                 int bucket_num, bool wantsDeleted=false);

    friend class Flusher;
    friend class BGFetchCallback;
    friend class VKeyStatBGFetchCallback;
    friend class TapBGFetchCallback;
    friend class TapConnection;
    friend class PersistenceCallback;
    friend class Deleter;

    EventuallyPersistentEngine &engine;
    EPStats                    &stats;
    bool                       doPersistence;
    StrategicSqlite3          *underlying;
    Dispatcher                *dispatcher;
    Dispatcher                *nonIODispatcher;
    Flusher                   *flusher;
    VBucketMap                 vbuckets;
    SyncObject                 mutex;
    AtomicQueue<QueuedItem>    towrite;
    std::queue<QueuedItem>     writing;
    pthread_t                  thread;
    Atomic<size_t>             bgFetchQueue;
    TransactionContext         tctx;
    Mutex                      vbsetMutex;
    uint32_t                   bgFetchDelay;

    DISALLOW_COPY_AND_ASSIGN(EventuallyPersistentStore);
};

/**
 * Object whose existence maintains a counter incremented.
 *
 * When the object is constructed, it increments the given counter,
 * when destructed, it decrements the counter.
 */
class BGFetchCounter {
public:

    BGFetchCounter(Atomic<size_t> &c) : counter(c) {
        ++counter;
    }

    ~BGFetchCounter() {
        --counter;
        assert(counter.get() < GIGANTOR);
    }

private:
    Atomic<size_t> &counter;
};

#endif /* EP_HH */
