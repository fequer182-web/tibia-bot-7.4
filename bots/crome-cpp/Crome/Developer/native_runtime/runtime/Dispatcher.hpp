#pragma once
#include <chrono>
#include <functional>
#include <queue>

namespace crome {
class Dispatcher {
public:
    using Task = std::function<void()>;
    void post(Task t) { queue_.push(std::move(t)); }
    std::size_t drain(std::size_t maxTasks = 32) {
        std::size_t n = 0;
        while (!queue_.empty() && n < maxTasks) {
            auto t = std::move(queue_.front()); queue_.pop(); t(); ++n;
        }
        return n;
    }
private:
    std::queue<Task> queue_;
};
}
