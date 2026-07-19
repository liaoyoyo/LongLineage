// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace longlineage::runtime {

enum class QueueOperationStatus {
    kSuccess,
    kClosed,
    kCancelled,
    kItemTooLarge,
    kInvalidByteSize,
};

inline const char* to_string(QueueOperationStatus status) noexcept {
    switch (status) {
        case QueueOperationStatus::kSuccess:
            return "SUCCESS";
        case QueueOperationStatus::kClosed:
            return "CLOSED";
        case QueueOperationStatus::kCancelled:
            return "CANCELLED";
        case QueueOperationStatus::kItemTooLarge:
            return "ITEM_TOO_LARGE";
        case QueueOperationStatus::kInvalidByteSize:
            return "INVALID_BYTE_SIZE";
    }
    return "UNKNOWN";
}

enum class QueueState {
    kOpen,
    kClosed,
    kCancelled,
};

struct QueueSnapshot {
    QueueState state = QueueState::kOpen;
    std::size_t capacity_bytes = 0;
    std::size_t queued_bytes = 0;
    std::size_t peak_queued_bytes = 0;
    std::size_t queued_items = 0;
    std::string cancellation_reason;
};

template <typename T>
struct QueuePopResult {
    QueueOperationStatus status = QueueOperationStatus::kClosed;
    std::optional<T> value;
    std::size_t charged_bytes = 0;
};

// A blocking queue whose admission control is based on caller-declared bytes.
//
// A zero-byte item is rejected: allowing it would make a byte bound ineffective.
// close() permits already queued work to drain. cancel() is fail-fast: it rejects
// new work, wakes all waiters, and discards work that has not started.
template <typename T>
class ByteBoundedQueue {
   public:
    explicit ByteBoundedQueue(std::size_t capacity_bytes) : capacity_bytes_(capacity_bytes) {
        if (capacity_bytes_ == 0) {
            throw std::invalid_argument("ByteBoundedQueue capacity_bytes must be positive");
        }
    }

    ByteBoundedQueue(const ByteBoundedQueue&) = delete;
    ByteBoundedQueue& operator=(const ByteBoundedQueue&) = delete;

    QueueOperationStatus push(T value, std::size_t charged_bytes) {
        if (charged_bytes == 0) {
            return QueueOperationStatus::kInvalidByteSize;
        }
        if (charged_bytes > capacity_bytes_) {
            return QueueOperationStatus::kItemTooLarge;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock,
                      [&] { return state_ != QueueState::kOpen || charged_bytes <= capacity_bytes_ - queued_bytes_; });
        if (state_ == QueueState::kCancelled) {
            return QueueOperationStatus::kCancelled;
        }
        if (state_ == QueueState::kClosed) {
            return QueueOperationStatus::kClosed;
        }

        items_.push_back(Item{std::move(value), charged_bytes});
        queued_bytes_ += charged_bytes;
        if (queued_bytes_ > peak_queued_bytes_) {
            peak_queued_bytes_ = queued_bytes_;
        }
        lock.unlock();
        changed_.notify_all();
        return QueueOperationStatus::kSuccess;
    }

    QueuePopResult<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(
            lock, [&] { return state_ == QueueState::kCancelled || !items_.empty() || state_ == QueueState::kClosed; });
        if (state_ == QueueState::kCancelled) {
            return {QueueOperationStatus::kCancelled, std::nullopt, 0};
        }
        if (items_.empty()) {
            return {QueueOperationStatus::kClosed, std::nullopt, 0};
        }

        Item item = std::move(items_.front());
        items_.pop_front();
        queued_bytes_ -= item.charged_bytes;
        lock.unlock();
        changed_.notify_all();
        return {QueueOperationStatus::kSuccess, std::move(item.value), item.charged_bytes};
    }

    // Returns true only for the transition OPEN -> CLOSED.
    bool close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != QueueState::kOpen) {
            return false;
        }
        state_ = QueueState::kClosed;
        changed_.notify_all();
        return true;
    }

    // Returns true only for the first transition to CANCELLED.
    bool cancel(std::string reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == QueueState::kCancelled) {
            return false;
        }
        state_ = QueueState::kCancelled;
        cancellation_reason_ = reason.empty() ? "cancelled without a reason" : std::move(reason);
        items_.clear();
        queued_bytes_ = 0;
        changed_.notify_all();
        return true;
    }

    QueueSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return QueueSnapshot{
            state_, capacity_bytes_, queued_bytes_, peak_queued_bytes_, items_.size(), cancellation_reason_,
        };
    }

   private:
    struct Item {
        T value;
        std::size_t charged_bytes;
    };

    const std::size_t capacity_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> items_;
    QueueState state_ = QueueState::kOpen;
    std::size_t queued_bytes_ = 0;
    std::size_t peak_queued_bytes_ = 0;
    std::string cancellation_reason_;
};

}  // namespace longlineage::runtime
