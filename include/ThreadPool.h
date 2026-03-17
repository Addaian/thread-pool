#pragma once

#include <condition_variable>
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
// ThreadPool
// ---------------------------------------------------------------------------
class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = std::thread::hardware_concurrency()) {
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back(&ThreadPool::worker_loop, this);
    }

    ~ThreadPool() { shutdown(); }

    // Non-copyable, non-movable (workers capture `this`)
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Explicit shutdown — blocks until all in-flight tasks finish.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }

    // Fire-and-forget submit.
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("submit on stopped ThreadPool");
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    // submit<F, Args...>() — returns std::future<ReturnType>.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;

        // packaged_task is move-only; wrap in shared_ptr so the lambda is copyable.
        auto task = std::make_shared<std::packaged_task<R()>>(
            [f = std::forward<F>(f),
             args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
                return std::apply(std::move(f), std::move(args_tuple));
            });

        std::future<R> future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("submit on stopped ThreadPool");
            tasks_.push([task]() { (*task)(); });
        }
        cv_.notify_one();

        return future;
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                // Drain remaining tasks before exiting on shutdown.
                if (stop_ && tasks_.empty()) return;
                task = tasks_.pop();
            }
            if (task) task();
        }
    }

    std::vector<std::thread> workers_;
    TaskQueue tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};
