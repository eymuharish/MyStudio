#pragma once
#include "telestudio/media/ComposedVideoSource.h"
#include <atomic>
#include <mutex>
#include <optional>
namespace telestudio {
struct DeliveredVideoState {
    uint64_t generation=0;
    ComposedSourceState source;
    VideoCompositionSettings settings;
    bool outputAccepted=false;
    bool outputFailed=false;
};
// Serial control-side owner; encoder/transport stays owned by each protocol.
class ComposedVideoSender final {
public:
    using Sink=std::function<bool(uint64_t,I420Frame)>;
    using Completion=std::function<void(bool)>;
    using AsyncSink=std::function<void(uint64_t,I420Frame,Completion)>;
    explicit ComposedVideoSender(ComposedVideoSource::CameraFactory camera={},
        ComposedVideoSource::DesktopFactory desktop={});
    ~ComposedVideoSender();
    void Start(uint64_t generation,VideoCompositionSettings settings,int fps,Sink sink);
    void StartAsync(uint64_t generation,VideoCompositionSettings settings,int fps,AsyncSink sink);
    void Stop();
    bool Camera(bool enabled,bool activeCall,std::string id={});
    bool Desktop(bool enabled,bool activeCall,std::string id={});
    void Configure(VideoCompositionSettings settings,int fps=0);
    std::optional<DeliveredVideoState> TakeState();
private:
    ComposedVideoSource source_;
    struct CompletionState {
        std::atomic<uint64_t> generation{0};
        std::mutex mutex;
        std::optional<DeliveredVideoState> pending;
        uint64_t observedRevision=0;
        uint64_t nextFrame=0;
        uint64_t completedFrame=0;
    };
    std::shared_ptr<CompletionState> completion_=std::make_shared<CompletionState>();
    uint64_t emittedRevision_=0;
    bool emittedAccepted_=false;
    bool emittedFailed_=false;
};
}
