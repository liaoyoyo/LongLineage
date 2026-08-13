// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace longlineage::artifact {

enum class RunRootStatus {
    kSuccess,
    kInvalidArgument,
    kPathUnsafe,
    kAlreadyExists,
    kNotFound,
    kDifferentDevice,
    kStateConflict,
    kDigestMismatch,
    kIoError,
};

const char* to_string(RunRootStatus status) noexcept;

struct RunRootResult {
    RunRootStatus status = RunRootStatus::kIoError;
    std::string message;
    std::string run_receipt_sha256;

    [[nodiscard]] bool ok() const noexcept { return status == RunRootStatus::kSuccess; }
};

enum class RunRootObservedState {
    kAbsent,
    kStaging,
    kFinalUnpublished,
    kPublished,
    kConflict,
};

const char* to_string(RunRootObservedState state) noexcept;

struct RunRootInspection {
    RunRootObservedState state = RunRootObservedState::kConflict;
    std::filesystem::path staging_root;
    std::filesystem::path final_root;
    bool query_visible = false;
    std::string message;
};

class RunRootTransaction;

struct RunRootCreateResult {
    RunRootResult result;
    std::unique_ptr<RunRootTransaction> transaction;
};

// Owns the publication mechanics for one run root.
//
// The transaction never removes a failed or incomplete root. A run becomes
// query-visible only after the final directory contains run_receipt.json.
class RunRootTransaction final {
   public:
    static RunRootCreateResult create_exclusive(const std::filesystem::path& output_base, std::string run_id);
    // Attaches to a producer-closed staging root after independent validation.
    // The staging directory must already exist, the final root must be absent,
    // and neither pending nor published final receipt may exist.
    static RunRootCreateResult open_existing_staging(const std::filesystem::path& output_base, std::string run_id);
    static RunRootInspection inspect(const std::filesystem::path& output_base, std::string_view run_id);
    static RunRootResult verify_same_device(const std::filesystem::path& source,
                                            const std::filesystem::path& destination_parent);
    // Deliberately fail-closed until recovery is routed through the independent
    // validator's full input/artifact publication replay.
    static RunRootResult recover_after_rename(const std::filesystem::path& output_base, std::string_view run_id,
                                              std::string_view expected_run_receipt_sha256);

    RunRootTransaction(const RunRootTransaction&) = delete;
    RunRootTransaction& operator=(const RunRootTransaction&) = delete;
    RunRootTransaction(RunRootTransaction&&) = delete;
    RunRootTransaction& operator=(RunRootTransaction&&) = delete;
    ~RunRootTransaction() = default;

    RunRootResult prepare_run_receipt(std::string_view canonical_receipt_bytes);
    RunRootResult rename_staging_to_final();
    RunRootResult publish_run_receipt(std::string_view expected_run_receipt_sha256);
    RunRootResult freeze(std::string_view canonical_receipt_bytes);

    [[nodiscard]] const std::filesystem::path& staging_root() const noexcept { return staging_root_; }
    [[nodiscard]] const std::filesystem::path& final_root() const noexcept { return final_root_; }

   private:
    enum class LocalState {
        kStaging,
        kPrepared,
        kRenamed,
        kPublished,
    };

    RunRootTransaction(std::filesystem::path output_base, std::string run_id, LocalState state);

    std::filesystem::path output_base_;
    std::string run_id_;
    std::filesystem::path staging_parent_;
    std::filesystem::path staging_root_;
    std::filesystem::path final_root_;
    LocalState state_ = LocalState::kStaging;
    std::string prepared_receipt_sha256_;
};

}  // namespace longlineage::artifact
