#pragma once

#include "telestudio/media/AudioTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace telestudio::media {

enum class ProcessLoopbackMode {
    IncludeTargetProcessTree,
    ExcludeTargetProcessTree,
};

enum class LoopbackSampleFormat {
    Pcm16,
    Pcm24,
    Pcm32,
    Float32,
};

struct ProcessLoopbackFormat {
    std::uint32_t sampleRateHz = 0;
    std::uint16_t channels = 0;
    LoopbackSampleFormat sampleFormat = LoopbackSampleFormat::Pcm16;
};

struct LoopbackPacket {
    const void* data = nullptr;
    std::uint32_t frameCount = 0;
    std::int64_t timestamp100ns = 0;
    bool silent = false;
    bool discontinuity = false;
};

class IProcessLoopbackBackend {
public:
    using ActivationCallback =
        std::function<void(bool, ProcessLoopbackFormat, std::string)>;
    using PacketCallback = std::function<void(const LoopbackPacket&)>;

    virtual ~IProcessLoopbackBackend() = default;
    virtual bool Start(std::uint32_t excludedProcessTreeRoot,
                       ProcessLoopbackMode mode,
                       ActivationCallback activation,
                       PacketCallback packets,
                       std::string& error) = 0;
    // Prevents new callbacks and waits up to timeoutMs for in-flight callbacks.
    // False means fail-closed detach: no new callback may begin, while an
    // already-entered callback/worker with shared lifetime state may unwind
    // later. A call from the current callback signals quiescence without
    // self-waiting and returns true.
    virtual bool Stop(std::uint32_t timeoutMs) noexcept = 0;
};

class SystemAudioCapture {
public:
    using Callback = std::function<void(OwnedAudioBlock&&)>;

    SystemAudioCapture();
    // Null selects the Windows backend; an injected backend must outlive capture.
    explicit SystemAudioCapture(IProcessLoopbackBackend* backend);
    explicit SystemAudioCapture(IProcessLoopbackBackend& backend);
    ~SystemAudioCapture();

    SystemAudioCapture(const SystemAudioCapture&) = delete;
    SystemAudioCapture& operator=(const SystemAudioCapture&) = delete;

    bool Start(std::uint32_t excludedProcessTreeRoot, Callback callback);
    // Bounded and fail-closed: IsRunning() is false on return. LastError()
    // reports a detached backend when its worker misses the shutdown deadline.
    void Stop() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] bool IsStarting() const noexcept;
    [[nodiscard]] std::string LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace telestudio::media
