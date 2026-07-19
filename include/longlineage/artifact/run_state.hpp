// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <mutex>
#include <string>
#include <string_view>

namespace longlineage::artifact {

enum class RunState {
    kRunning,
    kFailed,
    kValidated,
    kValidatedFrozen,
};

const char* to_string(RunState state) noexcept;

struct ValidationEvidence {
    bool all_pass = false;
    std::string producer_receipt_sha256;
    std::string validator_receipt_sha256;
    std::string validator_executable_sha256;
};

struct FreezeEvidence {
    bool atomic_rename_completed = false;
    std::string final_root;
};

struct RunTransitionResult {
    bool accepted = false;
    RunState previous = RunState::kRunning;
    RunState current = RunState::kRunning;
    std::string reason;
};

// Enforces RUNNING -> FAILED or RUNNING -> VALIDATED -> VALIDATED_FROZEN.
class RunStateGuard {
   public:
    RunStateGuard() = default;

    RunTransitionResult mark_failed(std::string failure_reason);
    RunTransitionResult mark_validated(const ValidationEvidence& evidence);
    RunTransitionResult mark_frozen(const FreezeEvidence& evidence);

    RunState state() const;
    std::string failure_reason() const;
    std::string producer_receipt_sha256() const;
    std::string validator_receipt_sha256() const;
    std::string validator_executable_sha256() const;
    std::string final_root() const;

   private:
    static bool is_sha256(std::string_view value) noexcept;
    RunTransitionResult reject(const std::string& reason) const;

    mutable std::mutex mutex_;
    RunState state_ = RunState::kRunning;
    std::string failure_reason_;
    std::string producer_receipt_sha256_;
    std::string validator_receipt_sha256_;
    std::string validator_executable_sha256_;
    std::string final_root_;
};

}  // namespace longlineage::artifact
