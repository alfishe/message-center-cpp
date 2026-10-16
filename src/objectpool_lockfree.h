#pragma once

#ifndef MESSAGE_CENTER_OBJECTPOOL_LOCKFREE_H
#define MESSAGE_CENTER_OBJECTPOOL_LOCKFREE_H

#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

// Lock-free object pool (single free-list, ABA-safe).
//
// Design goals (low-latency in-process messaging, e.g. emulator event buses):
// - Hot path (acquire/release) is one tagged 64-bit CAS on a single free-list head.
//   No thread_local lookups and no sharding: typical usage is either single-threaded
//   post+dispatch or one producer / one consumer, where sharding only adds misses.
// - ABA / use-after-free safety: the free-list stores 32-bit slot indices paired
//   with a 32-bit generation tag. Pooled slots are never freed while the pool lives,
//   so a stale reader can never touch released memory.
// - Lazy growth: slots are allocated in fixed-size chunks on demand (cold path,
//   guarded by a mutex), so constructing a pool with a large maxSize does not
//   commit memory up front.
// - Beyond maxSize, overflow objects are plain heap nodes deleted on release.

template <typename T> class ObjectPoolLockFree {
  static constexpr uint32_t INVALID_IDX = UINT32_MAX;
  static constexpr uint32_t CHUNK_SHIFT = 6; // 64 slots per chunk
  static constexpr uint32_t CHUNK_SIZE = 1u << CHUNK_SHIFT;
  static constexpr uint32_t CHUNK_MASK = CHUNK_SIZE - 1;

  struct Node {
    T data;
    std::atomic<uint32_t> nextIdx{INVALID_IDX};
    uint32_t slotIdx{INVALID_IDX};
  };

  // Read-only after construction: kept on a separate cache line from m_freeHead
  // so CAS updates on m_freeHead never invalidate m_base / m_baseSlots in L1.
  Node *m_base{nullptr};
  uint32_t m_baseSlots{0};
  size_t m_baseChunks{0};
  size_t m_numChunks;                                  // max chunk count (capacity / CHUNK_SIZE)
  std::unique_ptr<std::atomic<Node *>[]> m_chunks;    // chunk table, entries published once
  char m_padReadOnly[64 - ((sizeof(Node *) + sizeof(uint32_t) + sizeof(size_t) * 2 +
                            sizeof(std::unique_ptr<std::atomic<Node *>[]>)) & 63)];

  std::atomic<uint64_t> m_freeHead;                    // pack(tag, idx)
  char m_padFreeHead[64 - sizeof(std::atomic<uint64_t>)];

  std::atomic<size_t> m_allocated{0};
  size_t m_chunksUsed{0};                              // guarded by m_growMutex
  std::mutex m_growMutex;

  static inline uint64_t pack(uint32_t tag, uint32_t idx) {
    return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
  }
  static inline uint32_t unpackTag(uint64_t val) { return static_cast<uint32_t>(val >> 32); }
  static inline uint32_t unpackIdx(uint64_t val) { return static_cast<uint32_t>(val & 0xFFFFFFFFULL); }

  MC_FORCE_INLINE Node *slot(uint32_t idx) const {
    if (MC_LIKELY(idx < m_baseSlots))
      return m_base + idx;
    return m_chunks[idx >> CHUNK_SHIFT].load(std::memory_order_acquire) + (idx & CHUNK_MASK);
  }

public:
  explicit ObjectPoolLockFree(size_t initialSize = 0, size_t maxSize = 4096)
      : m_numChunks((std::max(initialSize, maxSize > 0 ? maxSize : size_t(4096)) + CHUNK_SIZE - 1) >>
                    CHUNK_SHIFT),
        m_chunks(new std::atomic<Node *>[m_numChunks]),
        m_freeHead(pack(0, INVALID_IDX)) {
    (void)m_padReadOnly;
    (void)m_padFreeHead;
    for (size_t i = 0; i < m_numChunks; ++i)
      m_chunks[i].store(nullptr, std::memory_order_relaxed);

    // Pre-warm initialSize slots (rounded up to whole chunks) as one contiguous block
    m_baseChunks = std::min((initialSize + CHUNK_SIZE - 1) >> CHUNK_SHIFT, m_numChunks);
    if (m_baseChunks > 0) {
      m_baseSlots = static_cast<uint32_t>(m_baseChunks * CHUNK_SIZE);
      m_base = new Node[m_baseSlots];
      for (uint32_t i = 0; i < m_baseSlots; ++i) {
        m_base[i].slotIdx = i;
        m_base[i].nextIdx.store((i + 1 < m_baseSlots) ? (i + 1) : INVALID_IDX,
                                std::memory_order_relaxed);
      }
      for (size_t c = 0; c < m_baseChunks; ++c)
        m_chunks[c].store(m_base + c * CHUNK_SIZE, std::memory_order_relaxed);
      m_chunksUsed = m_baseChunks;
      m_allocated.store(m_baseSlots, std::memory_order_relaxed);
      m_freeHead.store(pack(1, 0), std::memory_order_release);
    }
  }

  ~ObjectPoolLockFree() {
    for (size_t i = m_baseChunks; i < m_chunksUsed; ++i)
      delete[] m_chunks[i].load(std::memory_order_relaxed);
    delete[] m_base;
  }

  ObjectPoolLockFree(const ObjectPoolLockFree &) = delete;
  ObjectPoolLockFree &operator=(const ObjectPoolLockFree &) = delete;

  MC_FORCE_INLINE T *acquire() {
    Node *node = popFree();
    if (MC_LIKELY(node != nullptr))
      return &node->data;
    return acquireSlow();
  }

  // Batch acquire: pops up to 'count' pooled nodes from m_freeHead with a
  // single atomic CAS, falling back to acquire() if the pool needs to grow.
  MC_FORCE_INLINE void acquireBatch(T **outObjs, size_t count) {
    if (MC_UNLIKELY(count == 0))
      return;
    size_t got = popFreeBatch(outObjs, count);
    while (MC_UNLIKELY(got < count)) {
      outObjs[got++] = acquire();
    }
  }

  MC_FORCE_INLINE void release(T *obj) {
    if (MC_UNLIKELY(!obj))
      return;

    // T is the first member of Node
    Node *node = reinterpret_cast<Node *>(obj);
    if (MC_UNLIKELY(node->slotIdx == INVALID_IDX)) {
      delete node;
      m_allocated.fetch_sub(1, std::memory_order_relaxed);
    } else {
      pushFree(node, node->slotIdx);
    }
  }

  // Batch release: links all pooled nodes locally and returns the entire chain
  // to m_freeHead with a single atomic CAS (32x fewer CAS ops for 32-msg batches).
  MC_FORCE_INLINE void releaseBatch(T *const *objs, size_t count) {
    Node *firstNode = nullptr;
    Node *lastNode = nullptr;
    uint32_t firstIdx = INVALID_IDX;

    for (size_t i = 0; i < count; ++i) {
      T *obj = objs[i];
      if (MC_UNLIKELY(!obj))
        continue;
      Node *node = reinterpret_cast<Node *>(obj);
      uint32_t idx = node->slotIdx;
      if (MC_UNLIKELY(idx == INVALID_IDX)) {
        delete node;
        m_allocated.fetch_sub(1, std::memory_order_relaxed);
      } else {
        if (!firstNode) {
          firstNode = node;
          firstIdx = idx;
        } else {
          lastNode->nextIdx.store(idx, std::memory_order_relaxed);
        }
        lastNode = node;
      }
    }

    if (MC_LIKELY(firstNode != nullptr)) {
      pushFreeChain(firstIdx, lastNode);
    }
  }

  // Number of objects currently backed by memory (pooled slots + live overflow nodes)
  size_t allocated() const { return m_allocated.load(std::memory_order_relaxed); }

private:
  // Cold path: grow by one chunk, or fall back to a heap overflow node.
  MC_NOINLINE T *acquireSlow() {
    {
      std::lock_guard<std::mutex> lock(m_growMutex);
      // Another thread may have refilled the free-list while we waited
      Node *node = popFree();
      if (node)
        return &node->data;
      if (m_chunksUsed < m_numChunks)
        return &growLocked(true)->data;
    }

    Node *overflow = new Node();
    m_allocated.fetch_add(1, std::memory_order_relaxed);
    return &overflow->data;
  }

  // Allocates one chunk. If 'takeOne', returns its first slot to the caller and pushes the rest.
  MC_NOINLINE Node *growLocked(bool takeOne) {
    const uint32_t base = static_cast<uint32_t>(m_chunksUsed * CHUNK_SIZE);
    Node *chunk = new Node[CHUNK_SIZE];
    for (uint32_t i = 0; i < CHUNK_SIZE; ++i)
      chunk[i].slotIdx = base + i;

    m_chunks[m_chunksUsed].store(chunk, std::memory_order_release);
    ++m_chunksUsed;
    m_allocated.fetch_add(CHUNK_SIZE, std::memory_order_relaxed);

    // Link new chunk slots locally and publish with a single CAS on m_freeHead
    const uint32_t first = takeOne ? 1u : 0u;
    for (uint32_t i = first; i + 1 < CHUNK_SIZE; ++i)
      chunk[i].nextIdx.store(base + i + 1, std::memory_order_relaxed);
    pushFreeChain(base + first, &chunk[CHUNK_SIZE - 1]);

    return takeOne ? &chunk[0] : nullptr;
  }

  void pushFree(uint32_t idx) { pushFree(slot(idx), idx); }

  MC_FORCE_INLINE void pushFreeChain(uint32_t firstIdx, Node *lastNode) {
    uint64_t current = m_freeHead.load(std::memory_order_relaxed);
    for (;;) {
      lastNode->nextIdx.store(unpackIdx(current), std::memory_order_relaxed);
      if (MC_LIKELY(m_freeHead.compare_exchange_weak(
              current, pack(unpackTag(current) + 1, firstIdx),
              std::memory_order_release, std::memory_order_relaxed)))
        return;
    }
  }

  // Hot path variant: caller already holds the Node*, so no chunk-table lookup is needed
  MC_FORCE_INLINE void pushFree(Node *node, uint32_t idx) {
    uint64_t current = m_freeHead.load(std::memory_order_relaxed);
    for (;;) {
      node->nextIdx.store(unpackIdx(current), std::memory_order_relaxed);
      if (MC_LIKELY(m_freeHead.compare_exchange_weak(
              current, pack(unpackTag(current) + 1, idx),
              std::memory_order_release, std::memory_order_relaxed)))
        return;
    }
  }

  MC_FORCE_INLINE Node *popFree() {
    uint64_t current = m_freeHead.load(std::memory_order_acquire);
    for (;;) {
      uint32_t idx = unpackIdx(current);
      if (MC_UNLIKELY(idx == INVALID_IDX))
        return nullptr;
      Node *node = slot(idx);
      uint32_t nextIdx = node->nextIdx.load(std::memory_order_relaxed);
      if (MC_LIKELY(m_freeHead.compare_exchange_weak(
              current, pack(unpackTag(current) + 1, nextIdx),
              std::memory_order_acquire, std::memory_order_acquire)))
        return node;
    }
  }

  MC_FORCE_INLINE size_t popFreeBatch(T **outObjs, size_t maxCount) {
    uint64_t current = m_freeHead.load(std::memory_order_acquire);
    for (;;) {
      uint32_t idx = unpackIdx(current);
      if (MC_UNLIKELY(idx == INVALID_IDX))
        return 0;
      size_t n = 0;
      uint32_t walkIdx = idx;
      while (n < maxCount && walkIdx != INVALID_IDX) {
        Node *node = slot(walkIdx);
        outObjs[n++] = &node->data;
        walkIdx = node->nextIdx.load(std::memory_order_relaxed);
      }
      if (MC_LIKELY(m_freeHead.compare_exchange_weak(
              current, pack(unpackTag(current) + 1, walkIdx),
              std::memory_order_acquire, std::memory_order_acquire)))
        return n;
    }
  }
};

#endif // MESSAGE_CENTER_OBJECTPOOL_LOCKFREE_H
