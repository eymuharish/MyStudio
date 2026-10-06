#pragma once

#include "telestudio/media/DesktopCaptureSource.h"
#include "telestudio/media/MediaTypes.h"

#include <functional>
#include <memory>
#include <string>

namespace telestudio {

using DesktopSourceFactory = std::function<std::shared_ptr<IDesktopCaptureSource>()>;

class MediaSourceController {
public:
    explicit MediaSourceController(DesktopSourceFactory factory = {});
    ~MediaSourceController();

    MediaSourceController(const MediaSourceController&) = delete;
    MediaSourceController& operator=(const MediaSourceController&) = delete;

    void SetCameraSource(FrameCallback attach);
    void SetDesktopSource(std::string displayId);
    void DeliverCameraFrame(I420Frame frame);
    [[nodiscard]] VideoSource ActiveSource() const;
    [[nodiscard]] std::string LastError() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace telestudio
