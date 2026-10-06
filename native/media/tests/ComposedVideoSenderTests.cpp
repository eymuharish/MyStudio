#include "telestudio/media/ComposedVideoSender.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace telestudio;
using namespace std::chrono_literals;
void Check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
template<class I> struct Capture final:I {
    FrameCallback frame; int stops=0;
    void Start(std::string_view,int,FrameCallback f,CaptureErrorCallback) override {frame=std::move(f);}
    void Stop() override {++stops;}
};
int main() {try {
    auto c=std::make_shared<Capture<ICameraCaptureSource>>();
    auto d=std::make_shared<Capture<IDesktopCaptureSource>>();
    ComposedVideoSender sender([&]{return c;},[&]{return d;});
    Check(!sender.Camera(true,false,"camera"),"no call cannot acquire camera");
    std::atomic<int> samples=0;
    sender.Start(7,{640,480},30,[&](uint64_t g,I420Frame f){Check(g==7,"generation forwarded"); ++samples; return f.width==640;});
    Check(sender.Camera(true,true,"camera") && sender.Desktop(true,true,"display"),"independent requests accepted");
    ComposedVideoFrame f; f.width=8; f.height=8; f.y.assign(64,200); f.u.assign(16,128); f.v.assign(16,128);
    c->frame(f.View()); d->frame(f.View());
    auto wait=[&](auto predicate) {
        auto deadline=std::chrono::steady_clock::now()+2s;
        for(;;) {
            if(auto state=sender.TakeState(); state && predicate(*state)) return *state;
            Check(std::chrono::steady_clock::now()<deadline,"timed out awaiting delivered state");
            std::this_thread::sleep_for(5ms);
        }
    };
    auto both=wait([](const auto& s){return s.source.cameraActive && s.source.desktopActive && s.outputAccepted;});
    Check(sender.Desktop(false,false,{}),"disable allowed without readiness");
    auto camera=wait([](const auto& s){return s.source.cameraActive && !s.source.desktopActive && !s.source.desktopRequested;});
    Check(c->stops==0 && d->stops==1 && camera.source.revision>both.source.revision,"desktop stop does not stop camera");
    sender.Configure({640,480,CameraOverlaySize::Small,CameraOverlayPosition::TopLeft});
    auto layout=wait([](const auto& s){return s.settings.cameraSize==CameraOverlaySize::Small;});
    Check(layout.settings.cameraPosition==CameraOverlayPosition::TopLeft,"exact delivered layout");
    sender.Stop(); auto count=samples.load();
    c->frame(f.View()); d->frame(f.View()); std::this_thread::sleep_for(50ms);
    Check(samples==count && !sender.TakeState(),"stop fences state and output");
    sender.Start(8,{640,480},30,[](uint64_t,I420Frame){return false;});
    auto neutral=wait([](const auto& s){return !s.outputAccepted;});
    Check(neutral.generation==8 && !neutral.source.cameraRequested && !neutral.source.desktopRequested,"next call starts off");
    std::mutex completionMutex;ComposedVideoSender::Completion completion;
    sender.StartAsync(9,{640,480},30,[&](uint64_t,I420Frame,ComposedVideoSender::Completion done){
        std::lock_guard lock(completionMutex);completion=std::move(done);
    });
    std::this_thread::sleep_for(50ms);
    auto queued=sender.TakeState();Check(!queued || !queued->outputAccepted,"queue admission cannot confirm output");
    ComposedVideoSender::Completion first;
    {std::lock_guard lock(completionMutex);first=completion;}
    Check(bool(first),"delayed completion captured");first(false);
    auto failed=sender.TakeState();Check(!failed || !failed->outputAccepted,"failed encode never confirms");
    first(true);auto recovered=wait([](const auto& s){return s.outputAccepted;});
    Check(recovered.generation==9,"matching completion recovers");
    Check(sender.Camera(true,true,"camera"),"async capture enabled");c->frame(f.View());
    std::this_thread::sleep_for(50ms);first(true);
    auto stale=sender.TakeState();Check(!stale || !stale->outputAccepted,"old neutral completion cannot confirm new camera revision");
    Check(sender.Camera(false,false),"OFF accepted during stalled encoder");
    auto off=wait([](const auto& s){return !s.source.cameraRequested && !s.source.cameraActive;});
    Check(!off.outputAccepted,"OFF is observable without encoder success");
    sender.Stop();first(true);Check(!sender.TakeState(),"stale completion cannot revive stopped call");
    {ComposedVideoSender temporary;
      temporary.StartAsync(10,{640,480},30,[&](uint64_t,I420Frame,ComposedVideoSender::Completion done){
        std::lock_guard lock(completionMutex);completion=std::move(done);
      });std::this_thread::sleep_for(50ms);
    }
    {std::lock_guard lock(completionMutex);completion(true);} // weak lifetime is safe after owner destruction
    std::atomic<int> paced=0;
    sender.Start(11,{640,480},30,[&](uint64_t,I420Frame){++paced;return true;});
    sender.Configure({640,480},5);paced=0;std::this_thread::sleep_for(450ms);
    Check(paced>=1 && paced<=3,"negotiated fps changes actual producer clock without source restart");
    sender.Stop();
    std::vector<ComposedVideoSender::Completion> ordered;
    sender.StartAsync(12,{640,480},30,[&](uint64_t,I420Frame,ComposedVideoSender::Completion done){
        std::lock_guard lock(completionMutex);ordered.push_back(std::move(done));
    });
    std::this_thread::sleep_for(100ms);
    ComposedVideoSender::Completion older,newer;
    {std::lock_guard lock(completionMutex);Check(ordered.size()>=2,"two output frames captured");older=ordered[0];newer=ordered[1];}
    newer(true);wait([](const auto& s){return s.outputAccepted;});
    older(false);
    auto late=sender.TakeState();Check(!late || !late->outputFailed,"late older frame failure cannot revoke newer delivered output");
    sender.Stop();
    std::cout<<"composed sender: PASS\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
