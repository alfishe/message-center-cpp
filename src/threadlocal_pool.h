#pragma once

#ifndef MESSAGE_CENTER_THREADLOCAL_POOL_H
#define MESSAGE_CENTER_THREADLOCAL_POOL_H

#include "objectpool.h"
#include <atomic>
#include <vector>

/// region <Thread-Local Object Pool>

/// Thread-local object pool with global fallback
/// Each thread maintains its own pool (no mutex contention!)
/// Falls back to global pool when local pool is empty
///
/// Performance benefits:
/// - No mutex for local pool operations (thread-local storage)
/// - Scales linearly with thread count
/// - Global pool only used for cross-thread transfers
///
template <typename T> class ThreadLocalObjectPool {
private:
  // Global pool for cross-thread sharing (has mutex)
  static ObjectPool<T> s_globalPool;

  // Per-thread pool (no mutex needed!)
  thread_local static std::vector<T *> t_localPool;
  thread_local static size_t t_localAllocated;

  // Configuration
  static constexpr size_t LOCAL_POOL_MAX_SIZE = 256;
  static constexpr size_t GLOBAL_POOL_MAX_SIZE = 2048;

public:
  /// Acquire an object from the pool
  /// Try local pool first (fast, no lock), then global pool (slower, has lock)
  static T *acquire() {
    // Try local pool first (no lock!)
    if (!t_localPool.empty()) {
      T *obj = t_localPool.back();
      t_localPool.pop_back();
      return obj;
    }

    // Local pool empty - try global pool
    T *obj = s_globalPool.acquire();
    if (obj) {
      return obj;
    }

    // Both pools empty - allocate new
    t_localAllocated++;
    return new T();
  }

  /// Return an object to the pool
  /// Return to local pool if there's room, otherwise to global pool
  static void release(T *obj) {
    if (obj == nullptr)
      return;

    // Try to return to local pool first (no lock!)
    if (t_localPool.size() < LOCAL_POOL_MAX_SIZE) {
      t_localPool.push_back(obj);
      return;
    }

    // Local pool full - return to global pool
    s_globalPool.release(obj);
  }

  /// Get local pool size (for debugging)
  static size_t getLocalPoolSize() { return t_localPool.size(); }

  /// Get local allocated count (for debugging)
  static size_t getLocalAllocated() { return t_localAllocated; }

  /// Get global pool size (for debugging)
  static size_t getGlobalPoolSize() { return s_globalPool.size(); }

  /// Get global allocated count (for debugging)
  static size_t getGlobalAllocated() { return s_globalPool.allocated(); }

  /// Clear all pools (for testing)
  static void clearAll() {
    // Clear local pool
    for (T *obj : t_localPool) {
      delete obj;
    }
    t_localPool.clear();
    t_localAllocated = 0;

    // Clear global pool
    s_globalPool.clear();
  }
};

// Static member initialization
template <typename T>
ObjectPool<T> ThreadLocalObjectPool<T>::s_globalPool(
    0, ThreadLocalObjectPool<T>::GLOBAL_POOL_MAX_SIZE);

template <typename T>
thread_local std::vector<T *> ThreadLocalObjectPool<T>::t_localPool;

template <typename T>
thread_local size_t ThreadLocalObjectPool<T>::t_localAllocated = 0;

/// endregion </Thread-Local Object Pool>

#endif // MESSAGE_CENTER_THREADLOCAL_POOL_H
