#include "telestudio/media/SystemAudioCapture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <sdkddkver.h>
#include <Windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace telestudio::media {
namespace {
constexpr std::uint32_t kOutputRate = 48'000;
constexpr std::size_t kBlockFrames = 960;
constexpr std::size_t kMaximumBufferedFrames = 2'880;
constexpr std::int64_t kBlockDuration100ns = 200'000;
constexpr DWORD kMinimumProcessLoopbackBuild = 20'348;
constexpr std::uint32_t kStopDeadlineMs = 500;

std::string HResultText(const char* operation, HRESULT result) {
    std::ostringstream text;
    text << operation << " failed (0x" << std::hex << std::uppercase
         << static_cast<unsigned long>(result) << ')';
    return text.str();
}

std::int16_t FloatToPcm16(float sample) noexcept {
    const auto bounded = std::clamp(sample, -1.0F, 1.0F);
    const auto scaled = std::lrint(static_cast<double>(bounded) * 32'768.0);
    return static_cast<std::int16_t>(std::clamp<long>(scaled, -32'768, 32'767));
}

float ReadSample(const std::uint8_t* source, LoopbackSampleFormat format) noexcept {
    switch (format) {
    case LoopbackSampleFormat::Float32: {
        float sample = 0.0F;
        std::memcpy(&sample, source, sizeof(sample));
        return std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F;
    }
    case LoopbackSampleFormat::Pcm16: {
        std::int16_t sample = 0;
        std::memcpy(&sample, source, sizeof(sample));
        return static_cast<float>(sample) / 32'768.0F;
    }
    case LoopbackSampleFormat::Pcm24: {
        std::int32_t sample = static_cast<std::int32_t>(source[0]) |
                              (static_cast<std::int32_t>(source[1]) << 8) |
                              (static_cast<std::int32_t>(source[2]) << 16);
        if ((sample & 0x0080'0000) != 0) sample |= static_cast<std::int32_t>(0xFF00'0000);
        return static_cast<float>(sample) / 8'388'608.0F;
    }
    case LoopbackSampleFormat::Pcm32: {
        std::int32_t sample = 0;
        std::memcpy(&sample, source, sizeof(sample));
        return static_cast<float>(static_cast<double>(sample) / 2'147'483'648.0);
    }
    }
    return 0.0F;
}

std::size_t BytesPerSample(LoopbackSampleFormat format) noexcept {
    switch (format) {
    case LoopbackSampleFormat::Pcm16: return 2;
    case LoopbackSampleFormat::Pcm24: return 3;
    case LoopbackSampleFormat::Pcm32:
    case LoopbackSampleFormat::Float32: return 4;
    }
    return 0;
}

bool IsSupportedFormat(const ProcessLoopbackFormat& format) noexcept {
    return format.sampleRateHz != 0 && format.sampleRateHz <= 384'000 &&
           format.channels != 0 && format.channels <= 32 &&
           BytesPerSample(format.sampleFormat) != 0;
}

float ReadMonoFrame(const LoopbackPacket& packet,
                    const ProcessLoopbackFormat& format,
                    std::size_t frame) noexcept {
    if (packet.silent || packet.data == nullptr) return 0.0F;
    const auto bytesPerSample = BytesPerSample(format.sampleFormat);
    const auto* bytes = static_cast<const std::uint8_t*>(packet.data);
    double total = 0.0;
    for (std::size_t channel = 0; channel < format.channels; ++channel) {
        const auto offset = (frame * format.channels + channel) * bytesPerSample;
        total += ReadSample(bytes + offset, format.sampleFormat);
    }
    return static_cast<float>(total / format.channels);
}

struct StreamingResampler {
    void Reset() noexcept {
        havePrevious = false;
        previous = 0.0F;
        inputFrames = 0;
        nextOutputNumerator = 0;
    }

    template <typename Push>
    void Process(float current, std::uint32_t inputRate, Push&& push) noexcept {
        if (!havePrevious) {
            push(current);
            havePrevious = true;
            previous = current;
            inputFrames = 1;
            nextOutputNumerator = inputRate;
            return;
        }

        const auto currentIndex = inputFrames;
        const auto currentNumerator = currentIndex * static_cast<std::uint64_t>(kOutputRate);
        const auto previousNumerator = currentNumerator - kOutputRate;
        while (nextOutputNumerator <= currentNumerator) {
            const auto fraction = static_cast<float>(nextOutputNumerator - previousNumerator) /
                                  static_cast<float>(kOutputRate);
            push(previous + (current - previous) * fraction);
            nextOutputNumerator += inputRate;
        }
        previous = current;
        ++inputFrames;
    }

    bool havePrevious = false;
    float previous = 0.0F;
    std::uint64_t inputFrames = 0;
    std::uint64_t nextOutputNumerator = 0;
};

std::int64_t FramesTo100ns(std::size_t frames, std::uint32_t sampleRate) noexcept {
    return static_cast<std::int64_t>(frames) * 10'000'000 / sampleRate;
}

class ComCompletionHandler;

struct WindowsBackendState {
    ~WindowsBackendState() {
        if (operation != nullptr) operation->Release();
        if (audioClient != nullptr) audioClient->Release();
        if (activationEvent != nullptr) CloseHandle(activationEvent);
        if (sampleEvent != nullptr) CloseHandle(sampleEvent);
        if (stopEvent != nullptr) CloseHandle(stopEvent);
        if (doneEvent != nullptr) CloseHandle(doneEvent);
    }

    std::mutex mutex;
    HANDLE activationEvent = nullptr;
    HANDLE sampleEvent = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE doneEvent = nullptr;
    std::atomic<DWORD> workerThreadId{0};
    bool stopping = false;
    HRESULT activationResult = E_PENDING;
    IAudioClient* audioClient = nullptr;
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    std::shared_ptr<IProcessLoopbackBackend::ActivationCallback> activation;
    std::shared_ptr<IProcessLoopbackBackend::PacketCallback> packets;
    std::array<std::int16_t, kMaximumBufferedFrames * 2> packetScratch{};
};

class ComCompletionHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    explicit ComCompletionHandler(std::weak_ptr<WindowsBackendState> state) : state_(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (result == nullptr) return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *result = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (iid == __uuidof(IAgileObject)) {
            *result = static_cast<IAgileObject*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        const auto state = state_.lock();
        if (!state) return S_OK;

        HRESULT activationResult = E_FAIL;
        IUnknown* activated = nullptr;
        HRESULT callResult = operation->GetActivateResult(&activationResult, &activated);
        IAudioClient* client = nullptr;
        if (SUCCEEDED(callResult) && SUCCEEDED(activationResult) && activated != nullptr) {
            callResult = activated->QueryInterface(__uuidof(IAudioClient),
                                                   reinterpret_cast<void**>(&client));
        }
        if (activated != nullptr) activated->Release();
        const HRESULT finalResult = FAILED(callResult) ? callResult : activationResult;
        {
            std::lock_guard lock(state->mutex);
            state->activationResult = finalResult;
            if (!state->stopping && SUCCEEDED(finalResult)) {
                state->audioClient = client;
                client = nullptr;
            }
        }
        if (client != nullptr) client->Release();
        SetEvent(state->activationEvent);
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    std::weak_ptr<WindowsBackendState> state_;
};

DWORD CurrentWindowsBuild() noexcept {
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto function = module == nullptr ? nullptr : reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return function != nullptr && function(&version) == 0 ? version.dwBuildNumber : 0;
}

bool IsProcessAlive(std::uint32_t processId) noexcept {
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, processId);
    if (process == nullptr) return false;
    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(process, &exitCode) != FALSE && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

void NotifyFailure(const std::shared_ptr<WindowsBackendState>& state, std::string error) {
    std::shared_ptr<IProcessLoopbackBackend::ActivationCallback> callback;
    {
        std::lock_guard lock(state->mutex);
        if (!state->stopping) callback = state->activation;
    }
    if (callback) (*callback)(false, {}, std::move(error));
}

void RunWindowsCapture(const std::shared_ptr<WindowsBackendState>& state,
                       std::uint32_t excludedProcessTreeRoot) {
    state->workerThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult)) {
        NotifyFailure(state, HResultText("CoInitializeEx", comResult));
        SetEvent(state->doneEvent);
        return;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = excludedProcessTreeRoot;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activationParameters{};
    activationParameters.vt = VT_BLOB;
    activationParameters.blob.cbSize = sizeof(parameters);
    activationParameters.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

    auto* handler = new ComCompletionHandler(state);
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    const HRESULT requestResult = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activationParameters,
        handler, &operation);
    handler->Release();
    if (FAILED(requestResult)) {
        NotifyFailure(state, HResultText("ActivateAudioInterfaceAsync", requestResult));
        if (uninitialize) CoUninitialize();
        SetEvent(state->doneEvent);
        return;
    }
    {
        std::lock_guard lock(state->mutex);
        state->operation = operation;
    }

    const HANDLE activationWaits[] = {state->stopEvent, state->activationEvent};
    const auto activationWait = WaitForMultipleObjects(2, activationWaits, FALSE, 5'000);
    if (activationWait != WAIT_OBJECT_0 + 1) {
        if (activationWait == WAIT_TIMEOUT) NotifyFailure(state, "process-loopback activation timed out after five seconds");
        if (uninitialize) CoUninitialize();
        SetEvent(state->doneEvent);
        return;
    }

    IAudioClient* audioClient = nullptr;
    HRESULT activationResult = E_FAIL;
    {
        std::lock_guard lock(state->mutex);
        activationResult = state->activationResult;
        audioClient = state->audioClient;
        if (audioClient != nullptr) audioClient->AddRef();
    }
    if (FAILED(activationResult) || audioClient == nullptr) {
        NotifyFailure(state, HResultText("GetActivateResult", activationResult));
        if (audioClient != nullptr) audioClient->Release();
        if (uninitialize) CoUninitialize();
        SetEvent(state->doneEvent);
        return;
    }

    WAVEFORMATEX wave{};
    wave.wFormatTag = WAVE_FORMAT_PCM;
    wave.nChannels = 2;
    wave.nSamplesPerSec = kOutputRate;
    wave.wBitsPerSample = 16;
    wave.nBlockAlign = wave.nChannels * wave.wBitsPerSample / 8;
    wave.nAvgBytesPerSec = wave.nSamplesPerSec * wave.nBlockAlign;
    const ProcessLoopbackFormat format{kOutputRate, 2, LoopbackSampleFormat::Pcm16};
    HRESULT result = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &wave, nullptr);
    if (SUCCEEDED(result)) result = audioClient->SetEventHandle(state->sampleEvent);
    IAudioCaptureClient* captureClient = nullptr;
    if (SUCCEEDED(result)) {
        result = audioClient->GetService(__uuidof(IAudioCaptureClient),
                                         reinterpret_cast<void**>(&captureClient));
    }
    if (SUCCEEDED(result)) result = audioClient->Start();
    if (FAILED(result)) {
        NotifyFailure(state, HResultText("IAudioClient initialization", result));
        if (captureClient != nullptr) captureClient->Release();
        audioClient->Release();
        if (uninitialize) CoUninitialize();
        SetEvent(state->doneEvent);
        return;
    }

    std::shared_ptr<IProcessLoopbackBackend::ActivationCallback> activated;
    {
        std::lock_guard lock(state->mutex);
        if (!state->stopping) activated = state->activation;
    }
    if (activated) (*activated)(true, format, {});

    bool failed = false;
    const HANDLE captureWaits[] = {state->stopEvent, state->sampleEvent};
    for (;;) {
        const auto captureWait = WaitForMultipleObjects(2, captureWaits, FALSE, 250);
        if (captureWait == WAIT_OBJECT_0) break;
        if (captureWait != WAIT_OBJECT_0 + 1 && captureWait != WAIT_TIMEOUT) {
            result = E_FAIL;
            failed = true;
            break;
        }
        for (;;) {
            UINT32 packetFrames = 0;
            result = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(result) || packetFrames == 0) break;
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            result = captureClient->GetBuffer(&data, &packetFrames, &flags,
                                              &devicePosition, &qpcPosition);
            if (FAILED(result)) break;
            const auto retainedFrames = std::min<std::size_t>(packetFrames, kMaximumBufferedFrames);
            const auto skippedFrames = packetFrames - retainedFrames;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (!silent && data != nullptr) {
                const auto* source = reinterpret_cast<const std::int16_t*>(data) + skippedFrames * 2;
                std::copy_n(source, retainedFrames * 2, state->packetScratch.begin());
            }
            const HRESULT releaseResult = captureClient->ReleaseBuffer(packetFrames);
            if (FAILED(releaseResult)) {
                result = releaseResult;
                break;
            }
            std::shared_ptr<IProcessLoopbackBackend::PacketCallback> packets;
            {
                std::lock_guard lock(state->mutex);
                if (!state->stopping) packets = state->packets;
            }
            if (packets) {
                (*packets)(LoopbackPacket{
                    silent ? nullptr : state->packetScratch.data(),
                    static_cast<std::uint32_t>(retainedFrames),
                    static_cast<std::int64_t>(qpcPosition) + FramesTo100ns(skippedFrames, kOutputRate),
                    silent,
                    (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 || skippedFrames != 0});
            }
        }
        if (FAILED(result)) {
            failed = true;
            break;
        }
    }

    audioClient->Stop();
    captureClient->Release();
    audioClient->Release();
    if (failed) NotifyFailure(state, HResultText("IAudioCaptureClient", result));
    if (uninitialize) CoUninitialize();
    SetEvent(state->doneEvent);
}

class WindowsProcessLoopbackBackend final : public IProcessLoopbackBackend {
public:
    ~WindowsProcessLoopbackBackend() override { Stop(kStopDeadlineMs); }

    bool Start(std::uint32_t excludedProcessTreeRoot,
               ProcessLoopbackMode mode,
               ActivationCallback activation,
               PacketCallback packets,
               std::string& error) override {
        if (mode != ProcessLoopbackMode::ExcludeTargetProcessTree) {
            error = "process loopback requires EXCLUDE_TARGET_PROCESS_TREE";
            return false;
        }
        if (CurrentWindowsBuild() < kMinimumProcessLoopbackBuild) {
            error = "process loopback requires Windows build 20348 or newer";
            return false;
        }
        if (!IsProcessAlive(excludedProcessTreeRoot)) {
            error = "excluded process-tree root is not alive or accessible";
            return false;
        }

        auto state = std::make_shared<WindowsBackendState>();
        state->activationEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        state->sampleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        state->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        state->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (state->activationEvent == nullptr || state->sampleEvent == nullptr ||
            state->stopEvent == nullptr || state->doneEvent == nullptr) {
            error = "failed to create process-loopback synchronization events";
            return false;
        }
        state->activation = std::make_shared<ActivationCallback>(std::move(activation));
        state->packets = std::make_shared<PacketCallback>(std::move(packets));
        {
            std::lock_guard lock(mutex_);
            if (state_ != nullptr) {
                error = "process-loopback backend is already started";
                return false;
            }
            state_ = state;
        }
        try {
            std::thread([state, excludedProcessTreeRoot] {
                RunWindowsCapture(state, excludedProcessTreeRoot);
            }).detach();
        } catch (...) {
            std::lock_guard lock(mutex_);
            state_.reset();
            error = "failed to create process-loopback worker";
            return false;
        }
        return true;
    }

    bool Stop(std::uint32_t timeoutMs) noexcept override {
        std::shared_ptr<WindowsBackendState> state;
        {
            std::lock_guard lock(mutex_);
            state = std::move(state_);
        }
        if (!state) return true;
        {
            std::lock_guard lock(state->mutex);
            state->stopping = true;
            state->activation = {};
            state->packets = {};
        }
        SetEvent(state->stopEvent);
        if (GetCurrentThreadId() != state->workerThreadId.load(std::memory_order_acquire)) {
            return WaitForSingleObject(state->doneEvent, timeoutMs) == WAIT_OBJECT_0;
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::shared_ptr<WindowsBackendState> state_;
};
} // namespace

struct SystemAudioCapture::Impl {
    struct BackendHandle {
        explicit BackendHandle(std::unique_ptr<IProcessLoopbackBackend> ownedBackend)
            : owned(std::move(ownedBackend)), backend(owned.get()) {}
        explicit BackendHandle(IProcessLoopbackBackend& injected) : backend(&injected) {}

        std::unique_ptr<IProcessLoopbackBackend> owned;
        IProcessLoopbackBackend* backend = nullptr;
    };

    struct State {
        std::mutex mutex;
        std::mutex dispatchMutex;
        bool started = false;
        bool running = false;
        bool backendActive = false;
        std::uint64_t generation = 0;
        std::string lastError;
        std::shared_ptr<Callback> callback;
        ProcessLoopbackFormat format{};
        std::array<float, kMaximumBufferedFrames> pending{};
        std::size_t pendingFirst = 0;
        std::size_t pendingCount = 0;
        std::int64_t pendingTimestamp100ns = 0;
        std::uint64_t pendingTimestampRemainder = 0;
        bool pendingDiscontinuity = false;
        StreamingResampler resampler;
    };

    explicit Impl(std::unique_ptr<IProcessLoopbackBackend> owned)
        : backend(std::make_shared<BackendHandle>(std::move(owned))), state(std::make_shared<State>()) {}
    explicit Impl(IProcessLoopbackBackend& injected)
        : backend(std::make_shared<BackendHandle>(injected)), state(std::make_shared<State>()) {}

    std::shared_ptr<BackendHandle> backend;
    std::shared_ptr<State> state;
};

SystemAudioCapture::SystemAudioCapture()
    : SystemAudioCapture(static_cast<IProcessLoopbackBackend*>(nullptr)) {}

SystemAudioCapture::SystemAudioCapture(IProcessLoopbackBackend* backend)
    : impl_(backend ? std::make_unique<Impl>(*backend)
                    : std::make_unique<Impl>(std::make_unique<WindowsProcessLoopbackBackend>())) {}

SystemAudioCapture::SystemAudioCapture(IProcessLoopbackBackend& backend)
    : impl_(std::make_unique<Impl>(backend)) {}

SystemAudioCapture::~SystemAudioCapture() { Stop(); }

bool SystemAudioCapture::Start(std::uint32_t excludedProcessTreeRoot, Callback callback) {
    if (excludedProcessTreeRoot == 0) {
        std::lock_guard lock(impl_->state->mutex);
        impl_->state->lastError = "excluded process-tree root must be nonzero";
        return false;
    }
    if (!callback) {
        std::lock_guard lock(impl_->state->mutex);
        impl_->state->lastError = "system-audio callback is required";
        return false;
    }

    std::uint64_t generation = 0;
    {
        std::lock_guard lock(impl_->state->mutex);
        if (impl_->state->started || impl_->state->backendActive) {
            impl_->state->lastError = "system audio capture must be stopped before restart";
            return false;
        }
        impl_->state->started = true;
        impl_->state->running = false;
        impl_->state->backendActive = true;
        impl_->state->lastError.clear();
        impl_->state->callback = std::make_shared<Callback>(std::move(callback));
        impl_->state->pendingFirst = 0;
        impl_->state->pendingCount = 0;
        impl_->state->pendingTimestampRemainder = 0;
        impl_->state->resampler.Reset();
        impl_->state->pendingDiscontinuity = false;
        generation = ++impl_->state->generation;
    }

    const std::weak_ptr<Impl::State> weakState = impl_->state;
    auto activation = [weakState, generation](bool succeeded,
                                               ProcessLoopbackFormat format,
                                               std::string error) {
        const auto state = weakState.lock();
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (!state->started || state->generation != generation) return;
        if (!succeeded || !IsSupportedFormat(format)) {
            state->started = false;
            state->running = false;
            state->lastError = !error.empty() ? std::move(error) : "unsupported process-loopback format";
            state->callback.reset();
            state->pendingFirst = 0;
            state->pendingCount = 0;
            return;
        }
        state->format = format;
        state->running = true;
    };
    const std::weak_ptr<Impl::BackendHandle> weakBackend = impl_->backend;
    auto packets = [weakState, weakBackend, generation](const LoopbackPacket& packet) {
        const auto state = weakState.lock();
        if (!state || packet.frameCount == 0) return;

        const auto failConsumer = [&](std::string error) {
            bool quiesce = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->generation != generation) return;
                state->started = false;
                state->running = false;
                state->lastError = std::move(error);
                state->callback.reset();
                state->pendingFirst = 0;
                state->pendingCount = 0;
                quiesce = state->backendActive;
            }
            if (quiesce) {
                bool quiesced = true;
                if (const auto backend = weakBackend.lock()) {
                    quiesced = backend->backend->Stop(kStopDeadlineMs);
                }
                std::lock_guard lock(state->mutex);
                if (state->generation == generation) {
                    state->backendActive = false;
                    if (!quiesced) state->lastError += "; backend stop timed out and detached";
                    ++state->generation;
                }
            }
        };

        {
            std::lock_guard lock(state->mutex);
            if (!state->running || state->generation != generation) return;
            const auto& format = state->format;
            const auto maximumInputFrames =
                (kMaximumBufferedFrames * static_cast<std::size_t>(format.sampleRateHz) +
                 kOutputRate - 1) / kOutputRate + 1;
            const auto skippedInputFrames = packet.frameCount > maximumInputFrames
                ? packet.frameCount - maximumInputFrames : 0;
            if (packet.discontinuity || skippedInputFrames != 0) {
                state->pendingFirst = 0;
                state->pendingCount = 0;
                const auto skippedTimestampNumerator =
                    static_cast<std::uint64_t>(skippedInputFrames) * 10'000'000;
                state->pendingTimestamp100ns = packet.timestamp100ns +
                    skippedTimestampNumerator / format.sampleRateHz;
                state->pendingTimestampRemainder =
                    (skippedTimestampNumerator % format.sampleRateHz) * kOutputRate /
                    format.sampleRateHz;
                state->pendingDiscontinuity = true;
                state->resampler.Reset();
            } else if (state->pendingCount == 0) {
                state->pendingTimestamp100ns = packet.timestamp100ns;
                state->pendingTimestampRemainder = 0;
            }

            const auto advanceTimestamp = [&](std::size_t frames) {
                const auto numerator = state->pendingTimestampRemainder +
                    static_cast<std::uint64_t>(frames) * 10'000'000;
                state->pendingTimestamp100ns += numerator / kOutputRate;
                state->pendingTimestampRemainder = numerator % kOutputRate;
            };
            const auto push = [&](float sample) {
                if (state->pendingCount == kMaximumBufferedFrames) {
                    state->pendingFirst = (state->pendingFirst + 1) % kMaximumBufferedFrames;
                    --state->pendingCount;
                    advanceTimestamp(1);
                    state->pendingDiscontinuity = true;
                }
                const auto destination =
                    (state->pendingFirst + state->pendingCount) % kMaximumBufferedFrames;
                state->pending[destination] = sample;
                ++state->pendingCount;
            };
            for (std::size_t frame = skippedInputFrames; frame < packet.frameCount; ++frame) {
                state->resampler.Process(ReadMonoFrame(packet, format, frame),
                                         format.sampleRateHz, push);
            }
        }

        for (;;) {
            OwnedAudioBlock block;
            std::shared_ptr<Callback> callback;
            {
                std::lock_guard lock(state->mutex);
                if (!state->running || state->generation != generation ||
                    state->pendingCount < kBlockFrames) return;
                block.samples.resize(kBlockFrames);
                for (std::size_t index = 0; index < kBlockFrames; ++index) {
                    block.samples[index] = FloatToPcm16(
                        state->pending[(state->pendingFirst + index) % kMaximumBufferedFrames]);
                }
                state->pendingFirst = (state->pendingFirst + kBlockFrames) % kMaximumBufferedFrames;
                state->pendingCount -= kBlockFrames;
                block.sampleRateHz = static_cast<int>(kOutputRate);
                block.channels = 1;
                block.timestamp100ns = state->pendingTimestamp100ns;
                block.duration100ns = kBlockDuration100ns;
                block.source = AudioTapKind::SystemAudio;
                block.callGeneration = 0;
                block.discontinuity = state->pendingDiscontinuity;
                state->pendingTimestamp100ns += kBlockDuration100ns;
                state->pendingTimestampRemainder = 0;
                state->pendingDiscontinuity = false;
                callback = state->callback;
            }

            std::lock_guard dispatchLock(state->dispatchMutex);
            {
                std::lock_guard lock(state->mutex);
                if (!state->running || state->generation != generation || callback != state->callback) return;
            }
            try {
                (*callback)(std::move(block));
            } catch (const std::exception& error) {
                failConsumer(std::string("system-audio callback failed: ") + error.what());
                return;
            } catch (...) {
                failConsumer("system-audio callback failed");
                return;
            }
        }
    };

    std::string error;
    if (!impl_->backend->backend->Start(excludedProcessTreeRoot,
                                        ProcessLoopbackMode::ExcludeTargetProcessTree,
                                        std::move(activation), std::move(packets), error)) {
        std::lock_guard lock(impl_->state->mutex);
        if (impl_->state->generation == generation) {
            impl_->state->started = false;
            impl_->state->running = false;
            impl_->state->backendActive = false;
            impl_->state->lastError = !error.empty() ? std::move(error) : "process-loopback start failed";
            impl_->state->callback.reset();
        }
        return false;
    }
    return true;
}

void SystemAudioCapture::Stop() noexcept {
    if (!impl_) return;
    std::uint64_t stoppedGeneration = 0;
    {
        std::lock_guard lock(impl_->state->mutex);
        if (!impl_->state->backendActive) return;
        impl_->state->started = false;
        impl_->state->running = false;
        impl_->state->backendActive = false;
        stoppedGeneration = ++impl_->state->generation;
        impl_->state->callback.reset();
        impl_->state->pendingFirst = 0;
        impl_->state->pendingCount = 0;
    }
    if (!impl_->backend->backend->Stop(kStopDeadlineMs)) {
        std::lock_guard lock(impl_->state->mutex);
        if (impl_->state->generation == stoppedGeneration) {
            impl_->state->lastError = "system-audio stop timed out; backend detached";
        }
    }
}

bool SystemAudioCapture::IsRunning() const noexcept {
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->running;
}

bool SystemAudioCapture::IsStarting() const noexcept {
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->started && !impl_->state->running;
}

std::string SystemAudioCapture::LastError() const {
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->lastError;
}

} // namespace telestudio::media
