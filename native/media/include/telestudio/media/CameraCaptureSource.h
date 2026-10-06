#pragma once
#include "telestudio/media/DesktopCaptureSource.h"
namespace telestudio {
namespace detail {
// Capture-driver boundary: cancellation must be polled by cooperative backends.
using CameraCaptureOperation = std::function<void(std::string_view, int, FrameCallback,
    CaptureErrorCallback, std::function<bool()>)>;
}
struct CameraDeviceInfo { std::string id, name; };
// MF RGB32 memory buffer: bytes begin at lowest address; negative pitch is bottom-up.
// Normalizes signed pitch to owned, video-range I420 without retaining capture memory.
[[nodiscard]] I420Buffer ConvertCameraRgb32Frame(std::span<const uint8_t> bytes,
    int width, int height, int pitch, int64_t timestamp100ns);
class ICameraCaptureSource {
public:
    virtual ~ICameraCaptureSource() = default;
    virtual void Start(std::string_view deviceId, int targetFps, FrameCallback, CaptureErrorCallback) = 0;
    virtual void Stop() = 0;
};
class CameraCaptureSource final : public ICameraCaptureSource {
public:
    CameraCaptureSource();
    explicit CameraCaptureSource(detail::CameraCaptureOperation operation);
    ~CameraCaptureSource() override;
    CameraCaptureSource(const CameraCaptureSource&) = delete;
    CameraCaptureSource& operator=(const CameraCaptureSource&) = delete;
    static std::vector<CameraDeviceInfo> Enumerate();
    void Start(std::string_view deviceId, int targetFps, FrameCallback, CaptureErrorCallback) override;
    void Stop() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace telestudio
