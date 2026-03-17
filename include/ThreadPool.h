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
// TaskQueue — NOT thread-safe. ThreadPool owns all synchronization.
// ---------------------------------------------------------------------------
class TaskQueue {
public:
    void push(std::function<void()> task) {
        queue_.push(std::move(task));
    }

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
// WorkStealingDeque — per-worker double-ended queue.
// push_front/pop_front are called only by the owner (LIFO stack end).
// steal_back is called by any thread (FIFO steal end).
// NOT thread-safe between two callers of push_front/pop_front — only one
// owner thread may call those. steal_back may be called concurrently.
// ---------------------------------------------------------------------------
class WorkStealingDeque {
public:
    // Owner: push a new task onto the front (LIFO end).
    void push_front(std::function<void()> task) {
        std::lock_guard<std::mutex> lk(mutex_);
        deque_.push_front(std::move(task));
    }

    // Owner: pop the most recently pushed task (LIFO).
    std::function<void()> pop_front() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (deque_.empty()) return {};
        auto task = std::move(deque_.front());
        deque_.pop_front();
        return task;
    }

    // Stealer: take the oldest task from the back (FIFO).
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
// ThreadPool — work-stealing implementation.
//
// submit() distributes tasks round-robin directly to per-worker deques.
// Workers pop from their own deque first (no shared lock), then steal from
// a random victim. The only shared mutex (sleep_mutex_) is touched only when
// a worker has genuinely found no work and needs to sleep.
// ---------------------------------------------------------------------------

// Cache-line-padded wrapper to prevent false sharing between adjacent deques.
struct alignas(64) PaddedDeque {
    WorkStealingDeque deque;
};
static_assert(alignof(PaddedDeque) >= 64, "PaddedDeque must be cache-line aligned");

class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = std::max(1u, std::thread::hardware_concurrency()))
        : num_threads_(num_threads), deques_(num_threads) {
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back(&ThreadPool::worker_loop, this, i);
    }

    ~ThreadPool() { shutdown(); }

    // Non-copyable, non-movable (workers capture `this`)
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Explicit shutdown — blocks until all in-flight tasks finish.
    void shutdown() {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;  // already stopped
        cv_.notify_all();
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }

    // submit<F, Args...>() — returns std::future<ReturnType>.
    // Pushes to a worker deque directly (round-robin for external callers,
    // own deque for recursive worker submissions).
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;

        if (stop_.load(std::memory_order_acquire))
            throw std::runtime_error("submit on stopped ThreadPool");

        // packaged_task is move-only; wrap in shared_ptr so the lambda is copyable.
        auto task = std::make_shared<std::packaged_task<R()>>(
            [f = std::forward<F>(f),
             args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
                return std::apply(std::move(f), std::move(args_tuple));
            });

        std::future<R> future = task->get_future();
        auto wrapper = [task]() { (*task)(); };

        // Pick target deque: own if caller is a worker, round-robin otherwise.
        std::size_t target = (tl_worker_index_ != SIZE_MAX)
            ? tl_worker_index_
            : (next_worker_.fetch_add(1, std::memory_order_relaxed) % num_threads_);

        deques_[target].deque.push_front(std::move(wrapper));

        // Wake one sleeping worker.
        cv_.notify_one();

        return future;
    }

private:
    // xorshift64 — fast per-worker PRNG for victim selection.
    static std::uint64_t xorshift64(std::uint64_t& state) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    void worker_loop(std::size_t my_index) {
        tl_worker_index_ = my_index;
        std::uint64_t rng = my_index + 1;  // seed (must be non-zero)

        while (true) {
            std::function<void()> task;

            // Step 1: pop from own deque (no contention with other workers).
            task = deques_[my_index].deque.pop_front();

            // Step 2: steal from a random victim.
            if (!task) {
                for (std::size_t attempt = 0; attempt < num_threads_ - 1; ++attempt) {
                    std::size_t victim = xorshift64(rng) % num_threads_;
                    if (victim == my_index) continue;
                    task = deques_[victim].deque.steal_back();
                    if (task) break;
                }
            }

            // Step 3: nothing found — sleep until notified.
            if (!task) {
                std::unique_lock<std::mutex> lock(sleep_mutex_);
                cv_.wait(lock, [&] {
                    if (stop_.load(std::memory_order_relaxed)) return true;
                    for (std::size_t i = 0; i < num_threads_; ++i)
                        if (!deques_[i].deque.empty()) return true;
                    return false;
                });
                if (stop_.load(std::memory_order_relaxed)) {
                    // Drain remaining tasks across all deques before exiting.
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

            // Found work — propagate a wakeup before executing, so sleeping
            // workers can pick up tasks that become visible via stealing chains.
            cv_.notify_one();
            task();
        }
    }

    static thread_local std::size_t tl_worker_index_;

    std::size_t num_threads_;
    std::vector<PaddedDeque> deques_;          // per-worker LIFO queues
    std::vector<std::thread> workers_;

    std::atomic<std::size_t> next_worker_{0};  // round-robin counter for submit()
    std::atomic<bool> stop_{false};

    std::mutex sleep_mutex_;                   // only held when workers sleep
    std::condition_variable cv_;
};

inline thread_local std::size_t ThreadPool::tl_worker_index_ = SIZE_MAX;
