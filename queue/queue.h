#ifndef QUEUE_H
#define QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <cassert>
#include <thread>

template <typename T, typename Container = std::queue<T>>
class Queue // 无界队列
{
public:
    Queue() = default;
    ~Queue() = default;

    // 禁止拷贝和移动，编译器会自动delete
    /*Queue(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue& operator=(Queue&&) = delete;*/

    void push(const T &val)
    {
        emplace(val);
    }

    void push(T &&val)
    {
        emplace(std::move(val));
    }

    template <typename... Args>
    void emplace(Args &&...args)
    {
        std::lock_guard lk{mtx_};
        q_.push(std::forward<Args>(args)...);
        cv_.notify_one();
    }

    T pop() // 阻塞
    {
        std::unique_lock lk{mtx_};
        cv_.wait(lk, [this]
                 { return !q_.empty(); }); // 如果队列不为空就继续执行，否则阻塞
        assert(!q_.empty());
        T ret{std::move_if_noexcept(q_.front())};
        q_.pop();
        return ret;
    }

    std::optional<T> try_pop() // 非阻塞
    {
        std::unique_lock lk{mtx_};
        if (q_.empty())
            return {};
        std::optional<T> ret{std::move_if_noexcept(q_.front())};
        q_.pop();
        return ret;
    }
    bool empty() const
    {
        std::lock_guard lk{mtx_};
        return q_.empty();
    }

private:
    Container q_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
};

#endif