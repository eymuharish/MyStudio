#include "telestudio/media/RemoteVideoSurface.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace telestudio {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

enum class SlotState {
    Ready,
    PublishedAwaitOpened,
    Displayed,
    RetiredAwaitReleased,
    RemovalPending,
};

struct SurfaceEntry {
    std::unique_ptr<detail::IRemoteSurfaceSlot> resource;
    uint64_t sequence = 0;
    uint64_t hostHandleValue = 0;
    uint64_t epoch = 0;
    SlotState state = SlotState::Ready;
    detail::RemoteSurfaceTimePoint deadline{};
};

class D3dSurfaceSlot final : public detail::IRemoteSurfaceSlot {
public:
    D3dSurfaceSlot(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height)
        : context_(context), width_(width), height_(height) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &texture_))) {
            throw std::runtime_error("Could not create BGRA keyed-mutex shared texture");
        }
        if (FAILED(texture_.As(&keyedMutex_))) throw std::runtime_error("Shared texture does not expose IDXGIKeyedMutex");
        ComPtr<IDXGIResource1> resource;
        if (FAILED(texture_.As(&resource)) ||
            FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                nullptr, &ownerHandle_))) {
            throw std::runtime_error("Could not create originating NT shared handle");
        }
    }

    ~D3dSurfaceSlot() override { if (ownerHandle_ != nullptr) CloseHandle(ownerHandle_); }
    uint32_t Width() const noexcept override { return width_; }
    uint32_t Height() const noexcept override { return height_; }

    bool Upload(const DecodedBgraFrame& frame) override {
        const HRESULT acquired = keyedMutex_->AcquireSync(0, 0);
        if (acquired == WAIT_TIMEOUT || acquired == DXGI_ERROR_WAIT_TIMEOUT || FAILED(acquired)) return false;
        nativeLease_.store(true, std::memory_order_release);
        context_->UpdateSubresource(texture_.Get(), 0, nullptr, frame.bytes.data(), frame.stride, 0);
        const HRESULT released = keyedMutex_->ReleaseSync(1);
        nativeLease_.store(false, std::memory_order_release);
        return SUCCEEDED(released);
    }

    uint64_t DuplicateToProcess(uintptr_t processHandle) override {
        HANDLE targetHandle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), ownerHandle_, reinterpret_cast<HANDLE>(processHandle),
                             &targetHandle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            throw std::runtime_error("DuplicateHandle to registered host process failed");
        }
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(targetHandle));
    }

    bool HasNativeMutexLease() const noexcept override { return nativeLease_.load(std::memory_order_acquire); }

private:
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<IDXGIKeyedMutex> keyedMutex_;
    HANDLE ownerHandle_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::atomic_bool nativeLease_ = false;
};

class WindowsSurfacePlatform final : public detail::IRemoteSurfacePlatform {
public:
    explicit WindowsSurfacePlatform(ID3D11Device* mediaDevice) {
        if (mediaDevice != nullptr) {
            device_ = mediaDevice;
            device_->GetImmediateContext(&context_);
            return;
        }
        constexpr D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                           &device_, &selected, &context_);
        if (FAILED(result)) {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                       &device_, &selected, &context_);
        }
        if (FAILED(result)) throw std::runtime_error("Could not create D3D11 media surface device");
    }

    uintptr_t OpenProcess(uint32_t pid) override {
        return reinterpret_cast<uintptr_t>(::OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid));
    }
    void CloseProcess(uintptr_t processHandle) noexcept override {
        if (processHandle != 0) CloseHandle(reinterpret_cast<HANDLE>(processHandle));
    }
    bool IsProcessAlive(uintptr_t processHandle) const noexcept override {
        return processHandle != 0 && WaitForSingleObject(reinterpret_cast<HANDLE>(processHandle), 0) == WAIT_TIMEOUT;
    }
    std::unique_ptr<detail::IRemoteSurfaceSlot> CreateSlot(uint32_t width, uint32_t height) override {
        return std::make_unique<D3dSurfaceSlot>(device_.Get(), context_.Get(), width, height);
    }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
};

class ThreadDeadlineScheduler final : public detail::IRemoteSurfaceScheduler {
public:
    ThreadDeadlineScheduler() : worker_([this] { Run(); }) {}
    ~ThreadDeadlineScheduler() override { CancelAndWait(); }

    detail::RemoteSurfaceTimePoint Now() const noexcept override { return Clock::now(); }
    void Schedule(detail::RemoteSurfaceTimePoint deadline, std::function<void()> callback) override {
        std::lock_guard lock(mutex_);
        if (cancelled_) return;
        tasks_.push_back({deadline, std::move(callback)});
        condition_.notify_all();
    }
    void CancelAndWait() noexcept override {
        {
            std::lock_guard lock(mutex_);
            if (cancelled_) return;
            cancelled_ = true;
            tasks_.clear();
        }
        condition_.notify_all();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }

private:
    void Run() {
        std::unique_lock lock(mutex_);
        while (!cancelled_) {
            if (tasks_.empty()) {
                condition_.wait(lock, [this] { return cancelled_ || !tasks_.empty(); });
                continue;
            }
            const auto earliest = std::min_element(tasks_.begin(), tasks_.end(),
                [](const auto& left, const auto& right) { return left.first < right.first; });
            const auto deadline = earliest->first;
            if (condition_.wait_until(lock, deadline) != std::cv_status::timeout) continue;
            const auto now = Clock::now();
            std::vector<std::function<void()>> due;
            for (auto iterator = tasks_.begin(); iterator != tasks_.end();) {
                if (iterator->first <= now) {
                    due.push_back(std::move(iterator->second));
                    iterator = tasks_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            lock.unlock();
            for (auto& callback : due) callback();
            lock.lock();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::pair<detail::RemoteSurfaceTimePoint, std::function<void()>>> tasks_;
    bool cancelled_ = false;
    std::thread worker_;
};

} // namespace

std::optional<std::array<std::byte, 32>> DecodeSessionTokenBase64Url(std::string_view encoded) {
    if (encoded.size() != 43) return std::nullopt;
    auto value = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '-') return 62;
        if (character == '_') return 63;
        return -1;
    };
    std::array<std::byte, 32> decoded{};
    uint32_t accumulator = 0;
    int bits = 0;
    size_t output = 0;
    for (const char character : encoded) {
        const int sextet = value(character);
        if (sextet < 0) return std::nullopt;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(sextet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (output >= decoded.size()) return std::nullopt;
            decoded[output++] = static_cast<std::byte>((accumulator >> bits) & 0xff);
        }
    }
    if (output != decoded.size() || bits != 2 || (accumulator & 0x3u) != 0) return std::nullopt;
    return decoded;
}

class RemoteVideoSurface::Impl {
public:
    Impl(const HostRegistration& expectedHost,
         std::shared_ptr<detail::IRemoteSurfacePlatform> platform,
         std::shared_ptr<detail::IRemoteSurfaceScheduler> scheduler,
         detail::RemoteSurfaceTestConfig config)
        : expectedHost_(expectedHost), platform_(std::move(platform)), scheduler_(std::move(scheduler)),
          nextEpoch_(config.nextEpoch), nextSequence_(config.nextSequence) {
        if (expectedHost_.desktopPid == 0 || !platform_ || !scheduler_ || nextEpoch_ == 0 || nextSequence_ == 0) {
            throw std::invalid_argument("Expected host, platform, scheduler, sequence, and epoch are required");
        }
    }

    ~Impl() {
        scheduler_->CancelAndWait();
        std::lock_guard lock(mutex_);
        if (hostProcess_ != 0) platform_->CloseProcess(hostProcess_);
    }

    static bool SameToken(const std::array<std::byte, 32>& left,
                          const std::array<std::byte, 32>& right) noexcept {
        uint8_t difference = 0;
        for (size_t i = 0; i < left.size(); ++i) difference |= std::to_integer<uint8_t>(left[i] ^ right[i]);
        return difference == 0;
    }

    bool RegisterHost(const HostRegistration& registration) {
        std::lock_guard lock(mutex_);
        SweepTimeoutsLocked();
        if (registration.desktopPid == 0 || registration.desktopPid != expectedHost_.desktopPid ||
            !SameToken(registration.sessionToken, expectedHost_.sessionToken)) return false;
        if (hostProcess_ != 0) {
            return registration.desktopPid == registeredHost_.desktopPid &&
                   SameToken(registration.sessionToken, registeredHost_.sessionToken);
        }
        const uintptr_t process = platform_->OpenProcess(registration.desktopPid);
        if (process == 0 || !platform_->IsProcessAlive(process)) {
            if (process != 0) platform_->CloseProcess(process);
            return false;
        }
        registeredHost_ = registration;
        hostProcess_ = process;
        return true;
    }

    bool Update(const DecodedBgraFrame& frame) {
        std::lock_guard lock(mutex_);
        SweepTimeoutsLocked();
        if (closing_) throw std::logic_error("RemoteVideoSurface is in terminal removal");
        if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
            frame.stride < frame.width * 4 || frame.bytes.size() < static_cast<size_t>(frame.stride) * frame.height) {
            throw std::invalid_argument("BGRA frame is incomplete");
        }
        const auto thread = std::this_thread::get_id();
        if (renderThread_ == std::thread::id{}) renderThread_ = thread;
        else if (renderThread_ != thread) throw std::logic_error("RemoteVideoSurface updates are render-thread only");

        const bool resize = !current_ || current_->resource->Width() != static_cast<uint32_t>(frame.width) ||
                            current_->resource->Height() != static_cast<uint32_t>(frame.height);
        std::unique_ptr<detail::IRemoteSurfaceSlot> replacement;
        detail::IRemoteSurfaceSlot* destination = nullptr;
        if (resize) {
            replacement = platform_->CreateSlot(frame.width, frame.height);
            destination = replacement.get();
        } else {
            destination = current_->resource.get();
        }
        if (!destination->Upload(frame)) return false;

        if (resize) {
            if (current_ && current_->epoch != 0) {
                current_->state = SlotState::RetiredAwaitReleased;
                current_->deadline = scheduler_->Now() + std::chrono::seconds(5);
                ScheduleSweep(current_->deadline);
                retired_.push_back(std::move(*current_));
            }
            current_ = SurfaceEntry{};
            current_->resource = std::move(replacement);
        }
        return true;
    }

    PublishedSurface Publish() {
        std::lock_guard lock(mutex_);
        SweepTimeoutsLocked();
        if (closing_) throw std::logic_error("RemoteVideoSurface is in terminal removal");
        if (hostProcess_ == 0) throw std::logic_error("Host must be registered before publishing a surface");
        if (!platform_->IsProcessAlive(hostProcess_)) throw std::runtime_error("Registered host process has exited");
        if (!current_) throw std::logic_error("No complete BGRA frame is available for publication");
        if (current_->epoch == 0) {
            constexpr uint64_t maximum = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
            if (nextEpoch_ > maximum || nextSequence_ > maximum) {
                closing_ = true;
                throw std::overflow_error("Remote surface epoch/sequence space is exhausted");
            }
            current_->hostHandleValue = current_->resource->DuplicateToProcess(hostProcess_);
            if (current_->hostHandleValue == 0) throw std::runtime_error("DuplicateHandle returned a null target handle");
            current_->epoch = nextEpoch_++;
            current_->sequence = nextSequence_++;
            current_->state = SlotState::PublishedAwaitOpened;
            current_->deadline = scheduler_->Now() + std::chrono::seconds(2);
            ScheduleSweep(current_->deadline);
        }
        return {current_->sequence, current_->hostHandleValue, current_->resource->Width(),
                current_->resource->Height(), SurfaceFormat::Bgra8Unorm, current_->epoch};
    }

    void Opened(uint64_t epoch) {
        std::lock_guard lock(mutex_);
        SweepTimeoutsLocked();
        if (epoch != 0 && current_ && current_->epoch == epoch && current_->state == SlotState::PublishedAwaitOpened) {
            current_->state = SlotState::Displayed;
            current_->deadline = {};
        }
    }

    void Released(uint64_t epoch) {
        std::lock_guard lock(mutex_);
        SweepTimeoutsLocked();
        if (epoch == 0 || (current_ && current_->epoch == epoch)) return;
        const auto found = std::find_if(retired_.begin(), retired_.end(), [epoch](const auto& entry) {
            return entry.epoch == epoch && entry.state == SlotState::RetiredAwaitReleased;
        });
        if (found != retired_.end()) retired_.erase(found);
    }

    void Removed(uint64_t epoch) {
        std::lock_guard lock(mutex_);
        SweepTimeoutsLocked();
        if (epoch == 0) return;
        const auto found = std::find_if(removal_.begin(), removal_.end(), [epoch](const auto& entry) {
            return entry.epoch == epoch && entry.state == SlotState::RemovalPending;
        });
        if (found != removal_.end()) removal_.erase(found);
    }

    void BeginRemoval() {
        std::lock_guard lock(mutex_);
        SweepTimeoutsLocked();
        if (closing_) return;
        closing_ = true;
        const auto deadline = scheduler_->Now() + std::chrono::seconds(5);
        if (current_ && current_->epoch != 0) MoveToRemoval(*current_, deadline);
        current_.reset();
        while (!retired_.empty()) {
            MoveToRemoval(retired_.front(), deadline);
            retired_.erase(retired_.begin());
        }
        if (!removal_.empty()) ScheduleSweep(deadline);
    }

    uint64_t CurrentEpoch() const noexcept {
        std::lock_guard lock(mutex_);
        return current_ ? current_->epoch : 0;
    }

private:
    void MoveToRemoval(SurfaceEntry& entry, detail::RemoteSurfaceTimePoint deadline) {
        if (entry.resource->HasNativeMutexLease()) {
            throw std::logic_error("Cannot enter removal while producer owns keyed mutex");
        }
        entry.state = SlotState::RemovalPending;
        entry.deadline = deadline;
        removal_.push_back(std::move(entry));
    }

    void ScheduleSweep(detail::RemoteSurfaceTimePoint deadline) {
        scheduler_->Schedule(deadline, [this] {
            std::lock_guard lock(mutex_);
            SweepTimeoutsLocked();
        });
    }

    void SweepTimeoutsLocked() {
        const auto now = scheduler_->Now();
        if (current_ && current_->state == SlotState::PublishedAwaitOpened && now >= current_->deadline) current_.reset();
        std::erase_if(retired_, [now](const auto& entry) {
            return entry.state == SlotState::RetiredAwaitReleased && now >= entry.deadline;
        });
        std::erase_if(removal_, [now](const auto& entry) {
            return entry.state == SlotState::RemovalPending && now >= entry.deadline;
        });
    }

    HostRegistration expectedHost_{};
    HostRegistration registeredHost_{};
    std::shared_ptr<detail::IRemoteSurfacePlatform> platform_;
    std::shared_ptr<detail::IRemoteSurfaceScheduler> scheduler_;
    uintptr_t hostProcess_ = 0;
    std::optional<SurfaceEntry> current_;
    std::vector<SurfaceEntry> retired_;
    std::vector<SurfaceEntry> removal_;
    uint64_t nextEpoch_ = 1;
    uint64_t nextSequence_ = 1;
    bool closing_ = false;
    std::thread::id renderThread_{};
    mutable std::mutex mutex_;
};

RemoteVideoSurface::RemoteVideoSurface(const HostRegistration& expectedHost, ID3D11Device* mediaDevice)
    : RemoteVideoSurface(expectedHost, std::make_shared<WindowsSurfacePlatform>(mediaDevice),
                         std::make_shared<ThreadDeadlineScheduler>()) {}
RemoteVideoSurface::RemoteVideoSurface(const HostRegistration& expectedHost,
                                       std::shared_ptr<detail::IRemoteSurfacePlatform> platform,
                                       std::shared_ptr<detail::IRemoteSurfaceScheduler> scheduler,
                                       detail::RemoteSurfaceTestConfig config)
    : impl_(std::make_unique<Impl>(expectedHost, std::move(platform), std::move(scheduler), config)) {}
RemoteVideoSurface::~RemoteVideoSurface() = default;
RemoteVideoSurface::RemoteVideoSurface(RemoteVideoSurface&&) noexcept = default;
RemoteVideoSurface& RemoteVideoSurface::operator=(RemoteVideoSurface&&) noexcept = default;
bool RemoteVideoSurface::RegisterHost(const HostRegistration& registration) { return impl_->RegisterHost(registration); }
PublishedSurface RemoteVideoSurface::PublishForHost() { return impl_->Publish(); }
void RemoteVideoSurface::Opened(uint64_t epoch) { impl_->Opened(epoch); }
void RemoteVideoSurface::Released(uint64_t epoch) { impl_->Released(epoch); }
void RemoteVideoSurface::Removed(uint64_t epoch) { impl_->Removed(epoch); }
void RemoteVideoSurface::BeginRemoval() { impl_->BeginRemoval(); }
bool RemoteVideoSurface::UpdateOnRenderThread(const DecodedBgraFrame& frame) { return impl_->Update(frame); }
uint64_t RemoteVideoSurface::CurrentEpoch() const noexcept { return impl_->CurrentEpoch(); }

} // namespace telestudio
