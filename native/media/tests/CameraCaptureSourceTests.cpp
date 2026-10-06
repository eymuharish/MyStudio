#include "telestudio/media/CameraCaptureSource.h"
#include <iostream>
#include <stdexcept>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
int main() {
    // These validation/lifetime paths must never enumerate or activate hardware.
    telestudio::CameraCaptureSource camera;
    for(int fps : {0,-1,61}) {
        bool rejected=false;
        try { camera.Start("",fps,[](auto){},{}); } catch(const std::invalid_argument&) { rejected=true; }
        if(!rejected) { std::cerr<<"Invalid camera fps accepted\n"; return 1; }
    }
    bool rejected=false;
    try { camera.Start("",30,{},{}); } catch(const std::invalid_argument&) { rejected=true; }
    if(!rejected) { std::cerr<<"Missing camera callback accepted\n"; return 1; }
    camera.Stop(); camera.Stop();
    // Signed camera pitch and padding must not invert or contaminate rows.
    std::array<uint8_t,24> pixels={0,0,0,255,0,0,0,255,99,99,99,99,
                                  255,255,255,255,255,255,255,255,88,88,88,88};
    auto top=telestudio::ConvertCameraRgb32Frame(pixels,2,2,12,19);
    auto bottom=telestudio::ConvertCameraRgb32Frame(pixels,2,2,-12,20);
    if(top.Y()[0]!=16 || top.Y()[2]!=235 || bottom.Y()[0]!=235 || bottom.Y()[2]!=16 ||
       top.U()[0]!=128 || top.V()[0]!=128 || top.Timestamp100ns()!=19) return 1;
    pixels.fill(0);
    if(top.Y()[2]!=235) return 1;
    rejected=false;
    try { (void)telestudio::ConvertCameraRgb32Frame(std::span<const uint8_t>(pixels).first(15),2,2,12,0); }
    catch(const std::invalid_argument&) { rejected=true; }
    if(!rejected) return 1;
    // A hung driver retains its lease across destruction: retries cannot spawn
    // unbounded workers, and an eventual late sample cannot cross the Stop fence.
    using namespace std::chrono_literals;
    std::atomic<bool> entered=false, release=false, returned=false;
    std::atomic<int> delivered=0, starts=0, errors=0;
    auto driver=[&](std::string_view,int,telestudio::FrameCallback frame,
                    telestudio::CaptureErrorCallback,std::function<bool()>) {
        ++starts; entered=true;
        while(!release) std::this_thread::sleep_for(1ms);
        frame({}); returned=true;
    };
    {
        telestudio::CameraCaptureSource blocked(driver);
        blocked.Start("fake",30,[&](auto){++delivered;},{});
        while(!entered) std::this_thread::sleep_for(1ms);
        auto before=std::chrono::steady_clock::now(); blocked.Stop();
        if(std::chrono::steady_clock::now()-before>1500ms) return 1;
    }
    telestudio::CameraCaptureSource retry(driver);
    for(int i=0;i<5;++i) retry.Start("fake",30,[&](auto){++delivered;},[&](auto){++errors;});
    if(starts!=1 || errors!=5) {release=true; return 1;}
    release=true;
    while(!returned) std::this_thread::sleep_for(1ms);
    if(delivered!=0) return 1;
    // Completion releases the process lease; the next source can really run.
    auto deadline=std::chrono::steady_clock::now()+2s;
    while(starts==1 && std::chrono::steady_clock::now()<deadline) {
        retry.Start("fake",30,[&](auto){++delivered;},[&](auto){++errors;});
        std::this_thread::sleep_for(5ms);
    }
    retry.Stop();
    if(starts<2) return 1;
    std::cout<<"Camera non-hardware validation tests passed\n";
}
