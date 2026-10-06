#include "../include/telestudio/media/RecordingOutputOverride.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <stdexcept>
#include <string>
#include <vector>

namespace telestudio::media {
namespace detail {

#ifdef _WIN32
namespace {
std::wstring FinalPath(HANDLE handle) {
  const DWORD length = GetFinalPathNameByHandleW(
      handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0) return {};
  std::wstring result(length, L'\0');
  const DWORD written = GetFinalPathNameByHandleW(
      handle, result.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0 || written >= length) return {};
  result.resize(written);
  if (result.size() >= 4 && result.compare(0, 4, L"\\\\?\\") == 0) {
    result.erase(0, 4);
  }
  return result;
}

bool SameWindowsPath(const std::wstring& left,
                     const std::wstring& right) noexcept {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                              right.data(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}
}  // namespace

class RecordingDirectoryLeaseState final {
 public:
  ~RecordingDirectoryLeaseState() {
    if (lockHandle_ != INVALID_HANDLE_VALUE) CloseHandle(lockHandle_);
    for (const HANDLE handle : handles_) {
      if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
  }

  bool Validate() const noexcept {
    if (lockHandle_ == INVALID_HANDLE_VALUE || !ValidateDirectories()) {
      return false;
    }
    FILE_ATTRIBUTE_TAG_INFO lockAttributes{};
    const auto finalLockPath = FinalPath(lockHandle_);
    return GetFileInformationByHandleEx(lockHandle_, FileAttributeTagInfo,
                                        &lockAttributes,
                                        sizeof(lockAttributes)) &&
           (lockAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (lockAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
           !finalLockPath.empty() &&
           SameWindowsPath(finalLockPath, lockPath_.wstring());
  }

  bool ValidateDirectories() const noexcept {
    if (handles_.empty() || handles_.size() != canonicalPaths_.size()) {
      return false;
    }
    for (std::size_t index = 0; index < handles_.size(); ++index) {
      FILE_ATTRIBUTE_TAG_INFO attributes{};
      if (!GetFileInformationByHandleEx(handles_[index], FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
      }
      const auto finalPath = FinalPath(handles_[index]);
      if (finalPath.empty() ||
          !SameWindowsPath(finalPath, canonicalPaths_[index].wstring())) {
        return false;
      }
    }
    return true;
  }

  std::vector<HANDLE> handles_;
  std::vector<std::filesystem::path> canonicalPaths_;
  HANDLE lockHandle_ = INVALID_HANDLE_VALUE;
  std::filesystem::path lockPath_;
};
#else
class RecordingDirectoryLeaseState final {
 public:
  bool Validate() const noexcept {
    std::error_code error;
    return !directory_.empty() &&
           std::filesystem::is_directory(directory_, error) && !error;
  }
  std::filesystem::path directory_;
};
#endif

namespace {
void RejectExistingReparseComponents(
    const std::filesystem::path& absolutePath) {
  auto current = absolutePath.root_path();
  for (const auto& component : absolutePath.relative_path()) {
    if (component == ".") continue;
    current /= component;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      throw std::invalid_argument("test recording output must be pre-created");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw std::invalid_argument(
          "test recording output cannot contain a reparse point");
    }
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error || !std::filesystem::exists(status)) {
      throw std::invalid_argument("test recording output must be pre-created");
    }
    if (std::filesystem::is_symlink(status)) {
      throw std::invalid_argument(
          "test recording output cannot contain a symlink");
    }
#endif
  }
}

std::shared_ptr<RecordingDirectoryLeaseState> AcquireDirectoryLease(
    const std::filesystem::path& canonicalTemporaryRoot,
    const std::filesystem::path& canonicalOutput) {
  auto lease = std::make_shared<RecordingDirectoryLeaseState>();
#ifdef _WIN32
  std::vector<std::filesystem::path> paths{canonicalTemporaryRoot};
  auto current = canonicalTemporaryRoot;
  for (const auto& component :
       canonicalOutput.lexically_relative(canonicalTemporaryRoot)) {
    current /= component;
    paths.push_back(current);
  }
  for (const auto& path : paths) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      throw std::invalid_argument(
          "test recording output directory cannot be locked");
    }
    lease->handles_.push_back(handle);
    lease->canonicalPaths_.push_back(path);
    if (!lease->ValidateDirectories()) {
      throw std::invalid_argument(
          "test recording output directory identity changed");
    }
  }
  lease->lockPath_ = canonicalOutput / L".essastudio-recording.lock";
  lease->lockHandle_ = CreateFileW(
      lease->lockPath_.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
      FILE_SHARE_READ, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY |
          FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (lease->lockHandle_ == INVALID_HANDLE_VALUE || !lease->Validate()) {
    throw std::invalid_argument(
        "test recording output directory cannot be locked");
  }
#else
  lease->directory_ = canonicalOutput;
#endif
  return lease;
}
}  // namespace
}  // namespace detail

const std::filesystem::path& ValidatedRecordingOutputDirectory::Directory()
    const noexcept {
  return directory_;
}

bool ValidatedRecordingOutputDirectory::ValidateBeforeUse() const noexcept {
  return lease_ && lease_->Validate();
}

std::optional<ValidatedRecordingOutputDirectory>
ResolveTestRecordingOutputOverride(
    std::optional<std::string_view> testMode,
    std::optional<std::filesystem::path> environmentOverride,
    const std::filesystem::path& temporaryRoot) {
  if (!testMode && !environmentOverride) return std::nullopt;
  if (!testMode || *testMode != "1" || !environmentOverride ||
      environmentOverride->empty()) {
    throw std::invalid_argument(
        "test recording output requires ESSASTUDIO_TEST_MODE=1 and "
        "ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR");
  }
  if (!environmentOverride->is_absolute() || temporaryRoot.empty() ||
      !temporaryRoot.is_absolute()) {
    throw std::invalid_argument("test recording output must be absolute");
  }

  detail::RejectExistingReparseComponents(
      environmentOverride->lexically_normal());
  std::error_code error;
  const auto canonicalTemporaryRoot =
      std::filesystem::canonical(temporaryRoot, error);
  if (error || canonicalTemporaryRoot.empty() ||
      !std::filesystem::is_directory(canonicalTemporaryRoot, error) || error) {
    throw std::invalid_argument("temporary root is unavailable");
  }
  const auto canonicalOutput =
      std::filesystem::canonical(*environmentOverride, error);
  if (error || canonicalOutput.empty() ||
      !std::filesystem::is_directory(canonicalOutput, error) || error) {
    throw std::invalid_argument(
        "test recording output must be a pre-created directory");
  }

  const auto relative =
      canonicalOutput.lexically_relative(canonicalTemporaryRoot);
  if (relative.empty() || relative == "." || relative.is_absolute()) {
    throw std::invalid_argument(
        "test recording output must be below temporary root");
  }
  for (const auto& component : relative) {
    if (component == "..") {
      throw std::invalid_argument(
          "test recording output must be owned by temporary root");
    }
  }

  ValidatedRecordingOutputDirectory result;
  result.directory_ = canonicalOutput;
  result.lease_ =
      detail::AcquireDirectoryLease(canonicalTemporaryRoot, canonicalOutput);
  if (!result.ValidateBeforeUse()) {
    throw std::invalid_argument(
        "test recording output directory identity is invalid");
  }
  return result;
}

}  // namespace telestudio::media
