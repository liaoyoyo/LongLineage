// SPDX-License-Identifier: GPL-3.0-only

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "longlineage/artifact/run_root.hpp"

namespace {

using longlineage::artifact::RunRootObservedState;
using longlineage::artifact::RunRootStatus;
using longlineage::artifact::RunRootTransaction;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScratchDirectory final {
   public:
    ScratchDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("longlineage-run-root-" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        check(std::filesystem::create_directories(path_), "cannot create run-root scratch directory");
    }

    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

   private:
    std::filesystem::path path_;
};

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "cannot reopen published run receipt");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_exclusive_staging_and_recovery(const std::filesystem::path& output_base) {
    auto created = RunRootTransaction::create_exclusive(output_base, "recovery-run");
    check(created.result.ok() && created.transaction != nullptr, "exclusive staging creation must succeed");
    check(std::filesystem::is_directory(created.transaction->staging_root()), "staging root must exist");

    auto inspection = RunRootTransaction::inspect(output_base, "recovery-run");
    check(inspection.state == RunRootObservedState::kStaging && !inspection.query_visible,
          "staging root must not be query-visible");

    auto duplicate = RunRootTransaction::create_exclusive(output_base, "recovery-run");
    check(duplicate.result.status == RunRootStatus::kAlreadyExists && duplicate.transaction == nullptr,
          "second exclusive staging claim must fail");

    const std::string receipt = "{\"schema_name\":\"longlineage.run_receipt\",\"state\":\"VALIDATED_FROZEN\"}\n";
    const auto prepared = created.transaction->prepare_run_receipt(receipt);
    check(prepared.ok() && prepared.run_receipt_sha256.size() == 64U, "pending receipt must be written and hashed");
    inspection = RunRootTransaction::inspect(output_base, "recovery-run");
    check(inspection.state == RunRootObservedState::kStaging && !inspection.query_visible,
          "prepared staging receipt must remain invisible");

    const auto renamed = created.transaction->rename_staging_to_final();
    check(renamed.ok(), "staging directory rename must succeed");
    inspection = RunRootTransaction::inspect(output_base, "recovery-run");
    check(inspection.state == RunRootObservedState::kFinalUnpublished && !inspection.query_visible,
          "crash-after-rename state must remain query-invisible");
    check(!std::filesystem::exists(inspection.final_root / "run_receipt.json"),
          "run_receipt.json must not exist before publication");

    created.transaction.reset();
    const auto wrong_recovery =
        RunRootTransaction::recover_after_rename(output_base, "recovery-run", std::string(64U, '0'));
    check(wrong_recovery.status == RunRootStatus::kStateConflict,
          "receipt-only recovery must be disabled without independent replay");
    check(!RunRootTransaction::inspect(output_base, "recovery-run").query_visible,
          "failed recovery must not make the root visible");

    const auto recovered =
        RunRootTransaction::recover_after_rename(output_base, "recovery-run", prepared.run_receipt_sha256);
    check(recovered.status == RunRootStatus::kStateConflict,
          "even an exact receipt digest must not bypass independent replay");
    inspection = RunRootTransaction::inspect(output_base, "recovery-run");
    check(inspection.state == RunRootObservedState::kFinalUnpublished && !inspection.query_visible,
          "disabled recovery must keep final root query-invisible");
    check(!std::filesystem::exists(inspection.final_root / "run_receipt.json"),
          "disabled recovery must not publish receipt bytes");

    const auto repeated =
        RunRootTransaction::recover_after_rename(output_base, "recovery-run", prepared.run_receipt_sha256);
    check(repeated.status == RunRootStatus::kStateConflict,
          "receipt-only recovery must remain fail-closed on repeated attempts");
}

void test_one_call_freeze(const std::filesystem::path& output_base) {
    auto created = RunRootTransaction::create_exclusive(output_base, "freeze-run");
    check(created.result.ok() && created.transaction != nullptr, "freeze staging creation must succeed");
    const std::string receipt = "{\"state\":\"VALIDATED_FROZEN\"}\n";
    const auto frozen = created.transaction->freeze(receipt);
    check(frozen.ok(), "one-call freeze must complete");
    const auto inspection = RunRootTransaction::inspect(output_base, "freeze-run");
    check(inspection.state == RunRootObservedState::kPublished && inspection.query_visible,
          "one-call freeze must publish exactly one visible final root");
    check(!std::filesystem::exists(output_base / ".staging" / "freeze-run"),
          "atomic rename must remove the staging pathname");
    check(read_all(inspection.final_root / "run_receipt.json") == receipt,
          "one-call freeze must preserve exact receipt bytes");
}

void test_rejections(const std::filesystem::path& output_base) {
    auto invalid = RunRootTransaction::create_exclusive(output_base, "../escape");
    check(invalid.result.status == RunRootStatus::kInvalidArgument,
          "parent traversal in run_id must fail before filesystem mutation");

    std::filesystem::create_directory(output_base / "preexisting");
    auto preexisting = RunRootTransaction::create_exclusive(output_base, "preexisting");
    check(preexisting.result.status == RunRootStatus::kAlreadyExists,
          "pre-existing final root must reject staging creation");

    const auto real_base = output_base / "real-base";
    const auto alias_base = output_base / "alias-base";
    std::filesystem::create_directory(real_base);
    std::filesystem::create_directory_symlink(real_base, alias_base);
    auto symlinked = RunRootTransaction::create_exclusive(alias_base, "unsafe");
    check(symlinked.result.status == RunRootStatus::kPathUnsafe, "symlinked output base must fail closed");

    struct stat tmp_status {};
    struct stat shm_status {};
    if (::stat("/tmp", &tmp_status) == 0 && ::stat("/dev/shm", &shm_status) == 0 &&
        tmp_status.st_dev != shm_status.st_dev) {
        const auto cross_device = RunRootTransaction::verify_same_device("/dev/shm", "/tmp");
        check(cross_device.status == RunRootStatus::kDifferentDevice,
              "cross-device publication must reject silent copy fallback");
    }
}

}  // namespace

int main() {
    try {
        ScratchDirectory scratch;
        test_exclusive_staging_and_recovery(scratch.path());
        test_one_call_freeze(scratch.path());
        test_rejections(scratch.path());
        std::cout << "run-root transaction tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "run-root transaction test failure: " << error.what() << '\n';
        return 1;
    }
}
