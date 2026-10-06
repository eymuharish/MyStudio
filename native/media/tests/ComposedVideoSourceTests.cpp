#include "telestudio/media/ComposedVideoSource.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace telestudio;
using namespace std::chrono_literals;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void Until(F f) {
    auto end = std::chrono::steady_clock::now()+4s;
    while (!f()) { if (std::chrono::steady_clock::now()>end) throw std::runtime_error("Timed out"); std::this_thread::sleep_for(5ms); }
}
template<class Interface> struct Fake : Interface {
    FrameCallback frame; CaptureErrorCallback error; int starts=0, stops=0, expectedFps=30; std::string id;
    void Start(std::string_view value,int fps,FrameCallback f,CaptureErrorCallback e) override {
        Check(fps==expectedFps,"capture fps forwarded"); id=value; frame=std::move(f); error=std::move(e); ++starts;
    }
    void Stop() override { ++stops; }
};
ComposedVideoFrame Pixels(int y) { ComposedVideoFrame f; f.width=8; f.height=8; f.y.assign(64,uint8_t(y)); f.u.assign(16,90); f.v.assign(16,150); f.timestamp100ns=7; return f; }
int main() { try {
    {
        ComposedVideoFrame odd;odd.width=3;odd.height=3;odd.y.assign(9,210);odd.u.assign(4,128);odd.v.assign(4,128);
        Check(odd.View().strideU==2 && odd.View().strideV==2,"owned odd input must expose ceil chroma strides");
        auto input=std::make_shared<Fake<ICameraCaptureSource>>();
        ComposedVideoSource source([&]{return input;});source.Configure({64,64});
        std::atomic<int> pixel=0;
        source.Start([&](I420Frame frame){pixel=frame.y[0];});source.SetCameraEnabled(true);
        input->frame(odd.View());Until([&]{return pixel==210;});
        Check(source.Snapshot().cameraActive,"odd source survives owned controller composition");
    }
    {
        auto input=std::make_shared<Fake<ICameraCaptureSource>>();
        ComposedVideoSource source([&]{return input;});
        source.Configure({64,64});
        source.SetCameraEnabled(true);
        std::atomic<bool> checked=false, mismatch=false;
        source.StartWithState([&](I420Frame frame, ComposedSourceState captured, VideoCompositionSettings settings) {
            if (checked.exchange(true)) return;
            input->frame(Pixels(210).View());
            source.Configure({128,72,CameraOverlaySize::Large,CameraOverlayPosition::TopLeft});
            mismatch = frame.y[0]!=16 || captured.cameraActive || !captured.cameraRequested ||
                settings.width!=64 || settings.height!=64 || settings.cameraSize!=CameraOverlaySize::Medium ||
                !source.Snapshot().cameraActive || captured.revision>=source.Snapshot().revision;
        });
        Until([&]{return checked.load();});
        source.Stop();
        Check(!mismatch,"delivered state must belong to neutral frame, not input arriving during callback");
    }
    auto camera=std::make_shared<Fake<ICameraCaptureSource>>();
    auto desktop=std::make_shared<Fake<IDesktopCaptureSource>>();
    FrameCallback late; CaptureErrorCallback lateError;
    std::atomic<int> count=0, first=0, preview=0; std::atomic<int64_t> timestamp=0;
    {
        ComposedVideoSource source([&]{return camera;},[&]{return desktop;});
        VideoCompositionSettings settings; settings.width=64; settings.height=64; source.Configure(settings);
        source.SetPreviewCallback([&](I420Frame f){ preview=f.y[0]; });
        source.Start([&](I420Frame f){first=f.y[0]; timestamp=f.timestamp100ns; ++count;});
        Until([&]{return count>0;});
        Check(camera->starts==0 && desktop->starts==0,"Start must not acquire devices");
        Check(first==16 && preview==16,"neutral output and separate preview");
        source.SetCameraEnabled(true,"camera-id"); source.SetDesktopEnabled(true,"display-id");
        Check(!source.Snapshot().cameraActive && !source.Snapshot().desktopActive,"requested is not active");
        Check(camera->id=="camera-id" && desktop->id=="display-id","identifiers forwarded");
        auto c=Pixels(210), d=Pixels(70); camera->frame(c.View()); desktop->frame(d.View());
        c.y.assign(64,1); d.y.assign(64,2);
        Until([&]{return first==70;});
        Check(timestamp!=7,"output uses clock timestamp");
        Check(source.Snapshot().cameraActive && source.Snapshot().desktopActive,"first frames activate sources");
        late=camera->frame; lateError=camera->error;
        source.SetCameraEnabled(false);
        Check(camera->stops>0 && desktop->stops==0,"camera off stops only camera");
        late(Pixels(230).View()); lateError("late");
        Check(!source.Snapshot().cameraActive && source.Snapshot().cameraError.empty(),"old generation fenced");
        source.SetCameraEnabled(true);
        camera->frame(Pixels(210).View()); camera->error("disconnected");
        Check(!source.Snapshot().cameraActive && source.Snapshot().cameraError=="disconnected" && source.Snapshot().desktopActive,"failure clears affected layer");
        camera->frame(Pixels(210).View());
        Check(!source.Snapshot().cameraActive,"frames after terminal failure rejected");
        source.SetCameraEnabled(true); camera->frame(Pixels(210).View());
        Until([&]{return !source.Snapshot().cameraActive;});
        Check(!source.Snapshot().cameraError.empty() && source.Snapshot().desktopActive,"camera timeout does not expire static desktop");
        source.SetDesktopEnabled(false);
        Until([&]{return first==16 && preview==16;});
        source.Stop(); int stopped=count; std::this_thread::sleep_for(80ms);
        Check(count==stopped,"no output after Stop returns");
        Check(!source.Snapshot().cameraRequested && !source.Snapshot().desktopRequested,"Stop clears requests");
    }
    late(Pixels(200).View()); lateError("after destruction");
    {
        camera->expectedFps=15;
        ComposedVideoSource source([&]{return camera;},[&]{return desktop;});
        source.Start([](I420Frame){},15); source.SetCameraEnabled(true);
        Check(source.Snapshot().cameraError.empty(),"negotiated output clock forwarded to capture");
        camera->expectedFps=30;
    }
    // In-flight input delivery may overlap Stop/destruction; weak state must outlive no owner.
    {
        auto source=std::make_unique<ComposedVideoSource>([&]{return camera;},[&]{return desktop;});
        std::atomic<int> outputs=0;
        source->Start([&](I420Frame){++outputs;}); source->SetCameraEnabled(true);
        auto callback=camera->frame; auto error=camera->error;
        std::atomic<bool> keepSending=true, entered=false;
        std::thread input([&]{auto frame=Pixels(200); entered=true; while(keepSending) callback(frame.View());});
        Until([&]{return entered.load() && source->Snapshot().cameraActive;});
        source->Stop(); auto stopped=outputs.load(); source.reset();
        error("late concurrent failure"); std::this_thread::sleep_for(40ms);
        keepSending=false; input.join();
        Check(outputs==stopped,"in-flight input cannot resurrect stopped output");
    }
    {
        ComposedVideoSource source([&]{return camera;},[&]{return desktop;});
        std::thread cameraControls([&]{for(int i=0;i<30;++i) {source.SetCameraEnabled(true); source.SetCameraEnabled(false);}});
        std::thread desktopControls([&]{for(int i=0;i<30;++i) {source.SetDesktopEnabled(true,"display"); source.SetDesktopEnabled(false);}});
        cameraControls.join(); desktopControls.join();
        Check(!source.Snapshot().cameraRequested && !source.Snapshot().desktopRequested && source.Snapshot().revision>=120,
              "concurrent controls serialize without losing state revisions");
    }
    // Output callbacks may reenter controls and stop without self-join/deadlock.
    {
        ComposedVideoSource source([&]{return camera;},[&]{return desktop;});
        std::atomic<bool> done=false;
        source.Start([&](I420Frame){source.SetCameraEnabled(false); source.Stop(); done=true;});
        Until([&]{return done.load();});
    }
    // Invalid source frames must become observable failures, not stale pixels.
    {
        ComposedVideoSource source({},[&]{return desktop;});
        source.Configure({64,64}); source.Start([](I420Frame){});
        source.SetDesktopEnabled(true,"silent-window");
        auto lateDesktop=desktop->frame;
        const auto deadline=std::chrono::steady_clock::now()+5500ms;
        while(source.Snapshot().desktopError.empty() && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(10ms);
        Check(!source.Snapshot().desktopError.empty() && !source.Snapshot().desktopActive,
              "desktop with no first frame must fail within five seconds");
        lateDesktop(Pixels(70).View());
        Check(!source.Snapshot().desktopActive,"late desktop first frame after deadline stays fenced");
        source.SetDesktopEnabled(false);
        Check(!source.Snapshot().desktopRequested && source.Snapshot().desktopError.empty(),"off clears first-frame failure");
    }
    {
        ComposedVideoSource source([&]{return camera;},[&]{return desktop;});
        source.SetCameraEnabled(true); camera->frame(I420Frame{});
        Check(!source.Snapshot().cameraActive && !source.Snapshot().cameraError.empty(),"invalid input rejected");
    }
    std::cout<<"ComposedVideoSource lifecycle tests passed\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
