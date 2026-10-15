# Testing, Benchmarking, and Memory Safety Guide

## Overview

The `message-center-cpp` project provides multiple publish-subscribe and event queue implementations in C++:

1. **`EventQueue` / `MessageCenter`**: Classic mutex-and-condition-variable architecture. Supports dynamic string-to-ID topic resolution, multiple observer callback signatures (function pointers, class methods, and capturing `std::function` lambdas), and an asynchronous background worker thread.
2. **`MessageCenterFast`**: Hybrid architecture pairing a bounded lock-free MPMC queue (`MPMCQueue`) with an `ObjectPoolLockFree` message pool and adaptive worker thread.
3. **`EventQueueBroadcastFast`**: Zero-allocation inline/ref-counted broadcast queue with function pointer callbacks and pre-snapshotted observer arrays.
4. **`EventQueueBroadcastFastBatch`**: Batched variant grouping atomic operations for reduced tail latency.
5. **`EventQueueEmulator`**: Dual-queue priority architecture (Critical inline vs Normal bulk) designed for emulator and real-time gaming workloads.

---

## Test Suites

The test suite is built on GoogleTest and organized into modular files:

| Test File | Target Architecture | Description |
| :--- | :--- | :--- |
| `tests/eventqueue_test.cpp` | `EventQueue` | Topic resolution capacity, lookup correctness, callback/method/lambda observers, and ID-based removal. |
| `tests/messagecenter_test.cpp` | `MessageCenter` | Thread lifecycle (`Start`, `Stop`, autostart), queued processing, disposal safety. |
| `tests/messagecenter_fast_test.cpp` | `MessageCenterFast` | Post/receive delivery, payload integrity, multiple observers, throughput, idle CPU behavior. |
| `tests/memory_leak_test.cpp` | All Variants | Payload destruction verification, multi-threaded leaks, queue overflow behavior. |
| `tests/memory_safety_uaf_test.cpp` | All Variants | Use-After-Free (UAF), double-free prevention, re-entrant self and peer unsubscription, re-entrant posting, refcounted payload lifecycle, and destruction ordering. |
| `tests/concurrency_stress_test.cpp` | Multi-threaded | High-load multi-producer contention, dynamic observer addition/removal during active dispatches, bounded MPMC queue wrap-around saturation, and ObjectPool lock-free thread safety. |
| `tests/fuzz_regression_test.cpp` | All Variants | Deterministic pseudo-random state machine fuzzing across EventQueue, Broadcast, Fast, Batch, Emulator, and MessageCenterFast, boundary tests, and `ClearTopics` reset behavior. |
| `tests/rework_regression_test.cpp` | Pool, `MessageCenter`, `MessageCenterFast` | Lazy pool growth (no up-front commit), overflow accounting, producer→consumer pool hand-off, deferred `stop()` from inside a callback, and no lost wake-ups for a sleeping `MessageCenterFast` worker. |
| `tests/fuzz/message_center_fuzzer.cpp` | All Variants | Multi-flavor continuous libFuzzer harness with inline/managed payloads, priority routing, and batching. |

---

## Stabilized Architecture & Bug Fixes

### 1. `ObjectPoolLockFree` ABA & Memory Safety Resolution
- **Issue**: The original raw-pointer Treiber stack suffered from the classic ABA race on `head->next` and double-free crashes (`free(): double free detected in tcache 2`) under concurrent producer/consumer loads.
- **Resolution**: Replaced with a single-free-list, index-based pool whose head is a 64-bit tagged word (32-bit generation tag + 32-bit slot index) updated by CAS. Slots are never freed while the pool lives (a stale reader can never touch released memory), and the tag defeats ABA. Slots are allocated lazily in 64-slot chunks (`initialSize` pre-warmed as one contiguous block for a cheap index→pointer lookup); requests beyond `maxSize` fall back to heap overflow nodes that are deleted on release. No `thread_local`, no sharding — see [Review-Driven Rework](#review-driven-rework-emulator-workloads).

### 2. Peer Unsubscription & Use-After-Free in `EventQueue`
- **Issue**: When Observer A removed Observer B from within its callback during `Dispatch()`, Observer B was already present in `observersSnapshot`. If Observer B's owning object was deleted immediately upon removal, Observer B's subsequent callback execution caused a heap-use-after-free (UAF).
- **Resolution**: Added `std::shared_ptr<std::atomic<bool>> active` to `ObserverDescriptor`. On any `RemoveObserver*` call, `active->store(false, std::memory_order_release)` is invoked immediately. During `Dispatch()`, `if (observer.active && !observer.active->load(std::memory_order_acquire)) continue;` ensures cancelled observers are never called.

### 3. Cross-Queue Re-Entrancy Scoping
- **Issue**: A global `thread_local bool tl_inDispatch` broke nested dispatches or callbacks interacting with secondary `EventQueue` instances.
- **Resolution**: Replaced with `thread_local std::vector<const EventQueue*> tl_activeQueues`, accurately scoping the active dispatch wait avoidance to the specific `EventQueue` instance.

### 4. Memory Leaks in `EventQueue::dispose()`
- **Issue**: `AddObserver()` allocated `new ObserversVector()` which was never deleted in `dispose()`. Undispatched messages with `cleanupPayload = true` were also leaked.
- **Resolution**: `dispose()` unconditionally locks `m_mutexMessages` and `m_mutexObservers`, deletes any queued `message->obj` when `cleanupPayload` is true, deletes each `observerDescriptor`, and calls `delete it.second` for each topic's `ObserversVector`.

### 5. Deadlock Prevention in `MessageCenter` and `MessageCenterFast`
- **Issue**: Calling `Stop()` or destroying `MessageCenter` from within a callback worker thread caused `EDEADLK` because `m_thread->join()` was called on the calling thread itself. In `MessageCenterFast`, `dispatch()` held `m_observerMutex` during callback execution, causing deadlocks if a callback called `addObserver()`.
- **Resolution**:
  - `MessageCenter::Stop()` / `MessageCenterFast::stop()` called on the worker thread (from an observer callback) only *requests* the stop: the worker exits after the current callback and is joined by the next `Stop()`/destructor call from another thread. No `detach()` is used.
  - Destroying the instance from its own observer callback is unsupported and asserts in debug builds (a detached worker would otherwise keep running on a freed `this`).
  - In `MessageCenterFast::dispatch()`, observers are taken from a copy-on-write snapshot under `m_observerMutex`, and the lock is released before invoking callbacks.
  - Added unconsumed queue draining to `~MessageCenterFast()`.

### 6. RefCountedPayload Double-Free in Broadcast Flavors
- **Issue**: In `EventQueueBroadcast`, `post()` calculated `addRefs(count - 1)` from the observer count at post time, while `dispatchOne()` called `release()` once per observer in the *live* observer list. Observer additions/removals between post and dispatch caused double-frees, UAFs, or leaks. (`EventQueueBroadcastFast`, `...FastBatch` and `EventQueueEmulator` copy observers into the message at post time, so their original accounting was consistent; they were switched to the same model for simplicity — one atomic decrement instead of N.)
- **Resolution**: Refactored the payload ownership model: the message owns 1 reference (`refCount = 1`). Observers borrow the payload during synchronous dispatch. After all observer callbacks return, the message calls `managed->release()` exactly once. Observers wishing to retain the buffer beyond the callback can call `addRef()` / `release()`.

### 7. Full Batch Draining in `EventQueueBroadcastFastBatch`
- **Issue**: `dispatchBatch(maxCount)` previously capped the pop loop at `BATCH_SIZE = 32`, ignoring requests for larger batch sizes.
- **Resolution**: Updated to loop in `BATCH_SIZE` chunks until `maxCount` messages are dispatched or the queue is exhausted.

---

## Phase 2 Flavor-Aware Performance Optimizations

### 1. `EventQueue` & `MessageCenter` Architecture Optimizations
- **Fast Topic Resolution**: Converted `TopicResolveMap` from `std::map<std::string, int>` to `std::unordered_map<std::string, int>` with pre-reserved bucket capacity in `init()`. Replaced double-lookup patterns in `ResolveTopic` with single `.find()` calls.
- **$O(1)$ Direct Table Lookups**: Introduced direct array index lookup table `m_topicObserversTable[MAX_TOPICS]` on the critical dispatch path while synchronizing `m_topicObservers` map for backwards compatibility.
- **Low-Contention Lock-Elision Dispatch**: Eliminated lock acquisition of `m_mutexDispatches` on uncontended dispatch hot paths via `std::atomic<uint32_t> m_activeDispatches` and `m_waitingRemovers`.
- **Zero-Allocation Dispatch Buffer**: Initially a thread-local `tl_snapshotBuffer`; superseded by the copy-on-write snapshots described in Phase 3.
- **Out-of-Lock Allocation & Conditional Notify**: In `EventQueue::Post()`, the `Message` node is allocated before locking `m_mutexMessages`, and the condition variable is only notified if the queue was previously empty (`wasEmpty`).
- **Batch Dequeue Worker**: In `MessageCenter::ThreadWorker()`, the message queue is dequeued via `localBatch.swap(m_messageQueue)` under lock, reducing mutex hold time and executing message dispatches in bulk.

### 2. `MessageCenterFast` Optimizations
- **Adaptive Sleeping Notification**: Added `std::atomic<bool> m_sleeping` indicator. `post()` only invokes `m_cv.notify_one()` if the worker thread is actively sleeping on an empty queue, dropping post latency from 313 ns to 16.2 ns.
- **Eliminated Redundant Memory Zeroing**: Removed unnecessary `memset` in `MessageFast` constructor.
- **Observer Snapshot**: Initially a thread-local `tl_fastSnapshot`; superseded by the copy-on-write snapshots described in Phase 3.
- **Unordered Topic Map**: Converted `m_topicMap` to `std::unordered_map` for fast topic lookup.

### 3. ~~`ObjectPoolLockFree` Sharded Free-Lists~~ (reverted)
- An 8-way sharded pool with `thread_local` shard affinity was tried and **reverted**: emulator workloads are single-threaded or 1 producer / 1 consumer, where sharding only causes own-shard misses plus a `thread_local` guard on every call (an `emutls` call on MinGW), and eager allocation of the full capacity grew memory 2–4x.

### 4. ~~Broadcast & Emulator False Sharing Elimination~~ (reverted)
- `alignas(64)` on `TopicObservers` / `TopicInfo` was **reverted**: topic tables are read-mostly (written only at registration), so there is no false sharing to remove; the padding only grew the tables (e.g. 136 → 192 B per emulator topic) and measured as noise.
- Topic lookup maps remain `std::unordered_map`.

### Performance Comparison: Baseline vs. Optimized

> [!WARNING]
> Superseded: these numbers were collected before the review-driven rework and several do not reproduce in a controlled A/B run against the original code (e.g. `BM_Fast_Post_Small` measures ≈280–300 ns in both trees; `MessageCenter` latency gains depended on the now opt-in worker spin). See [Review-Driven Rework](#review-driven-rework-emulator-workloads) for current numbers.

| Benchmark | Baseline (Before) | Optimized (After) | Speedup / Improvement |
| :--- | :--- | :--- | :--- |
| `BM_Fast_Post_Small` | 313 ns (3.20M msg/s) | **16.2 ns (61.68M msg/s)** | **~19.3x faster** (sleeping-check elision) |
| `BM_Original_Post_Small` | 138 ns (7.32M msg/s) | **71.5 ns (13.98M msg/s)** | **~1.9x faster** (out-of-lock alloc & conditional notify) |
| `BM_Comprehensive_TopicResolveHit/100` | 82.8 ns | **19.4 ns** | **~4.3x faster** ($O(1)$ unordered_map find) |
| `BM_Comprehensive_TopicResolveHit/500` | 139 ns | **22.3 ns** | **~6.2x faster** ($O(1)$ unordered_map find) |
| `BM_Comprehensive_TopicResolveMiss` | 51.5 ns | **13.1 ns** | **~3.9x faster** |
| `BM_MessageCenter_TopicResolution` | 102 ns | **41.7 ns** | **~2.4x faster** |
| `BM_Compare_Fast_Throughput` | 309 ns (3.23M msg/s) | **99.7 ns (10.03M msg/s)** | **~3.1x faster** (thread-local snapshot buffer) |
| `BM_Fast_FanOut/16` | 204 ns (78.59M msg/s) | **95.7 ns (167.14M msg/s)** | **~2.1x faster** |
| `BM_Comprehensive_ObserverAddRemove/100` | 17,923 ns (11.14M/s) | **13,747 ns (14.53M/s)** | **~23% faster** |

---

## Phase 3 Deep Flavor-Specific Dispatch/Allocation Optimizations & Multi-Flavor Fuzzing

### 1. Zero-Copy Copy-On-Write (COW) Observer Snapshots
- **`EventQueue` (`src/eventqueue.h`, `src/eventqueue.cpp`)**:
  - Replaced per-dispatch vector copying with an array of immutable shared snapshot vectors (`std::shared_ptr<const std::vector<ObserverDescriptor>> m_topicSnapshots[MAX_TOPICS]`).
  - On the critical dispatch hot path, taking a snapshot is now a single atomic shared_ptr reference count bump—**zero** `std::function` copies or heap allocations per dispatched message!
  - Mutations in `AddObserver` or `RemoveObserver*` invalidate `m_topicSnapshots[topicId].reset()` under `m_mutexObservers`.
- **`MessageCenterFast` (`src/messagecenter_fast.h`)**:
  - Added `std::shared_ptr<const std::vector<ObserverEntryFast>> m_observerSnapshots[MC_FAST_MAX_TOPICS]`.
  - In `dispatch()`, lazy snapshot caching eliminates per-message observer allocations and eliminates holding `m_observerMutex` during callback execution.

### 2. Worker Spin Phase in `MessageCenter` (now opt-in: `MESSAGECENTER_WORKER_SPIN`)
- An adaptive spin-yield phase (64 yields before blocking on the condition variable) cut ping-pong latency (`BM_MessageCenter_Latency`, `BM_Original_Latency`) from `~12–13 μs` to `~1.4–1.7 μs` p50.
- **Trade-off found during re-measurement**: while spinning, the worker picks messages up one by one as they arrive, so during bursts every post fights the worker for the queue mutex/cache lines. Per-post producer cost roughly doubled in burst benchmarks (e.g. 256-event frame: ~150 → ~330 ns/post). Sporadic events (e.g. once per video frame) never benefited, since they arrive long after the spin window.
- **Resolution**: the spin is now compile-time opt-in via `-DMESSAGECENTER_WORKER_SPIN=N` (default `0` = block immediately, baseline-like burst throughput). When enabled, the spin polls a lock-free `m_pendingMessages` hint instead of locking `m_mutexMessages`.
- `Post()` now notifies only when the worker is actually blocked (`m_workerSleeping`, set under `m_mutexMessages` before waiting, so no wake-up can be lost).

### 3. Sub-Millisecond Flush & Idle Sleep in `MessageCenterFast`
- Replaced the fixed 1ms sleep in `flush()` with a 100-iteration spin/yield loop falling back to 100 μs micro-sleep, accelerating benchmark and test completions.
- Preserved clean heap management in `MessageFast::reset()` by immediately freeing non-inline payloads (`> 64 B`), ensuring zero memory waste for fluctuating payloads.

### 4. Benchmark Timestamp Precision & Auto-Cleanup Fixes
- Fixed timestamp truncation in `benchmarks/messagecenter_benchmark.cpp`: changed `SmallPayload::value` from `uint32_t` to `uint64_t`, correcting bogus `86.6166G` p50 latency reporting to accurate microsecond measurements.
- Enabled `autoCleanupPayload = true` across all heap-allocated payloads in benchmarks, eliminating benchmark memory leaks.

### 5. Multi-Flavor Deterministic & Continuous Fuzzing
- Added 5 new deterministic state-machine fuzz tests in `tests/fuzz_regression_test.cpp`:
  - `RandomizedBroadcastQueueStateMachine`
  - `RandomizedBroadcastFastQueueStateMachine`
  - `RandomizedBroadcastBatchQueueStateMachine`
  - `RandomizedEmulatorDualQueueStateMachine`
  - `RandomizedMessageCenterFastStateMachine`
- Extended `tests/fuzz/message_center_fuzzer.cpp` to continuously fuzz all 5 queue flavors with both inline ($\le 48\text{ B}$) and managed ($> 48\text{ B}$) payloads, priority queues, and batch dispatches under LLVM libFuzzer with AddressSanitizer and LeakSanitizer.

### 6. Strict Cross-Platform & Cross-Compiler Portability
- Maintained clean C++14 compliance for `src/` and `tests/`.
- Cleaned up system includes: moved `#include <pthread.h>` in `src/messagecenter.cpp` to file scope under `#if defined(__APPLE__) || defined(__linux__)`.
- Validated on both **GCC 15.2.0** and **Clang 21.1.8** with zero warnings and 100% passing tests.

### Measured Latency & Throughput Profile (Phase 3)

> [!WARNING]
> Superseded: these numbers were collected before the review-driven rework and several do not reproduce in a controlled A/B run against the original code (e.g. `BM_Fast_Post_Small` measures ≈280–300 ns in both trees; `MessageCenter` latency gains depended on the now opt-in worker spin). See [Review-Driven Rework](#review-driven-rework-emulator-workloads) for current numbers.

| Benchmark / Metric | Latency (p50) | Latency (p99) | Peak Throughput | Architecture Notes |
| :--- | :--- | :--- | :--- | :--- |
| `BM_Fast_Post_Small` | — | — | **70.2M msg/s** (14.2 ns) | Lock-free MPMC + sleeping-notify elision |
| `BM_Fast_Latency` | **1.11 μs** | **2.10 μs** | — | Adaptive thread wake + zero-copy COW |
| `BM_MessageCenter_Latency` | **1.48 μs** | **5.65 μs** | — | Fixed 64-bit timestamp + adaptive yield |
| `BM_Original_Latency` | **1.45 μs** | **5.86 μs** | — | EventQueue + adaptive worker sleep |
| `BM_Emu_DualQueue` | **0.94 μs** | **23.1 μs** | **41.3M msg/s** | Critical inline priority queue |
| `BM_ArchCompare_BroadcastFast_Inline32` | — | — | **41.9M msg/s** (23.9 ns) | Zero-alloc lock-free broadcast |
| `BM_Comprehensive_FanOut_BroadcastFast/16` | — | — | **270.9M msg/s** (59.1 ns) | 16-subscriber fan-out |

---

## Review-Driven Rework (Emulator Workloads)

Primary target: low-latency in-process broadcast for retro emulators (Amiga, ZX Spectrum, ...), where events are typically posted in bursts on the emulation thread and drained at frame boundaries on the same thread or one consumer thread.

| Change | Why |
| :--- | :--- |
| `ObjectPoolLockFree`: single tagged free-list, lazy 64-slot chunks, contiguous pre-warmed block, no `thread_local`, coordinated with `QueueCapacity` | Sharding didn't help 1-producer/1-consumer usage and slowed the hot path; eager allocation of `maxSize` grew memory 2–4x; coordinating `maxSize = QueueCapacity + headroom` prevents pool cliff fallback to `new`/`delete` on large backlogs |
| `MPMCQueue`: demand-allocated 1,024-slot (`64 KB`) Vyukov ring segments + heap-allocated topic tables | Reduced stack frames from `1.3–5.1 MB` to `< 1 KB` and initial ring memory from `4 MB` (`65536` slots) to `64 KB` (`1024` slots) during normal work, while lazily scaling up to `Capacity` on backlog spikes and recycling drained segments |
| Anti-DoS for occasional bigger payloads (`> 48 B`) | `EventQueueEmulator::post()` / `tryPost()` automatically falls back to the bulk lane for critical messages `> 48 B` instead of dropping them; `dispatchPriority(64)` bounds contiguous fast drains so continuous small messages never starve the bulk queue; `tryPost` checks `m_queue.full()` before allocating large payloads |
| Reverted `alignas(64)` on topic tables (`TopicObservers`, `TopicInfo`) and on `EventQueue::m_workerSleeping` | Read-mostly data / no measured benefit; avoided over-aligned `new` in C++14 |
| `MessageCenterFast::post()` takes `m_cvMutex` briefly before notifying a sleeping worker | Closed a lost-wakeup window that could stall delivery for the full 10 ms `wait_for` timeout |
| `stop()` / `Stop()` from inside a callback is deferred (no `detach()`); destroying from a callback asserts | `detach()` + `delete` turned a crash into a silent use-after-free |
| `MessageCenter` worker spin made opt-in (`MESSAGECENTER_WORKER_SPIN`, default 0) | Spin roughly doubled producer cost in bursts; it only helps request/response ping-pong |

**Regression tests**: `tests/rework_regression_test.cpp` (15 tests). The libFuzzer harness drains single-threaded queues before saturation, and all lock-free broadcast flavors now also expose non-blocking `tryPost` (`tryPostFast` / `tryPostBulk` on `EventQueueEmulator`) that return `false` immediately when the ring buffer is full.

Additional hardening in the final pass:
- **`EventQueueBroadcastFastBatch` multi-instance & cross-thread-destruction safety**: bound `thread_local BatchBuffer` to its owning queue instance (`m_threadBuffers`), auto-flushing when switching instances and cleaning up unflushed buffers in `~EventQueueBroadcastFastBatch()`.
- **Lock-free immutable observer snapshots in `MessageCenterFast` & `EventQueueBroadcast`**: replaced C++20-deprecated `std::atomic_load(&shared_ptr)` (which used a global spinlock in `libstdc++`) and per-dispatch `m_observerMutex` locks with atomic `const ObserverList*` pointers and lazy 64-topic chunks.
- **Guaranteed 64-byte hardware cache-line alignment in `MPMCQueue::Segment`**: `Segment::create()` aligns the segment base to a 64-byte boundary without relying on C++17 over-aligned `operator new`, eliminating `-Waligned-new` warnings under `-std=c++14 -Wall -Werror` and preventing 64-byte `Slot`s from straddling cache lines.
- **Edge-case guards**: `RefCountedPayload::create(nullptr, size)` zero-initializes instead of dereferencing `nullptr`; `registerTopic()` clamps `uint16_t` counters at `MAX_TOPICS` via CAS so they never wrap around; `start()` / `Start()` joins any deferred callback self-`stop()` thread before spawning a new worker.

**Measured against the original code (`HEAD`, same benchmark sources, same machine)**:

| Metric | Baseline | Rework |
| :--- | :--- | :--- |
| `EventQueueEmulator` same-thread post+dispatch (GCC / Clang) | 20.5–21.2 ns / 16.5 ns | 22.2 ns / **16.5 ns** |
| Stack frame `sizeof` (`MessageCenterFast` / `BroadcastFast` / `Batch` / `Broadcast` / `Emulator`) | 4,168 / 5,152 / 5,152 / 4,224 / 1,314 KB | **0.75 / 0.30 / 0.30 / 0.50 / 0.56 KB** (`< 1 KB`) |
| `MPMCQueue<*, 65536>` normal-work ring memory (`backlog < 1024`) | 4,096 KB (65,536 slots touched on init) | **64 KB** (1 segment of 1,024 slots; grows on demand up to 64 segments) |
| `ObjectPoolLockFree` acquire+release, isolated tight loop | 6.5 ns | 9.7 ns (tagged-index + bounds check; not visible in end-to-end queue numbers) |
| `BM_Fast_MultiProducer`, `BM_Fast_16B/48B`, `BM_Batch_32B` | **crash (double free)** | run clean |
| `BM_Comprehensive_TopicResolveHit/100` / `Miss` | 87.8 / 52.4 ns | 19.6 / 13.4 ns |
| `BM_MessageCenter_TopicResolution` | 93.4 ns | 36.8 ns |
| `BM_Emu_DualQueue` critical p99 / p99.9 | 27.1 / 80.8 µs | 20.6 / 54.9 µs |
| `MessageCenter` paced frames (64 events/frame), drain p50 | 35.8–38.5 µs | 32.2–32.8 µs |
| `MessageCenter` paced frames (256 events/frame), producer ns/post | 162–176 ns | 227–243 ns |
| `BM_Original_Post_Small` / `BM_MessageCenter_Post_Small` (unbounded flood, producer-side only) | 82 / 147 ns | 237 / 309 ns |
| `MessageCenter::PostBatch` (64–256 events/batch, unbounded flood) | — | **182–231 ns/post** (241–288 ns/msg end-to-end) |

### Handling High-Rate Flooding & Backpressure

Root-cause analysis of unpaced flooding on classic `MessageCenter` showed that the slowdown is **not** in the mutex or `std::deque` container, but in **cross-thread heap allocator contention** (`new Message` + `new MessagePayload` on the producer thread freed via `delete` on the worker thread). In the original code, the worker was mostly starved during an unpaced flood (~6% delivered while posting, leaving a **9.7–11.5 ms** tail backlog after posting finished), so frees happened later without cross-thread arena contention. In the reworked code, the worker drains concurrently (25–60% delivered while posting, cutting tail drain after posting to **2.3–3.5 ms**), which triggers concurrent cross-thread `free()` contention in glibc `ptmalloc`.

Four complementary mechanisms are provided:

1. **Workload Routing (Recommended Architecture)**:
   - Route high-rate / per-scanline / per-frame emulator traffic (audio sync, VBlank, input, raster/trace streams) to **`EventQueueEmulator`**, **`EventQueueBroadcastFast`**, **`EventQueueBroadcastFastBatch`**, or **`MessageCenterFast`**, which use pooled messages and inline payloads (`25–40M msg/s`, zero per-message heap allocation).
   - Keep classic **`EventQueue` / `MessageCenter`** for UI, configuration, and control events.
2. **Configurable Queue Bound & Non-Blocking `tryPost`**:
   - `EventQueue::SetMaxQueueSize(size_t maxQueueSize)` (default `0` = unbounded) caps the classic queue length. When full, `Post()` returns `false` and automatically deletes `obj` if `autoCleanupPayload == true`, preventing unbounded backlog growth and memory spikes.
   - `EventQueueBroadcast::tryPost`, `EventQueueBroadcastFast::tryPost`, `EventQueueBroadcastFastBatch::tryPost`, and `EventQueueEmulator::tryPost` (`tryPostFast` / `tryPostBulk`) return `false` immediately (cleaning up any pooled/refcounted resources) when the ring buffer is full instead of spinning in `yield()`.
3. **Amortized Batch Posting (`EventQueue::PostBatch`)**:
   - `PostBatch(topic, objs, count, autoCleanupPayload)` enqueues a burst of messages (e.g., at an emulator frame boundary) under a single lock acquisition and single wakeup per chunk, bringing glibc flood throughput back to `~182–231 ns/post` (`241–288 ns/msg` end-to-end) while respecting `GetMaxQueueSize()`.
4. **Allocator Selection for Heap-Heavy Classic Workloads**:
   - Applications that intentionally flood heap-allocated `MessagePayload` objects across threads through classic `MessageCenter` can link or preload `jemalloc` or `mimalloc`, which eliminates glibc cross-thread `tcache`/arena lock contention (`~145–250 ns/msg` end-to-end with ~100% concurrent delivery).

---

## Verification & Sanity Gates

All 5 verification gates pass with zero errors, zero warnings, and zero leaks:

### 1. Release Build & Tests (GCC & Clang)
```bash
# GCC 15
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
timeout 30s ./build/bin/message-center-tests
# Result: 73/73 tests PASSED (0 failures)

# Clang 21
cmake -B build_clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build_clang -j$(nproc)
timeout 30s ./build_clang/bin/message-center-tests
# Result: 73/73 tests PASSED (0 failures)

# Benchmark Suite
timeout 30s ./build/bin/message-center-benchmark --benchmark_min_time=0.01s
# Result: All benchmarks completed with exit code 0
```

### 2. AddressSanitizer (ASan) + UndefinedBehaviorSanitizer (UBSan) + LeakSanitizer (LSan)
```bash
cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g"
cmake --build build_asan -j$(nproc)
timeout 30s ./build_asan/bin/message-center-tests
# Result: 73/73 tests PASSED, 0 leaks, 0 memory errors
```

### 3. ThreadSanitizer (TSan)
```bash
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build_tsan -j$(nproc)
timeout 30s ./build_tsan/bin/message-center-tests
# Result: 73/73 tests PASSED, 0 data races, 0 thread leaks
```

### 4. libFuzzer with Leak Detection (Multi-Flavor)
```bash
clang++ -std=c++17 -fsanitize=fuzzer,address,undefined -g -O1 \
  -Isrc -Iinclude src/eventqueue.cpp tests/fuzz/message_center_fuzzer.cpp \
  -lpthread -o tests/fuzz/message_center_fuzzer
timeout 30s ./tests/fuzz/message_center_fuzzer -detect_leaks=1 -max_total_time=10
# Result: >11,000 multi-flavor runs executed across all 5 flavors, 0 crashes, 0 leaks (~48 MB RSS)
```

### 5. Valgrind Memcheck (`RelWithDebInfo`)
```bash
cmake -B build_valgrind -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_valgrind -j$(nproc)
valgrind --tool=memcheck --leak-check=full \
  --show-leak-kinds=all --track-origins=yes --error-exitcode=1 \
  ./build_valgrind/bin/message-center-tests
# Result: 73/73 tests PASSED in 4.5s (no --max-stackframe workaround required)
# HEAP SUMMARY: in use at exit: 0 bytes in 0 blocks
#   total heap usage: 276,148 allocs, 276,148 frees, 25,961,951 bytes allocated (55% less heap than before)
# All heap blocks were freed -- no leaks are possible
# ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
```
