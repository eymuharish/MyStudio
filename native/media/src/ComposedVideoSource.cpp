#include "telestudio/media/ComposedVideoSource.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
namespace telestudio {
namespace {
using Clock=std::chrono::steady_clock;
constexpr auto CameraTimeout=std::chrono::seconds(2);
ComposedVideoFrame Copy(I420Frame f) {
    if(f.width<1 || f.height<1 || f.width>4096 || f.height>2160 || !f.y || !f.u || !f.v ||
       f.strideY<f.width || f.strideU<(f.width+1)/2 || f.strideV<(f.width+1)/2)
        throw std::invalid_argument("Invalid captured I420 frame");
    ComposedVideoFrame result; result.width=f.width; result.height=f.height; result.timestamp100ns=f.timestamp100ns;
    int cw=(f.width+1)/2, ch=(f.height+1)/2;
    result.y.resize(size_t(f.width)*f.height); result.u.resize(size_t(cw)*ch); result.v.resize(size_t(cw)*ch);
    for(int y=0;y<f.height;++y) std::memcpy(result.y.data()+size_t(y)*f.width,f.y+size_t(y)*f.strideY,f.width);
    for(int y=0;y<ch;++y) { std::memcpy(result.u.data()+size_t(y)*cw,f.u+size_t(y)*f.strideU,cw); std::memcpy(result.v.data()+size_t(y)*cw,f.v+size_t(y)*f.strideV,cw); }
    return result;
}
struct Slot { uint64_t generation=0; std::optional<ComposedVideoFrame> frame; Clock::time_point received; };
struct State {
    std::mutex mutex;
    // Order: delivery -> operation -> state. Inputs acquire only state.
    std::recursive_mutex delivery;
    std::condition_variable wake;
    Slot camera, desktop;
    ComposedSourceState status;
    VideoCompositionSettings settings;
    ComposedVideoSource::StateFrameCallback output;
    FrameCallback preview;
    uint64_t run=0;
    bool running=false;
    int fps=30;
};
void Fail(State& s, bool camera, const std::string& error) {
    auto& slot=camera?s.camera:s.desktop;
    slot.frame.reset(); ++slot.generation;
    (camera?s.status.cameraActive:s.status.desktopActive)=false;
    (camera?s.status.cameraError:s.status.desktopError)=error;
    ++s.status.revision;
}
std::pair<FrameCallback,CaptureErrorCallback> Callbacks(const std::shared_ptr<State>& state,bool camera,uint64_t generation) {
    std::weak_ptr<State> weak=state;
    return {
        [weak,camera,generation](I420Frame frame) {
            auto s=weak.lock(); if(!s) return;
            std::lock_guard lock(s->mutex); auto& slot=camera?s->camera:s->desktop;
            if(slot.generation!=generation) return;
            try { slot.frame=Copy(frame); slot.received=Clock::now();
                auto& active=camera?s->status.cameraActive:s->status.desktopActive;
                if(!active) { active=true; ++s->status.revision; }
            } catch(const std::exception& e) { Fail(*s,camera,e.what()); }
        },
        [weak,camera,generation](const std::string& error) {
            auto s=weak.lock(); if(!s) return;
            std::lock_guard lock(s->mutex);
            if((camera?s->camera:s->desktop).generation==generation) Fail(*s,camera,error.empty()?"Capture failed":error);
        }
    };
}
void Run(std::shared_ptr<State> s,uint64_t run) {
    for(;;) {
        auto next=Clock::now();
        {
            std::lock_guard delivery(s->delivery);
            ComposedVideoSource::StateFrameCallback output;
            FrameCallback preview; ComposedVideoFrame composed; ComposedSourceState captured; VideoCompositionSettings settings;
            {
                std::lock_guard lock(s->mutex);
                if(!s->running || s->run!=run) return;
                next+=std::chrono::nanoseconds(1000000000/s->fps);
                if(s->status.cameraRequested && s->status.cameraError.empty() && Clock::now()-s->camera.received>=CameraTimeout)
                    Fail(*s,true,"Camera produced no frame within 2 seconds");
                if(s->status.desktopRequested && !s->status.desktopActive && s->status.desktopError.empty() &&
                   Clock::now()-s->desktop.received>=std::chrono::seconds(5))
                    Fail(*s,false,"Desktop produced no first frame within 5 seconds");
                auto c=s->camera.frame? s->camera.frame->View():I420Frame{};
                auto d=s->desktop.frame? s->desktop.frame->View():I420Frame{};
                auto timestamp=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;
                composed=ComposeVideoFrame(s->desktop.frame?&d:nullptr,s->camera.frame?&c:nullptr,s->settings,timestamp);
                output=s->output; preview=s->preview; captured=s->status; settings=s->settings;
            }
            // A consumer exception must not terminate the capture process.
            try { if(output) output(composed.View(), captured, settings); } catch(...) {}
            bool current;
            { std::lock_guard lock(s->mutex); current=s->running && s->run==run; }
            try { if(current && preview) preview(composed.View()); } catch(...) {}
        }
        std::unique_lock lock(s->mutex);
        s->wake.wait_until(lock,next,[&]{return !s->running || s->run!=run;});
        if(!s->running || s->run!=run) return;
    }
}
}
struct ComposedVideoSource::Impl {
    std::shared_ptr<State> state=std::make_shared<State>();
    std::mutex operation;
    CameraFactory cameraFactory;
    DesktopFactory desktopFactory;
    std::shared_ptr<ICameraCaptureSource> camera;
    std::shared_ptr<IDesktopCaptureSource> desktop;
    std::thread worker;
    int captureFps=30;
    explicit Impl(CameraFactory c,DesktopFactory d):cameraFactory(std::move(c)),desktopFactory(std::move(d)) {}
    void Enable(bool isCamera,bool enabled,const std::string& id) {
        std::lock_guard delivery(state->delivery);
        std::lock_guard op(operation);
        uint64_t generation;
        {
            std::lock_guard lock(state->mutex); auto& slot=isCamera?state->camera:state->desktop;
            generation=++slot.generation; slot.frame.reset(); slot.received=Clock::now();
            (isCamera?state->status.cameraRequested:state->status.desktopRequested)=enabled;
            (isCamera?state->status.cameraActive:state->status.desktopActive)=false;
            (isCamera?state->status.cameraError:state->status.desktopError).clear(); ++state->status.revision;
        }
        // Inputs cannot acquire operation/delivery, so device Stop may drain them safely.
        if(isCamera && camera) { camera->Stop(); camera.reset(); }
        if(!isCamera && desktop) { desktop->Stop(); desktop.reset(); }
        if(!enabled) return;
        auto [frame,error]=Callbacks(state,isCamera,generation);
        try {
            if(isCamera) { camera=cameraFactory(); if(!camera) throw std::runtime_error("No camera capture source"); camera->Start(id,captureFps,frame,error); }
            else { desktop=desktopFactory(); if(!desktop) throw std::runtime_error("No desktop capture source"); desktop->Start(id,captureFps,frame,error); }
        } catch(const std::exception& e) { error(e.what()); }
    }
};
ComposedVideoSource::ComposedVideoSource(CameraFactory c,DesktopFactory d):impl_(std::make_unique<Impl>(
    c?std::move(c):CameraFactory([]{return std::make_shared<CameraCaptureSource>();}),
    d?std::move(d):DesktopFactory([]{return std::make_shared<DesktopCaptureSource>();}))) {}
ComposedVideoSource::~ComposedVideoSource() { Stop(); }
void ComposedVideoSource::Start(FrameCallback output,int fps) {
    if(!output) throw std::invalid_argument("Output callback required");
    StartWithState([output=std::move(output)](I420Frame frame, ComposedSourceState, VideoCompositionSettings) { output(frame); },fps);
}
void ComposedVideoSource::StartWithState(StateFrameCallback output,int fps) {
    if(!output || fps<1 || fps>60) throw std::invalid_argument("Output callback and fps 1..60 required");
    std::lock_guard delivery(impl_->state->delivery); std::lock_guard op(impl_->operation);
    std::lock_guard lock(impl_->state->mutex);
    if(impl_->state->running) throw std::logic_error("Composed source already running");
    impl_->captureFps=fps;impl_->state->fps=fps;
    impl_->state->output=std::move(output); impl_->state->running=true; auto run=++impl_->state->run;
    impl_->worker=std::thread(Run,impl_->state,run);
}
void ComposedVideoSource::Stop() {
    std::thread worker;
    {
        std::lock_guard delivery(impl_->state->delivery); std::lock_guard op(impl_->operation);
        { std::lock_guard lock(impl_->state->mutex); auto& s=*impl_->state;
          s.running=false; ++s.run; ++s.camera.generation; ++s.desktop.generation;
          s.camera.frame.reset(); s.desktop.frame.reset(); s.output={};
          s.status.cameraRequested=s.status.desktopRequested=s.status.cameraActive=s.status.desktopActive=false;
          ++s.status.revision; s.wake.notify_all(); }
        if(impl_->camera) { impl_->camera->Stop(); impl_->camera.reset(); }
        if(impl_->desktop) { impl_->desktop->Stop(); impl_->desktop.reset(); }
        worker=std::move(impl_->worker);
    }
    if(worker.joinable()) { if(worker.get_id()==std::this_thread::get_id()) worker.detach(); else worker.join(); }
}
void ComposedVideoSource::Configure(VideoCompositionSettings settings,int fps) {
    if(fps<0 || fps>60) throw std::invalid_argument("Composed fps must be 1..60 or unchanged");
    (void)ComposeVideoFrame(nullptr,nullptr,settings,0); // Shared validation, before mutation.
    std::lock_guard delivery(impl_->state->delivery); std::lock_guard op(impl_->operation);
    std::lock_guard lock(impl_->state->mutex); impl_->state->settings=settings;
    if(fps) {impl_->state->fps=fps;impl_->captureFps=fps;}
    ++impl_->state->status.revision;
}
void ComposedVideoSource::SetCameraEnabled(bool enabled,std::string id) { impl_->Enable(true,enabled,id); }
void ComposedVideoSource::SetDesktopEnabled(bool enabled,std::string id) { impl_->Enable(false,enabled,id); }
void ComposedVideoSource::SetPreviewCallback(FrameCallback callback) {
    std::lock_guard delivery(impl_->state->delivery); std::lock_guard lock(impl_->state->mutex); impl_->state->preview=std::move(callback);
}
ComposedSourceState ComposedVideoSource::Snapshot() const { std::lock_guard lock(impl_->state->mutex); return impl_->state->status; }
} // namespace telestudio
