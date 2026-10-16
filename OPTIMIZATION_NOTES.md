# EventQueue & Lock-Free Architecture Optimization Notes

## Summary

This document summarizes the memory safety, concurrency, footprint, and cross-compiler optimizations integrated across all queue variants in `src/`:

- **Stack footprint reduced by >99.9%**: `sizeof` for `MessageCenterFast`, `EventQueueBroadcastFast`, `EventQueueBroadcastFastBatch`, `EventQueueBroadcast`, and `EventQueueEmulator` dropped from `1.3–5.1 MB` to **`0.30–0.75 KB` (`< 1 KB`)**.
- **Normal-work ring memory reduced by 98.4%**: `MPMCQueue<T, 65536>` starts with a single **1,024-slot (`64 KB`) segment** and lazily allocates/recycles additional segments only when queue depth exceeds 1,024 messages.
- **ABA-safe, coordinated `ObjectPoolLockFree`**: 64-bit tagged index head (`32-bit generation tag + 32-bit slot index`), lazy 64-slot slab growth, and capacity coordinated with `QueueCapacity`.
- **Cross-compiler throughput parity (`60–70M msg/s`)**: Hot/cold inlining annotations (`MC_FORCE_INLINE` / `MC_NOINLINE`) and single-observer struct assignment (`if (MC_LIKELY(count == 1))`) bring GCC 15, GCC 16, and Clang 21 to parity.
- **Anti-DoS for mixed payload sizes**: `EventQueueEmulator::post()` / `tryPost()` automatically falls back to the bulk queue when a critical topic posts a payload `> 48 B`, and `dispatchPriority(64)` bounds contiguous fast-lane drains so bulk messages are never starved.

---

## Modified & Added Components

### Core Headers (`src/`)
- `src/platform.h` — Cross-compiler `MC_FORCE_INLINE`, `MC_NOINLINE`, `MC_LIKELY`, `MC_UNLIKELY`, and `MC_CACHE_LINE` definitions.
- `src/mpmc_queue.h` — Segmented Vyukov MPMC ring buffer with 64-byte aligned `Segment::create()` / `Segment::destroy()`.
- `src/objectpool_lockfree.h` — Tagged-index lock-free slab pool with lazy 64-slot chunks and overflow reclamation.
- `src/payload_refcounted.h` — 16-byte aligned (`alignof(std::max_align_t)`) reference-counted payload with single-decrement message ownership.
- `src/eventqueue_fast.h`, `src/eventqueue_batch.h`, `src/eventqueue_emulator.h`, `src/eventqueue_broadcast.h`, `src/messagecenter_fast.h` — Lazy topic chunk tables, lock-free immutable observer snapshots, non-blocking `tryPost`, and compile-time checked `post<T>(topic, const T&)` / `tryPost<T>(topic, const T&)` helpers.
- `src/eventqueue.h`, `src/eventqueue.cpp`, `src/messagecenter.h`, `src/messagecenter.cpp` — Copy-on-write observer snapshots, active cancellation flags (`std::shared_ptr<std::atomic<bool>>`), `PostBatch`, `SetMaxQueueSize`, and deferred self-`Stop()` from observer callbacks.

---

## Verification Summary

- **74/74 GoogleTest unit & regression tests** pass across **GCC 15.2**, **GCC 16.1**, and **Clang 21.1** under `-std=c++14`, `-std=c++17`, and `-std=c++20` with `-Wall -Wextra -Wpedantic -Werror`.
- **0 errors / 0 leaks** under **AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer**, **ThreadSanitizer**, **Valgrind Memcheck**, and **LLVM libFuzzer**.

See [doc/TESTING_AND_BENCHMARKS.md](doc/TESTING_AND_BENCHMARKS.md) for full details and benchmark tables.
