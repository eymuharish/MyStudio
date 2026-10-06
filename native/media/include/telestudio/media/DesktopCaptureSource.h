#pragma once

#include "telestudio/media/FrameConverter.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace telestudio {

struct DisplayInfo {
    std::string id;
    std::wstring name;
    int width;
    int height;
    bool primary;
};

enum class CaptureItemKind { Monitor, Window };

struct CaptureItemInfo {
    std::string id;
    CaptureItemKind kind;
    std::string name;
    int width;
    int height;
};

struct OwnedCaptureFrame {
    std::shared_ptr<const I420Buffer> pixels;
    std::int64_t timestamp100ns;
};

using FrameCallback = std::function<void(I420Frame)>;
using OwnedFrameCallback = std::function<void(OwnedCaptureFrame)>;
using CaptureErrorCallback = std::function<void(const std::string&)>;

class IDesktopCaptureSource {
public:
    virtual ~IDesktopCaptureSource() = default;
    virtual void Start(std::string_view displayId, int targetFps, FrameCallback callback,
                       CaptureErrorCallback errorCallback) = 0;
    virtual void Stop() = 0;
};

class DesktopCaptureSource final : public IDesktopCaptureSource {
public:
    DesktopCaptureSource();
    ~DesktopCaptureSource() override;

    DesktopCaptureSource(const DesktopCaptureSource&) = delete;
    DesktopCaptureSource& operator=(const DesktopCaptureSource&) = delete;

    static std::vector<DisplayInfo> Enumerate();
    static std::vector<CaptureItemInfo> EnumerateCaptureItems();
    static bool ValidateCaptureItem(std::string_view id) noexcept;
    void Start(std::string_view displayId, int targetFps, FrameCallback callback);
    void Start(std::string_view displayId, int targetFps, FrameCallback callback,
               CaptureErrorCallback errorCallback) override;
    void Stop() override;
    void StartOwned(std::string_view captureItemId, int targetFps, int canvasWidth,
                    int canvasHeight, OwnedFrameCallback callback,
                    CaptureErrorCallback errorCallback = {});
    [[nodiscard]] uint64_t DroppedFrames() const noexcept;
    [[nodiscard]] std::string LastError() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace telestudio
