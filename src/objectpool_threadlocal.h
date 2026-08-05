#pragma once

#ifndef MESSAGE_CENTER_OBJECTPOOL_THREADLOCAL_H
#define MESSAGE_CENTER_OBJECTPOOL_THREADLOCAL_H

#include <atomic>
#include <cstddef>
#include <vector>

// Thread-local object pool - zero contention on the hot path.
// Each thread gets its own free list. Objects are returned to the
// thread that allocated them (via thread ID stored in object).
//
// Tradeoff: Objects may accumulate on threads that allocate but don't release.
// Best for balanced producer/consumer workloads.

template <typename T>
class ObjectPoolThreadLocal {
  static constexpr size_t MAX_THREADS = 64;
  static constexpr size_t LOCAL_CACHE_SIZE = 256;

  struct alignas(64) ThreadCache {
    T* objects[LOCAL_CACHE_SIZE];
    size_t count = 0;
    std::atomic<size_t> allocated{0};
  };

  alignas(64) ThreadCache m_caches[MAX_THREADS];
  std::atomic<size_t> m_nextThreadId{0};

  static thread_local size_t t_threadId;
  static thread_local bool t_initialized;

  size_t getThreadId() {
    if (!t_initialized) {
      t_threadId = m_nextThreadId.fetch_add(1, std::memory_order_relaxed) % MAX_THREADS;
      t_initialized = true;
    }
    return t_threadId;
  }

public:
  ObjectPoolThreadLocal() = default;

  ~ObjectPoolThreadLocal() {
    for (size_t i = 0; i < MAX_THREADS; ++i) {
      ThreadCache& cache = m_caches[i];
      for (size_t j = 0; j < cache.count; ++j) {
        delete cache.objects[j];
      }
    }
  }

  ObjectPoolThreadLocal(const ObjectPoolThreadLocal&) = delete;
  ObjectPoolThreadLocal& operator=(const ObjectPoolThreadLocal&) = delete;

  T* acquire() {
    size_t tid = getThreadId();
    ThreadCache& cache = m_caches[tid];

    if (cache.count > 0) {
      return cache.objects[--cache.count];
    }

    cache.allocated.fetch_add(1, std::memory_order_relaxed);
    return new T();
  }

  void release(T* obj) {
    if (!obj) return;

    size_t tid = getThreadId();
    ThreadCache& cache = m_caches[tid];

    if (cache.count < LOCAL_CACHE_SIZE) {
      cache.objects[cache.count++] = obj;
    } else {
      delete obj;
      cache.allocated.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  size_t totalAllocated() const {
    size_t total = 0;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
      total += m_caches[i].allocated.load(std::memory_order_relaxed);
    }
    return total;
  }
};

template <typename T>
thread_local size_t ObjectPoolThreadLocal<T>::t_threadId = 0;

template <typename T>
thread_local bool ObjectPoolThreadLocal<T>::t_initialized = false;

#endif // MESSAGE_CENTER_OBJECTPOOL_THREADLOCAL_H
