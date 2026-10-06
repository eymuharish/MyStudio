#include "telestudio/media/CameraCaptureSource.h"
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
namespace telestudio {
I420Buffer ConvertCameraRgb32Frame(std::span<const uint8_t> bytes,int width,int height,int pitch,int64_t timestamp) {
    if(width<2 || height<2 || width>4096 || height>2160 || width%2 || height%2)
        throw std::invalid_argument("Invalid camera dimensions");
    auto stride=static_cast<size_t>(std::abs(int64_t(pitch)));
    if(stride<size_t(width)*4 || stride*(height-1)+size_t(width)*4>bytes.size())
        throw std::invalid_argument("Invalid camera RGB32 buffer or pitch");
    std::vector<uint8_t> normalized(size_t(width)*height*4);
    auto top=pitch<0?bytes.data()+stride*(height-1):bytes.data();
    for(int row=0;row<height;++row)
        std::memcpy(normalized.data()+size_t(row)*width*4,top+ptrdiff_t(row)*pitch,size_t(width)*4);
    return ConvertBgraToI420(normalized,width,height,width*4,timestamp);
}
namespace {
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
void Check(HRESULT hr,const char* what) {
    if(FAILED(hr)) throw std::runtime_error(std::string(what)+" (HRESULT "+std::to_string(static_cast<unsigned long>(hr))+")");
}
struct Runtime {
    Runtime() {
        Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Camera COM MTA initialization");
        auto hr=MFStartup(MF_VERSION);
        if(FAILED(hr)) { CoUninitialize(); Check(hr,"Media Foundation startup"); }
    }
    ~Runtime() { MFShutdown(); CoUninitialize(); }
};
std::string Utf8(const wchar_t* value,UINT32 length) {
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,static_cast<int>(length),nullptr,0,nullptr,nullptr);
    if(bytes<=0) throw std::runtime_error("Invalid camera identifier/name");
    std::string result(bytes,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,static_cast<int>(length),result.data(),bytes,nullptr,nullptr);
    return result;
}
std::string Attribute(IMFAttributes* attrs,REFGUID key) {
    wchar_t* value=nullptr; UINT32 length=0;
    Check(attrs->GetAllocatedString(key,&value,&length),"Read camera attribute");
    try { auto result=Utf8(value,length); CoTaskMemFree(value); return result; }
    catch(...) { CoTaskMemFree(value); throw; }
}
std::vector<ComPtr<IMFActivate>> Devices() {
    ComPtr<IMFAttributes> attrs; Check(MFCreateAttributes(&attrs,1),"Camera attributes");
    Check(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),"Camera source type");
    IMFActivate** raw=nullptr; UINT32 count=0;
    Check(MFEnumDeviceSources(attrs.Get(),&raw,&count),"Enumerate cameras");
    std::vector<ComPtr<IMFActivate>> result;
    try { result.reserve(count); }
    catch(...) { for(UINT32 i=0;i<count;++i) raw[i]->Release(); CoTaskMemFree(raw); throw; }
    for(UINT32 i=0;i<count;++i) { ComPtr<IMFActivate> device; device.Attach(raw[i]); result.push_back(std::move(device)); }
    CoTaskMemFree(raw); return result;
}
struct Session {
    std::mutex mutex;
    std::condition_variable wake;
    std::recursive_mutex delivery;
    std::atomic<bool> stopped=false;
    bool ready=false, done=false;
    HRESULT status=S_OK;
    DWORD flags=0;
    LONGLONG timestamp=0;
    ComPtr<IMFSample> sample;
    FrameCallback frame;
    CaptureErrorCallback error;
    std::thread::id worker;
};
// At most one real camera worker process-wide, including an OS-blocked teardown.
// Recreating a controller cannot accumulate abandoned device/worker sessions.
std::mutex cameraLeaseMutex;
std::weak_ptr<Session> cameraLease;
// MF owns this COM callback. Weak session ownership avoids reader/callback cycles.
// Callback only hands off one pending sample; all MF operations stay on the MTA worker.
class ReaderCallback final : public IMFSourceReaderCallback {
    std::atomic<ULONG> refs_{1};
    std::weak_ptr<Session> session_;
public:
    explicit ReaderCallback(std::weak_ptr<Session> session):session_(std::move(session)) {}
    STDMETHODIMP QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER;
        *out=nullptr;
        if(iid==__uuidof(IUnknown) || iid==__uuidof(IMFSourceReaderCallback)) { *out=static_cast<IMFSourceReaderCallback*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { auto n=--refs_; if(!n) delete this; return n; }
    STDMETHODIMP OnReadSample(HRESULT hr,DWORD,DWORD flags,LONGLONG timestamp,IMFSample* sample) override {
        if(auto s=session_.lock()) {
            std::lock_guard lock(s->mutex);
            if(!s->stopped) { s->status=hr; s->flags=flags; s->timestamp=timestamp; s->sample=sample; s->ready=true; s->wake.notify_all(); }
        }
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD,IMFMediaEvent* event) override {
        HRESULT status=S_OK;
        if(event && SUCCEEDED(event->GetStatus(&status)) && FAILED(status)) return OnReadSample(status,0,0,0,nullptr);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
};
struct CaptureObjects {
    ComPtr<IMFActivate> activation;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    ~CaptureObjects() {
        if(reader) reader->Flush(MF_SOURCE_READER_ALL_STREAMS);
        if(source) source->Shutdown();
        reader.Reset(); source.Reset();
        if(activation) activation->ShutdownObject();
    }
};
void SetFormat(IMFSourceReader* reader,int fps) {
    // Prefer native resolution nearest HD and rate nearest requested clock.
    ComPtr<IMFMediaType> best; uint64_t bestScore=std::numeric_limits<uint64_t>::max();
    for(DWORD i=0;;++i) {
        ComPtr<IMFMediaType> candidate;
        auto hr=reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,i,&candidate);
        if(hr==MF_E_NO_MORE_TYPES) break;
        Check(hr,"Enumerate camera formats");
        UINT32 width=0,height=0,num=0,den=0;
        if(FAILED(MFGetAttributeSize(candidate.Get(),MF_MT_FRAME_SIZE,&width,&height)) ||
           width<2 || height<2 || width>4096 || height>2160 || width%2 || height%2) continue;
        MFGetAttributeRatio(candidate.Get(),MF_MT_FRAME_RATE,&num,&den);
        auto score=uint64_t(std::abs(int(width)-1280)+std::abs(int(height)-720))*100+
                   uint64_t(den?std::abs(int(num/den)-fps):1000);
        if(score<bestScore) { best=candidate; bestScore=score; }
    }
    if(!best) throw std::runtime_error("Camera has no supported bounded video format");
    Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,best.Get()),"Select native camera format");
    ComPtr<IMFMediaType> output; Check(MFCreateMediaType(&output),"Camera output type");
    Check(output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Camera major type");
    Check(output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32),"Camera RGB32 type");
    Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,output.Get()),"Camera RGB32 conversion unavailable");
}
I420Buffer ConvertSample(IMFSourceReader* reader,IMFSample* sample,LONGLONG timestamp) {
    ComPtr<IMFMediaType> type; Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type),"Current camera format");
    GUID subtype{}; Check(type->GetGUID(MF_MT_SUBTYPE,&subtype),"Camera subtype");
    if(subtype!=MFVideoFormat_RGB32) throw std::runtime_error("Unexpected camera output format");
    UINT32 w=0,h=0; Check(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h),"Camera dimensions");
    if(w<2 || h<2 || w>4096 || h>2160 || w%2 || h%2) throw std::runtime_error("Invalid camera dimensions");
    ComPtr<IMFMediaBuffer> buffer; Check(sample->ConvertToContiguousBuffer(&buffer),"Camera sample buffer");
    std::vector<uint8_t> bgra(size_t(w)*h*4);
    ComPtr<IMF2DBuffer> twoD;
    if(SUCCEEDED(buffer.As(&twoD))) {
        BYTE* scanline=nullptr; LONG pitch=0; Check(twoD->Lock2D(&scanline,&pitch),"Camera Lock2D");
        if(std::abs(int64_t(pitch))<int64_t(w)*4) { twoD->Unlock2D(); throw std::runtime_error("Invalid camera stride"); }
        for(UINT32 row=0;row<h;++row) std::memcpy(bgra.data()+size_t(row)*w*4,scanline+ptrdiff_t(row)*pitch,size_t(w)*4);
        twoD->Unlock2D();
    } else {
        UINT32 storedStride=0; LONG pitch=0;
        if(SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,&storedStride))) pitch=static_cast<LONG>(storedStride);
        else Check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,w,&pitch),"Camera default stride");
        BYTE* data=nullptr; DWORD length=0; Check(buffer->Lock(&data,nullptr,&length),"Camera buffer lock");
        try {
            auto result=ConvertCameraRgb32Frame(std::span<const uint8_t>(data,length),static_cast<int>(w),static_cast<int>(h),pitch,timestamp);
            buffer->Unlock(); return result;
        } catch(...) { buffer->Unlock(); throw; }
    }
    return ConvertCameraRgb32Frame(bgra,static_cast<int>(w),static_cast<int>(h),static_cast<int>(w)*4,timestamp);
}
void Capture(std::shared_ptr<Session> s,const std::string& id,int fps) {
    Runtime runtime;
    CaptureObjects objects;
    for(auto& device:Devices()) {
        if(id.empty() || Attribute(device.Get(),MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)==id) { objects.activation=device; break; }
    }
    if(s->stopped) return;
    if(!objects.activation) throw std::runtime_error(id.empty()?"No camera device available":"Requested camera device not found");
    Check(objects.activation->ActivateObject(IID_PPV_ARGS(&objects.source)),"Activate camera (check permission and device availability)");
    if(s->stopped) return;
    ComPtr<IMFAttributes> attrs; Check(MFCreateAttributes(&attrs,3),"Camera reader attributes");
    ComPtr<IMFSourceReaderCallback> callback; callback.Attach(new ReaderCallback(s));
    Check(attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,callback.Get()),"Asynchronous camera reader");
    Check(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE),"Camera color conversion");
    Check(MFCreateSourceReaderFromMediaSource(objects.source.Get(),attrs.Get(),&objects.reader),"Create camera reader");
    Check(objects.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Disable camera streams");
    Check(objects.reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),"Select camera video");
    SetFormat(objects.reader.Get(),fps);
    auto next=std::chrono::steady_clock::now();
    while(!s->stopped) {
        // Documented async path: all output arguments null, one request outstanding.
        Check(objects.reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,nullptr,nullptr,nullptr),"Request camera sample");
        ComPtr<IMFSample> sample; HRESULT status; DWORD flags; LONGLONG timestamp;
        {
            std::unique_lock lock(s->mutex);
            if(!s->wake.wait_for(lock,2s,[&]{return s->stopped || s->ready;})) throw std::runtime_error("Camera produced no sample within 2 seconds");
            if(s->stopped) return;
            status=s->status; flags=s->flags; timestamp=s->timestamp; sample=std::move(s->sample); s->ready=false;
        }
        Check(status,"Camera asynchronous capture failed");
        if(flags & (MF_SOURCE_READERF_ENDOFSTREAM|MF_SOURCE_READERF_ERROR)) throw std::runtime_error("Camera disconnected or ended stream");
        if(sample) {
            auto frame=ConvertSample(objects.reader.Get(),sample.Get(),timestamp);
            std::lock_guard delivery(s->delivery);
            auto callback=s->frame;
            if(!s->stopped && callback) callback(frame.View());
        }
        next=std::chrono::steady_clock::now()+std::chrono::nanoseconds(1000000000/fps);
        std::unique_lock lock(s->mutex); s->wake.wait_until(lock,next,[&]{return s->stopped.load();});
    }
}
void Worker(std::shared_ptr<Session> s,std::string id,int fps,detail::CameraCaptureOperation operation) {
    { std::lock_guard lock(s->mutex); s->worker=std::this_thread::get_id(); }
    try {
        if(operation) {
            operation(id,fps,[s](I420Frame frame) {
                std::lock_guard delivery(s->delivery); auto callback=s->frame;
                if(!s->stopped && callback) callback(frame);
            },[s](const std::string& error) {
                std::lock_guard delivery(s->delivery); auto callback=s->error;
                if(!s->stopped && callback) callback(error);
            },[s]{return s->stopped.load();});
        } else Capture(s,id,fps);
    }
    catch(const std::exception& e) {
        std::lock_guard delivery(s->delivery);
        auto callback=s->error;
        try { if(!s->stopped && callback) callback(e.what()); } catch(...) {}
    }
    catch(...) {
        std::lock_guard delivery(s->delivery);
        auto callback=s->error;
        try { if(!s->stopped && callback) callback("Unexpected camera capture failure"); } catch(...) {}
    }
    { std::lock_guard delivery(s->delivery); s->stopped=true; s->frame={}; s->error={}; }
    { std::lock_guard lock(s->mutex); s->sample.Reset(); s->done=true; s->wake.notify_all(); }
}
void Cancel(const std::shared_ptr<Session>& s) {
    if(!s) return;
    { std::lock_guard delivery(s->delivery); s->stopped=true; s->frame={}; s->error={}; s->wake.notify_all(); }
    std::unique_lock lock(s->mutex);
    if(s->worker!=std::this_thread::get_id()) s->wake.wait_for(lock,500ms,[&]{return s->done;});
    // A broken driver may block an OS shutdown call. It cannot call consumers after
    // this fence; the detached worker retains only its own session until OS returns.
}
}
struct CameraCaptureSource::Impl { std::mutex mutex; std::shared_ptr<Session> session; detail::CameraCaptureOperation operation; };
CameraCaptureSource::CameraCaptureSource():impl_(std::make_unique<Impl>()) {}
CameraCaptureSource::CameraCaptureSource(detail::CameraCaptureOperation operation):CameraCaptureSource() {
    if(!operation) throw std::invalid_argument("Camera capture operation required");
    impl_->operation=std::move(operation);
}
CameraCaptureSource::~CameraCaptureSource() { Stop(); }
std::vector<CameraDeviceInfo> CameraCaptureSource::Enumerate() {
    std::vector<CameraDeviceInfo> result; std::exception_ptr error;
    // Enumeration can be requested by an STA UI thread; do all MF work on an MTA.
    std::thread worker([&] { try { Runtime runtime; for(auto& device:Devices()) result.push_back({
        Attribute(device.Get(),MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK),
        Attribute(device.Get(),MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)}); } catch(...) {error=std::current_exception();} });
    worker.join(); if(error) std::rethrow_exception(error); return result;
}
void CameraCaptureSource::Start(std::string_view id,int fps,FrameCallback frame,CaptureErrorCallback error) {
    if(fps<1 || fps>60 || !frame) throw std::invalid_argument("Camera callback and fps 1..60 required");
    Stop();
    auto s=std::make_shared<Session>(); s->frame=std::move(frame); s->error=std::move(error);
    bool unavailable=false;
    {
        std::lock_guard leaseLock(cameraLeaseMutex);
        if(auto prior=cameraLease.lock()) {
            std::lock_guard lock(prior->mutex); unavailable=!prior->done;
        }
        if(!unavailable) {
            std::lock_guard lock(impl_->mutex);
            impl_->session=s; cameraLease=s;
            try { std::thread(Worker,s,std::string(id),fps,impl_->operation).detach(); }
            catch(...) { impl_->session.reset(); cameraLease.reset(); throw; }
        }
    }
    if(unavailable && s->error) s->error("Camera unavailable: previous capture or driver teardown has not completed");
}
void CameraCaptureSource::Stop() {
    std::shared_ptr<Session> old;
    { std::lock_guard lock(impl_->mutex); old=impl_->session; }
    Cancel(old);
}
} // namespace telestudio
