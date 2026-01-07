# Message Queue Optimization

## Quick Start: Use the Optimized Implementation

The optimized EventQueue implementation is available in the `optimization-experiments` branch.

### Recommended: EventQueueSimple

```bash
# Switch to optimization branch
git checkout optimization-experiments

# Use in your code
#include "eventqueue_simple.h"
EventQueueSimple queue;  // Drop-in replacement for EventQueue
```

### Performance

| Metric | Original | EventQueueSimple | Improvement |
|:-------|:---------|:-----------------|:------------|
| Single-thread | 1.47 M/s | **9.27 M/s** | **+531%** |
| Multi-thread (8P+4C) | 0.5 M/s | **2.10 M/s** | **+320%** |
| Safety | ❌ Race conditions | ✅ **Thread-safe** | Fixed |
| Dependencies | None | None | Same |

### Why EventQueueSimple?

1. ✅ **Fastest** - Mutex + Object Pooling + COW observers
2. ✅ **Safest** - No race conditions, no dangling callbacks
3. ✅ **Simplest** - 250 lines, zero external dependencies
4. ✅ **Compatible** - Same API as EventQueue

### Features

- **Object Pooling**: Messages recycled (2x faster)
- **Copy-On-Write Observers**: Lock-free dispatch
- **Dispatch Tracking**: Safe observer unregistration
- **140x Headroom**: 2.10 M/s vs 15k/s requirement

### Documentation

See `optimization-experiments` branch for:
- `BRANCH_README.md` - Quick reference
- `history/README_OPTIMIZATION.md` - Executive summary
- `history/COMPLETE_STRATEGY_ANALYSIS.md` - Full comparison of 8 strategies
- `history/COW_ARCHITECTURE_GUIDE.md` - How it works

### Master Branch

The master branch contains:
- Original EventQueue (baseline)
- ObjectPool infrastructure (for future integration)
- Documentation pointing to optimization work

For production use, switch to `optimization-experiments` branch and use `EventQueueSimple`.

---

**TL;DR**: `git checkout optimization-experiments` and use `EventQueueSimple` for 6x faster, thread-safe message passing! 🚀
