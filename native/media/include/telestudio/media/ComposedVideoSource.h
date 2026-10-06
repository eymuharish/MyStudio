#pragma once
#include "telestudio/media/CameraCaptureSource.h"
#include "telestudio/media/VideoCompositor.h"
namespace telestudio {
struct ComposedSourceState {
    bool cameraRequested=false, desktopRequested=false, cameraActive=false, desktopActive=false;
    uint64_t revision=0;
    std::string cameraError, desktopError;
};
class ComposedVideoSource final {
public:
    using CameraFactory = std::function<std::shared_ptr<ICameraCaptureSource>()>;
    using DesktopFactory = std::function<std::shared_ptr<IDesktopCaptureSource>()>;
    explicit ComposedVideoSource(CameraFactory camera={}, DesktopFactory desktop={});
    ~ComposedVideoSource();
    ComposedVideoSource(const ComposedVideoSource&) = delete;
    ComposedVideoSource& operator=(const ComposedVideoSource&) = delete;
    void Start(FrameCallback output, int fps=30);
    using StateFrameCallback = std::function<void(I420Frame, ComposedSourceState, VideoCompositionSettings)>;
    // State is captured atomically with the pixels, before delivery.
    void StartWithState(StateFrameCallback output, int fps=30);
    void Stop();
    void Configure(VideoCompositionSettings settings,int fps=0);
    void SetCameraEnabled(bool enabled, std::string deviceId={});
    void SetDesktopEnabled(bool enabled, std::string displayId={});
    // Borrowed view, valid only during callback; exact final encoder-input picture.
    void SetPreviewCallback(FrameCallback preview);
    [[nodiscard]] ComposedSourceState Snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace telestudio
