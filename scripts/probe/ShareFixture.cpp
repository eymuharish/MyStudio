// Two owned windows: cyan selected content and a magenta desktop decoy.
// This test fixture never reads other windows or captures the user's desktop.
#include <Windows.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
HWND selected=nullptr,decoy=nullptr; unsigned tick=0;
auto began=std::chrono::steady_clock::now(); int seconds=150;
LRESULT CALLBACK WindowProcedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_NCCREATE)SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    if(message==WM_TIMER) {
        ++tick;InvalidateRect(window,nullptr,FALSE);
        if(std::chrono::steady_clock::now()-began>=std::chrono::seconds(seconds))PostMessageW(selected,WM_CLOSE,0,0);
        return 0;
    }
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT area{};GetClientRect(window,&area);
        bool source=GetWindowLongPtrW(window,GWLP_USERDATA)==2;
        auto brush=CreateSolidBrush(source?RGB(0,160,240):RGB(240,0,240));FillRect(dc,&area,brush);DeleteObject(brush);
        if(source) {
            RECT marker{20+static_cast<LONG>((tick*7)%350),80,100+static_cast<LONG>((tick*7)%350),140};
            FillRect(dc,&marker,reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
            const wchar_t* label=L"SELECTED SHARE ONLY - CYAN";TextOutW(dc,20,20,label,lstrlenW(label));
        }
        EndPaint(window,&paint);return 0;
    }
    if(message==WM_CLOSE) {
        if(decoy&&IsWindow(decoy)&&decoy!=window)DestroyWindow(decoy);
        DestroyWindow(window);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
int wmain(int argc,wchar_t**argv) {
    if(argc==2)seconds=_wtoi(argv[1]);if(seconds<15||seconds>2400)return 2;
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW type{};type.lpfnWndProc=WindowProcedure;type.hInstance=instance;
    type.lpszClassName=L"MyStudioOwnedSharePattern";type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));RegisterClassW(&type);
    selected=CreateWindowExW(0,type.lpszClassName,L"MyStudio selected share fixture",WS_OVERLAPPEDWINDOW,20,50,600,400,nullptr,nullptr,instance,reinterpret_cast<void*>(2));
    decoy=CreateWindowExW(0,type.lpszClassName,L"MyStudio excluded desktop decoy",WS_OVERLAPPEDWINDOW,660,50,600,400,nullptr,nullptr,instance,reinterpret_cast<void*>(1));
    if(!selected||!decoy)return 2;
    ShowWindow(decoy,SW_SHOWNOACTIVATE);ShowWindow(selected,SW_SHOWNOACTIVATE);
    SetTimer(selected,1,150,nullptr);SetTimer(decoy,1,150,nullptr);
    std::cout<<"{\"selectedHandle\":"<<reinterpret_cast<uintptr_t>(selected)<<",\"decoyHandle\":"<<reinterpret_cast<uintptr_t>(decoy)<<",\"seconds\":"<<seconds<<"}"<<std::endl;
    std::thread([]{std::string line;while(std::getline(std::cin,line))if(line=="quit"){PostMessageW(selected,WM_CLOSE,0,0);return;}}).detach();
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return 0;
}
