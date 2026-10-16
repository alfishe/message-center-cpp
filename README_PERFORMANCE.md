# Message Queue Performance & Architecture Overview

## Quick Start: Choosing an Implementation

All queue variants are integrated directly into `src/` and require zero external dependencies:

| Header | Class | Architecture | Single-Thread / Frame Burst | Stack Footprint | Best For |
|:-------|:------|:-------------|:----------------------------|:----------------|:---------|
| `src/eventqueue_emulator.h` | `EventQueueEmulator<N,M>` | Dual lock-free MPMC (Critical + Bulk) | **61.7–69.5 M/s** (`~15 ns`) | **0.56 KB** | Emulators, games, mixed-criticality workloads |
| `src/eventqueue_fast.h` | `EventQueueBroadcastFast<N>` | Lock-free MPMC + raw function pointers | **60.7–67.8 M/s** (`~15 ns`) | **0.30 KB** | General-purpose high throughput |
| `src/eventqueue_batch.h` | `EventQueueBroadcastFastBatch<N>` | Lock-free MPMC + batching | **39.2–42.2 M/s** (`~24 ns`) | **0.30 KB** | Lowest tail latency (P999) |
| `src/eventqueue_broadcast.h` | `EventQueueBroadcast<N>` | Lock-free MPMC + `std::function` | **16.5 M/s** (`~60 ns`) | **0.50 KB** | Capturing lambdas with lock-free queue |
| `src/messagecenter_fast.h` | `MessageCenterFast` | Lock-free MPMC + background worker | **24.6 M/s** (`~40 ns`, P50 `1.11 μs`) | **0.75 KB** | Async fire-and-forget + low latency |
| `src/messagecenter.h` | `MessageCenter` / `EventQueue` | Mutex + `std::deque` + background worker | **4.3–5.5 M/s** (`PostBatch`) | **0.65 KB** | App/UI events, `MessagePayload*` polymorphism |

---

## Key Highlights

1. **Coordinated Segmented Memory (`< 1 KB` Stack Frames)**:
   - `MPMCQueue` allocates **1,024-slot (`64 KB`) Vyukov ring segments on demand** instead of a monolithic `4 MB` array on the stack/heap.
   - `ObjectPoolLockFree` allocates **64-slot slabs lazily** and coordinates its pool ceiling (`QueueCapacity + headroom`) with the queue capacity so large backlogs never hit a heap allocation cliff.
2. **Typed & Non-Blocking Post APIs**:
   - Every lock-free queue supports both raw buffers (`post(topic, ptr, size)`) and compile-time checked typed values (`post(topic, value)` / `tryPost(topic, value)`), automatically storing payloads `≤ 48 B` inline and `> 48 B` in a reference-counted buffer.
   - Non-blocking `tryPost` (`tryPostFast` / `tryPostBulk`) returns `false` immediately when the queue is full instead of spinning.
3. **Cross-Compiler Code Generation**:
   - Hot/cold inlining partitioning (`MC_FORCE_INLINE` / `MC_NOINLINE`) and single-observer fast paths deliver consistent top-tier codegen across **GCC 15**, **GCC 16**, **Clang 21**, and **MSVC** under **C++11/14/17/20**.

---

## Documentation Links

- [Choosing a Queue](doc/CHOOSING_A_QUEUE.md) — Detailed comparison and decision guide
- [Testing, Benchmarking & Memory Safety](doc/TESTING_AND_BENCHMARKS.md) — Full test matrix, sanitizer/Valgrind verification, and cross-compiler benchmarks
- [Performance Analysis](doc/PERFORMANCE_ANALYSIS.md) — Multi-threaded latency percentiles and payload scaling
- [MessageCenter Optimization Notes](doc/MESSAGECENTER_OPTIMIZATION.md) — `MessageCenter` vs. `MessageCenterFast`
