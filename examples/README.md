# EventQueue Examples

Ready-to-build examples demonstrating each queue variant.

## Building All Examples

```bash
cd examples
mkdir build && cd build
cmake ..
make
```

## Individual Examples

### fast_queue - High Performance

Best for general-purpose high-throughput event systems.

```bash
cd fast_queue
mkdir build && cd build
cmake .. && make
./fast_queue_example
```

**Features demonstrated:**
- Topic registration and observer setup
- Fire-and-forget posting
- Single and multi-threaded usage
- 5.5M msg/sec, P50=1.2μs

### batch_queue - Low Tail Latency

Best when P99/P999 latency matters.

```bash
cd batch_queue
mkdir build && cd build
cmake .. && make
./batch_queue_example
```

**Features demonstrated:**
- Batch dispatch (32 messages at a time)
- Multi-producer, multi-consumer
- P999 improved by 47% vs single dispatch

### emulator_queue - Mixed Criticality

Best for emulators, games, and real-time systems.

```bash
cd emulator_queue
mkdir build && cd build
cmake .. && make
./emulator_queue_example
```

**Features demonstrated:**
- Dual-queue architecture (fast + bulk)
- Priority-based dispatch
- Critical events (audio, vblank) never blocked by bulk (traces)
- P99=99μs vs 2200μs (22x better)

## Memory Leak Testing

Build with AddressSanitizer:

```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" ..
make
./fast_queue_example  # ASan reports leaks at exit
```

Or use Valgrind:

```bash
valgrind --leak-check=full ./fast_queue_example
```
