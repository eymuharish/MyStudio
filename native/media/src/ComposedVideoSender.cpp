#include "telestudio/media/ComposedVideoSender.h"
#include <stdexcept>
namespace telestudio {
ComposedVideoSender::ComposedVideoSender(ComposedVideoSource::CameraFactory camera,
    ComposedVideoSource::DesktopFactory desktop):source_(std::move(camera),std::move(desktop)) {}
ComposedVideoSender::~ComposedVideoSender(){Stop();}
void ComposedVideoSender::Start(uint64_t generation,VideoCompositionSettings settings,int fps,Sink sink) {
    if(!sink) throw std::invalid_argument("sender sink required");
    StartAsync(generation,settings,fps,[sink=std::move(sink)](uint64_t g,I420Frame f,Completion done){done(sink(g,f));});
}
void ComposedVideoSender::StartAsync(uint64_t generation,VideoCompositionSettings settings,int fps,AsyncSink sink) {
    if(!generation || !sink) throw std::invalid_argument("sender generation and sink required");
    Stop(); source_.Configure(settings); completion_->generation=generation;
    try {
        source_.StartWithState([this,generation,sink=std::move(sink)](I420Frame frame,
                ComposedSourceState state,VideoCompositionSettings settings) {
            if(completion_->generation.load()!=generation) return;
            DeliveredVideoState context{generation,state,settings,false};
            uint64_t frameSequence=0;
            {std::lock_guard lock(completion_->mutex);
                frameSequence=++completion_->nextFrame;
                // OFF and capture errors are observable even when encoding is unavailable.
                if(state.revision!=completion_->observedRevision &&
                    (!state.cameraRequested || !state.desktopRequested || !state.cameraError.empty() || !state.desktopError.empty()))
                    completion_->pending=context;
                completion_->observedRevision=state.revision;
            }
            Completion done=[weak=std::weak_ptr<CompletionState>(completion_),context,frameSequence](bool accepted) mutable {
                if(auto shared=weak.lock()) {
                    std::lock_guard lock(shared->mutex);
                    if(shared->generation.load()!=context.generation || context.source.revision<shared->observedRevision || frameSequence<shared->completedFrame) return;
                    shared->completedFrame=frameSequence;
                    context.outputAccepted=accepted;context.outputFailed=!accepted;shared->pending=context;
                }
            };
            try {sink(generation,frame,done);} catch(...) {done(false);}
        },fps);
    } catch(...) {completion_->generation=0;throw;}
}
void ComposedVideoSender::Stop() {
    completion_->generation=0; source_.Stop();
    std::lock_guard lock(completion_->mutex); completion_->pending.reset();completion_->observedRevision=0;completion_->nextFrame=0;completion_->completedFrame=0; emittedRevision_=0; emittedAccepted_=false;emittedFailed_=false;
}
bool ComposedVideoSender::Camera(bool enabled,bool activeCall,std::string id) {
    if(enabled && (!activeCall || completion_->generation.load()==0)) return false;
    source_.SetCameraEnabled(enabled,std::move(id)); return true;
}
bool ComposedVideoSender::Desktop(bool enabled,bool activeCall,std::string id) {
    if(enabled && (!activeCall || completion_->generation.load()==0 || id.empty())) return false;
    source_.SetDesktopEnabled(enabled,std::move(id)); return true;
}
void ComposedVideoSender::Configure(VideoCompositionSettings settings,int fps) {source_.Configure(settings,fps);}
std::optional<DeliveredVideoState> ComposedVideoSender::TakeState() {
    // A control/input change after the stored frame invalidates that acknowledgement.
    const auto minimum=source_.Snapshot().revision;
    std::lock_guard lock(completion_->mutex);auto& pending=completion_->pending;
    if(!pending || pending->generation!=completion_->generation.load() || pending->source.revision<minimum ||
       (pending->source.revision==emittedRevision_ && pending->outputAccepted==emittedAccepted_ && pending->outputFailed==emittedFailed_)) return {};
    auto result=std::move(pending); pending.reset();
    emittedRevision_=result->source.revision; emittedAccepted_=result->outputAccepted;
    emittedFailed_=result->outputFailed;
    return result;
}
}
