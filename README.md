# Message Center

macOS-inspired message queue — cross-platform (Windows, Linux, macOS)

## Why Message Center?

In-process message queues decouple components, enabling clean architecture where producers and consumers evolve independently. Unlike direct function calls, message passing allows asynchronous communication, multiple observers per event, and natural boundaries between subsystems (UI, audio, networking, logging).

**What makes this implementation different:**

Most in-process queues force a tradeoff: either simple API with mutex overhead, or complex lock-free code with manual memory management. This library provides both — a fire-and-forget API where callers post and immediately move on, while the queue handles thread-safe delivery and automatic cleanup. Under the hood, lock-free MPMC ring buffers and object pooling achieve **5.5M msg/sec** with **1.2μs P50 latency** (15x faster than mutex-based alternatives).

For latency-sensitive workloads like emulators or games, the dual-queue `EventQueueEmulator` variant separates critical events (audio sync, input, vblank) from bulk data (debug traces, state saves), ensuring time-critical callbacks never wait behind large payloads — achieving **P99 under 100μs** even at 10,000+ fps in turbo mode.

See [Choosing a Queue](doc/CHOOSING_A_QUEUE.md) for variant comparison, [Performance Analysis](doc/PERFORMANCE_ANALYSIS.md) for benchmarks, and [examples/](examples/) for ready-to-build code.

## Benchmark Results

**Test configuration**: Apple Silicon (ARM64), 10 cores, 2 producers + 8 consumers, 32B payloads

| Queue Variant | P50 | P99 | P999 | Throughput | Best For |
|---------------|-----|-----|------|------------|----------|
| EventQueueFast | **1.2 μs** | 108 μs | 144 μs | **5.75 M/s** | General-purpose |
| EventQueueBatch | 1.2 μs | 17 μs | **76 μs** | 5.44 M/s | Low tail latency |
| EventQueueEmulator | 0.17 μs | **99 μs** | 130 μs | 128 M/s | Emulator/games |
| Original (mutex) | 6,617 μs | 9,873 μs | — | 372 K/s | Baseline |

### Comparison to Alternatives

| Library | Architecture | Typical Throughput | Latency | API Complexity |
|---------|--------------|-------------------|---------|----------------|
| **This library** | Lock-free MPMC + pool | **5.5 M/s** | **1-2 μs** | Fire-and-forget |
| std::queue + mutex | Blocking | 100-500 K/s | 5-10 ms | Manual locking |
| Boost.Lockfree | Lock-free bounded | 1-3 M/s | 2-5 μs | Manual memory |
| moodycamel::ConcurrentQueue | Lock-free unbounded | 3-8 M/s | 1-3 μs | Manual memory |
| Intel TBB concurrent_queue | Lock-free | 1-4 M/s | 2-10 μs | Manual memory |
| folly::MPMCQueue | Lock-free bounded | 2-5 M/s | 2-5 μs | Manual memory |

**Key advantages over alternatives:**
- **Automatic memory management**: Fire-and-forget API — post and move on, no cleanup needed
- **Built-in observer pattern**: Topic-based pub/sub with automatic fan-out
- **Adaptive payload handling**: Inline for small data (≤48B), ref-counted for large
- **Dual-queue support**: Separate latency-critical events from bulk data
- **No external dependencies**: Header-only, C++11 compatible

## Quick Start

```cpp
#include "messagecenter.h"

// Define typed payload (derives from MessagePayload)
struct PlayerMoved : public MessagePayload {
    uint32_t playerId;
    float x, y;
    PlayerMoved(uint32_t id, float x, float y) : playerId(id), x(x), y(y) {}
};

int main() {
    // Get singleton instance (auto-starts dispatcher thread)
    auto& mc = MessageCenter::DefaultMessageCenter();
    
    // Subscribe: logging observer (lambda wrapped in ObserverCallbackFunc)
    mc.AddObserver("player.moved", ObserverCallbackFunc([](int id, Message* msg) {
        auto* e = static_cast<PlayerMoved*>(msg->obj);
        printf("Player %u moved to (%.1f, %.1f)\n", e->playerId, e->x, e->y);
    }));
    
    // Subscribe: boundary checker (lambda capturing state)
    float maxX = 100.0f;
    mc.AddObserver("player.moved", ObserverCallbackFunc([maxX](int id, Message* msg) {
        auto* e = static_cast<PlayerMoved*>(msg->obj);
        if (e->x > maxX) printf("Player %u out of bounds!\n", e->playerId);
    }));
    
    // Publish: fire-and-forget, payload auto-cleaned after dispatch
    mc.Post("player.moved", new PlayerMoved(1, 10.5f, 20.0f));
    
    return 0;
}
```

## Queue Variants

| Header | Class | Use Case |
|--------|-------|----------|
| `eventqueue_fast.h` | `EventQueueBroadcastFast<N>` | General-purpose, highest throughput |
| `eventqueue_batch.h` | `EventQueueBroadcastFastBatch<N>` | Low tail latency (P999) |
| `eventqueue_emulator.h` | `EventQueueEmulator<N,M>` | Dual-queue for games/emulators |
| `eventqueue_broadcast.h` | `EventQueueBroadcast<N>` | Flexible API with std::function |

**Observer types supported:**
- Static functions
- Class instance methods (via userData pointer)
- Lambdas (EventQueueBroadcast only)

## Getting Started

**Prerequisites:** CMake 3.13+, C++11 compiler (GCC 4.8+, Clang 3.3+, MSVC 2015+)

**Clone:**

```bash
git clone --recurse-submodules https://github.com/alfishe/message-center-cpp.git
cd message-center-cpp
```

**Build:**
```bash
mkdir build && cd build
cmake ..
cmake --build . --parallel
```

On Windows, use `cmake --build . --config Release` (MSVC ignores CMAKE_BUILD_TYPE).

**Update:**
```bash
git pull --recurse-submodules
```

Subdirectories `/src`, `/tests`, `/benchmarks`, `/examples` can be built independently.

## Examples

| Example | Description |
|---------|-------------|
| [message_center](examples/message_center/) | High-level singleton API with typed payloads, class methods, and lambdas |
| [fast_queue](examples/fast_queue/) | Lock-free queue with raw function pointers for maximum throughput |
| [batch_queue](examples/batch_queue/) | Batch dispatch variant for lowest tail latency (P999) |
| [emulator_queue](examples/emulator_queue/) | Dual-queue system separating critical and bulk events |
| [turbo_benchmark](examples/turbo_benchmark/) | Stress test for high-fps scenarios (10k+ fps turbo mode) |

Build examples: `cd examples && mkdir build && cd build && cmake .. && cmake --build .`
