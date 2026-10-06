#include <telestudio/media/DesktopCaptureSource.h>
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr wchar_t Title[] = L"MyStudio Capture Identity Regression";
void Require(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
 if(message==WM_DESTROY) { PostQuitMessage(0); return 0; }
 return DefWindowProcW(window,message,w,l);
}
struct Child {
 PROCESS_INFORMATION process{}; HWND window=nullptr;
 ~Child() {
  if(window) PostMessageW(window,WM_CLOSE,0,0);
  if(process.hProcess) { if(WaitForSingleObject(process.hProcess,3000)!=WAIT_OBJECT_0) TerminateProcess(process.hProcess,1); CloseHandle(process.hProcess); }
  if(process.hThread) CloseHandle(process.hThread);
 }
};
}
int main(int argc,char**) { try {
 if(argc>1) {
  WNDCLASSW cls{}; cls.lpfnWndProc=WindowProc; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=Title;
  Require(RegisterClassW(&cls)!=0,"helper window class registration failed");
  HWND window=CreateWindowExW(0,Title,Title,WS_OVERLAPPEDWINDOW,100,100,320,180,nullptr,nullptr,cls.hInstance,nullptr);
  Require(window!=nullptr,"helper window creation failed"); ShowWindow(window,SW_SHOWNOACTIVATE);
  MSG message{}; while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); } return 0;
 }
 wchar_t executable[32768]; Require(GetModuleFileNameW(nullptr,executable,32768)>0,"test executable path unavailable");
 std::wstring command=L"\""+std::wstring(executable)+L"\" --capture-window";
 STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE; Child child;
 Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.process)!=0,"helper process failed");
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(std::chrono::steady_clock::now()<deadline) {
  child.window=FindWindowW(Title,Title);
  if(child.window&&IsWindowVisible(child.window)) break;
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
 }
 Require(child.window&&IsWindowVisible(child.window),"visible helper window unavailable");
 std::string original;
 for(const auto& item:telestudio::DesktopCaptureSource::EnumerateCaptureItems())
  if(item.kind==telestudio::CaptureItemKind::Window&&item.name=="MyStudio Capture Identity Regression") original=item.id;
 Require(!original.empty(),"helper window missing from capture catalog");
 Require(telestudio::DesktopCaptureSource::ValidateCaptureItem(original),"original window identity rejected");
 std::string wrongProcess=original; wrongProcess[21]=wrongProcess[21]=='A'?'B':'A';
 Require(!telestudio::DesktopCaptureSource::ValidateCaptureItem(wrongProcess),"wrong-process window identity accepted");
 std::mutex mutex; std::condition_variable arrived; bool gotFrame=false;
 telestudio::DesktopCaptureSource capture;
 auto start=[&](const std::string& id) {
  {std::scoped_lock lock(mutex); gotFrame=false;}
  capture.StartOwned(id,15,160,90,[&](telestudio::OwnedCaptureFrame frame) {
   std::scoped_lock lock(mutex); gotFrame=frame.pixels&&frame.pixels->Width()==160; arrived.notify_all();
  });
  std::unique_lock lock(mutex); Require(arrived.wait_for(lock,std::chrono::seconds(3),[&]{return gotFrame;}),"reused window failed to produce fresh capture pixels");
  lock.unlock(); capture.Stop();
 };
 start(original);
 Require(SendMessageW(child.window,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"MyStudio Capture Identity Regression - Slide 2"))!=0,"helper title rename failed");
 Require(telestudio::DesktopCaptureSource::ValidateCaptureItem(original),"same live window rejected after mutable title change");
 bool stable=false;
 for(const auto& item:telestudio::DesktopCaptureSource::EnumerateCaptureItems())
  if(item.name=="MyStudio Capture Identity Regression - Slide 2") stable=item.id==original;
 Require(stable,"catalog identity changes when a live window title changes");
 // esw1 originally stored a title hash in the final eight bytes. A cached
 // legacy token must still resolve to the same validated HWND/process.
 std::string legacy=original; legacy.replace(37,11,"__________8");
 Require(telestudio::DesktopCaptureSource::ValidateCaptureItem(legacy),"legacy title-bearing token rejected for same live window");
 start(legacy);
 PostMessageW(child.window,WM_CLOSE,0,0); Require(WaitForSingleObject(child.process.hProcess,3000)==WAIT_OBJECT_0,"helper window failed to close"); child.window=nullptr;
 Require(!telestudio::DesktopCaptureSource::ValidateCaptureItem(original),"closed window identity accepted");
 Require(!telestudio::DesktopCaptureSource::ValidateCaptureItem("esw1.invalid"),"malformed identity accepted");
 std::cout<<"Capture identity title-change lifecycle PASS\n"; return 0;
 }catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
