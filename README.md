# Thread Pool

A C++ thread pool built from scratch in five stages, each motivated by a measurable problem with the previous design. The progression from a single-threaded queue to a work-stealing pool demonstrates why production schedulers are designed the way they are.

## Build

```sh
make          # release build  (-O2)
make debug    # ThreadSanitizer build (-fsanitize=thread)
make clean
```

Requires: clang++ with C++17 support.

## Usage

```cpp
#include "ThreadPool.h"

ThreadPool pool(4);  // 4 worker threads

// Fire-and-forget
pool.submit([] { do_work(); });

// With return value
auto future = pool.submit([](int x) { return x * x; }, 7);
std::cout << future.get();  // 49

// Explicit shutdown (or let the destructor do it)
pool.shutdown();
```

---

## How it was built

### Stage 1 — Single-threaded task queue

Starting point: a `TaskQueue` class wrapping `std::queue<std::function<void()>>` with `push()` and `pop()`. No threads, no concurrency — just the core data structure working correctly before adding any complexity.

### Stage 2 — Thread pool with a global mutex

Added a `ThreadPool` class: N worker threads sharing one mutex and one `std::queue`. Each worker loops: lock → check queue → pop task → unlock → execute. A `std::condition_variable` puts idle workers to sleep until work arrives.

This works, and the destructor joins all threads gracefully — queued tasks finish before the pool dies. But the design has a fundamental scaling problem.

### Stage 3 — Futures: submit() returns a result

Made `submit()` templated so callers can block on a result:

```cpp
auto f = pool.submit([](int x) { return x * x; }, 7);
int result = f.get();  // blocks until the task completes
```

`std::packaged_task` is move-only, but `std::function` requires copyability. The fix: wrap the `packaged_task` in a `shared_ptr`. The lambda stored in the queue captures the pointer; calling it executes the task exactly once. Arguments are forwarded with `std::apply` over a `std::tuple` (cleaner than `std::bind`, C++17).

### Stage 4 — Graceful shutdown hardening

Three edge cases made explicit:
- `submit()` after `shutdown()` throws `std::runtime_error`
- Double-shutdown is safe (idempotent via `compare_exchange_strong`)
- All queued tasks drain before workers exit — verified by submitting 1,000 tasks and asserting the counter equals 1,000 after the destructor returns

### Stage 5 — Work stealing

**The problem.** Benchmarking the global-mutex design revealed a sharp regression at high thread counts:

| Threads | Time (ms) | Speedup |
|---------|-----------|---------|
| serial  | 31.73     | 1.00x   |
| 4       | 15.41     | 2.06x   |
| 8       | 24.48     | **1.30x — slower than 4** |
| 10      | 19.63     | 1.62x   |

Every worker locks the same mutex on every task pop. With 8 threads, mutex contention dominates actual work — threads spend more time waiting for the lock than running tasks.

**The fix: work stealing.** Give each worker its own deque. Workers push and pop from the front of their own deque (LIFO — keeps recently-submitted tasks hot in cache). When a worker runs dry, it steals from the *back* of a random victim's deque (FIFO — takes the oldest task, avoids racing with the victim for recently-pushed work).

`submit()` distributes tasks round-robin to worker deques via an atomic counter. The only shared mutex (`sleep_mutex_`) is touched only when a worker has genuinely found no work anywhere and needs to sleep. Under load it is never contended.

Each `WorkStealingDeque` is wrapped in `alignas(64) PaddedDeque` to keep adjacent deques on separate cache lines, preventing false sharing.

**Result:**

| Threads | Time (ms) | Speedup |
|---------|-----------|---------|
| serial  | 31.73     | 1.00x   |
| 1       | 33.98     | 0.93x   |
| 2       | 18.06     | 1.76x   |
| 4       | 10.21     | 3.11x   |
| 8       |  8.38     | **3.78x** |
| 10      |  8.39     | 3.78x   |

8 threads went from slower-than-4 to the fastest configuration. Scaling is near-linear up to the core count.

**10 threads matches 8** — the remaining overhead is `std::function` type erasure and one `shared_ptr` allocation per task. At ~30µs of compute per task, these allocations are the ceiling. A move-only function wrapper would eliminate the `shared_ptr` indirection and push throughput higher.

---

## Design notes

**Why mutex-per-deque and not Chase-Lev lock-free?** Chase-Lev requires careful atomic memory ordering on a circular buffer with power-of-two sizing and hazard-pointer-based resizing. A mutex-per-deque is correct under ThreadSanitizer, dramatically better than one global lock, and shows the key insight (decouple workers) without the implementation complexity of a lock-free structure.

**Why random victim selection?** Round-robin steal attempts are correlated — all idle workers target the same victim simultaneously. Random victims decorrelate steal attempts under contention, which is what Intel TBB and the Go scheduler both use.

**Why `stop_` is atomic but the deques are not.** `stop_` is read on every submission and checked in the CV predicate without holding a lock, so it must be atomic. The deques are already protected by their own mutexes — adding atomics would be redundant and slower.

**Thread safety verified with `-fsanitize=thread` (ThreadSanitizer) at every stage. No data races.**
