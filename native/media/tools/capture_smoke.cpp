#include "telestudio/media/DesktopCaptureSource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Arguments {
    std::string display = "primary";
    int seconds = 5;
    int fps = 15;
    bool animate = false;
    std::string output;
};

int ParsePositive(const char* text, const char* name) {
    const std::string value(text);
    size_t consumed = 0;
    const int result = std::stoi(value, &consumed);
    if (consumed != value.size() || result <= 0) {
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    }
    return result;
}

Arguments ParseArguments(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string option(argv[index]);
        if (index + 1 >= argc) {
            throw std::invalid_argument("Missing value for " + option);
        }
        if (option == "--output") {
            arguments.output = argv[++index];
        } else if (option == "--display") {
            arguments.display = argv[++index];
        } else if (option == "--seconds") {
            arguments.seconds = ParsePositive(argv[++index], "--seconds");
        } else if (option == "--fps") {
            arguments.fps = ParsePositive(argv[++index], "--fps");
        } else if (option == "--animate") {
            const std::string value(argv[++index]);
            if (value != "true" && value != "false") {
                throw std::invalid_argument("--animate must be true or false");
            }
            arguments.animate = value == "true";
        } else {
            throw std::invalid_argument("Unknown capture_smoke option: " + option);
        }
    }
    return arguments;
}

struct MonitorLookup {
    std::string device;
    RECT bounds{};
    bool found = false;
};

BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM contextValue) {
    auto& context = *reinterpret_cast<MonitorLookup*>(contextValue);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    const int required = WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 1) return TRUE;
    std::string device(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, device.data(), required,
                        nullptr, nullptr);
    device.resize(static_cast<std::size_t>(required - 1));
    if (device != context.device) return TRUE;
    context.bounds = info.rcMonitor;
    context.found = true;
    return FALSE;
}

class AnimatedCaptureStimulus {
public:
    explicit AnimatedCaptureStimulus(const std::string& displayId) {
        MonitorLookup lookup{displayId};
        EnumDisplayMonitors(nullptr, nullptr, FindMonitor,
                            reinterpret_cast<LPARAM>(&lookup));
        if (!lookup.found) throw std::runtime_error("Animated capture monitor was not found");

        instance_ = GetModuleHandleW(nullptr);
        className_ = L"EssaStudioCaptureSmokeStimulus";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = className_.c_str();
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        registered_ = RegisterClassW(&windowClass) != 0;
        if (!registered_ && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Animated capture window class registration failed");
        }
        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  className_.c_str(), L"EssaStudio capture stimulus",
                                  WS_POPUP, lookup.bounds.left + 20, lookup.bounds.top + 20,
                                  180, 110, nullptr, nullptr, instance_, this);
        if (!window_) {
            UnregisterOwnedClass();
            throw std::runtime_error("Animated capture window creation failed");
        }
        if (!SetTimer(window_, 1, 16, nullptr)) {
            DestroyWindow(window_);
            window_ = nullptr;
            UnregisterOwnedClass();
            throw std::runtime_error("Animated capture timer creation failed");
        }
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
    }

    ~AnimatedCaptureStimulus() {
        if (window_) {
            KillTimer(window_, 1);
            DestroyWindow(window_);
        }
        UnregisterOwnedClass();
    }

    void Pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

private:
    void UnregisterOwnedClass() noexcept {
        if (registered_) {
            UnregisterClassW(className_.c_str(), instance_);
            registered_ = false;
        }
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam,
                                            LPARAM lparam) {
        auto* self = reinterpret_cast<AnimatedCaptureStimulus*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<AnimatedCaptureStimulus*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else if (message == WM_TIMER && self) {
            ++self->phase_;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        } else if (message == WM_PAINT && self) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            const COLORREF color = RGB(self->phase_, 80 + (self->phase_ % 120),
                                       255 - self->phase_);
            HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &paint.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    std::wstring className_;
    bool registered_ = false;
    unsigned char phase_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = ParseArguments(argc, argv);
        const auto displays = telestudio::DesktopCaptureSource::Enumerate();
        if (displays.empty()) {
            throw std::runtime_error("No physical display is available for Windows Graphics Capture");
        }
        std::string displayId = arguments.display;
        if (displayId == "primary") {
            const auto primary = std::find_if(displays.begin(), displays.end(),
                                              [](const telestudio::DisplayInfo& display) {
                                                  return display.primary;
                                              });
            if (primary == displays.end()) {
                throw std::runtime_error("No primary display is available");
            }
            displayId = primary->id;
        }

        std::atomic<uint64_t> convertedFrames = 0;
        std::atomic<int> width = 0;
        std::atomic<int> height = 0;
        std::string runtimeError;
        std::unique_ptr<AnimatedCaptureStimulus> stimulus;
        if (arguments.animate) {
            stimulus = std::make_unique<AnimatedCaptureStimulus>(displayId);
        }
        telestudio::DesktopCaptureSource source;
        source.Start(
            displayId, arguments.fps,
            [&](telestudio::I420Frame frame) {
                width = frame.width;
                height = frame.height;
                auto count = ++convertedFrames;
                if (count == 5 && !arguments.output.empty()) {
                    std::ofstream out(arguments.output, std::ios::binary);
                    out << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
                    for (int y=0; y<frame.height; ++y) for (int x=0; x<frame.width; ++x) {
                        int l=int(frame.y[y*frame.strideY+x])-16,u=int(frame.u[(y/2)*frame.strideU+x/2])-128,v=int(frame.v[(y/2)*frame.strideV+x/2])-128;
                        unsigned char rgb[]={static_cast<unsigned char>(std::clamp((298*l+409*v+128)>>8,0,255)),static_cast<unsigned char>(std::clamp((298*l-100*u-208*v+128)>>8,0,255)),static_cast<unsigned char>(std::clamp((298*l+516*u+128)>>8,0,255))};
                        out.write(reinterpret_cast<const char*>(rgb),3);
                    }
                    if (!out) throw std::runtime_error("Capture output could not be written");
                }
            },
            [&](const std::string& error) { runtimeError = error; });

        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::seconds(arguments.seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (stimulus) stimulus->Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        source.Stop();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const uint64_t frames = convertedFrames.load();
        std::cout << "display=" << displayId
                  << " width=" << width.load()
                  << " height=" << height.load()
                  << " converted_frames=" << frames
                  << " dropped_frames=" << source.DroppedFrames()
                  << " elapsed_seconds=" << elapsed << '\n';
        if (!runtimeError.empty()) {
            throw std::runtime_error(runtimeError);
        }
        if (width.load() <= 0 || height.load() <= 0 || frames < 60) {
            throw std::runtime_error("Capture gate requires non-zero dimensions and at least 60 converted frames");
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "capture_smoke: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
