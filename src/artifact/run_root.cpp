// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/artifact/run_root.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace longlineage::artifact {
namespace {

constexpr const char* kPendingReceiptName = ".run_receipt.json.pending";
constexpr const char* kPublishedReceiptName = "run_receipt.json";

RunRootResult success(std::string message, std::string digest = {}) {
    return {RunRootStatus::kSuccess, std::move(message), std::move(digest)};
}

RunRootResult failure(RunRootStatus status, std::string message) { return {status, std::move(message), {}}; }

std::string errno_message(const std::string& operation, const std::filesystem::path& path) {
    return operation + " failed for " + path.string() + ": " + std::strerror(errno);
}

bool valid_run_id(std::string_view run_id) noexcept {
    if (run_id.empty() || run_id.size() > 128U) {
        return false;
    }
    for (const char raw_character : run_id) {
        const auto character = static_cast<unsigned char>(raw_character);
        const bool allowed = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                             character == '-';
        if (!allowed) {
            return false;
        }
    }
    const unsigned char first = static_cast<unsigned char>(run_id.front());
    return (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
}

bool valid_sha256(std::string_view digest) noexcept {
    if (digest.size() != 64U) {
        return false;
    }
    for (const char raw_character : digest) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

class FileDescriptor final {
   public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

   private:
    int value_;
};

RunRootResult fsync_directory(const std::filesystem::path& path) {
    FileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.valid()) {
        return failure(RunRootStatus::kIoError, errno_message("open directory", path));
    }
    if (::fsync(descriptor.get()) != 0) {
        return failure(RunRootStatus::kIoError, errno_message("fsync directory", path));
    }
    return success("directory fsync completed");
}

RunRootResult require_real_directory(const std::filesystem::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        return failure(errno == ENOENT ? RunRootStatus::kNotFound : RunRootStatus::kIoError,
                       errno_message("lstat", path));
    }
    if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode)) {
        return failure(RunRootStatus::kPathUnsafe, "path is not a real directory: " + path.string());
    }
    return success("real directory verified");
}

enum class PathKind {
    kAbsent,
    kRegular,
    kDirectory,
    kOther,
    kError,
};

PathKind path_kind(const std::filesystem::path& path, std::string& detail) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) {
            return PathKind::kAbsent;
        }
        detail = errno_message("lstat", path);
        return PathKind::kError;
    }
    if (S_ISREG(status.st_mode)) {
        return PathKind::kRegular;
    }
    if (S_ISDIR(status.st_mode)) {
        return PathKind::kDirectory;
    }
    detail = "path is a symlink or unsupported file type: " + path.string();
    return PathKind::kOther;
}

std::string sha256_bytes(std::string_view bytes) {
    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        (!bytes.empty() && EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1)) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 || digest_size != 32U) {
        return {};
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(64U);
    for (unsigned int index = 0; index < digest_size; ++index) {
        const unsigned char byte = digest[index];
        encoded.push_back(kHex[(byte >> 4U) & 0x0fU]);
        encoded.push_back(kHex[byte & 0x0fU]);
    }
    return encoded;
}

RunRootResult sha256_file(const std::filesystem::path& path, std::string& digest) {
    FileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.valid()) {
        return failure(RunRootStatus::kIoError, errno_message("open receipt", path));
    }
    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return failure(RunRootStatus::kIoError, "cannot initialize receipt SHA-256");
    }
    std::array<char, 65536> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failure(RunRootStatus::kIoError, errno_message("read receipt", path));
        }
        if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
            return failure(RunRootStatus::kIoError, "cannot update receipt SHA-256");
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> raw{};
    unsigned int raw_size = 0;
    if (EVP_DigestFinal_ex(context.get(), raw.data(), &raw_size) != 1 || raw_size != 32U) {
        return failure(RunRootStatus::kIoError, "cannot finalize receipt SHA-256");
    }
    constexpr char kHex[] = "0123456789abcdef";
    digest.clear();
    digest.reserve(64U);
    for (unsigned int index = 0; index < raw_size; ++index) {
        const unsigned char byte = raw[index];
        digest.push_back(kHex[(byte >> 4U) & 0x0fU]);
        digest.push_back(kHex[byte & 0x0fU]);
    }
    return success("receipt SHA-256 recomputed", digest);
}

RunRootResult write_exclusive_file(const std::filesystem::path& path, std::string_view bytes) {
    FileDescriptor descriptor(
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
    if (!descriptor.valid()) {
        return failure(errno == EEXIST ? RunRootStatus::kAlreadyExists : RunRootStatus::kIoError,
                       errno_message("create exclusive file", path));
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failure(RunRootStatus::kIoError, errno_message("write file", path));
        }
        if (written == 0) {
            return failure(RunRootStatus::kIoError, "write returned zero bytes for " + path.string());
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor.get()) != 0) {
        return failure(RunRootStatus::kIoError, errno_message("fsync file", path));
    }
    return success("exclusive file written and fsynced");
}

}  // namespace

const char* to_string(RunRootStatus status) noexcept {
    switch (status) {
        case RunRootStatus::kSuccess:
            return "SUCCESS";
        case RunRootStatus::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case RunRootStatus::kPathUnsafe:
            return "PATH_UNSAFE";
        case RunRootStatus::kAlreadyExists:
            return "ALREADY_EXISTS";
        case RunRootStatus::kNotFound:
            return "NOT_FOUND";
        case RunRootStatus::kDifferentDevice:
            return "DIFFERENT_DEVICE";
        case RunRootStatus::kStateConflict:
            return "STATE_CONFLICT";
        case RunRootStatus::kDigestMismatch:
            return "DIGEST_MISMATCH";
        case RunRootStatus::kIoError:
            return "IO_ERROR";
    }
    return "UNKNOWN";
}

const char* to_string(RunRootObservedState state) noexcept {
    switch (state) {
        case RunRootObservedState::kAbsent:
            return "ABSENT";
        case RunRootObservedState::kStaging:
            return "STAGING";
        case RunRootObservedState::kFinalUnpublished:
            return "FINAL_UNPUBLISHED";
        case RunRootObservedState::kPublished:
            return "PUBLISHED";
        case RunRootObservedState::kConflict:
            return "CONFLICT";
    }
    return "UNKNOWN";
}

RunRootTransaction::RunRootTransaction(std::filesystem::path output_base, std::string run_id, LocalState state)
    : output_base_(std::move(output_base)),
      run_id_(std::move(run_id)),
      staging_parent_(output_base_ / ".staging"),
      staging_root_(staging_parent_ / run_id_),
      final_root_(output_base_ / run_id_),
      state_(state) {}

RunRootCreateResult RunRootTransaction::create_exclusive(const std::filesystem::path& output_base, std::string run_id) {
    if (!valid_run_id(run_id) || output_base.empty() || !output_base.is_absolute()) {
        return {failure(RunRootStatus::kInvalidArgument, "absolute output base and safe run_id are required"), nullptr};
    }
    const std::filesystem::path normalized_base = output_base.lexically_normal();
    const RunRootResult base_check = require_real_directory(normalized_base);
    if (!base_check.ok()) {
        return {base_check, nullptr};
    }

    const std::filesystem::path staging_parent = normalized_base / ".staging";
    if (::mkdir(staging_parent.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
        return {failure(RunRootStatus::kIoError, errno_message("mkdir", staging_parent)), nullptr};
    }
    const RunRootResult parent_check = require_real_directory(staging_parent);
    if (!parent_check.ok()) {
        return {parent_check, nullptr};
    }

    auto transaction = std::unique_ptr<RunRootTransaction>(
        new RunRootTransaction(normalized_base, std::move(run_id), LocalState::kStaging));
    std::string detail;
    if (path_kind(transaction->final_root_, detail) != PathKind::kAbsent) {
        return {
            failure(RunRootStatus::kAlreadyExists,
                    detail.empty() ? "final run root already exists: " + transaction->final_root_.string() : detail),
            nullptr};
    }
    if (::mkdir(transaction->staging_root_.c_str(), S_IRWXU) != 0) {
        return {failure(errno == EEXIST ? RunRootStatus::kAlreadyExists : RunRootStatus::kIoError,
                        errno_message("mkdir exclusive staging root", transaction->staging_root_)),
                nullptr};
    }
    const RunRootResult device_check = verify_same_device(transaction->staging_root_, normalized_base);
    if (!device_check.ok()) {
        return {device_check, nullptr};
    }
    const RunRootResult sync = fsync_directory(staging_parent);
    if (!sync.ok()) {
        return {sync, nullptr};
    }
    return {success("exclusive staging root created"), std::move(transaction)};
}

RunRootCreateResult RunRootTransaction::open_existing_staging(const std::filesystem::path& output_base,
                                                              std::string run_id) {
    if (!valid_run_id(run_id) || output_base.empty() || !output_base.is_absolute()) {
        return {failure(RunRootStatus::kInvalidArgument, "absolute output base and safe run_id are required"), nullptr};
    }
    const std::filesystem::path normalized_base = output_base.lexically_normal();
    const RunRootResult base_check = require_real_directory(normalized_base);
    if (!base_check.ok()) {
        return {base_check, nullptr};
    }
    const std::filesystem::path staging_parent = normalized_base / ".staging";
    const RunRootResult parent_check = require_real_directory(staging_parent);
    if (!parent_check.ok()) {
        return {parent_check, nullptr};
    }
    auto transaction = std::unique_ptr<RunRootTransaction>(
        new RunRootTransaction(normalized_base, std::move(run_id), LocalState::kStaging));
    const RunRootResult staging_check = require_real_directory(transaction->staging_root_);
    if (!staging_check.ok()) {
        return {staging_check, nullptr};
    }
    std::string detail;
    if (path_kind(transaction->final_root_, detail) != PathKind::kAbsent) {
        return {
            failure(RunRootStatus::kAlreadyExists,
                    detail.empty() ? "final run root already exists: " + transaction->final_root_.string() : detail),
            nullptr};
    }
    for (const char* forbidden : {kPendingReceiptName, kPublishedReceiptName}) {
        const std::filesystem::path path = transaction->staging_root_ / forbidden;
        if (path_kind(path, detail) != PathKind::kAbsent) {
            return {
                failure(RunRootStatus::kStateConflict,
                        detail.empty() ? "staging root already contains a final receipt: " + path.string() : detail),
                nullptr};
        }
    }
    const RunRootResult device_check = verify_same_device(transaction->staging_root_, normalized_base);
    if (!device_check.ok()) {
        return {device_check, nullptr};
    }
    return {success("existing validated staging root attached"), std::move(transaction)};
}

RunRootInspection RunRootTransaction::inspect(const std::filesystem::path& output_base, std::string_view run_id) {
    RunRootInspection inspection;
    inspection.staging_root = output_base / ".staging" / std::string(run_id);
    inspection.final_root = output_base / std::string(run_id);
    if (!valid_run_id(run_id) || output_base.empty() || !output_base.is_absolute()) {
        inspection.message = "absolute output base and safe run_id are required";
        return inspection;
    }

    std::string detail;
    const PathKind staging = path_kind(inspection.staging_root, detail);
    if (staging == PathKind::kError || staging == PathKind::kOther ||
        (staging != PathKind::kAbsent && staging != PathKind::kDirectory)) {
        inspection.message = detail.empty() ? "staging root has an invalid type" : detail;
        return inspection;
    }
    const PathKind final = path_kind(inspection.final_root, detail);
    if (final == PathKind::kError || final == PathKind::kOther ||
        (final != PathKind::kAbsent && final != PathKind::kDirectory)) {
        inspection.message = detail.empty() ? "final root has an invalid type" : detail;
        return inspection;
    }
    if (staging != PathKind::kAbsent && final != PathKind::kAbsent) {
        inspection.message = "staging and final roots both exist";
        return inspection;
    }
    if (staging == PathKind::kDirectory) {
        const PathKind premature = path_kind(inspection.staging_root / kPublishedReceiptName, detail);
        if (premature != PathKind::kAbsent) {
            inspection.message = "staging root contains a published run receipt";
            return inspection;
        }
        inspection.state = RunRootObservedState::kStaging;
        inspection.message = "run remains staging-only";
        return inspection;
    }
    if (final == PathKind::kAbsent) {
        inspection.state = RunRootObservedState::kAbsent;
        inspection.message = "run root does not exist";
        return inspection;
    }

    const PathKind pending = path_kind(inspection.final_root / kPendingReceiptName, detail);
    const PathKind receipt = path_kind(inspection.final_root / kPublishedReceiptName, detail);
    if ((pending != PathKind::kAbsent && pending != PathKind::kRegular) ||
        (receipt != PathKind::kAbsent && receipt != PathKind::kRegular) ||
        (pending == PathKind::kRegular && receipt == PathKind::kRegular)) {
        inspection.message = detail.empty() ? "final receipt state is inconsistent" : detail;
        return inspection;
    }
    if (receipt == PathKind::kRegular) {
        inspection.state = RunRootObservedState::kPublished;
        inspection.query_visible = true;
        inspection.message = "final root contains a regular run receipt";
        return inspection;
    }
    inspection.state = RunRootObservedState::kFinalUnpublished;
    inspection.message = pending == PathKind::kRegular
                             ? "atomic directory rename completed; run receipt publication is pending"
                             : "final root exists without a run receipt";
    return inspection;
}

RunRootResult RunRootTransaction::verify_same_device(const std::filesystem::path& source,
                                                     const std::filesystem::path& destination_parent) {
    struct stat source_status {};
    struct stat destination_status {};
    if (::stat(source.c_str(), &source_status) != 0) {
        return failure(RunRootStatus::kIoError, errno_message("stat source", source));
    }
    if (::stat(destination_parent.c_str(), &destination_status) != 0) {
        return failure(RunRootStatus::kIoError, errno_message("stat destination parent", destination_parent));
    }
    if (source_status.st_dev != destination_status.st_dev) {
        return failure(RunRootStatus::kDifferentDevice,
                       "staging and final parent are on different filesystems; copy fallback is forbidden");
    }
    return success("staging and final parent are on the same filesystem");
}

RunRootResult RunRootTransaction::prepare_run_receipt(std::string_view canonical_receipt_bytes) {
    if (state_ != LocalState::kStaging) {
        return failure(RunRootStatus::kStateConflict, "run receipt can be prepared only from STAGING");
    }
    if (canonical_receipt_bytes.empty()) {
        return failure(RunRootStatus::kInvalidArgument, "run receipt bytes must not be empty");
    }
    prepared_receipt_sha256_ = sha256_bytes(canonical_receipt_bytes);
    if (!valid_sha256(prepared_receipt_sha256_)) {
        return failure(RunRootStatus::kIoError, "cannot compute run receipt SHA-256");
    }
    const std::filesystem::path pending = staging_root_ / kPendingReceiptName;
    const RunRootResult write = write_exclusive_file(pending, canonical_receipt_bytes);
    if (!write.ok()) {
        prepared_receipt_sha256_.clear();
        return write;
    }
    const RunRootResult sync = fsync_directory(staging_root_);
    if (!sync.ok()) {
        return sync;
    }
    state_ = LocalState::kPrepared;
    return success("pending run receipt prepared", prepared_receipt_sha256_);
}

RunRootResult RunRootTransaction::rename_staging_to_final() {
    if (state_ != LocalState::kPrepared) {
        return failure(RunRootStatus::kStateConflict, "atomic rename requires a prepared run receipt");
    }
    const RunRootResult device_check = verify_same_device(staging_root_, output_base_);
    if (!device_check.ok()) {
        return device_check;
    }
    std::string detail;
    if (path_kind(final_root_, detail) != PathKind::kAbsent) {
        return failure(RunRootStatus::kAlreadyExists,
                       detail.empty() ? "final run root already exists: " + final_root_.string() : detail);
    }
    if (::rename(staging_root_.c_str(), final_root_.c_str()) != 0) {
        return failure(errno == EXDEV ? RunRootStatus::kDifferentDevice : RunRootStatus::kIoError,
                       errno_message("atomic directory rename", staging_root_));
    }
    const RunRootResult destination_sync = fsync_directory(output_base_);
    if (!destination_sync.ok()) {
        return destination_sync;
    }
    const RunRootResult source_sync = fsync_directory(staging_parent_);
    if (!source_sync.ok()) {
        return source_sync;
    }
    state_ = LocalState::kRenamed;
    return success("staging root atomically renamed; final root remains unpublished", prepared_receipt_sha256_);
}

RunRootResult RunRootTransaction::publish_run_receipt(std::string_view expected_run_receipt_sha256) {
    if (state_ != LocalState::kRenamed) {
        return failure(RunRootStatus::kStateConflict, "run receipt publication requires an atomically renamed root");
    }
    if (!valid_sha256(expected_run_receipt_sha256)) {
        return failure(RunRootStatus::kInvalidArgument, "expected run receipt SHA-256 is malformed");
    }
    const std::filesystem::path pending = final_root_ / kPendingReceiptName;
    const std::filesystem::path published = final_root_ / kPublishedReceiptName;
    std::string observed;
    const RunRootResult hashed = sha256_file(pending, observed);
    if (!hashed.ok()) {
        return hashed;
    }
    if (observed != expected_run_receipt_sha256 ||
        (!prepared_receipt_sha256_.empty() && observed != prepared_receipt_sha256_)) {
        return failure(RunRootStatus::kDigestMismatch, "pending run receipt SHA-256 differs from validated evidence");
    }
    std::string detail;
    if (path_kind(published, detail) != PathKind::kAbsent) {
        return failure(RunRootStatus::kAlreadyExists, detail.empty() ? "published run receipt already exists" : detail);
    }
    if (::rename(pending.c_str(), published.c_str()) != 0) {
        return failure(RunRootStatus::kIoError, errno_message("publish run receipt", pending));
    }
    const RunRootResult root_sync = fsync_directory(final_root_);
    if (!root_sync.ok()) {
        return root_sync;
    }
    const RunRootResult parent_sync = fsync_directory(output_base_);
    if (!parent_sync.ok()) {
        return parent_sync;
    }
    state_ = LocalState::kPublished;
    return success("run receipt atomically published", observed);
}

RunRootResult RunRootTransaction::freeze(std::string_view canonical_receipt_bytes) {
    RunRootResult result = prepare_run_receipt(canonical_receipt_bytes);
    if (!result.ok()) {
        return result;
    }
    const std::string expected_digest = result.run_receipt_sha256;
    result = rename_staging_to_final();
    if (!result.ok()) {
        return result;
    }
    return publish_run_receipt(expected_digest);
}

RunRootResult RunRootTransaction::recover_after_rename(const std::filesystem::path& output_base,
                                                       std::string_view run_id,
                                                       std::string_view expected_run_receipt_sha256) {
    static_cast<void>(output_base);
    static_cast<void>(run_id);
    if (!valid_sha256(expected_run_receipt_sha256)) {
        return failure(RunRootStatus::kInvalidArgument, "expected run receipt SHA-256 is malformed");
    }
    return failure(RunRootStatus::kStateConflict,
                   "receipt-only recovery is disabled; rerun independent artifact/input replay before publication");
}

}  // namespace longlineage::artifact
