# Thread Pool

A C++ thread pool and task scheduler built from scratch. Supports fire-and-forget tasks and tasks that return values via `std::future`.

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

## Design

### `TaskQueue`

A thin wrapper around `std::queue<std::function<void()>>` with no internal synchronization. Keeping it non-thread-safe is intentional: `ThreadPool` guards both the queue and the `stop_` flag under a single mutex, which avoids TOCTOU races that would arise if the queue had its own lock.

### `ThreadPool`

Each worker thread runs a loop: wait on a condition variable, pop a task, execute it outside the lock. The CV predicate is `stop_ || !tasks_.empty()`, so workers wake on new tasks and on shutdown. On shutdown the worker exits only when `stop_ && tasks_.empty()` — this guarantees all already-queued tasks drain before the pool dies.

### Templated `submit()` and the `shared_ptr<packaged_task>` idiom

`std::function<void()>` requires its stored callable to be copyable. `std::packaged_task` is move-only. The standard fix: wrap the `packaged_task` in a `shared_ptr`, then capture the shared pointer in the lambda stored in the queue. Copies of the lambda share ownership of the task; calling any copy executes it exactly once.

Arguments are forwarded via `std::apply` over a `std::tuple`, which is cleaner than `std::bind` and handles overloaded functions correctly (C++17).

### Shutdown protocol

1. Lock the mutex, set `stop_ = true`, release.
2. `notify_all()` — wakes all sleeping workers.
3. `join()` each thread — blocks until all in-flight work finishes.

`submit()` checks `stop_` under the lock and throws `std::runtime_error` if the pool is stopped.

### Thread safety

Verified with `-fsanitize=thread` (ThreadSanitizer). No data races.

## Benchmark

10,000 CPU-bound tasks (each computes `sum(sin(i)) for i in [0, 1000)`), 5 runs per configuration, median reported. Measured on Apple M-series, 10 cores.

| Threads | Median time (ms) | Speedup vs serial |
|---------|-----------------|-------------------|
| serial  | 31.73           | 1.00x             |
| 1       | 49.36           | 0.64x             |
| 2       | 24.52           | 1.29x             |
| 4       | 15.41           | 2.06x             |
| 8       | 24.48           | 1.30x             |
| 10      | 19.63           | 1.62x             |

**What the numbers show:**

- **1 thread is slower than serial** — the overhead of `std::function` type erasure, `shared_ptr` allocation, mutex/CV round-trips, and task queue bookkeeping costs more than the task itself warrants when there's no parallelism to gain.
- **4 threads is the sweet spot** — 2x speedup with tasks that are short enough for scheduling overhead to matter. Amdahl's Law predicts diminishing returns as thread count grows.
- **8 and 10 threads regress** — at this task granularity, contention on the single mutex becomes the bottleneck. Each thread frequently wakes, acquires the lock, pops one task, releases, and repeats. With finer-grained tasks, a work-stealing design with per-thread queues would scale better.

**Takeaway:** For coarser tasks (longer compute, fewer submissions) this pool scales linearly up to the core count. For very fine-grained work, batch tasks together to amortize scheduling overhead.
