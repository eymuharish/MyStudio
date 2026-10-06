#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace telestudio::media::test {

struct MountPointReparseDataBuffer {
  ULONG reparseTag;
  USHORT reparseDataLength;
  USHORT reserved;
  USHORT substituteNameOffset;
  USHORT substituteNameLength;
  USHORT printNameOffset;
  USHORT printNameLength;
  wchar_t pathBuffer[1];
};

struct InPlaceJunctionAttempt {
  bool handleOpened;
  bool applied;
  DWORD error;
};

inline InPlaceJunctionAttempt TrySetDirectoryJunctionInPlace(
    const std::filesystem::path& directory,
    const std::filesystem::path& target) {
  const std::wstring substituteName = L"\\??\\" + target.wstring();
  const std::wstring printName = target.wstring();
  const std::size_t pathBytes =
      (substituteName.size() + 1 + printName.size() + 1) * sizeof(wchar_t);
  std::vector<std::byte> storage(
      offsetof(MountPointReparseDataBuffer, pathBuffer) + pathBytes);
  auto* buffer =
      reinterpret_cast<MountPointReparseDataBuffer*>(storage.data());
  buffer->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  buffer->reserved = 0;
  buffer->substituteNameOffset = 0;
  buffer->substituteNameLength =
      static_cast<USHORT>(substituteName.size() * sizeof(wchar_t));
  buffer->printNameOffset =
      static_cast<USHORT>((substituteName.size() + 1) * sizeof(wchar_t));
  buffer->printNameLength =
      static_cast<USHORT>(printName.size() * sizeof(wchar_t));
  auto* pathBuffer = buffer->pathBuffer;
  std::copy(substituteName.begin(), substituteName.end(), pathBuffer);
  pathBuffer[substituteName.size()] = L'\0';
  std::copy(printName.begin(), printName.end(),
            pathBuffer + substituteName.size() + 1);
  pathBuffer[substituteName.size() + 1 + printName.size()] = L'\0';
  buffer->reparseDataLength = static_cast<USHORT>(8 + pathBytes);

  const HANDLE handle = CreateFileW(
      directory.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return {false, false, GetLastError()};
  DWORD bytesReturned = 0;
  const bool applied = DeviceIoControl(
      handle, FSCTL_SET_REPARSE_POINT, buffer,
      8 + buffer->reparseDataLength, nullptr, 0,
      &bytesReturned, nullptr) != FALSE;
  const DWORD error = applied ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  return {true, applied, error};
}

inline void RestoreOwnedDirectoryAfterInPlaceJunction(
    const std::filesystem::path& directory) {
  if (!RemoveDirectoryW(directory.c_str()) ||
      !CreateDirectoryW(directory.c_str(), nullptr)) {
    throw std::runtime_error("test in-place junction cleanup failed");
  }
}

class OwnedDirectory final {
 public:
  OwnedDirectory(const std::filesystem::path& temporaryRoot,
                 std::string_view label) {
    path_ = temporaryRoot /
            (std::filesystem::path(label).wstring() + L"-" +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(path_);
  }
  OwnedDirectory(const OwnedDirectory&) = delete;
  OwnedDirectory& operator=(const OwnedDirectory&) = delete;
  ~OwnedDirectory() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

class OwnedJunction final {
 public:
  OwnedJunction(const std::filesystem::path& temporaryRoot,
                std::string_view label, bool targetInsideTemporaryRoot) {
    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64());
    const auto stem = std::filesystem::path(
        L"EssaStudio-" + std::filesystem::path(label).wstring() + L"-" +
        suffix);
    owner_ = temporaryRoot / stem;
    target_ = (targetInsideTemporaryRoot ? temporaryRoot
                                         : temporaryRoot.parent_path()) /
              (std::filesystem::path(label).wstring() + L"-target-" + suffix);
    link_ = owner_ / "redirect";
    output_ = link_ / "recordings";
    std::filesystem::create_directories(owner_);
    std::filesystem::create_directories(target_ / "recordings");
    const std::wstring command =
        L"cmd.exe /d /c mklink /J \"" + link_.wstring() + L"\" \"" +
        target_.wstring() + L"\" >NUL";
    if (_wsystem(command.c_str()) != 0) {
      Cleanup();
      throw std::runtime_error("test junction creation failed");
    }
    junctionCreated_ = true;
  }

  OwnedJunction(const OwnedJunction&) = delete;
  OwnedJunction& operator=(const OwnedJunction&) = delete;
  ~OwnedJunction() { Cleanup(); }

  const std::filesystem::path& Output() const noexcept { return output_; }
  std::filesystem::path Probe() const { return target_ / "recordings" / "forbidden.mp4"; }

 private:
  void Cleanup() noexcept {
    std::error_code ignored;
    if (junctionCreated_) {
      (void)RemoveDirectoryW(link_.c_str());
      junctionCreated_ = false;
    }
    std::filesystem::remove(owner_, ignored);
    std::filesystem::remove(target_ / "recordings", ignored);
    std::filesystem::remove(target_, ignored);
  }

  std::filesystem::path owner_;
  std::filesystem::path target_;
  std::filesystem::path link_;
  std::filesystem::path output_;
  bool junctionCreated_ = false;
};

}  // namespace telestudio::media::test
