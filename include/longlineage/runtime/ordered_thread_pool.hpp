// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "longlineage/runtime/byte_bounded_queue.hpp"

namespace longlineage::runtime {

enum class PoolStatus {
    kSuccess,
    kCancelled,
    kWorkerError,
    kSubmissionClosed,
    kItemTooLarge,
    kInvalidByteSize,
    kInternalInvariantError,
};

inline const char* to_string(PoolStatus status) noexcept {
    switch (status) {
        case PoolStatus::kSuccess:
            return "SUCCESS";
        case PoolStatus::kCancelled:
            return "CANCELLED";
        case PoolStatus::kWorkerError:
            return "WORKER_ERROR";
        case PoolStatus::kSubmissionClosed:
            return "SUBMISSION_CLOSED";
        case PoolStatus::kItemTooLarge:
            return "ITEM_TOO_LARGE";
        case PoolStatus::kInvalidByteSize:
            return "INVALID_BYTE_SIZE";
        case PoolStatus::kInternalInvariantError:
            return "INTERNAL_INVARIANT_ERROR";
    }
    return "UNKNOWN";
}

struct PoolSubmitResult {
    PoolStatus status = PoolStatus::kSubmissionClosed;
    std::optional<std::uint64_t> sequence;
    std::string message;
};

template <typename Result>
struct OrderedBatchResult {
    PoolStatus status = PoolStatus::kInternalInvariantError;
    std::vector<Result> ordered_results;
    std::optional<std::uint64_t> failed_sequence;
    std::string message;
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::size_t peak_queued_bytes = 0;
};

// One bounded thread pool with deterministic batch publication.
//
// Tasks may execute and finish out of order. finish() publishes results in
// submission order only when every submitted task succeeded. On any worker
// exception, queued tasks are cancelled and the returned result vector is empty,
// preventing accidental publication of a partial batch.
template <typename Result>
class OrderedThreadPool {
   public:
    using Work = std::function<Result()>;

    OrderedThreadPool(std::size_t worker_count, std::size_t queue_capacity_bytes) : tasks_(queue_capacity_bytes) {
        if (worker_count == 0) {
            throw std::invalid_argument("OrderedThreadPool worker_count must be positive");
        }
        workers_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    OrderedThreadPool(const OrderedThreadPool&) = delete;
    OrderedThreadPool& operator=(const OrderedThreadPool&) = delete;

    ~OrderedThreadPool() {
        if (!joined_) {
            cancel("thread pool destroyed before finish()");
            join_workers();
        }
    }

    PoolSubmitResult submit(std::size_t charged_bytes, Work work) {
        if (!work) {
            return {PoolStatus::kInternalInvariantError, std::nullopt, "task callback is empty"};
        }

        std::lock_guard<std::mutex> submit_lock(submit_mutex_);
        if (finish_started_) {
            return {PoolStatus::kSubmissionClosed, std::nullopt, "finish() has started"};
        }

        const std::uint64_t sequence = next_sequence_;
        const QueueOperationStatus queue_status = tasks_.push(Task{sequence, std::move(work)}, charged_bytes);
        if (queue_status != QueueOperationStatus::kSuccess) {
            return {map_queue_status(queue_status), std::nullopt, to_string(queue_status)};
        }
        ++next_sequence_;
        submitted_.fetch_add(1, std::memory_order_relaxed);
        return {PoolStatus::kSuccess, sequence, {}};
    }

    bool cancel(std::string reason) {
        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            if (manual_cancellation_reason_.empty()) {
                manual_cancellation_reason_ = reason.empty() ? "cancelled without a reason" : reason;
            }
        }
        return tasks_.cancel(std::move(reason));
    }

    OrderedBatchResult<Result> finish() {
        {
            std::lock_guard<std::mutex> submit_lock(submit_mutex_);
            if (finish_started_) {
                throw std::logic_error("OrderedThreadPool::finish may be called only once");
            }
            finish_started_ = true;
            tasks_.close();
        }
        join_workers();

        OrderedBatchResult<Result> batch;
        batch.submitted = submitted_.load(std::memory_order_relaxed);
        batch.completed = completed_.load(std::memory_order_relaxed);
        batch.peak_queued_bytes = tasks_.snapshot().peak_queued_bytes;

        std::lock_guard<std::mutex> result_lock(result_mutex_);
        if (first_worker_error_sequence_.has_value()) {
            batch.status = PoolStatus::kWorkerError;
            batch.failed_sequence = first_worker_error_sequence_;
            batch.message = first_worker_error_message_;
            return batch;
        }
        if (!manual_cancellation_reason_.empty() || tasks_.snapshot().state == QueueState::kCancelled) {
            batch.status = PoolStatus::kCancelled;
            batch.message = manual_cancellation_reason_.empty() ? tasks_.snapshot().cancellation_reason
                                                                : manual_cancellation_reason_;
            return batch;
        }
        if (results_.size() != batch.submitted || batch.completed != batch.submitted) {
            batch.status = PoolStatus::kInternalInvariantError;
            batch.message = "completed result count differs from submitted task count";
            return batch;
        }

        batch.ordered_results.reserve(batch.submitted);
        for (std::uint64_t sequence = 0; sequence < batch.submitted; ++sequence) {
            auto result = results_.find(sequence);
            if (result == results_.end()) {
                batch.ordered_results.clear();
                batch.status = PoolStatus::kInternalInvariantError;
                batch.message = "ordered result sequence contains a gap";
                return batch;
            }
            batch.ordered_results.push_back(std::move(result->second));
        }
        batch.status = PoolStatus::kSuccess;
        return batch;
    }

    QueueSnapshot queue_stats() const { return tasks_.snapshot(); }

   private:
    struct Task {
        std::uint64_t sequence;
        Work work;
    };

    static PoolStatus map_queue_status(QueueOperationStatus status) noexcept {
        switch (status) {
            case QueueOperationStatus::kSuccess:
                return PoolStatus::kSuccess;
            case QueueOperationStatus::kClosed:
                return PoolStatus::kSubmissionClosed;
            case QueueOperationStatus::kCancelled:
                return PoolStatus::kCancelled;
            case QueueOperationStatus::kItemTooLarge:
                return PoolStatus::kItemTooLarge;
            case QueueOperationStatus::kInvalidByteSize:
                return PoolStatus::kInvalidByteSize;
        }
        return PoolStatus::kInternalInvariantError;
    }

    void worker_loop() noexcept {
        while (true) {
            QueuePopResult<Task> next = tasks_.pop();
            if (next.status != QueueOperationStatus::kSuccess) {
                return;
            }
            try {
                Result result = (*next.value).work();
                bool inserted = false;
                {
                    std::lock_guard<std::mutex> result_lock(result_mutex_);
                    inserted = results_.emplace((*next.value).sequence, std::move(result)).second;
                }
                if (!inserted) {
                    record_worker_failure((*next.value).sequence, "duplicate result sequence");
                    return;
                }
                completed_.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& error) {
                record_worker_failure((*next.value).sequence, error.what());
                return;
            } catch (...) {
                record_worker_failure((*next.value).sequence, "non-standard worker exception");
                return;
            }
        }
    }

    void record_worker_failure(std::uint64_t sequence, std::string message) noexcept {
        {
            std::lock_guard<std::mutex> result_lock(result_mutex_);
            if (!first_worker_error_sequence_.has_value()) {
                first_worker_error_sequence_ = sequence;
                first_worker_error_message_ = message.empty() ? "worker failed without a message" : std::move(message);
            }
        }
        tasks_.cancel("worker error");
    }

    void join_workers() noexcept {
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        joined_ = true;
    }

    ByteBoundedQueue<Task> tasks_;
    std::vector<std::thread> workers_;
    mutable std::mutex submit_mutex_;
    mutable std::mutex result_mutex_;
    std::map<std::uint64_t, Result> results_;
    std::atomic<std::size_t> submitted_{0};
    std::atomic<std::size_t> completed_{0};
    std::uint64_t next_sequence_ = 0;
    bool finish_started_ = false;
    bool joined_ = false;
    std::optional<std::uint64_t> first_worker_error_sequence_;
    std::string first_worker_error_message_;
    std::string manual_cancellation_reason_;
};

}  // namespace longlineage::runtime
