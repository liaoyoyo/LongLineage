// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace longlineage::runtime {

enum class ReorderSinkStatus {
    kSuccess,
    kClosed,
    kCancelled,
    kFailed,
    kDuplicateSequence,
    kLateSequence,
    kItemTooLarge,
    kInvalidByteSize,
    kGapAtClose,
    kEmitError,
};

inline const char* to_string(ReorderSinkStatus status) noexcept {
    switch (status) {
        case ReorderSinkStatus::kSuccess:
            return "SUCCESS";
        case ReorderSinkStatus::kClosed:
            return "CLOSED";
        case ReorderSinkStatus::kCancelled:
            return "CANCELLED";
        case ReorderSinkStatus::kFailed:
            return "FAILED";
        case ReorderSinkStatus::kDuplicateSequence:
            return "DUPLICATE_SEQUENCE";
        case ReorderSinkStatus::kLateSequence:
            return "LATE_SEQUENCE";
        case ReorderSinkStatus::kItemTooLarge:
            return "ITEM_TOO_LARGE";
        case ReorderSinkStatus::kInvalidByteSize:
            return "INVALID_BYTE_SIZE";
        case ReorderSinkStatus::kGapAtClose:
            return "GAP_AT_CLOSE";
        case ReorderSinkStatus::kEmitError:
            return "EMIT_ERROR";
    }
    return "FAILED";
}

enum class ReorderSinkState {
    kOpen,
    kClosed,
    kCancelled,
    kFailed,
};

struct ReorderPublishResult {
    ReorderSinkStatus status = ReorderSinkStatus::kFailed;
    std::size_t emitted_items = 0;
    std::string message;
};

struct ReorderSinkSnapshot {
    ReorderSinkState state = ReorderSinkState::kOpen;
    std::size_t capacity_bytes = 0;
    std::size_t max_regular_item_bytes = 0;
    std::size_t buffered_bytes = 0;
    std::size_t peak_buffered_bytes = 0;
    std::size_t peak_retained_bytes = 0;
    std::size_t buffered_items = 0;
    std::uint64_t next_expected_sequence = 0;
    std::size_t emitted_items = 0;
    std::string terminal_reason;
};

// A deterministic result sink whose admitted out-of-order payloads are bounded
// by a caller-defined logical retained-byte charge.
//
// Non-frontier items may consume at most capacity - max_regular_item_bytes.
// The reserved frontier credit prevents a full out-of-order buffer from blocking
// the next expected sequence forever. Every item larger than
// max_regular_item_bytes is rejected regardless of completion order. Therefore
// scheduling cannot decide whether identical logical data succeeds or fails.
// T::retained_bytes() is the sole noexcept sizing authority. This contract does
// not bound allocator overhead, HTSlib/BGZF state, thread stacks, task queues,
// callback-local payloads, or process RSS; those require a production-wide
// permit pool and separate measurement.
//
// The emit callback is serialized under the sink mutex and must not call back
// into this object. A callback exception fails the sink and discards buffered
// results. Already emitted rows remain staging data and are never completion
// evidence until close(expected_count) succeeds.
template <typename T>
class ByteBoundedReorderSink {
   public:
    using Emit = std::function<void(std::uint64_t, T&&)>;

    ByteBoundedReorderSink(std::size_t capacity_bytes, std::size_t max_regular_item_bytes, Emit emit)
        : capacity_bytes_(capacity_bytes), max_regular_item_bytes_(max_regular_item_bytes), emit_(std::move(emit)) {
        static_assert(noexcept(std::declval<const T&>().retained_bytes()),
                      "reorder result type must provide noexcept retained_bytes()");
        if (capacity_bytes_ == 0) {
            throw std::invalid_argument("ByteBoundedReorderSink capacity_bytes must be positive");
        }
        if (max_regular_item_bytes_ == 0 || max_regular_item_bytes_ > capacity_bytes_) {
            throw std::invalid_argument(
                "ByteBoundedReorderSink max_regular_item_bytes must be within [1, capacity_bytes]");
        }
        if (!emit_) {
            throw std::invalid_argument("ByteBoundedReorderSink emit callback must be present");
        }
    }

    ByteBoundedReorderSink(const ByteBoundedReorderSink&) = delete;
    ByteBoundedReorderSink& operator=(const ByteBoundedReorderSink&) = delete;

    ReorderPublishResult publish(std::uint64_t sequence, T value) {
        const std::size_t charged_bytes = value.retained_bytes();
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ != ReorderSinkState::kOpen) {
            return terminal_result_locked();
        }
        if (charged_bytes == 0) {
            fail_locked("result retained-byte size must be positive");
            changed_.notify_all();
            return {ReorderSinkStatus::kInvalidByteSize, 0, terminal_reason_};
        }
        if (charged_bytes > max_regular_item_bytes_) {
            fail_locked("result retained-byte size exceeds the deterministic per-item ceiling: " +
                        std::to_string(charged_bytes));
            changed_.notify_all();
            return {ReorderSinkStatus::kItemTooLarge, 0, terminal_reason_};
        }

        changed_.wait(lock, [&] {
            if (state_ != ReorderSinkState::kOpen || sequence < next_expected_sequence_ ||
                pending_.count(sequence) != 0U) {
                return true;
            }
            if (sequence == next_expected_sequence_) {
                return charged_bytes <= capacity_bytes_ - buffered_bytes_;
            }
            const std::size_t non_frontier_capacity = capacity_bytes_ - max_regular_item_bytes_;
            return buffered_bytes_ <= non_frontier_capacity && charged_bytes <= non_frontier_capacity - buffered_bytes_;
        });

        if (state_ != ReorderSinkState::kOpen) {
            return terminal_result_locked();
        }
        if (sequence < next_expected_sequence_) {
            fail_locked("result sequence was already emitted: " + std::to_string(sequence));
            changed_.notify_all();
            return {ReorderSinkStatus::kLateSequence, 0, terminal_reason_};
        }
        if (pending_.count(sequence) != 0U) {
            fail_locked("result sequence is already buffered: " + std::to_string(sequence));
            changed_.notify_all();
            return {ReorderSinkStatus::kDuplicateSequence, 0, terminal_reason_};
        }

        const bool inserted = pending_.emplace(sequence, Pending{std::move(value), charged_bytes}).second;
        if (!inserted) {
            fail_locked("result sequence insertion duplicated: " + std::to_string(sequence));
            changed_.notify_all();
            return {ReorderSinkStatus::kDuplicateSequence, 0, terminal_reason_};
        }
        buffered_bytes_ += charged_bytes;
        if (buffered_bytes_ > peak_buffered_bytes_) {
            peak_buffered_bytes_ = buffered_bytes_;
        }
        if (buffered_bytes_ > peak_retained_bytes_) {
            peak_retained_bytes_ = buffered_bytes_;
        }
        const ReorderPublishResult drained = drain_frontier_locked();
        changed_.notify_all();
        return drained;
    }

    ReorderPublishResult close(std::uint64_t expected_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ReorderSinkState::kOpen) {
            return terminal_result_locked();
        }
        if (next_expected_sequence_ != expected_count || !pending_.empty()) {
            fail_locked("close observed a missing, extra, or buffered result sequence");
            changed_.notify_all();
            return {ReorderSinkStatus::kGapAtClose, 0, terminal_reason_};
        }
        state_ = ReorderSinkState::kClosed;
        terminal_reason_ = "all expected result sequences emitted";
        changed_.notify_all();
        return {ReorderSinkStatus::kSuccess, 0, {}};
    }

    bool cancel(std::string reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ReorderSinkState::kOpen) {
            return false;
        }
        state_ = ReorderSinkState::kCancelled;
        terminal_reason_ = reason.empty() ? "cancelled without a reason" : std::move(reason);
        pending_.clear();
        buffered_bytes_ = 0;
        changed_.notify_all();
        return true;
    }

    [[nodiscard]] ReorderSinkSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ReorderSinkSnapshot{state_,          capacity_bytes_,         max_regular_item_bytes_,
                                   buffered_bytes_, peak_buffered_bytes_,    peak_retained_bytes_,
                                   pending_.size(), next_expected_sequence_, emitted_items_,
                                   terminal_reason_};
    }

   private:
    struct Pending {
        T value;
        std::size_t charged_bytes;
    };

    ReorderPublishResult drain_frontier_locked() {
        std::size_t emitted_now = 0;
        while (true) {
            auto next = pending_.find(next_expected_sequence_);
            if (next == pending_.end()) {
                break;
            }
            Pending item = std::move(next->second);
            pending_.erase(next);
            buffered_bytes_ -= item.charged_bytes;
            try {
                emit_(next_expected_sequence_, std::move(item.value));
            } catch (const std::exception& error) {
                fail_locked(error.what());
                return {ReorderSinkStatus::kEmitError, emitted_now, terminal_reason_};
            } catch (...) {
                fail_locked("emit callback raised a non-standard exception");
                return {ReorderSinkStatus::kEmitError, emitted_now, terminal_reason_};
            }
            ++next_expected_sequence_;
            ++emitted_items_;
            ++emitted_now;
        }
        return {ReorderSinkStatus::kSuccess, emitted_now, {}};
    }

    void fail_locked(std::string reason) {
        state_ = ReorderSinkState::kFailed;
        terminal_reason_ = reason.empty() ? "reorder sink failed without a reason" : std::move(reason);
        pending_.clear();
        buffered_bytes_ = 0;
    }

    [[nodiscard]] ReorderPublishResult terminal_result_locked() const {
        switch (state_) {
            case ReorderSinkState::kOpen:
                return {ReorderSinkStatus::kSuccess, 0, {}};
            case ReorderSinkState::kClosed:
                return {ReorderSinkStatus::kClosed, 0, terminal_reason_};
            case ReorderSinkState::kCancelled:
                return {ReorderSinkStatus::kCancelled, 0, terminal_reason_};
            case ReorderSinkState::kFailed:
                return {ReorderSinkStatus::kFailed, 0, terminal_reason_};
        }
        return {ReorderSinkStatus::kFailed, 0, "unknown terminal state"};
    }

    const std::size_t capacity_bytes_;
    const std::size_t max_regular_item_bytes_;
    Emit emit_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::uint64_t, Pending> pending_;
    ReorderSinkState state_ = ReorderSinkState::kOpen;
    std::size_t buffered_bytes_ = 0;
    std::size_t peak_buffered_bytes_ = 0;
    std::size_t peak_retained_bytes_ = 0;
    std::uint64_t next_expected_sequence_ = 0;
    std::size_t emitted_items_ = 0;
    std::string terminal_reason_;
};

}  // namespace longlineage::runtime
