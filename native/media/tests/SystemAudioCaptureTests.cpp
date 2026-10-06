#include "telestudio/media/SystemAudioCapture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::atomic<bool> gTrackAllocations = false;
std::atomic<std::size_t> gAllocationCount = 0;
}

void* operator new(std::size_t size) {
    if (gTrackAllocations.load(std::memory_order_relaxed)) {
        gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using telestudio::media::AudioTapKind;
using telestudio::media::IProcessLoopbackBackend;
using telestudio::media::LoopbackPacket;
using telestudio::media::LoopbackSampleFormat;
using telestudio::media::OwnedAudioBlock;
using telestudio::media::ProcessLoopbackFormat;
using telestudio::media::ProcessLoopbackMode;
using telestudio::media::SystemAudioCapture;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class FakeLoopbackBackend final : public IProcessLoopbackBackend {
public:
    bool Start(std::uint32_t excludedProcessTreeRoot,
               ProcessLoopbackMode mode,
               ActivationCallback activation,
               PacketCallback packets,
               std::string& error) override {
        std::lock_guard lock(mutex_);
        ++startCalls;
        if (active) {
            error = "backend-already-active";
            return false;
        }
        requestedRoot = excludedProcessTreeRoot;
        requestedMode = mode;
        activation_ = std::make_shared<ActivationCallback>(std::move(activation));
        packets_ = std::make_shared<PacketCallback>(std::move(packets));
        if (!startSucceeds) {
            error = startError;
            return false;
        }
        active = true;
        stopping_ = false;
        if (autoActivate) (*activation_)(true, format, {});
        return true;
    }

    bool Stop(std::uint32_t timeoutMs) noexcept override {
        std::unique_lock lock(mutex_);
        ++stopCalls;
        active = false;
        stopping_ = true;
        if (blockStop) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
            quiesceTimedOut = true;
            return false;
        }
        stopWaiting_ = true;
        condition_.notify_all();
        if (inFlight_ != 0 && emitterThread_ != std::this_thread::get_id() &&
            !condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                 [&] { return inFlight_ == 0; })) {
            quiesceTimedOut = true;
        }
        stopWaiting_ = false;
        return !quiesceTimedOut;
    }

    void CompleteActivation(bool succeeded,
                            ProcessLoopbackFormat activatedFormat,
                            std::string error) {
        std::shared_ptr<ActivationCallback> activation;
        {
            std::lock_guard lock(mutex_);
            format = activatedFormat;
            activation = activation_;
        }
        if (activation) (*activation)(succeeded, activatedFormat, std::move(error));
    }

    void Emit(const void* data,
              std::uint32_t frames,
              std::int64_t timestamp100ns,
              bool silent = false,
              bool discontinuity = false) {
        std::shared_ptr<PacketCallback> packets;
        {
            std::unique_lock lock(mutex_);
            if (!active) return;
            ++inFlight_;
            emitterThread_ = std::this_thread::get_id();
            gateEntered_ = gateBeforeDispatch_;
            condition_.notify_all();
            if (gateBeforeDispatch_) condition_.wait(lock, [&] { return releaseGate_; });
            packets = packets_;
        }
        try {
            if (packets) (*packets)(LoopbackPacket{data, frames, timestamp100ns, silent, discontinuity});
        } catch (...) {
            FinishEmit();
            throw;
        }
        FinishEmit();
    }

    void GateBeforeDispatch() {
        std::lock_guard lock(mutex_);
        gateBeforeDispatch_ = true;
        releaseGate_ = false;
    }

    void WaitUntilGateEntered() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return gateEntered_; });
    }

    void WaitUntilStopWaiting() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return stopWaiting_; });
    }

    void ReleaseDispatchGate() {
        std::lock_guard lock(mutex_);
        releaseGate_ = true;
        condition_.notify_all();
    }

    void EmitStereoFloat48k(const std::vector<float>& interleaved,
                            std::int64_t timestamp100ns,
                            bool discontinuity = false) {
        Emit(interleaved.data(), static_cast<std::uint32_t>(interleaved.size() / 2),
             timestamp100ns, false, discontinuity);
    }

    bool autoActivate = true;
    bool startSucceeds = true;
    std::string startError = "backend-start-failed";
    ProcessLoopbackFormat format{48'000, 2, LoopbackSampleFormat::Float32};
    int startCalls = 0;
    int stopCalls = 0;
    bool active = false;
    bool quiesceTimedOut = false;
    bool blockStop = false;
    std::uint32_t requestedRoot = 0;
    ProcessLoopbackMode requestedMode = ProcessLoopbackMode::IncludeTargetProcessTree;

private:
    void FinishEmit() noexcept {
        std::lock_guard lock(mutex_);
        --inFlight_;
        if (inFlight_ == 0) emitterThread_ = {};
        condition_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::shared_ptr<ActivationCallback> activation_;
    std::shared_ptr<PacketCallback> packets_;
    std::size_t inFlight_ = 0;
    std::thread::id emitterThread_{};
    bool stopping_ = false;
    bool gateBeforeDispatch_ = false;
    bool gateEntered_ = false;
    bool releaseGate_ = false;
    bool stopWaiting_ = false;
};

std::vector<float> StereoFloat(std::size_t frames, float left, float right) {
    std::vector<float> samples(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        samples[frame * 2] = left;
        samples[frame * 2 + 1] = right;
    }
    return samples;
}

bool AllSamples(const OwnedAudioBlock& block, std::int16_t value) {
    return std::all_of(block.samples.begin(), block.samples.end(), [value](std::int16_t sample) {
        return sample == value;
    });
}

void VerifyExcludedTreeAndStereoFloatNormalization() {
    FakeLoopbackBackend backend;
    std::vector<OwnedAudioBlock> blocks;
    SystemAudioCapture capture(backend);

    Require(capture.Start(4242, [&](OwnedAudioBlock&& block) { blocks.push_back(std::move(block)); }),
            "capture must accept an explicit nonzero exclusion root");
    Require(backend.requestedRoot == 4242 &&
                backend.requestedMode == ProcessLoopbackMode::ExcludeTargetProcessTree,
            "capture must request exclusion of exactly the supplied process tree");
    Require(capture.IsRunning(), "successful activation must confirm capture running");

    const auto first = StereoFloat(480, 0.25F, 0.75F);
    const auto second = StereoFloat(480, 0.25F, 0.75F);
    backend.EmitStereoFloat48k(first, 1'000'000);
    Require(blocks.empty(), "ten milliseconds must wait for a complete outbound block");
    backend.EmitStereoFloat48k(second, 1'100'000);

    Require(blocks.size() == 1 && blocks.front().samples.size() == 960,
            "two 10-ms packets must coalesce into one 20-ms block");
    Require(AllSamples(blocks.front(), 16'384),
            "stereo float must downmix and convert to signed 16-bit PCM");
    Require(blocks.front().sampleRateHz == 48'000 && blocks.front().channels == 1 &&
                blocks.front().timestamp100ns == 1'000'000 &&
                blocks.front().duration100ns == 200'000 &&
                blocks.front().source == AudioTapKind::SystemAudio,
            "system-audio output must carry the 48-kHz mono 20-ms contract");

    capture.Stop();
    Require(!capture.IsRunning() && backend.stopCalls == 1,
            "Stop must synchronously leave capture confirmed off");
}

void VerifyZeroRootNeverFallsBack() {
    FakeLoopbackBackend backend;
    SystemAudioCapture capture(backend);
    Require(!capture.Start(0, [](OwnedAudioBlock&&) {}),
            "zero exclusion root must be rejected");
    Require(backend.startCalls == 0 && !capture.LastError().empty(),
            "invalid roots must not activate all-system or current-process loopback");
}

void VerifyLateActivationCannotRestartStoppedCapture() {
    FakeLoopbackBackend backend;
    backend.autoActivate = false;
    std::vector<OwnedAudioBlock> blocks;
    SystemAudioCapture capture(backend);
    Require(capture.Start(77, [&](OwnedAudioBlock&& block) { blocks.push_back(std::move(block)); }),
            "asynchronous activation request must be accepted");
    Require(!capture.IsRunning(), "pending activation is not confirmed running");

    capture.Stop();
    backend.CompleteActivation(true, backend.format, {});
    const auto audio = StereoFloat(960, 0.5F, 0.5F);
    backend.EmitStereoFloat48k(audio, 2'000'000);
    Require(!capture.IsRunning() && blocks.empty(),
            "late completion and packets must not revive a stopped generation");
}

void VerifyActivationFailureIsObservable() {
    FakeLoopbackBackend backend;
    backend.autoActivate = false;
    SystemAudioCapture capture(backend);
    Require(capture.Start(91, [](OwnedAudioBlock&&) {}), "activation request must start");
    backend.CompleteActivation(false, backend.format, "access-denied");
    Require(!capture.IsRunning() && capture.LastError() == "access-denied",
            "activation-result failure must leave capture off with a stable error");
}

void VerifyFailedActivationCanBeStoppedAndRetried() {
    FakeLoopbackBackend backend;
    backend.autoActivate = false;
    SystemAudioCapture capture(backend);
    Require(capture.Start(92, [](OwnedAudioBlock&&) {}), "first activation request must start");
    backend.CompleteActivation(false, backend.format, "device-invalidated");
    Require(!capture.Start(92, [](OwnedAudioBlock&&) {}),
            "a failed backend generation must be stopped before retry");
    capture.Stop();
    Require(backend.stopCalls == 1,
            "Stop must release a backend whose asynchronous activation failed");

    backend.autoActivate = true;
    Require(capture.Start(92, [](OwnedAudioBlock&&) {}) && capture.IsRunning(),
            "capture must be restartable after a stopped activation failure");
}

void VerifyPcm16ResamplesTo48kMono() {
    FakeLoopbackBackend backend;
    backend.format = {44'100, 1, LoopbackSampleFormat::Pcm16};
    std::vector<OwnedAudioBlock> blocks;
    SystemAudioCapture capture(backend);
    Require(capture.Start(123, [&](OwnedAudioBlock&& block) { blocks.push_back(std::move(block)); }),
            "PCM activation must start");

    const std::vector<std::int16_t> pcm(883, 1'234);
    backend.Emit(pcm.data(), 883, 3'000'000);
    Require(blocks.size() == 1 && blocks.front().samples.size() == 960 &&
                AllSamples(blocks.front(), 1'234),
            "44.1-kHz PCM16 plus its interpolation boundary must form one 48-kHz mono block");
}

void VerifyOnePacketCannotQueueMoreThan60Milliseconds() {
    FakeLoopbackBackend backend;
    std::vector<OwnedAudioBlock> blocks;
    SystemAudioCapture capture(backend);
    Require(capture.Start(555, [&](OwnedAudioBlock&& block) { blocks.push_back(std::move(block)); }),
            "bounded-buffer capture must start");

    const auto burst = StereoFloat(4'800, 0.125F, 0.125F);
    backend.EmitStereoFloat48k(burst, 4'000'000);
    Require(blocks.size() == 3,
            "a 100-ms burst must retain no more than the newest 60 ms");
    Require(blocks.front().timestamp100ns == 4'400'000 && blocks.front().discontinuity,
            "dropping stale burst audio must advance time and mark discontinuity");
}

void VerifyStopQuiescesPacketWaitingBeforeConsumerDispatch() {
    FakeLoopbackBackend backend;
    backend.GateBeforeDispatch();
    std::atomic<int> callbacks = 0;
    SystemAudioCapture capture(backend);
    Require(capture.Start(600, [&](OwnedAudioBlock&&) { callbacks.fetch_add(1); }),
            "overlap capture must start");
    const auto audio = StereoFloat(960, 0.25F, 0.25F);
    std::thread emitter([&] { backend.EmitStereoFloat48k(audio, 6'000'000); });
    backend.WaitUntilGateEntered();
    std::thread stopper([&] { capture.Stop(); });
    backend.WaitUntilStopWaiting();
    backend.ReleaseDispatchGate();
    emitter.join();
    stopper.join();

    Require(!backend.quiesceTimedOut && callbacks.load() == 0 && !capture.IsRunning(),
            "Stop must quiesce a pre-dispatch packet without lock inversion or late delivery");
    backend.EmitStereoFloat48k(audio, 6'200'000);
    Require(callbacks.load() == 0, "no packet may reach the consumer after Stop returns");
}

void VerifyStopInsideConsumerDoesNotSelfDeadlock() {
    FakeLoopbackBackend backend;
    std::atomic<int> callbacks = 0;
    SystemAudioCapture capture(backend);
    Require(capture.Start(601, [&](OwnedAudioBlock&&) {
        callbacks.fetch_add(1);
        capture.Stop();
    }), "self-stop capture must start");
    const auto audio = StereoFloat(960, 0.25F, 0.25F);
    backend.EmitStereoFloat48k(audio, 6'500'000);
    backend.EmitStereoFloat48k(audio, 6'700'000);
    Require(callbacks.load() == 1 && backend.stopCalls == 1 && !backend.quiesceTimedOut &&
                !capture.IsRunning(),
            "Stop from the consumer callback must complete and suppress later callbacks");
}

void VerifyPendingActivationStopHasBoundedDeadline() {
    FakeLoopbackBackend backend;
    backend.autoActivate = false;
    backend.blockStop = true;
    std::atomic<int> callbacks = 0;
    SystemAudioCapture capture(backend);
    Require(capture.Start(605, [&](OwnedAudioBlock&&) { callbacks.fetch_add(1); }),
            "blocked-activation capture must start");

    const auto started = std::chrono::steady_clock::now();
    capture.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Require(elapsed < std::chrono::milliseconds(800),
            "Stop must not inherit an unbounded activation-worker wait");
    Require(!capture.IsRunning() && capture.LastError().find("timed out") != std::string::npos,
            "timed-out activation stop must remain failed closed and observable");

    backend.CompleteActivation(true, backend.format, {});
    const auto audio = StereoFloat(960, 0.25F, 0.25F);
    backend.EmitStereoFloat48k(audio, 6'900'000);
    Require(callbacks.load() == 0, "late activation and packets must stay detached after bounded Stop");
}

void VerifyBlockedCaptureDestructionIsBoundedAndLifetimeSafe() {
    FakeLoopbackBackend backend;
    backend.blockStop = true;
    std::atomic<int> callbacks = 0;
    auto capture = std::make_unique<SystemAudioCapture>(backend);
    Require(capture->Start(606, [&](OwnedAudioBlock&&) { callbacks.fetch_add(1); }) &&
                capture->IsRunning(),
            "blocked-capture destruction fixture must run");

    const auto started = std::chrono::steady_clock::now();
    capture.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Require(elapsed < std::chrono::milliseconds(800),
            "capture destruction must detach safely at the shutdown deadline");

    backend.CompleteActivation(true, backend.format, {});
    const auto audio = StereoFloat(960, 0.25F, 0.25F);
    backend.EmitStereoFloat48k(audio, 7'100'000);
    Require(callbacks.load() == 0,
            "detached late worker callbacks must not access the destroyed capture");
}

std::vector<std::int16_t> CaptureRamp(const std::vector<float>& input,
                                      const std::vector<std::size_t>& chunks) {
    FakeLoopbackBackend backend;
    backend.format = {44'100, 1, LoopbackSampleFormat::Float32};
    std::vector<std::int16_t> samples;
    SystemAudioCapture capture(backend);
    Require(capture.Start(602, [&](OwnedAudioBlock&& block) {
        samples.insert(samples.end(), block.samples.begin(), block.samples.end());
    }), "ramp capture must start");
    std::size_t offset = 0;
    for (const auto count : chunks) {
        backend.Emit(input.data() + offset, static_cast<std::uint32_t>(count),
                     7'000'000 + static_cast<std::int64_t>(offset) * 10'000'000 / 44'100);
        offset += count;
    }
    Require(offset == input.size(), "ramp chunk fixture must consume the complete input");
    return samples;
}

void VerifyStreamingResamplerIsChunkEquivalent() {
    std::vector<float> ramp(1'764);
    for (std::size_t index = 0; index < ramp.size(); ++index) {
        ramp[index] = static_cast<float>(static_cast<int>(index % 401) - 200) / 500.0F;
    }
    const auto whole = CaptureRamp(ramp, {ramp.size()});
    const auto chunked = CaptureRamp(ramp, {1, 7, 113, 2, 499, 37, 701, 404});
    Require(whole.size() == 960 && whole == chunked,
            "streaming resampling must produce identical samples across irregular packet boundaries");
}

void VerifyDiscontinuityResetsResamplerBeforePacket() {
    std::vector<float> discarded(101, 0.75F);
    std::vector<float> segment(883);
    for (std::size_t index = 0; index < segment.size(); ++index) {
        segment[index] = static_cast<float>(static_cast<int>(index % 257) - 128) / 300.0F;
    }

    const auto run = [&](bool dirty) {
        FakeLoopbackBackend backend;
        backend.format = {44'100, 1, LoopbackSampleFormat::Float32};
        std::vector<std::int16_t> samples;
        SystemAudioCapture capture(backend);
        Require(capture.Start(603, [&](OwnedAudioBlock&& block) {
            samples.insert(samples.end(), block.samples.begin(), block.samples.end());
        }), "discontinuity capture must start");
        if (dirty) backend.Emit(discarded.data(), static_cast<std::uint32_t>(discarded.size()), 8'000'000);
        backend.Emit(segment.data(), 882, 9'000'000, false, true);
        backend.Emit(segment.data() + 882, 1,
                     9'000'000 + static_cast<std::int64_t>(882) * 10'000'000 / 44'100);
        return samples;
    };
    const auto fresh = run(false);
    const auto afterPriorPhase = run(true);
    Require(fresh.size() == 960 && fresh == afterPriorPhase,
            "discontinuity must reset fractional phase before processing its packet");
}

void VerifyBurstProcessingUsesBoundedAllocations() {
    FakeLoopbackBackend backend;
    std::atomic<int> blocks = 0;
    SystemAudioCapture capture(backend);
    Require(capture.Start(604, [&](OwnedAudioBlock&& block) {
        Require(block.samples.size() == 960, "hot-path output block is malformed");
        blocks.fetch_add(1);
    }), "allocation-bound capture must start");
    const auto burst = StereoFloat(48'000, 0.125F, 0.125F);
    gAllocationCount.store(0);
    gTrackAllocations.store(true);
    backend.EmitStereoFloat48k(burst, 10'000'000);
    gTrackAllocations.store(false);
    Require(blocks.load() == 3 && gAllocationCount.load() <= 3,
            "a sustained burst must use only the three owned output allocations after startup");
}

void VerifyThrowingConsumerFailsClosed() {
    FakeLoopbackBackend backend;
    SystemAudioCapture capture(backend);
    Require(capture.Start(556, [](OwnedAudioBlock&&) {
        throw std::runtime_error("consumer-failed");
    }), "throwing-consumer capture must start");
    const auto audio = StereoFloat(960, 0.25F, 0.25F);
    bool escaped = false;
    try {
        backend.EmitStereoFloat48k(audio, 5'000'000);
    } catch (...) {
        escaped = true;
    }
    Require(!escaped && !capture.IsRunning() && !capture.LastError().empty(),
            "consumer exceptions must not escape an audio callback or leave capture running");
    Require(backend.stopCalls == 1 && !backend.active,
            "consumer failure must synchronously quiesce the backend");
    Require(capture.Start(556, [](OwnedAudioBlock&&) {}) && capture.IsRunning(),
            "consumer failure must permit immediate restart without hidden cleanup");
}

UINT toneDevice = WAVE_MAPPER;
void RenderTone() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48'000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    HWAVEOUT waveOut = nullptr;
    Require(waveOutOpen(&waveOut, toneDevice, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR,
            "tone smoke could not open the selected render endpoint");

    std::vector<std::int16_t> tone(48'000 * 2);
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t frame = 0; frame < tone.size() / 2; ++frame) {
        const auto sample = static_cast<std::int16_t>(
            std::sin(2.0 * pi * 1'000.0 * frame / 48'000.0) * 10'000.0);
        tone[frame * 2] = sample;
        tone[frame * 2 + 1] = sample;
    }
    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(tone.data());
    header.dwBufferLength = static_cast<DWORD>(tone.size() * sizeof(tone.front()));
    Require(waveOutPrepareHeader(waveOut, &header, sizeof(header)) == MMSYSERR_NOERROR,
            "tone smoke could not prepare its render buffer");
    try {
        Require(waveOutWrite(waveOut, &header, sizeof(header)) == MMSYSERR_NOERROR,
                "tone smoke could not render its test tone");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while ((header.dwFlags & WHDR_DONE) == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Require((header.dwFlags & WHDR_DONE) != 0, "tone smoke render did not complete");
    } catch (...) {
        waveOutReset(waveOut);
        waveOutUnprepareHeader(waveOut, &header, sizeof(header));
        waveOutClose(waveOut);
        throw;
    }
    waveOutUnprepareHeader(waveOut, &header, sizeof(header));
    waveOutClose(waveOut);
}

int ToneAmplitude(const OwnedAudioBlock& block) {
    constexpr double pi = 3.14159265358979323846;
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t index = 0; index < block.samples.size(); ++index) {
        const auto angle = 2.0 * pi * 1'000.0 * index / 48'000.0;
        real += block.samples[index] * std::cos(angle);
        imaginary -= block.samples[index] * std::sin(angle);
    }
    return static_cast<int>(2.0 * std::sqrt(real * real + imaginary * imaginary) /
                            block.samples.size());
}

void RunWindowsToneSmoke() {
    wchar_t executable[MAX_PATH]{};
    Require(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0,
            "tone smoke could not resolve its helper executable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --render-tone-child";
    if(toneDevice!=WAVE_MAPPER)command+=L" --render-device "+std::to_wstring(toneDevice);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable, mutableCommand.data(), nullptr, nullptr, FALSE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process) != FALSE,
            "tone smoke could not create its excluded-tree renderer");

    try {
        std::atomic<int> phase = 0;
        std::atomic<int> excludedAmplitude = 0;
        std::atomic<int> outsideAmplitude = 0;
        std::atomic<unsigned> outsideBlocks = 0;
        SystemAudioCapture capture;
        Require(capture.Start(process.dwProcessId, [&](OwnedAudioBlock&& block) {
            const auto amplitude = ToneAmplitude(block);
            if(phase.load()!=0)++outsideBlocks;
            auto& destination = phase.load() == 0 ? excludedAmplitude : outsideAmplitude;
            destination.store(std::max(destination.load(), amplitude));
        }), "real process-loopback activation request failed");

        const auto activationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture.IsRunning() && std::chrono::steady_clock::now() < activationDeadline) {
            if (!capture.LastError().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Require(capture.IsRunning(), capture.LastError().empty()
            ? "real process-loopback activation timed out" : capture.LastError().c_str());

        Require(ResumeThread(process.hThread) != static_cast<DWORD>(-1),
                "tone smoke could not start its excluded-tree renderer");
        CloseHandle(process.hThread);
        process.hThread = nullptr;
        Require(WaitForSingleObject(process.hProcess, 6'000) == WAIT_OBJECT_0,
                "excluded-tree tone renderer timed out");
        DWORD childExitCode = 1;
        Require(GetExitCodeProcess(process.hProcess, &childExitCode) != FALSE && childExitCode == 0,
                "excluded-tree tone renderer failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        phase.store(1);
        RenderTone();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        capture.Stop();
        Require(outsideAmplitude.load() >= 5'000,
                "real process loopback did not capture the outside-tree tone");
        Require(excludedAmplitude.load() <= std::max(250, outsideAmplitude.load() / 10),
                "real process loopback captured the excluded-tree tone");
        std::cout<<"loopback_pcm excludedAmplitude="<<excludedAmplitude.load()<<" outsideAmplitude="<<outsideAmplitude.load()<<" outsideBlocks="<<outsideBlocks.load()<<'\n';
    } catch (...) {
        if (process.hThread != nullptr) CloseHandle(process.hThread);
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        throw;
    }
    CloseHandle(process.hProcess);
    std::cout << "system_audio_capture_windows_tone_smoke: PASS excluded=quiet outside=captured device=" << toneDevice << '\n';
}
}

int main(int argc, char** argv) {
    try {
        if(argc==2&&std::string(argv[1])=="--list-render-devices"){
            for(UINT id=0;id<waveOutGetNumDevs();++id){WAVEOUTCAPSA caps{};if(waveOutGetDevCapsA(id,&caps,sizeof(caps))==MMSYSERR_NOERROR)std::cout<<id<<" "<<caps.szPname<<'\n';}
            return EXIT_SUCCESS;
        }
        if(argc==4&&std::string(argv[1])=="--windows-tone-smoke"&&std::string(argv[2])=="--render-device"){
            toneDevice=UINT(std::stoul(argv[3]));RunWindowsToneSmoke();return EXIT_SUCCESS;
        }
        if(argc==4&&std::string(argv[1])=="--render-tone-child"&&std::string(argv[2])=="--render-device"){
            toneDevice=UINT(std::stoul(argv[3]));RenderTone();return EXIT_SUCCESS;
        }
        if (argc == 2 && std::string(argv[1]) == "--windows-tone-smoke") {
            RunWindowsToneSmoke();
            return EXIT_SUCCESS;
        }
        if (argc == 2 && std::string(argv[1]) == "--render-tone-child") {
            RenderTone();
            return EXIT_SUCCESS;
        }
        VerifyExcludedTreeAndStereoFloatNormalization();
        VerifyZeroRootNeverFallsBack();
        VerifyLateActivationCannotRestartStoppedCapture();
        VerifyActivationFailureIsObservable();
        VerifyFailedActivationCanBeStoppedAndRetried();
        VerifyPcm16ResamplesTo48kMono();
        VerifyOnePacketCannotQueueMoreThan60Milliseconds();
        VerifyStopQuiescesPacketWaitingBeforeConsumerDispatch();
        VerifyStopInsideConsumerDoesNotSelfDeadlock();
        VerifyPendingActivationStopHasBoundedDeadline();
        VerifyBlockedCaptureDestructionIsBoundedAndLifetimeSafe();
        VerifyStreamingResamplerIsChunkEquivalent();
        VerifyDiscontinuityResetsResamplerBeforePacket();
        VerifyBurstProcessingUsesBoundedAllocations();
        VerifyThrowingConsumerFailsClosed();
        std::cout << "system_audio_capture_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "system_audio_capture_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
