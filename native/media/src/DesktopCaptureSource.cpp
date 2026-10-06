#include "telestudio/media/DesktopCaptureSource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

namespace telestudio {
namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::SizeInt32;

struct MonitorRecord {
    HMONITOR handle = nullptr;
    DisplayInfo display;
};

struct NativeCaptureRecord {
    CaptureItemInfo info;
    bool primary = false;
    HMONITOR monitor = nullptr;
    HWND window = nullptr;
};

std::string WideToUtf8(std::wstring_view text);
std::string NormalizeCaptureName(std::string value);

constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string EncodeBase64Url(std::span<const std::uint8_t> bytes) {
    std::string result;
    result.reserve((bytes.size() * 4 + 2) / 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const auto byte : bytes) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            result.push_back(kBase64Url[(accumulator >> bits) & 63]);
        }
    }
    if (bits != 0) result.push_back(kBase64Url[(accumulator << (6 - bits)) & 63]);
    return result;
}

std::optional<std::vector<std::uint8_t>> DecodeBase64Url(std::string_view text) {
    if (text.empty() || text.find('=') != std::string_view::npos) return std::nullopt;
    std::vector<std::uint8_t> result;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char ch : text) {
        const char* found = std::find(std::begin(kBase64Url), std::end(kBase64Url) - 1, ch);
        if (found == std::end(kBase64Url) - 1) return std::nullopt;
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(found - kBase64Url);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    if (bits != 0 && (accumulator & ((1u << bits) - 1u)) != 0) return std::nullopt;
    return result;
}

template <typename T>
void AppendScalar(std::vector<std::uint8_t>& bytes, T value) {
    for (std::size_t index = 0; index != sizeof(T); ++index) {
        bytes.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> (index * 8)) & 0xff));
    }
}

template <typename T>
T ReadScalar(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index != sizeof(T); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
    }
    return static_cast<T>(value);
}

std::uint64_t ProcessCreationTime(DWORD pid) noexcept {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    if (!ok) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

std::wstring WindowTitle(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0 || length > 8192) return {};
    std::wstring title(static_cast<std::size_t>(length + 1), L'\0');
    const int written = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (written <= 0) return {};
    title.resize(static_cast<std::size_t>(written));
    return title;
}

std::string EncodeWindowId(HWND window, DWORD pid, std::uint64_t creation) {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'E','S','W',1});
    AppendScalar(bytes, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window)));
    AppendScalar(bytes, static_cast<std::uint32_t>(pid));
    AppendScalar(bytes, creation);
    // Keep the existing token size; titles are mutable presentation metadata,
    // not window identity. Old esw1 tokens remain readable below.
    AppendScalar(bytes, std::uint64_t{0});
    return "esw1." + EncodeBase64Url(bytes);
}

bool IsWindowCapturable(HWND window, CaptureItemInfo* output = nullptr) noexcept {
    try {
        if (!window || !IsWindow(window) || !IsWindowVisible(window) ||
            GetAncestor(window, GA_ROOT) != window || GetWindow(window, GW_OWNER) != nullptr) {
            return false;
        }
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((style & WS_CHILD) != 0 || (exStyle & WS_EX_TOOLWINDOW) != 0) return false;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
            return false;
        }
        RECT bounds{};
        if (!GetWindowRect(window, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            return false;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) return false;
        const std::uint64_t creation = ProcessCreationTime(pid);
        const std::wstring title = WindowTitle(window);
        if (creation == 0 || title.empty()) return false;
        if (output) {
            output->id = EncodeWindowId(window, pid, creation);
            output->kind = CaptureItemKind::Window;
            output->name = NormalizeCaptureName(WideToUtf8(title));
            output->width = bounds.right - bounds.left;
            output->height = bounds.bottom - bounds.top;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<HWND> DecodeAndRevalidateWindow(std::string_view id) noexcept {
    try {
        if (!id.starts_with("esw1.")) return std::nullopt;
        const auto bytes = DecodeBase64Url(id.substr(5));
        if (!bytes || bytes->size() != 32 || (*bytes)[0] != 'E' || (*bytes)[1] != 'S' ||
            (*bytes)[2] != 'W' || (*bytes)[3] != 1) return std::nullopt;
        const auto hwndValue = ReadScalar<std::uint64_t>(*bytes, 4);
        const auto pid = ReadScalar<std::uint32_t>(*bytes, 12);
        const auto creation = ReadScalar<std::uint64_t>(*bytes, 16);
        HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hwndValue));
        CaptureItemInfo current;
        if (!IsWindowCapturable(window, &current)) return std::nullopt;
        DWORD currentPid = 0;
        GetWindowThreadProcessId(window, &currentPid);
        if (currentPid != pid || ProcessCreationTime(pid) != creation) return std::nullopt;
        return window;
    } catch (...) {
        return std::nullopt;
    }
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    }
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                           static_cast<int>(text.size()), result.data(), size, nullptr, nullptr) != size) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    }
    return result;
}

std::string NormalizeCaptureName(std::string value) {
    std::size_t index = 0;
    std::size_t scalars = 0;
    while (index < value.size() && scalars < 511 && index < 2045) {
        const auto first = static_cast<unsigned char>(value[index]);
        const std::size_t length = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 :
                                   (first & 0xf0) == 0xe0 ? 3 : 4;
        if (index + length > value.size() || index + length + 3 > 2048) break;
        index += length;
        ++scalars;
    }
    if (index < value.size()) {
        value.resize(index);
        value += "\xE2\x80\xA6";
    }
    return value;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            result.data(), size) != size) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    }
    return result;
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& records = *reinterpret_cast<std::vector<MonitorRecord>*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    const std::wstring deviceName(info.szDevice);
    try {
        records.push_back({monitor,
                           {WideToUtf8(deviceName), deviceName, width, height,
                            (info.dwFlags & MONITORINFOF_PRIMARY) != 0}});
    } catch (...) {
        return FALSE;
    }
    return TRUE;
}

std::vector<MonitorRecord> EnumerateMonitorRecords() {
    std::vector<MonitorRecord> records;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                             reinterpret_cast<LPARAM>(&records))) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    }
    return records;
}

BOOL CALLBACK CollectWindow(HWND window, LPARAM context) {
    auto& records = *reinterpret_cast<std::vector<NativeCaptureRecord>*>(context);
    CaptureItemInfo info;
    if (IsWindowCapturable(window, &info)) records.push_back({std::move(info), false, nullptr, window});
    return TRUE;
}

std::vector<NativeCaptureRecord> EnumerateNativeCaptureRecords() {
    std::vector<NativeCaptureRecord> records;
    for (const auto& monitor : EnumerateMonitorRecords()) {
        records.push_back({{monitor.display.id, CaptureItemKind::Monitor,
                            NormalizeCaptureName(WideToUtf8(monitor.display.name)),
                            monitor.display.width, monitor.display.height},
                           monitor.display.primary, monitor.handle, nullptr});
    }
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&records));
    std::sort(records.begin(), records.end(), [](const NativeCaptureRecord& left,
                                                 const NativeCaptureRecord& right) {
        if (left.info.kind != right.info.kind) return left.info.kind == CaptureItemKind::Monitor;
        if (left.info.kind == CaptureItemKind::Monitor) {
            if (left.primary != right.primary) return left.primary;
            return left.info.id < right.info.id;
        }
        const auto leftName = Utf8ToWide(left.info.name);
        const auto rightName = Utf8ToWide(right.info.name);
        const int comparison = CompareStringOrdinal(leftName.data(), static_cast<int>(leftName.size()),
                                                    rightName.data(), static_cast<int>(rightName.size()), TRUE);
        return comparison == CSTR_EQUAL ? left.info.id < right.info.id : comparison == CSTR_LESS_THAN;
    });
    return records;
}

std::string HResultMessage(const winrt::hresult_error& error) {
    return WideToUtf8(error.message());
}

} // namespace

struct DesktopCaptureSource::Impl : std::enable_shared_from_this<DesktopCaptureSource::Impl> {
    enum class LifecycleState {
        Stopped,
        Starting,
        Running,
        Stopping,
        Failed,
    };

    mutable std::mutex mutex;
    std::condition_variable stateChanged;
    std::condition_variable frameReady;
    LifecycleState state = LifecycleState::Stopped;
    uint64_t generation = 0;
    int targetFps = 0;
    int64_t lastAcceptedTimestamp = std::numeric_limits<int64_t>::min();
    FrameCallback callback;
    OwnedFrameCallback ownedCallback;
    bool ownedMode = false;
    int canvasWidth = 0;
    int canvasHeight = 0;
    CaptureErrorCallback errorCallback;
    std::string lastError;
    uint64_t droppedFrames = 0;
    bool deliveryExit = false;
    std::deque<OwnedCaptureFrame> frames;
    std::thread captureThread;
    std::thread deliveryThread;

    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    IDirect3DDevice direct3DDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool framePool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::event_token frameArrivedToken{};
    winrt::event_token itemClosedToken{};
    bool frameHandlerAttached = false;
    bool closedHandlerAttached = false;
    int stagingWidth = 0;
    int stagingHeight = 0;

    void Start(std::string displayId, int fps, FrameCallback frameCallback,
               OwnedFrameCallback ownedFrameCallback, int requestedCanvasWidth,
               int requestedCanvasHeight, CaptureErrorCallback captureErrorCallback) {
        if (displayId.empty()) {
            throw std::invalid_argument("Display ID is required");
        }
        if (fps <= 0 || fps > 240) {
            throw std::invalid_argument("Target FPS must be between 1 and 240");
        }
        if (!frameCallback && !ownedFrameCallback) {
            throw std::invalid_argument("Frame callback is required");
        }

        JoinCompletedCaptureThread();

        uint64_t requestedGeneration = 0;
        {
            std::lock_guard lock(mutex);
            if (state != LifecycleState::Stopped) {
                throw std::logic_error("Desktop capture is already starting or running");
            }
            state = LifecycleState::Starting;
            requestedGeneration = ++generation;
            targetFps = fps;
            lastAcceptedTimestamp = std::numeric_limits<int64_t>::min();
            callback = std::move(frameCallback);
            ownedCallback = std::move(ownedFrameCallback);
            ownedMode = static_cast<bool>(ownedCallback);
            canvasWidth = requestedCanvasWidth;
            canvasHeight = requestedCanvasHeight;
            errorCallback = std::move(captureErrorCallback);
            lastError.clear();
            droppedFrames = 0;
            deliveryExit = false;
            frames.clear();
        }

        const auto self = shared_from_this();
        captureThread = std::thread([self, displayId = std::move(displayId), requestedGeneration] {
            self->CaptureThreadMain(displayId, requestedGeneration);
        });

        std::unique_lock lock(mutex);
        stateChanged.wait(lock, [&] { return state != LifecycleState::Starting; });
        if (state == LifecycleState::Running) {
            return;
        }
        const std::string error = lastError.empty() ? "Desktop capture failed to start" : lastError;
        lock.unlock();
        JoinCompletedCaptureThread();
        throw std::runtime_error(error);
    }

    void Stop() {
        const auto current = std::this_thread::get_id();
        {
            std::lock_guard lock(mutex);
            if ((deliveryThread.joinable() && deliveryThread.get_id() == current) ||
                (captureThread.joinable() && captureThread.get_id() == current)) {
                throw std::logic_error("DesktopCaptureSource::Stop cannot run from a capture callback");
            }
            if (state != LifecycleState::Stopped) {
                ++generation;
                state = LifecycleState::Stopping;
                frames.clear();
                stateChanged.notify_all();
                frameReady.notify_all();
            }
        }
        JoinCompletedCaptureThread();
    }

    void JoinCompletedCaptureThread() {
        std::thread thread;
        {
            std::lock_guard lock(mutex);
            if (captureThread.joinable() && captureThread.get_id() != std::this_thread::get_id() &&
                state == LifecycleState::Stopped) {
                thread = std::move(captureThread);
            }
        }
        if (thread.joinable()) {
            thread.join();
            return;
        }

        std::unique_lock lock(mutex);
        if (captureThread.joinable() && captureThread.get_id() != std::this_thread::get_id() &&
            state != LifecycleState::Running && state != LifecycleState::Starting) {
            stateChanged.wait(lock, [&] { return state == LifecycleState::Stopped; });
            thread = std::move(captureThread);
            lock.unlock();
            thread.join();
        }
    }

    void CaptureThreadMain(const std::string& displayId, uint64_t requestedGeneration) noexcept {
        bool runningBegan = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            std::unique_lock lock(mutex);
            if (state != LifecycleState::Starting || generation != requestedGeneration) {
                state = LifecycleState::Stopped;
                stateChanged.notify_all();
                lock.unlock();
                winrt::clear_factory_cache();
                winrt::uninit_apartment();
                return;
            }
            SetupLocked(displayId, requestedGeneration);
            deliveryThread = std::thread([self = shared_from_this(), requestedGeneration] {
                self->DeliveryThreadMain(requestedGeneration);
            });
            session.StartCapture();
            state = LifecycleState::Running;
            runningBegan = true;
            stateChanged.notify_all();
            stateChanged.wait(lock, [&] {
                return state == LifecycleState::Stopping || state == LifecycleState::Failed ||
                       generation != requestedGeneration;
            });
        } catch (const winrt::hresult_error& error) {
            std::lock_guard lock(mutex);
            lastError = HResultMessage(error);
            state = LifecycleState::Failed;
            stateChanged.notify_all();
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            lastError = error.what();
            state = LifecycleState::Failed;
            stateChanged.notify_all();
        } catch (...) {
            std::lock_guard lock(mutex);
            lastError = "Unknown Windows capture failure";
            state = LifecycleState::Failed;
            stateChanged.notify_all();
        }

        CaptureErrorCallback reportError;
        std::string error;
        {
            std::lock_guard lock(mutex);
            const bool failed = state == LifecycleState::Failed;
            if (failed && runningBegan) {
                reportError = errorCallback;
                error = lastError;
            }
            CleanupLocked();
            deliveryExit = true;
            frames.clear();
            frameReady.notify_all();
        }
        if (deliveryThread.joinable()) {
            deliveryThread.join();
        }
        {
            std::lock_guard lock(mutex);
            ReleaseLocked();
            state = LifecycleState::Stopped;
            stateChanged.notify_all();
        }
        if (reportError) {
            reportError(error);
        }
        // This worker can be the last MTA thread. Its shutdown invalidates
        // cached capture factories; release them while the apartment is alive
        // so a later share starts with valid activation factories.
        winrt::clear_factory_cache();
        winrt::uninit_apartment();
    }

    void SetupLocked(const std::string& displayId, uint64_t requestedGeneration) {
        const auto captures = EnumerateNativeCaptureRecords();
        const auto window = displayId.starts_with("esw1.") ? DecodeAndRevalidateWindow(displayId) : std::nullopt;
        const auto selected = std::find_if(captures.begin(), captures.end(), [&](const NativeCaptureRecord& record) {
            return window ? record.info.kind == CaptureItemKind::Window && record.window == *window : record.info.id == displayId;
        });
        if (selected == captures.end()) {
            throw std::runtime_error("Requested capture item is unavailable");
        }
        if (!GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error("Windows Graphics Capture is not supported in this session");
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                               nullptr, 0, D3D11_SDK_VERSION, d3dDevice.put(),
                                               nullptr, d3dContext.put()));
        const auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        direct3DDevice = inspectable.as<IDirect3DDevice>();

        const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (selected->info.kind == CaptureItemKind::Monitor) {
            winrt::check_hresult(interop->CreateForMonitor(selected->monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                                          winrt::put_abi(item)));
        } else {
            if (!window || *window != selected->window) throw std::runtime_error("Requested window is stale");
            winrt::check_hresult(interop->CreateForWindow(*window, winrt::guid_of<GraphicsCaptureItem>(),
                                                         winrt::put_abi(item)));
        }
        const auto initialSize = item.Size();
        if (initialSize.Width <= 0 || initialSize.Height <= 0) {
            throw std::runtime_error("Requested display has zero capture size");
        }
        CreateStagingLocked(initialSize.Width, initialSize.Height);
        framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            direct3DDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, initialSize);

        const std::weak_ptr<Impl> weakSelf = shared_from_this();
        frameArrivedToken = framePool.FrameArrived(
            [weakSelf, requestedGeneration](const Direct3D11CaptureFramePool& sender,
                                            const winrt::Windows::Foundation::IInspectable&) {
                if (const auto self = weakSelf.lock()) {
                    self->OnFrameArrived(sender, requestedGeneration);
                }
            });
        frameHandlerAttached = true;
        itemClosedToken = item.Closed(
            [weakSelf, requestedGeneration](const GraphicsCaptureItem&,
                                            const winrt::Windows::Foundation::IInspectable&) {
                if (const auto self = weakSelf.lock()) {
                    std::lock_guard lock(self->mutex);
                    if (self->generation == requestedGeneration &&
                        self->state == LifecycleState::Running) {
                        self->FailLocked("Captured display was closed or disappeared");
                    }
                }
            });
        closedHandlerAttached = true;
        session = framePool.CreateCaptureSession(item);
    }

    void CreateStagingLocked(int width, int height) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(d3dDevice->CreateTexture2D(&description, nullptr, stagingTexture.put()));
        stagingWidth = width;
        stagingHeight = height;
    }

    void OnFrameArrived(const Direct3D11CaptureFramePool& sender, uint64_t eventGeneration) noexcept {
        try {
            Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
            if (!frame) {
                return;
            }
            std::unique_lock lock(mutex);
            if (state != LifecycleState::Running || generation != eventGeneration) {
                frame.Close();
                return;
            }
            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0) {
                frame.Close();
                FailLocked("Captured display reported zero content size");
                return;
            }
            if (size.Width != stagingWidth || size.Height != stagingHeight) {
                frame.Close();
                frames.clear();
                stagingTexture = nullptr;
                CreateStagingLocked(size.Width, size.Height);
                framePool.Recreate(direct3DDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                lastAcceptedTimestamp = std::numeric_limits<int64_t>::min();
                return;
            }

            const int64_t timestamp = frame.SystemRelativeTime().count();
            const int64_t minimumInterval = 10'000'000 / targetFps;
            if (lastAcceptedTimestamp != std::numeric_limits<int64_t>::min() &&
                timestamp >= lastAcceptedTimestamp && timestamp - lastAcceptedTimestamp < minimumInterval) {
                frame.Close();
                return;
            }

            const auto access = frame.Surface().as<
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> sourceTexture;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), sourceTexture.put_void()));
            d3dContext->CopyResource(stagingTexture.get(), sourceTexture.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(d3dContext->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped));
            std::shared_ptr<const I420Buffer> converted;
            try {
                const size_t mappedBytes = static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(size.Height);
                if (ownedMode) {
                    converted = std::make_shared<const I420Buffer>(ConvertBgraToRecordingI420(
                        {static_cast<const uint8_t*>(mapped.pData), mappedBytes}, size.Width, size.Height,
                        static_cast<int>(mapped.RowPitch), canvasWidth, canvasHeight, timestamp));
                } else {
                    converted = std::make_shared<const I420Buffer>(ConvertBgraToI420(
                        {static_cast<const uint8_t*>(mapped.pData), mappedBytes}, size.Width, size.Height,
                        static_cast<int>(mapped.RowPitch), timestamp));
                }
            } catch (...) {
                d3dContext->Unmap(stagingTexture.get(), 0);
                throw;
            }
            d3dContext->Unmap(stagingTexture.get(), 0);
            frame.Close();
            lastAcceptedTimestamp = timestamp;
            if (frames.size() == 2) {
                frames.pop_front();
                ++droppedFrames;
            }
            frames.push_back({std::move(converted), timestamp});
            frameReady.notify_one();
        } catch (const winrt::hresult_error& error) {
            std::lock_guard lock(mutex);
            if (generation == eventGeneration && state == LifecycleState::Running) {
                FailLocked(HResultMessage(error));
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            if (generation == eventGeneration && state == LifecycleState::Running) {
                FailLocked(error.what());
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            if (generation == eventGeneration && state == LifecycleState::Running) {
                FailLocked("Unknown frame conversion failure");
            }
        }
    }

    void DeliveryThreadMain(uint64_t deliveryGeneration) noexcept {
        while (true) {
            OwnedCaptureFrame frame;
            FrameCallback deliver;
            OwnedFrameCallback deliverOwned;
            {
                std::unique_lock lock(mutex);
                frameReady.wait(lock, [&] { return deliveryExit || !frames.empty(); });
                if (deliveryExit) {
                    return;
                }
                if (generation != deliveryGeneration || state != LifecycleState::Running) {
                    frames.clear();
                    continue;
                }
                frame = std::move(frames.front());
                frames.pop_front();
                deliver = callback;
                deliverOwned = ownedCallback;
            }
            try {
                if (deliverOwned) deliverOwned(std::move(frame));
                else deliver(frame.pixels->View());
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex);
                if (generation == deliveryGeneration && state == LifecycleState::Running) {
                    FailLocked(std::string("Frame callback failed: ") + error.what());
                }
            } catch (...) {
                std::lock_guard lock(mutex);
                if (generation == deliveryGeneration && state == LifecycleState::Running) {
                    FailLocked("Frame callback failed");
                }
            }
        }
    }

    void FailLocked(std::string error) {
        lastError = std::move(error);
        state = LifecycleState::Failed;
        frames.clear();
        stateChanged.notify_all();
        frameReady.notify_all();
    }

    void CleanupLocked() noexcept {
        try {
            if (frameHandlerAttached && framePool) {
                framePool.FrameArrived(frameArrivedToken);
            }
        } catch (...) {
        }
        frameHandlerAttached = false;
        try {
            if (closedHandlerAttached && item) {
                item.Closed(itemClosedToken);
            }
        } catch (...) {
        }
        closedHandlerAttached = false;
        try {
            if (session) {
                session.Close();
            }
        } catch (...) {
        }
        try {
            if (framePool) {
                framePool.Close();
            }
        } catch (...) {
        }
    }

    void ReleaseLocked() noexcept {
        session = nullptr;
        framePool = nullptr;
        item = nullptr;
        direct3DDevice = nullptr;
        stagingTexture = nullptr;
        d3dContext = nullptr;
        d3dDevice = nullptr;
        stagingWidth = 0;
        stagingHeight = 0;
        callback = {};
        ownedCallback = {};
        ownedMode = false;
        canvasWidth = 0;
        canvasHeight = 0;
        errorCallback = {};
    }
};

DesktopCaptureSource::DesktopCaptureSource() : impl_(std::make_shared<Impl>()) {}

DesktopCaptureSource::~DesktopCaptureSource() {
    try {
        impl_->Stop();
    } catch (...) {
    }
}

std::vector<DisplayInfo> DesktopCaptureSource::Enumerate() {
    std::vector<DisplayInfo> displays;
    for (auto& record : EnumerateMonitorRecords()) {
        displays.push_back(std::move(record.display));
    }
    return displays;
}

std::vector<CaptureItemInfo> DesktopCaptureSource::EnumerateCaptureItems() {
    std::vector<CaptureItemInfo> result;
    for (auto& record : EnumerateNativeCaptureRecords()) result.push_back(std::move(record.info));
    return result;
}

bool DesktopCaptureSource::ValidateCaptureItem(std::string_view id) noexcept {
    if (id.empty()) return false;
    try {
        if (id.starts_with("esw1.")) return DecodeAndRevalidateWindow(id).has_value();
        for (const auto& record : EnumerateNativeCaptureRecords()) {
            if (record.info.id == id) {
                return record.info.kind == CaptureItemKind::Monitor || DecodeAndRevalidateWindow(id).has_value();
            }
        }
    } catch (...) {
    }
    return false;
}

void DesktopCaptureSource::Start(std::string_view displayId, int targetFps, FrameCallback callback) {
    Start(displayId, targetFps, std::move(callback), {});
}

void DesktopCaptureSource::Start(std::string_view displayId, int targetFps, FrameCallback callback,
                                 CaptureErrorCallback errorCallback) {
    impl_->Start(std::string(displayId), targetFps, std::move(callback), {}, 0, 0,
                 std::move(errorCallback));
}

void DesktopCaptureSource::StartOwned(std::string_view captureItemId, int targetFps,
                                      int requestedCanvasWidth, int requestedCanvasHeight,
                                      OwnedFrameCallback callback, CaptureErrorCallback errorCallback) {
    if (requestedCanvasWidth <= 0 || requestedCanvasHeight <= 0 || requestedCanvasWidth > 1920 ||
        requestedCanvasHeight > 1080 || (requestedCanvasWidth & 1) || (requestedCanvasHeight & 1)) {
        throw std::invalid_argument("Owned capture canvas must be positive, even, and at most 1920x1080");
    }
    impl_->Start(std::string(captureItemId), std::min(targetFps, 30), {}, std::move(callback),
                 requestedCanvasWidth, requestedCanvasHeight, std::move(errorCallback));
}

void DesktopCaptureSource::Stop() {
    impl_->Stop();
}

uint64_t DesktopCaptureSource::DroppedFrames() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->droppedFrames;
}

std::string DesktopCaptureSource::LastError() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->lastError;
}

} // namespace telestudio
