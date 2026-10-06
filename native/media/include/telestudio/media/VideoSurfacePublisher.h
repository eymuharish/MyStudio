#pragma once
#include "telestudio/media/RemoteVideoSurface.h"
#include "telestudio/media/IVideoEncoder.h"
#include <memory>
namespace telestudio {
enum class SurfaceAcknowledgement { Opened, Released, Removed };
std::optional<HostRegistration> SurfaceHostFromEnvironment();
// One instance per direction. Submit copies one latest frame; Pump runs on one
// control/render thread. No encoder, capture device, audio or transport ownership.
class VideoSurfacePublisher final {
public:
    explicit VideoSurfacePublisher(HostRegistration host);
    ~VideoSurfacePublisher();
    void Start(uint64_t generation);
    void Stop();
    bool SubmitI420(uint64_t generation, const I420Frame& frame);
    bool SubmitBgra(uint64_t generation, const DecodedBgraFrame& frame);
    std::optional<PublishedSurface> Pump();
    void Acknowledge(SurfaceAcknowledgement kind, uint64_t epoch);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
