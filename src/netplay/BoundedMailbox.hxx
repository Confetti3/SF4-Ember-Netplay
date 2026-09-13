#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace sf4e { namespace netplay {

// Cross-thread handoff for commands/control events, never gameplay packets.
// Callers provide serialized byte cost and handle rejection explicitly. Each
// queue has both item and byte limits; closing drops owned queued resources.
template <typename T> class BoundedMailbox {
public:
    BoundedMailbox(size_t maxItems, size_t maxBytes) : maxItems_(maxItems), maxBytes_(maxBytes) {}

    bool TryPush(T value, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || bytes == 0 || queue_.size() >= maxItems_ || bytes > maxBytes_ - bytes_) {
            ++rejections_;
            return false;
        }
        queue_.emplace_back(std::move(value), bytes);
        bytes_ += bytes;
        return true;
    }

    bool TryPop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) { return false; }
        value = std::move(queue_.front().first);
        bytes_ -= queue_.front().second;
        queue_.pop_front();
        return true;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        queue_.clear();
        bytes_ = 0;
    }

    size_t Rejections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejections_;
    }

private:
    const size_t maxItems_;
    const size_t maxBytes_;
    mutable std::mutex mutex_;
    std::deque<std::pair<T, size_t>> queue_;
    size_t bytes_ = 0;
    size_t rejections_ = 0;
    bool closed_ = false;
};

} } // namespace sf4e::netplay
