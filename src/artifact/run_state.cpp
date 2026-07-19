// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/artifact/run_state.hpp"

#include <utility>

namespace longlineage::artifact {

const char* to_string(RunState state) noexcept {
    switch (state) {
        case RunState::kRunning:
            return "RUNNING";
        case RunState::kFailed:
            return "FAILED";
        case RunState::kValidated:
            return "VALIDATED";
        case RunState::kValidatedFrozen:
            return "VALIDATED_FROZEN";
    }
    return "UNKNOWN";
}

bool RunStateGuard::is_sha256(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (char character : value) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

RunTransitionResult RunStateGuard::reject(const std::string& reason) const { return {false, state_, state_, reason}; }

RunTransitionResult RunStateGuard::mark_failed(std::string failure_reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RunState::kRunning) {
        return reject("FAILED is legal only from RUNNING");
    }
    if (failure_reason.empty()) {
        return reject("FAILED requires a non-empty failure reason");
    }
    const RunState previous = state_;
    state_ = RunState::kFailed;
    failure_reason_ = std::move(failure_reason);
    return {true, previous, state_, {}};
}

RunTransitionResult RunStateGuard::mark_validated(const ValidationEvidence& evidence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RunState::kRunning) {
        return reject("VALIDATED is legal only from RUNNING");
    }
    if (!evidence.all_pass) {
        return reject("VALIDATED requires independent validator all_pass=true");
    }
    if (!is_sha256(evidence.producer_receipt_sha256)) {
        return reject("VALIDATED requires a lowercase 64-hex producer receipt SHA-256");
    }
    if (!is_sha256(evidence.validator_receipt_sha256)) {
        return reject("VALIDATED requires a lowercase 64-hex validator receipt SHA-256");
    }
    if (!is_sha256(evidence.validator_executable_sha256)) {
        return reject("VALIDATED requires a lowercase 64-hex validator executable SHA-256");
    }
    const RunState previous = state_;
    state_ = RunState::kValidated;
    producer_receipt_sha256_ = evidence.producer_receipt_sha256;
    validator_receipt_sha256_ = evidence.validator_receipt_sha256;
    validator_executable_sha256_ = evidence.validator_executable_sha256;
    return {true, previous, state_, {}};
}

RunTransitionResult RunStateGuard::mark_frozen(const FreezeEvidence& evidence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RunState::kValidated) {
        return reject("VALIDATED_FROZEN is legal only from VALIDATED");
    }
    if (!evidence.atomic_rename_completed) {
        return reject("VALIDATED_FROZEN requires a completed atomic rename");
    }
    if (evidence.final_root.empty()) {
        return reject("VALIDATED_FROZEN requires a non-empty final root");
    }
    const RunState previous = state_;
    state_ = RunState::kValidatedFrozen;
    final_root_ = evidence.final_root;
    return {true, previous, state_, {}};
}

RunState RunStateGuard::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string RunStateGuard::failure_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_reason_;
}

std::string RunStateGuard::producer_receipt_sha256() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return producer_receipt_sha256_;
}

std::string RunStateGuard::validator_receipt_sha256() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return validator_receipt_sha256_;
}

std::string RunStateGuard::validator_executable_sha256() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return validator_executable_sha256_;
}

std::string RunStateGuard::final_root() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return final_root_;
}

}  // namespace longlineage::artifact
