#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace telestudio::media {
namespace detail {
class RecordingDirectoryLeaseState;
}

class ValidatedRecordingOutputDirectory final {
 public:
  const std::filesystem::path& Directory() const noexcept;
  bool ValidateBeforeUse() const noexcept;

 private:
  friend std::optional<ValidatedRecordingOutputDirectory>
  ResolveTestRecordingOutputOverride(
      std::optional<std::string_view>,
      std::optional<std::filesystem::path>, const std::filesystem::path&);

  std::filesystem::path directory_;
  std::shared_ptr<detail::RecordingDirectoryLeaseState> lease_;
};

std::optional<ValidatedRecordingOutputDirectory>
ResolveTestRecordingOutputOverride(
    std::optional<std::string_view> testMode,
    std::optional<std::filesystem::path> environmentOverride,
    const std::filesystem::path& temporaryRoot);

}  // namespace telestudio::media
