#pragma once

#ifndef MESSAGE_CENTER_MPMC_QUEUE_H
#define MESSAGE_CENTER_MPMC_QUEUE_H

#include "platform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

// Bounded lock-free MPMC queue using demand-allocated Vyukov ring segments.
//
// Each slot has a sequence number that indicates its state:
//   seq == pos                : slot is ready for writing
//   seq == pos + 1            : slot contains valid data, ready for reading
//   seq == pos + segment_size : slot has been read, ready for next write cycle
//
// Coordinated low-memory / high-backlog design:
// - Normal operation (backlog < SEG_SIZE, up to 1024 messages) stays inside a
//   single 64 KB L2-cache-resident segment with zero allocations and zero locks.
// - When a large backlog builds up (e.g. while a consumer processes a larger
//   payload or during a producer burst), additional 64 KB segments are allocated
//   lazily on demand up to Capacity, and recycled once drained.

template <typename T, size_t Capacity> class MPMCQueue {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of 2");
  static_assert(Capacity >= 2, "Capacity must be at least 2");

  static constexpr size_t SEG_SIZE = (Capacity < 1024) ? Capacity : 1024;
  static constexpr size_t SEG_MASK = SEG_SIZE - 1;
  static constexpr size_t MAX_SEGS = Capacity / SEG_SIZE;
  static constexpr size_t RING_SIZE = (MAX_SEGS < 2) ? 2 : (MAX_SEGS * 2);
  static constexpr size_t RING_MASK = RING_SIZE - 1;
  static constexpr size_t SEALED_BIT = size_t(1) << (sizeof(size_t) * 8 - 2);

  static constexpr size_t SLOT_BODY_SIZE =
      sizeof(std::atomic<size_t>) + sizeof(T);
  static constexpr size_t SLOT_PAD_SIZE = 64 - (SLOT_BODY_SIZE & 63);

  struct Slot {
    std::atomic<size_t> seq;
    T data;
    char pad[SLOT_PAD_SIZE];
  };

  struct Segment {
    Slot m_slots[SEG_SIZE];
    std::atomic<size_t> m_head{0}; // next position to dequeue
    char m_padHead[64 - sizeof(std::atomic<size_t>)];
    std::atomic<size_t> m_tail{0}; // next position to enqueue (or SEALED_BIT)
    char m_padTail[64 - sizeof(std::atomic<size_t>)];
    void *m_rawMem{nullptr};

    Segment() {
      for (size_t i = 0; i < SEG_SIZE; ++i) {
        m_slots[i].seq.store(i, std::memory_order_relaxed);
      }
    }

    static Segment *create() {
      void *raw = ::operator new(sizeof(Segment) + 64);
      uintptr_t addr =
          (reinterpret_cast<uintptr_t>(raw) + 63u) & ~uintptr_t(63u);
      Segment *seg = new (reinterpret_cast<void *>(addr)) Segment();
      seg->m_rawMem = raw;
      return seg;
    }

    static void destroy(Segment *seg) {
      if (!seg)
        return;
      void *raw = seg->m_rawMem;
      seg->~Segment();
      ::operator delete(raw);
    }

    MC_FORCE_INLINE bool try_push(const T &item) {
      size_t tail = m_tail.load(std::memory_order_relaxed);

      for (;;) {
        Slot &slot = m_slots[tail & SEG_MASK];
        size_t seq = slot.seq.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail);

        if (MC_LIKELY(diff == 0)) {
          if (MC_LIKELY(m_tail.compare_exchange_weak(
                  tail, tail + 1, std::memory_order_relaxed))) {
            slot.data = item;
            slot.seq.store(tail + 1, std::memory_order_release);
            return true;
          }
        } else if (diff < 0) {
          return false;
        } else {
          tail = m_tail.load(std::memory_order_relaxed);
        }
      }
    }

    MC_FORCE_INLINE bool try_pop(T &item) {
      size_t head = m_head.load(std::memory_order_relaxed);

      for (;;) {
        Slot &slot = m_slots[head & SEG_MASK];
        size_t seq = slot.seq.load(std::memory_order_acquire);
        intptr_t diff =
            static_cast<intptr_t>(seq) - static_cast<intptr_t>(head + 1);

        if (MC_LIKELY(diff == 0)) {
          if (MC_LIKELY(m_head.compare_exchange_weak(
                  head, head + 1, std::memory_order_relaxed))) {
            item = slot.data;
            slot.seq.store(head + SEG_SIZE, std::memory_order_release);
            return true;
          }
        } else if (diff < 0) {
          return false;
        } else {
          head = m_head.load(std::memory_order_relaxed);
        }
      }
    }

    bool has_in_flight_write() const {
      size_t head = m_head.load(std::memory_order_acquire);
      size_t tail = m_tail.load(std::memory_order_acquire) & ~SEALED_BIT;
      return head < tail;
    }

    bool has_free_slot() const {
      size_t rawTail = m_tail.load(std::memory_order_acquire);
      if (rawTail & SEALED_BIT)
        return false;
      return (rawTail - m_head.load(std::memory_order_acquire)) < SEG_SIZE;
    }

    bool is_sealed() const {
      return (m_tail.load(std::memory_order_acquire) & SEALED_BIT) != 0;
    }

    size_t size_approx() const {
      size_t tail = m_tail.load(std::memory_order_relaxed) & ~SEALED_BIT;
      size_t head = m_head.load(std::memory_order_relaxed);
      return tail >= head ? tail - head : 0;
    }

    bool empty() const { return size_approx() == 0; }
  };

  std::atomic<Segment *> m_readSeg{nullptr};
  char m_padReadSeg[64 - sizeof(std::atomic<Segment *>)];
  std::atomic<Segment *> m_writeSeg{nullptr};
  char m_padWriteSeg[64 - sizeof(std::atomic<Segment *>)];
  std::atomic<size_t> m_readPos{0};
  std::atomic<size_t> m_writePos{0};
  std::atomic<size_t> m_allocatedSegs{1};

  mutable std::mutex m_segMutex;
  std::unique_ptr<Segment *[]> m_ring{new Segment *[RING_SIZE]()};
  std::unique_ptr<Segment *[]> m_freeSegs{new Segment *[RING_SIZE]()};
  size_t m_freeCount{0};

public:
  MPMCQueue() {
    Segment *initial = Segment::create();
    m_ring[0] = initial;
    m_readSeg.store(initial, std::memory_order_relaxed);
    m_writeSeg.store(initial, std::memory_order_relaxed);
  }

  ~MPMCQueue() {
    size_t rPos = m_readPos.load(std::memory_order_relaxed);
    size_t wPos = m_writePos.load(std::memory_order_relaxed);
    for (size_t p = rPos; p <= wPos; ++p) {
      Segment::destroy(m_ring[p & RING_MASK]);
    }
    for (size_t i = 0; i < m_freeCount; ++i) {
      Segment::destroy(m_freeSegs[i]);
    }
  }

  // Non-copyable, non-movable
  MPMCQueue(const MPMCQueue &) = delete;
  MPMCQueue &operator=(const MPMCQueue &) = delete;

  // Try to enqueue an item. Returns true on success, false if queue is full.
  MC_FORCE_INLINE bool try_push(const T &item) {
    Segment *seg = m_writeSeg.load(std::memory_order_acquire);
    if (MC_LIKELY(seg->try_push(item)))
      return true;
    return (MAX_SEGS > 1) ? try_push_slow(item) : false;
  }

  // Try to dequeue an item. Returns true on success, false if queue is empty.
  MC_FORCE_INLINE bool try_pop(T &item) {
    Segment *seg = m_readSeg.load(std::memory_order_acquire);
    if (MC_LIKELY(seg->try_pop(item)))
      return true;
    if (MC_LIKELY(MAX_SEGS == 1 || !seg->is_sealed()))
      return false;
    return try_pop_slow(item);
  }

  // Approximate size (may be stale)
  size_t size_approx() const {
    size_t rPos = m_readPos.load(std::memory_order_acquire);
    size_t wPos = m_writePos.load(std::memory_order_acquire);
    Segment *rSeg = m_readSeg.load(std::memory_order_acquire);
    if (rPos == wPos)
      return rSeg->size_approx();
    Segment *wSeg = m_writeSeg.load(std::memory_order_acquire);
    return rSeg->size_approx() + (wPos - rPos - 1) * SEG_SIZE +
           wSeg->size_approx();
  }

  bool empty() const { return size_approx() == 0; }

  bool full() const { return size_approx() >= Capacity; }

  static constexpr size_t capacity() { return Capacity; }
  static constexpr size_t segment_capacity() { return SEG_SIZE; }
  size_t allocated_segments() const {
    return m_allocatedSegs.load(std::memory_order_relaxed);
  }
  size_t allocated_bytes() const {
    return allocated_segments() * sizeof(Segment);
  }

private:
  MC_NOINLINE bool try_push_slow(const T &item) {
    std::lock_guard<std::mutex> lock(m_segMutex);
    for (;;) {
      Segment *seg = m_writeSeg.load(std::memory_order_relaxed);
      while (seg->has_free_slot()) {
        if (seg->try_push(item))
          return true;
      }
      if (seg->try_push(item))
        return true;

      size_t rPos = m_readPos.load(std::memory_order_acquire);
      size_t wPos = m_writePos.load(std::memory_order_relaxed);
      Segment *rSeg = m_ring[rPos & RING_MASK];
      size_t activeItems =
          (rPos == wPos) ? seg->size_approx()
                         : (rSeg->size_approx() + (wPos - rPos) * SEG_SIZE);
      if (activeItems >= Capacity || wPos - rPos >= MAX_SEGS)
        return false;

      // Seal current segment so no late producer can push into it out of order
      size_t tail = seg->m_tail.load(std::memory_order_relaxed);
      while ((tail & SEALED_BIT) == 0) {
        if (seg->m_tail.compare_exchange_weak(tail, tail | SEALED_BIT,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
          break;
        }
      }

      Segment *nextSeg = nullptr;
      if (m_freeCount > 0) {
        nextSeg = m_freeSegs[--m_freeCount];
        size_t nextTail =
            nextSeg->m_tail.load(std::memory_order_relaxed) & ~SEALED_BIT;
        nextSeg->m_tail.store(nextTail, std::memory_order_relaxed);
      } else {
        nextSeg = Segment::create();
        m_allocatedSegs.fetch_add(1, std::memory_order_relaxed);
      }

      nextSeg->try_push(item);
      ++wPos;
      m_ring[wPos & RING_MASK] = nextSeg;
      m_writePos.store(wPos, std::memory_order_release);
      m_writeSeg.store(nextSeg, std::memory_order_release);
      return true;
    }
  }

  MC_NOINLINE bool try_pop_slow(T &item) {
    for (;;) {
      Segment *seg = m_readSeg.load(std::memory_order_acquire);
      while (seg->has_in_flight_write()) {
        if (seg->try_pop(item))
          return true;
      }
      if (seg->try_pop(item))
        return true;
      if (!seg->is_sealed())
        return false;

      std::lock_guard<std::mutex> lock(m_segMutex);
      seg = m_readSeg.load(std::memory_order_relaxed);
      while (seg->has_in_flight_write()) {
        if (seg->try_pop(item))
          return true;
      }
      if (seg->try_pop(item))
        return true;
      if (!seg->is_sealed())
        return false;

      // Ensure any consumer that claimed one of the final slots in seg has
      // finished copying slot.data and storing head + SEG_SIZE.
      for (size_t i = 0; i < SEG_SIZE; ++i) {
        while ((seg->m_slots[i].seq.load(std::memory_order_acquire) &
                SEG_MASK) != i) {
          std::this_thread::yield();
        }
      }

      size_t rPos = m_readPos.load(std::memory_order_relaxed);
      m_ring[rPos & RING_MASK] = nullptr;
      m_freeSegs[m_freeCount++] = seg; // Retains SEALED_BIT while idle
      ++rPos;
      Segment *nextSeg = m_ring[rPos & RING_MASK];
      m_readPos.store(rPos, std::memory_order_release);
      m_readSeg.store(nextSeg, std::memory_order_release);
    }
  }
};

#endif // MESSAGE_CENTER_MPMC_QUEUE_H
