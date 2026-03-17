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
// Phase 1 demo: single-threaded TaskQueue
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
// Phase 2 demo: fire-and-forget thread pool
// ---------------------------------------------------------------------------
static void demo_thread_pool() {
    std::cout << "\n=== Phase 2: ThreadPool (fire-and-forget) ===\n";
    {
        ThreadPool pool(4);
        std::mutex cout_mutex;
        for (int i = 0; i < 20; ++i) {
            pool.submit([i, &cout_mutex] {
                std::lock_guard<std::mutex> lk(cout_mutex);
                std::cout << "  Task " << std::setw(2) << i
                          << " on thread " << std::this_thread::get_id() << "\n";
            });
        }
    }  // destructor joins all workers
    std::cout << "  All tasks complete.\n";
}

// ---------------------------------------------------------------------------
// Phase 3 demo: futures and exception propagation
// ---------------------------------------------------------------------------
static void demo_futures() {
    std::cout << "\n=== Phase 3: submit with futures ===\n";
    ThreadPool pool(4);

    auto f1 = pool.submit([] { return 42; });
    auto f2 = pool.submit([](int a, int b) { return a + b; }, 10, 20);

    std::cout << "  Result 1: " << f1.get() << " (expected 42)\n";
    std::cout << "  Result 2: " << f2.get() << " (expected 30)\n";

    // Exception propagation
    auto f3 = pool.submit([] () -> int { throw std::runtime_error("intentional error"); });
    try {
        f3.get();
    } catch (const std::runtime_error& e) {
        std::cout << "  Caught expected exception: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Phase 4 demo: shutdown edge cases
// ---------------------------------------------------------------------------
static void demo_shutdown() {
    std::cout << "\n=== Phase 4: Graceful shutdown edge cases ===\n";

    // Test 1: submit after shutdown throws
    {
        ThreadPool pool(2);
        pool.shutdown();
        try {
            pool.submit([] {});
            std::cout << "  ERROR: expected exception not thrown\n";
        } catch (const std::runtime_error& e) {
            std::cout << "  submit-after-shutdown threw as expected: " << e.what() << "\n";
        }
    }  // destructor on already-stopped pool — should be safe

    // Test 2: all queued tasks drain before shutdown completes
    {
        constexpr int NUM_TASKS = 1000;
        std::atomic<int> counter{0};
        {
            ThreadPool pool(4);
            for (int i = 0; i < NUM_TASKS; ++i)
                pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }  // destructor joins — all tasks should have run
        std::cout << "  Task drain: " << counter.load() << "/" << NUM_TASKS
                  << (counter.load() == NUM_TASKS ? " PASS" : " FAIL") << "\n";
    }
}

// ---------------------------------------------------------------------------
// Phase 5: benchmark
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
    // Use results to prevent the compiler from eliding the computation.
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
        for (int i = 0; i < num_tasks; ++i)
            results[i] = futures[i].get();
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    volatile double sink = std::accumulate(results.begin(), results.end(), 0.0);
    (void)sink;
    return ms_double(t1 - t0).count();
}

static double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static void run_benchmark() {
    std::cout << "\n=== Phase 5: Benchmark (10,000 tasks, 5 runs each) ===\n";

    constexpr int NUM_TASKS = 10'000;
    constexpr int RUNS = 5;

    // Serial baseline
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
        demo_task_queue();
        demo_thread_pool();
        demo_futures();
        demo_shutdown();
        std::cout << "\nRun with --benchmark for performance measurements.\n";
    } else {
        run_benchmark();
    }
}
