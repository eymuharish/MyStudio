#include "telestudio/media/VideoSurfacePublisher.h"
#include <libyuv/convert_argb.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
namespace telestudio {
std::optional<HostRegistration> SurfaceHostFromEnvironment() {
    const auto pid=std::getenv("ESSASTUDIO_SURFACE_HOST_PID");
    const auto token=std::getenv("ESSASTUDIO_SURFACE_SESSION_TOKEN");
    if(!pid || !token) return {};
    const std::string_view value(pid);
    uint32_t parsed=0;
    auto result=std::from_chars(value.data(),value.data()+value.size(),parsed);
    auto bytes=DecodeSessionTokenBase64Url(token);
    if(value.empty() || value.front()=='0' || result.ec!=std::errc{} ||
       result.ptr!=value.data()+value.size() || !parsed || !bytes) return {};
    return HostRegistration{parsed,*bytes};
}
struct VideoSurfacePublisher::Impl {
    struct Owner {
        std::unique_ptr<RemoteVideoSurface> surface;
        uint64_t base=0, last=0;
        std::chrono::steady_clock::time_point expires;
    };
    HostRegistration host;
    std::mutex mutex;
    uint64_t generation=0,lastEpoch=0;
    std::optional<DecodedBgraFrame> pending;
    Owner current;
    std::deque<Owner> retired;
    void Sweep() {
        const auto now=std::chrono::steady_clock::now();
        std::erase_if(retired,[&](const Owner& o){return now>=o.expires;});
    }
    void Stop() {
        generation=0; pending.reset();
        if(current.surface) {
            current.surface->BeginRemoval();
            current.expires=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            retired.push_back(std::move(current)); current={};
        }
        Sweep();
    }
};
VideoSurfacePublisher::VideoSurfacePublisher(HostRegistration host):impl_(std::make_unique<Impl>()) {impl_->host=host;}
VideoSurfacePublisher::~VideoSurfacePublisher()=default;
void VideoSurfacePublisher::Start(uint64_t generation) {
    if(!generation) throw std::invalid_argument("surface call generation required");
    std::lock_guard lock(impl_->mutex);
    if(impl_->generation==generation) return;
    impl_->Stop();
    auto surface=std::make_unique<RemoteVideoSurface>(impl_->host);
    if(!surface->RegisterHost(impl_->host)) throw std::runtime_error("surface host registration rejected");
    impl_->current.surface=std::move(surface); impl_->current.base=impl_->lastEpoch;
    impl_->generation=generation;
}
void VideoSurfacePublisher::Stop() {std::lock_guard lock(impl_->mutex); impl_->Stop();}
bool VideoSurfacePublisher::SubmitI420(uint64_t generation,const I420Frame& f) {
    if(f.width<2 || f.height<2 || f.width>4096 || f.height>2160 || !f.y || !f.u || !f.v ||
       f.strideY<f.width || f.strideU<(f.width+1)/2 || f.strideV<(f.width+1)/2) return false;
    std::lock_guard lock(impl_->mutex);
    if(!generation || generation!=impl_->generation) return false;
    DecodedBgraFrame bgra{f.width,f.height,f.width*4,f.timestamp100ns,
        std::vector<uint8_t>(size_t(f.width)*f.height*4)};
    if(libyuv::I420ToARGB(f.y,f.strideY,f.u,f.strideU,f.v,f.strideV,
        bgra.bytes.data(),bgra.stride,f.width,f.height)!=0) return false;
    impl_->pending=std::move(bgra); return true;
}
bool VideoSurfacePublisher::SubmitBgra(uint64_t generation,const DecodedBgraFrame& frame) {
    if(frame.width<2 || frame.height<2 || frame.width>4096 || frame.height>2160 ||
       frame.stride<frame.width*4 || frame.bytes.size()<size_t(frame.stride)*frame.height) return false;
    std::lock_guard lock(impl_->mutex);
    if(!generation || generation!=impl_->generation) return false;
    impl_->pending=frame; return true;
}
std::optional<PublishedSurface> VideoSurfacePublisher::Pump() {
    std::lock_guard lock(impl_->mutex); impl_->Sweep();
    if(!impl_->generation || !impl_->pending) return {};
    auto frame=std::move(*impl_->pending); impl_->pending.reset();
    auto& current=impl_->current;
    if(!current.surface->UpdateOnRenderThread(frame)) return {};
    auto publication=current.surface->PublishForHost(); publication.epoch+=current.base;
    if(publication.epoch==current.last) return {};
    current.last=publication.epoch; impl_->lastEpoch=publication.epoch;
    return publication;
}
void VideoSurfacePublisher::Acknowledge(SurfaceAcknowledgement kind,uint64_t epoch) {
    std::lock_guard lock(impl_->mutex); impl_->Sweep();
    auto apply=[&](Impl::Owner& owner) {
        if(!owner.surface || epoch<=owner.base || epoch>owner.last) return;
        const auto local=epoch-owner.base;
        switch(kind) {
        case SurfaceAcknowledgement::Opened: owner.surface->Opened(local); break;
        case SurfaceAcknowledgement::Released: owner.surface->Released(local); break;
        case SurfaceAcknowledgement::Removed: owner.surface->Removed(local); break;
        }
    };
    apply(impl_->current); for(auto& owner:impl_->retired) apply(owner);
}
}
