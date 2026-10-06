#include "../OpalMedia.h"
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <thread>
#include <stdexcept>
#include <iostream>
using namespace telestudio;
using Microsoft::WRL::ComPtr;
static void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static void RequireSelectedSource(const std::vector<uint8_t>& bytes){
 constexpr unsigned width=320,height=240,pixelBytes=width*height*3/2;
 Require(bytes.size()>=pixelBytes,"Main raw video payload is truncated");
 const auto* pixels=bytes.data()+bytes.size()-pixelBytes;
 Require(pixels[(height/2)*width+width/2]>190,"Main video does not contain the selected shared source");
}
class WhiteWindow final:public IDesktopCaptureSource {
 void Start(std::string_view,int,FrameCallback callback,CaptureErrorCallback)override{
  std::array<uint8_t,256> y;std::array<uint8_t,64> u,v;y.fill(200);u.fill(128);v.fill(128);
  callback({y.data(),u.data(),v.data(),16,8,8,16,16,1});
 }
 void Stop()override{}
};
static uint8_t ReadPixel(const PublishedSurface& surface){
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
 if(FAILED(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
 Require(SUCCEEDED(hr),"host GPU device");ComPtr<ID3D11Device1> v1;device.As(&v1);
 ComPtr<ID3D11Texture2D> texture;Require(SUCCEEDED(v1->OpenSharedResource1(reinterpret_cast<HANDLE>(surface.hostHandleValue),IID_PPV_ARGS(&texture))),"open preview");
 ComPtr<IDXGIKeyedMutex> mutex;texture.As(&mutex);auto ownership=mutex->AcquireSync(1,0);if(ownership!=S_OK)ownership=mutex->AcquireSync(0,100);Require(ownership==S_OK,"preview ownership");
 D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> staging;Require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"staging");context->CopyResource(staging.Get(),texture.Get());
 D3D11_MAPPED_SUBRESOURCE mapped{};Require(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"read preview");
 const auto pixel=static_cast<uint8_t*>(mapped.pData)[(desc.Height/2)*mapped.RowPitch+(desc.Width/2)*4];
 context->Unmap(staging.Get(),0);mutex->ReleaseSync(0);return pixel;
}
int main(){try{
 _putenv_s("ESSASTUDIO_TEST_MODE","1");studio::opal::StudioMedia media([]{return std::make_shared<WhiteWindow>();});
 media.localSurface=std::make_unique<VideoSurfacePublisher>(HostRegistration{GetCurrentProcessId(),{}});media.Start(1);
 media.Desktop(true,"window:white",true);std::this_thread::sleep_for(std::chrono::milliseconds(200));
 std::vector<uint8_t> bytes;uint32_t stamp=9000;
 Require(media.ReadVideo(bytes,320,240,stamp,true),"presentation read");
 auto publication=media.localSurface->Pump();Require(publication.has_value(),"presentation preview");
 auto surface=*publication;Require(ReadPixel(surface)>190,"selected white window not previewed");media.Ack(true,SurfaceAcknowledgement::Opened,surface.epoch);
 for(int i=0;i<12;++i){
  Require(media.ReadVideo(bytes,320,240,stamp+=3000,false),"primary media must continue");RequireSelectedSource(bytes);media.localSurface->Pump();
  Require(ReadPixel(surface)>190,"primary video overwrote presentation preview");
  Require(media.ReadVideo(bytes,320,240,stamp+=3000,true),"presentation continues");media.localSurface->Pump();
  Require(ReadPixel(surface)>190,"primary video overwrote presentation preview");
 }
 media.Desktop(false,{},true);Require(media.ReadVideo(bytes,320,240,stamp+=3000,false),"primary after stop");media.localSurface->Pump();
 Require(ReadPixel(surface)<190,"stopped sharing left stale presentation preview");
 for(unsigned cycle=0;cycle<3;++cycle){
  media.Desktop(true,"window:white",false);std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Require(media.ReadVideo(bytes,320,240,stamp+=3000,false),"fallback share read");media.localSurface->Pump();
  Require(ReadPixel(surface)>190,"fallback sharing did not restore selected source on the published epoch");
  media.Desktop(false,{},false);Require(media.ReadVideo(bytes,320,240,stamp+=3000,false),"fallback stop read");media.localSurface->Pump();
  Require(ReadPixel(surface)<190,"fallback stop retained selected source");
  media.Desktop(true,"window:white",true);std::this_thread::sleep_for(std::chrono::milliseconds(200));
  for(unsigned frame=0;frame<4;++frame){
   Require(media.ReadVideo(bytes,320,240,stamp+=3000,true),"dedicated restart read");media.localSurface->Pump();
   Require(ReadPixel(surface)>190,"dedicated restart lost selected source on the published epoch");
   Require(media.ReadVideo(bytes,320,240,stamp+=3000,false),"main during dedicated restart");RequireSelectedSource(bytes);media.localSurface->Pump();
   Require(ReadPixel(surface)>190,"main overwrote dedicated restart preview");
  }
  media.Desktop(false,{},true);Require(media.ReadVideo(bytes,320,240,stamp+=3000,false),"dedicated stop read");media.localSurface->Pump();
  Require(ReadPixel(surface)<190,"dedicated stop retained selected source");
 }
 media.Stop();media.Ack(true,SurfaceAcknowledgement::Removed,surface.epoch);CloseHandle(reinterpret_cast<HANDLE>(surface.hostHandleValue));
 std::cout<<"local preview stream routing: PASS\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
