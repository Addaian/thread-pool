#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

// ---------------------------------------------------------------------------
// WorkStealingDeque isolation demo
//
// Verifies the two ends behave correctly before testing them under threads.
// Push 0..4 onto the front; the internal order becomes [4, 3, 2, 1, 0]
// (front→back). pop_front takes from the hot/LIFO end; steal_back takes from
// the cold/FIFO end.
// ---------------------------------------------------------------------------
static void demo_work_stealing_deque() {
    std::cout << "=== WorkStealingDeque: LIFO pop and FIFO steal ===\n";

    WorkStealingDeque d;
    std::vector<int> out;

    for (int i = 0; i < 5; ++i)
        d.push_front([i, &out] { out.push_back(i); });

    // pop_front (LIFO): takes 4, 3, 2 off the front
    for (int i = 0; i < 3; ++i) { auto t = d.pop_front(); if (t) t(); }
    // steal_back (FIFO): remaining deque is [1, 0]; back is 0, then 1
    for (int i = 0; i < 2; ++i) { auto t = d.steal_back(); if (t) t(); }

    std::cout << "  output: ";
    for (int v : out) std::cout << v << " ";
    std::cout << "(expected: 4 3 2 0 1)\n";
}

// ---------------------------------------------------------------------------
// Phase 1: single-threaded TaskQueue
// ---------------------------------------------------------------------------
static void demo_task_queue() {
    std::cout << "=== Phase 1: TaskQueue demo ===\n";
    TaskQueue q;
    for (int i = 0; i < 10; ++i)
        q.push([i] { std::cout << "  Task " << i << "\n"; });
    while (!q.empty()) {
        auto task = q.pop();
        if (task) task();
    }
}

// ---------------------------------------------------------------------------
// Phase 2: fire-and-forget thread pool
// ---------------------------------------------------------------------------
static void demo_thread_pool() {
    std::cout << "\n=== Phase 2: ThreadPool (fire-and-forget) ===\n";
    {
        // cout_mutex must be declared before pool so that it is destroyed
        // AFTER the pool destructor joins all worker threads. C++ destroys
        // block-scope objects in reverse declaration order; if pool came
        // first, it would be destroyed (and workers joined) before
        // cout_mutex, which is safe. But if cout_mutex came first, it would
        // be destroyed while workers may still be locking it — use-after-
        // destruction.
        std::mutex cout_mutex;
        ThreadPool pool(4);
        for (int i = 0; i < 20; ++i) {
            pool.submit([i, &cout_mutex] {
                std::lock_guard<std::mutex> lk(cout_mutex);
                std::cout << "  Task " << std::setw(2) << i
                          << " on thread " << std::this_thread::get_id() << "\n";
            });
        }
    }  // pool destructor joins all workers, then cout_mutex is destroyed
    std::cout << "  All tasks complete.\n";
}

// ---------------------------------------------------------------------------
// Phase 3: futures and exception propagation
//
// Exceptions thrown inside a task are captured by the packaged_task and
// re-thrown when the caller calls future.get(). This is standard behaviour
// for std::future, but worth verifying explicitly.
// ---------------------------------------------------------------------------
static void demo_futures() {
    std::cout << "\n=== Phase 3: submit with futures ===\n";
    ThreadPool pool(4);

    auto f1 = pool.submit([] { return 42; });
    auto f2 = pool.submit([](int a, int b) { return a + b; }, 10, 20);

    std::cout << "  Result 1: " << f1.get() << " (expected 42)\n";
    std::cout << "  Result 2: " << f2.get() << " (expected 30)\n";

    auto f3 = pool.submit([]() -> int { throw std::runtime_error("intentional error"); });
    try {
        f3.get();
    } catch (const std::runtime_error& e) {
        std::cout << "  Caught expected exception: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Phase 4: shutdown edge cases
// ---------------------------------------------------------------------------
static void demo_shutdown() {
    std::cout << "\n=== Phase 4: Graceful shutdown edge cases ===\n";

    // submit() after shutdown() must throw immediately.
    {
        ThreadPool pool(2);
        pool.shutdown();
        try {
            pool.submit([] {});
            std::cout << "  ERROR: expected exception not thrown\n";
        } catch (const std::runtime_error& e) {
            std::cout << "  submit-after-shutdown threw as expected: " << e.what() << "\n";
        }
    }  // destructor called on an already-stopped pool — must not double-join

    // All queued tasks must complete before the destructor returns.
    // The atomic counter is the authoritative check: if any task was lost,
    // dropped, or the destructor returned before all tasks ran, the count
    // will be less than NUM_TASKS.
    {
        constexpr int NUM_TASKS = 1000;
        std::atomic<int> counter{0};
        {
            ThreadPool pool(4);
            for (int i = 0; i < NUM_TASKS; ++i)
                pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }  // destructor blocks here until all 1000 tasks have run
        std::cout << "  Task drain: " << counter.load() << "/" << NUM_TASKS
                  << (counter.load() == NUM_TASKS ? " PASS" : " FAIL") << "\n";
    }
}

// ---------------------------------------------------------------------------
// Phase 5: benchmark
//
// Task design: compute sum(sin(seed+i)) for i in [0, 1000). This is ~30µs
// of pure CPU work per task — long enough to amortize scheduling overhead,
// short enough to show scaling differences across thread counts. Results are
// written to a pre-allocated vector by index, so there is no inter-task
// contention.
// ---------------------------------------------------------------------------
static double compute_task(int seed) {
    double sum = 0.0;
    for (int i = 0; i < 1000; ++i)
        sum += std::sin(static_cast<double>(seed + i));
    return sum;
}

using ms_double = std::chrono::duration<double, std::milli>;

static double measure_serial(int num_tasks) {
    std::vector<double> results(num_tasks);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_tasks; ++i)
        results[i] = compute_task(i);
    auto t1 = std::chrono::high_resolution_clock::now();
    // Accumulate into a volatile to prevent the compiler from proving that
    // results is never read and eliminating the entire computation as dead code.
    volatile double sink = std::accumulate(results.begin(), results.end(), 0.0);
    (void)sink;
    return ms_double(t1 - t0).count();
}

static double measure_pool(int num_tasks, std::size_t num_threads) {
    std::vector<double> results(num_tasks);
    std::vector<std::future<double>> futures;
    futures.reserve(num_tasks);

    auto t0 = std::chrono::high_resolution_clock::now();
    {
        ThreadPool pool(num_threads);
        for (int i = 0; i < num_tasks; ++i)
            futures.push_back(pool.submit(compute_task, i));
        // Collect results sequentially. Since all tasks have uniform compute
        // cost, futures[0] is unlikely to lag significantly behind futures[N].
        // For non-uniform workloads, collecting in completion order (e.g. via
        // a result queue) would be more accurate.
        for (int i = 0; i < num_tasks; ++i)
            results[i] = futures[i].get();
    }  // pool destructor joins threads; all futures already resolved so this is instant
    auto t1 = std::chrono::high_resolution_clock::now();

    volatile double sink = std::accumulate(results.begin(), results.end(), 0.0);
    (void)sink;
    return ms_double(t1 - t0).count();
}

// Returns the median of v (sorts in place). Using median rather than mean
// reduces the influence of OS scheduling noise on individual runs.
static double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static void run_benchmark() {
    std::cout << "\n=== Phase 5: Benchmark (10,000 tasks, 5 runs each) ===\n";

    constexpr int NUM_TASKS = 10'000;
    constexpr int RUNS = 5;

    std::vector<double> times(RUNS);
    for (int r = 0; r < RUNS; ++r) times[r] = measure_serial(NUM_TASKS);
    double serial_ms = median(times);

    std::cout << "\n"
              << std::setw(10) << "Threads"
              << std::setw(20) << "Median time (ms)"
              << std::setw(16) << "Speedup\n"
              << std::string(46, '-') << "\n";

    std::cout << std::setw(10) << "serial"
              << std::setw(20) << std::fixed << std::setprecision(2) << serial_ms
              << std::setw(15) << "1.00x\n";

    for (std::size_t threads : {1u, 2u, 4u, 8u, 10u}) {
        for (int r = 0; r < RUNS; ++r) times[r] = measure_pool(NUM_TASKS, threads);
        double pool_ms = median(times);
        double speedup = serial_ms / pool_ms;

        std::cout << std::setw(10) << threads
                  << std::setw(20) << std::fixed << std::setprecision(2) << pool_ms
                  << std::setw(14) << std::fixed << std::setprecision(2) << speedup << "x\n";
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    bool benchmark = argc > 1 && std::strcmp(argv[1], "--benchmark") == 0;

    if (!benchmark) {
        demo_work_stealing_deque();
        demo_task_queue();
        demo_thread_pool();
        demo_futures();
        demo_shutdown();
        std::cout << "\nRun with --benchmark for performance measurements.\n";
    } else {
        run_benchmark();
    }
}
