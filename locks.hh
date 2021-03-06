/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef LOCKS_H
#define LOCKS_H 1

#include <stdexcept>
#include <iostream>
#include <sstream>

#include "common.hh"
#include "mutex.hh"
#include "syncobject.hh"

/**
 * RAII lock holder to guarantee release of the lock.
 *
 * It is a very bad idea to unlock a lock held by a LockHolder without
 * using the LockHolder::unlock method.
 */
class LockHolder {
public:
    /**
     * Acquire the lock in the given mutex.
     */
    LockHolder(Mutex &m) : mutex(m), locked(false) {
        lock();
    }

    /**
     * Release the lock.
     */
    ~LockHolder() {
        unlock();
    }

    /**
     * Relock a lock that was manually unlocked.
     */
    void lock() {
        mutex.acquire();
        locked = true;
    }

    /**
     * Manually unlock a lock.
     */
    void unlock() {
        if (locked) {
            locked = false;
            mutex.release();
        }
    }

private:
    Mutex &mutex;
    bool locked;

    DISALLOW_COPY_AND_ASSIGN(LockHolder);
};

/**
 * RAII lock holder over multiple locks.
 */
class MultiLockHolder {
public:

    /**
     * Acquire a series of locks.
     *
     * @param m beginning of an array of locks
     * @param n the number of locks to lock
     */
    MultiLockHolder(Mutex *m, size_t n) : mutexes(m),
                                          locked(NULL),
                                          n_locks(n) {
        locked = new bool[n];
        lock();
    }

    ~MultiLockHolder() {
        unlock();
        delete[] locked;
    }

    /**
     * Relock the series after having manually unlocked it.
     */
    void lock() {
        for (size_t i = 0; i < n_locks; i++) {
            mutexes[i].acquire();
            locked[i] = true;
        }
    }

    /**
     * Manually unlock the series.
     */
    void unlock() {
        for (size_t i = 0; i < n_locks; i++) {
            if (locked[i]) {
                locked[i] = false;
                mutexes[i].release();
            }
        }
    }

private:
    Mutex  *mutexes;
    bool   *locked;
    size_t  n_locks;

    DISALLOW_COPY_AND_ASSIGN(MultiLockHolder);
};

#endif /* LOCKS_H */
