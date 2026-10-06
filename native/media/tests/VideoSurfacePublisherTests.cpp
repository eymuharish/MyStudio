#include "telestudio/media/VideoSurfacePublisher.h"
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
using namespace telestudio;
using Microsoft::WRL::ComPtr;
void Require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
uint8_t ReadPixel(PublishedSurface surface) {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"host device");
    ComPtr<ID3D11Device1> device1; device.As(&device1);
    ComPtr<ID3D11Texture2D> texture;
    Require(SUCCEEDED(device1->OpenSharedResource1(reinterpret_cast<HANDLE>(surface.hostHandleValue),IID_PPV_ARGS(&texture))),"host open");
    ComPtr<IDXGIKeyedMutex> mutex; texture.As(&mutex);
    Require(SUCCEEDED(mutex->AcquireSync(1,100)),"host ownership");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"staging");
    context->CopyResource(staging.Get(),texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; Require(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"readback");
    auto pixel=static_cast<uint8_t*>(mapped.pData)[0]; context->Unmap(staging.Get(),0);
    mutex->ReleaseSync(0); return pixel;
}
int main() { try {
    HostRegistration host{GetCurrentProcessId(),{}};
    VideoSurfacePublisher local(host), remote(host);
    local.Start(1); remote.Start(1);
    std::vector<uint8_t> y(64,16), u(16,128), v(16,128);
    I420Frame frame{y.data(),u.data(),v.data(),8,4,4,8,8,1};
    Require(!local.SubmitI420(99,frame),"stale call must not submit");
    Require(local.SubmitI420(1,frame),"local enqueue");
    y.assign(64,235);
    Require(remote.SubmitI420(1,frame),"remote enqueue");
    const auto a=local.Pump(), b=remote.Pump();
    Require(a && b && a->hostHandleValue!=b->hostHandleValue,"directions own separate GPU resources");
    Require(ReadPixel(*a)==0 && ReadPixel(*b)==255,"owned exact pixels survive caller mutation and stay separate");
    local.Acknowledge(SurfaceAcknowledgement::Opened,a->epoch);
    remote.Acknowledge(SurfaceAcknowledgement::Opened,b->epoch);
    local.Stop();
    Require(!local.SubmitI420(1,frame) && !local.Pump(),"stop fences queued output");
    local.Start(2); Require(!local.SubmitI420(1,frame),"old call cannot revive");
    local.SubmitI420(2,frame); auto c=local.Pump();
    Require(c && c->epoch>a->epoch && ReadPixel(*c)==255,"new call retains monotonic local epochs");
    local.Acknowledge(SurfaceAcknowledgement::Removed,a->epoch);
    local.Acknowledge(SurfaceAcknowledgement::Removed,c->epoch);
    local.Stop(); remote.Stop();
    local.Acknowledge(SurfaceAcknowledgement::Removed,c->epoch);
    remote.Acknowledge(SurfaceAcknowledgement::Removed,b->epoch);
    for(auto s:{*a,*b,*c}) CloseHandle(reinterpret_cast<HANDLE>(s.hostHandleValue));
    std::cout<<"surface publisher: PASS\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
