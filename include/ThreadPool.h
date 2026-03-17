#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// TaskQueue
//
// A thin FIFO wrapper around std::queue. NOT thread-safe — the caller is
// responsible for all synchronization. Keeping synchronization out of the
// queue itself avoids the TOCTOU race that would arise if the queue had its
// own internal mutex: a caller would need to check empty() and then pop()
// as two separate operations, and another thread could drain the queue
// between those two calls.
// ---------------------------------------------------------------------------
class TaskQueue {
public:
    void push(std::function<void()> task) {
        queue_.push(std::move(task));
    }

    // Returns a default-constructed (empty) std::function if the queue is
    // empty. Callers test with `if (task)` rather than checking empty() first,
    // keeping the check-and-pop atomic from the caller's perspective.
    std::function<void()> pop() {
        if (queue_.empty()) return {};
        auto task = std::move(queue_.front());
        queue_.pop();
        return task;
    }

    bool empty() const { return queue_.empty(); }
    std::size_t size() const { return queue_.size(); }

private:
    std::queue<std::function<void()>> queue_;
};

// ---------------------------------------------------------------------------
// WorkStealingDeque
//
// A double-ended queue with a single per-instance mutex. Two roles:
//
//   Owner  (push_front / pop_front): the thread that "owns" this deque.
//          Operates on the front end in LIFO order. LIFO keeps recently
//          submitted tasks hot in the owner's cache — important for recursive
//          divide-and-conquer workloads where a parent submits children.
//
//   Stealer (steal_back): any other thread that has run out of work.
//           Operates on the back end in FIFO order. Taking the oldest task
//           avoids racing with the owner for recently-pushed work, and
//           distributes the oldest (and often largest) tasks first.
//
// Why a mutex and not Chase-Lev lock-free? Chase-Lev needs careful atomic
// memory ordering, a power-of-two circular buffer, and hazard pointers for
// safe resizing. A per-deque mutex is correct, verifiable with ThreadSanitizer,
// and still far better than one global lock: contention only happens between
// an owner and its stealers, never between two owners on different deques.
// ---------------------------------------------------------------------------
class WorkStealingDeque {
public:
    // Owner only. Pushes to the front (hot/LIFO end).
    void push_front(std::function<void()> task) {
        std::lock_guard<std::mutex> lk(mutex_);
        deque_.push_front(std::move(task));
    }

    // Owner only. Pops the most recently pushed task (LIFO).
    // Returns an empty function if the deque is empty.
    std::function<void()> pop_front() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (deque_.empty()) return {};
        auto task = std::move(deque_.front());
        deque_.pop_front();
        return task;
    }

    // Any thread. Steals the oldest task from the back (FIFO).
    // Returns an empty function if the deque is empty.
    std::function<void()> steal_back() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (deque_.empty()) return {};
        auto task = std::move(deque_.back());
        deque_.pop_back();
        return task;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return deque_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return deque_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> deque_;
};

// ---------------------------------------------------------------------------
// PaddedDeque
//
// Wraps WorkStealingDeque with cache-line alignment. Without this, adjacent
// deques in the vector would share a cache line (typically 64 bytes). When
// worker 0 modifies deques_[0] and worker 1 modifies deques_[1], the CPU
// must bounce the shared cache line between their L1 caches on every write —
// "false sharing" — even though they touch entirely different data.
// alignas(64) forces each entry onto its own cache line.
// ---------------------------------------------------------------------------
struct alignas(64) PaddedDeque {
    WorkStealingDeque deque;
};
static_assert(alignof(PaddedDeque) >= 64, "PaddedDeque must be cache-line aligned");

// ---------------------------------------------------------------------------
// ThreadPool
//
// Work-stealing design. submit() distributes tasks round-robin directly to
// per-worker deques via an atomic counter — no shared queue on the submission
// hot path. Each worker pops its own deque first, then steals from a random
// victim. The only shared mutex (sleep_mutex_) is touched only when a worker
// has exhausted all local and remote work and needs to sleep.
//
// Lock ordering (must always be acquired in this order to prevent deadlock):
//   sleep_mutex_ → deques_[i].deque.mutex_
// sleep_mutex_ is held while the CV predicate scans deques_[i].empty().
// No code path acquires a deque mutex and then sleep_mutex_.
// ---------------------------------------------------------------------------
class ThreadPool {
public:
    // Defaults to hardware_concurrency(), with a floor of 1.
    // hardware_concurrency() is allowed to return 0 on platforms where the
    // value is not computable; a pool with 0 workers would accept tasks but
    // never run them, causing future.get() to hang forever.
    explicit ThreadPool(std::size_t num_threads = std::max(1u, std::thread::hardware_concurrency()))
        : num_threads_(num_threads), deques_(num_threads) {
        // Reserve before launching threads so that emplace_back never
        // reallocates workers_ while threads are already running (they
        // capture `this`, and a realloc would not move the pool).
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back(&ThreadPool::worker_loop, this, i);
    }

    ~ThreadPool() { shutdown(); }

    // Non-copyable and non-movable. Worker threads capture `this` in their
    // loop; copying or moving the pool object would invalidate that pointer.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Signals all workers to stop, then blocks until every in-flight task
    // has finished and every worker thread has exited.
    //
    // compare_exchange_strong makes this idempotent: the destructor calls
    // shutdown() unconditionally, so an explicit shutdown() followed by
    // destruction must not double-join the threads.
    void shutdown() {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;  // already stopped — nothing to do
        cv_.notify_all();
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }

    // Submits a callable with arguments and returns a future for its result.
    //
    // Why shared_ptr<packaged_task>?
    //   std::packaged_task is move-only, but std::function (which we store in
    //   the deque) requires its target to be copyable. Wrapping in a shared_ptr
    //   gives us a copyable handle; all copies share ownership of the same task,
    //   and invoking any copy runs it exactly once.
    //
    // Why std::apply over std::bind?
    //   std::bind has surprising behaviour with overloaded functions and
    //   reference_wrapper. std::apply over a tuple is explicit and correct.
    //
    // Routing:
    //   - If the caller is itself a pool worker (tl_worker_index_ != SIZE_MAX),
    //     push to its own deque. This is the recursive-decomposition fast path:
    //     a task that spawns subtasks pushes them locally, keeping hot data in
    //     the same worker's cache and avoiding contention with the round-robin
    //     counter.
    //   - Otherwise, pick the next worker's deque via an atomic fetch_add.
    //     memory_order_relaxed is safe here: we only need the increment to be
    //     atomic; we do not need it to synchronize with any other memory.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;

        // Check stop_ before doing any allocation. Note: there is a narrow
        // TOCTOU window — stop_ could be set after this check but before the
        // push_front. In that case the task lands in a deque and is drained
        // by the shutdown logic. The future remains valid; the caller can
        // still call .get(). We accept this: strict ordering would require
        // holding sleep_mutex_ for the entire submit(), which is the global
        // bottleneck we designed out.
        if (stop_.load(std::memory_order_acquire))
            throw std::runtime_error("submit on stopped ThreadPool");

        auto task = std::make_shared<std::packaged_task<R()>>(
            [f = std::forward<F>(f),
             args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
                return std::apply(std::move(f), std::move(args_tuple));
            });

        std::future<R> future = task->get_future();
        auto wrapper = [task]() { (*task)(); };

        std::size_t target = (tl_worker_index_ != SIZE_MAX)
            ? tl_worker_index_
            : (next_worker_.fetch_add(1, std::memory_order_relaxed) % num_threads_);

        deques_[target].deque.push_front(std::move(wrapper));

        // Wake exactly one sleeping worker. If all workers are busy, the
        // notification is lost — that is fine, workers will pick up the task
        // naturally when they finish their current work.
        cv_.notify_one();

        return future;
    }

private:
    // Three-step xorshift64 — a well-known fast PRNG for integers.
    // We need unpredictable victim selection, not cryptographic randomness,
    // so this is sufficient and essentially free (3 XOR-shift ops).
    // State must never be zero (any XOR-shift of 0 stays 0), which is why
    // workers seed with (my_index + 1).
    static std::uint64_t xorshift64(std::uint64_t& state) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    void worker_loop(std::size_t my_index) {
        // Store index in thread-local storage so submit() can detect when
        // it is called from inside a worker and route to that worker's own
        // deque without touching the shared round-robin counter.
        tl_worker_index_ = my_index;
        std::uint64_t rng = my_index + 1;  // xorshift64 seed — must be non-zero

        while (true) {
            std::function<void()> task;

            // --- Step 1: pop own deque (zero contention with other workers) ---
            task = deques_[my_index].deque.pop_front();

            // --- Step 2: steal from a random victim ---
            // Random selection decorrelates steal attempts: if all idle workers
            // targeted the same victim (e.g. round-robin), they would pile onto
            // that one deque simultaneously. Randomness spreads the load.
            // We attempt at most num_threads_-1 victims before giving up.
            if (!task) {
                for (std::size_t attempt = 0; attempt < num_threads_ - 1; ++attempt) {
                    std::size_t victim = xorshift64(rng) % num_threads_;
                    if (victim == my_index) continue;  // never steal from self
                    task = deques_[victim].deque.steal_back();
                    if (task) break;
                }
            }

            // --- Step 3: genuinely idle — sleep until notified ---
            // We hold sleep_mutex_ while scanning deques_[i].empty() in the
            // predicate. This is safe: the lock ordering is always
            // sleep_mutex_ → deque.mutex_, and no code path acquires them
            // in the opposite order.
            if (!task) {
                std::unique_lock<std::mutex> lock(sleep_mutex_);
                cv_.wait(lock, [&] {
                    if (stop_.load(std::memory_order_relaxed)) return true;
                    for (std::size_t i = 0; i < num_threads_; ++i)
                        if (!deques_[i].deque.empty()) return true;
                    return false;
                });

                if (stop_.load(std::memory_order_relaxed)) {
                    // Drain all remaining tasks before exiting. All workers
                    // participate in the drain concurrently (steal_back is
                    // thread-safe), which is faster than having one worker
                    // drain everything while others sit idle.
                    bool found_any = true;
                    while (found_any) {
                        found_any = false;
                        for (std::size_t i = 0; i < num_threads_; ++i) {
                            auto t = deques_[i].deque.steal_back();
                            if (t) { found_any = true; t(); }
                        }
                    }
                    return;
                }
                continue;  // re-enter the acquisition loop
            }

            // Propagate a wakeup before executing. If this worker stole a
            // task from a victim that had many tasks, other sleeping workers
            // would not have been notified (only submit() calls notify_one).
            // Calling it here ensures the wakeup ripples through sleeping
            // workers as long as there is still work in the system.
            cv_.notify_one();
            task();
        }
    }

    // SIZE_MAX is the sentinel meaning "this thread is not a pool worker".
    // It is set to a worker's index when that worker starts, and used by
    // submit() to route recursive submissions to the caller's own deque.
    static thread_local std::size_t tl_worker_index_;

    std::size_t num_threads_;
    std::vector<PaddedDeque> deques_;          // one per worker, cache-line padded
    std::vector<std::thread> workers_;

    std::atomic<std::size_t> next_worker_{0};  // round-robin index for external submit()
    std::atomic<bool> stop_{false};

    std::mutex sleep_mutex_;                   // guards cv_; held only while sleeping
    std::condition_variable cv_;
};

inline thread_local std::size_t ThreadPool::tl_worker_index_ = SIZE_MAX;
