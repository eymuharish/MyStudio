#pragma once

#include "telestudio/media/FfmpegVideoDecoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

struct ID3D11Device;

namespace telestudio {

struct HostRegistration {
    uint32_t desktopPid = 0;
    std::array<std::byte, 32> sessionToken{};
};

enum class SurfaceFormat : uint32_t {
    Bgra8Unorm = 1,
};

struct PublishedSurface {
    uint64_t sequence = 0;
    uint64_t hostHandleValue = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    SurfaceFormat format = SurfaceFormat::Bgra8Unorm;
    uint64_t epoch = 0;
};

std::optional<std::array<std::byte, 32>> DecodeSessionTokenBase64Url(std::string_view encoded);

namespace detail {

using RemoteSurfaceTimePoint = std::chrono::steady_clock::time_point;

class IRemoteSurfaceSlot {
public:
    virtual ~IRemoteSurfaceSlot() = default;
    virtual uint32_t Width() const noexcept = 0;
    virtual uint32_t Height() const noexcept = 0;
    virtual bool Upload(const DecodedBgraFrame& frame) = 0;
    virtual uint64_t DuplicateToProcess(uintptr_t processHandle) = 0;
    virtual bool HasNativeMutexLease() const noexcept = 0;
};

class IRemoteSurfacePlatform {
public:
    virtual ~IRemoteSurfacePlatform() = default;
    virtual uintptr_t OpenProcess(uint32_t pid) = 0;
    virtual void CloseProcess(uintptr_t processHandle) noexcept = 0;
    virtual bool IsProcessAlive(uintptr_t processHandle) const noexcept = 0;
    virtual std::unique_ptr<IRemoteSurfaceSlot> CreateSlot(uint32_t width, uint32_t height) = 0;
};

class IRemoteSurfaceScheduler {
public:
    virtual ~IRemoteSurfaceScheduler() = default;
    virtual RemoteSurfaceTimePoint Now() const noexcept = 0;
    virtual void Schedule(RemoteSurfaceTimePoint deadline, std::function<void()> callback) = 0;
    virtual void CancelAndWait() noexcept = 0;
};

struct RemoteSurfaceTestConfig {
    uint64_t nextSequence = 1;
    uint64_t nextEpoch = 1;
};

} // namespace detail

class RemoteVideoSurface {
public:
    explicit RemoteVideoSurface(const HostRegistration& expectedHost, ID3D11Device* mediaDevice = nullptr);
    RemoteVideoSurface(const HostRegistration& expectedHost,
                       std::shared_ptr<detail::IRemoteSurfacePlatform> platform,
                       std::shared_ptr<detail::IRemoteSurfaceScheduler> scheduler,
                       detail::RemoteSurfaceTestConfig config = {});
    ~RemoteVideoSurface();
    RemoteVideoSurface(RemoteVideoSurface&&) noexcept;
    RemoteVideoSurface& operator=(RemoteVideoSurface&&) noexcept;
    RemoteVideoSurface(const RemoteVideoSurface&) = delete;
    RemoteVideoSurface& operator=(const RemoteVideoSurface&) = delete;

    bool RegisterHost(const HostRegistration& registration);
    PublishedSurface PublishForHost();
    void Opened(uint64_t epoch);
    void Released(uint64_t epoch);
    void Removed(uint64_t epoch);
    void BeginRemoval();

    bool UpdateOnRenderThread(const DecodedBgraFrame& frame);
    uint64_t CurrentEpoch() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace telestudio
