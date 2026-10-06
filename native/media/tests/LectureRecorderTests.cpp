#include "telestudio/media/LectureRecorder.h"
#include "telestudio/media/DesktopCaptureSource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <bcrypt.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class FakeClock final : public telestudio::media::IMonotonicClock {
public:
    explicit FakeClock(std::int64_t value) : value_(value) {}
    std::int64_t Now100ns() noexcept override { return value_; }
    std::int64_t value_;
};

class FakeWorker final : public telestudio::IRecordingWorker {
public:
    telestudio::RecordingError Start(const telestudio::RecordingOptions&, std::uint64_t,
                                     std::int64_t) noexcept override {
        if (startError != telestudio::RecordingError::None) return startError;
        quiesced = false; return telestudio::RecordingError::None;
    }
    bool Push(telestudio::media::OwnedAudioBlock) noexcept override { return true; }
    void TapUnavailable(telestudio::media::AudioTapKind, std::uint64_t) noexcept override {}
    bool SetPaused(bool value, std::int64_t) noexcept override { paused=value; return true; }
    bool paused=false;
    void Stop() noexcept override {}
    std::optional<telestudio::RecordingWorkerUpdate> Poll() noexcept override {
        if (updates.empty()) return std::nullopt;
        auto result = updates.front(); updates.erase(updates.begin()); return result;
    }
    telestudio::RecordingWaitOutcome WaitQuiesced(std::chrono::milliseconds) noexcept override {
        ++waitCalls;
        return waitOutcome;
    }
    bool IsQuiesced() const noexcept override { return quiesced; }
    void PublishReady() { updates.push_back({telestudio::RecordingWorkerUpdateKind::Ready}); }
    void PublishCompleted(std::string name) {
        quiesced = true;
        updates.push_back({telestudio::RecordingWorkerUpdateKind::Completed,
                           telestudio::RecordingError::None, std::move(name)});
    }
    void PublishFailed(telestudio::RecordingError error, std::string name = {}) {
        quiesced = true;
        updates.push_back({telestudio::RecordingWorkerUpdateKind::Failed, error, std::move(name)});
    }
    bool quiesced = true;
    telestudio::RecordingWaitOutcome waitOutcome = telestudio::RecordingWaitOutcome::Quiesced;
    int waitCalls = 0;
    telestudio::RecordingError startError = telestudio::RecordingError::None;
    std::vector<telestudio::RecordingWorkerUpdate> updates;
};

class ManualGate {
public:
    void EnterAndWait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
    }
    bool WaitEntered(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return entered_; });
    }
    void Release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
};

telestudio::media::OwnedAudioBlock RecorderBlock(std::int64_t timestamp,
                                                  telestudio::media::AudioTapKind source) {
    return {std::vector<std::int16_t>(960, 1), 48000, 1, timestamp, 200'000,
            source, 1, false};
}

constexpr wchar_t kHarnessClass[] = L"EssaStudioRecordingHarnessClass";
constexpr wchar_t kHarnessTitle[] = L"EssaStudio Recording Harness";

LRESULT CALLBACK HarnessProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

int RunHarnessWindow() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = HarnessProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kHarnessClass;
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;
    HWND window = CreateWindowExW(0, kHarnessClass, kHarnessTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  100, 100, 640, 360, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return 3;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

struct ChildProcess {
    PROCESS_INFORMATION info{};
    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept : info(std::exchange(other.info, {})) {}
    ChildProcess& operator=(ChildProcess&& other) noexcept {
        if (this != &other) { Close(); info = std::exchange(other.info, {}); }
        return *this;
    }
    void Close() noexcept {
        if (info.hProcess) {
            TerminateProcess(info.hProcess, 0);
            WaitForSingleObject(info.hProcess, 3000);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            info = {};
        }
    }
    ~ChildProcess() { Close(); }
};

ChildProcess StartHarnessProcess() {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) throw std::runtime_error("harness-launch");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --harness";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    ChildProcess child;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child.info)) throw std::runtime_error("harness-launch");
    return child;
}

std::string SelectTarget(std::string_view target) {
    for (int retry = 0; retry != 50; ++retry) {
        const auto items = telestudio::DesktopCaptureSource::EnumerateCaptureItems();
        if (target == "PrimaryDisplay") {
            for (const auto& item : items) if (item.kind == telestudio::CaptureItemKind::Monitor) return item.id;
        } else {
            for (const auto& item : items) {
                if (item.kind == telestudio::CaptureItemKind::Window && item.name == "EssaStudio Recording Harness")
                    return item.id;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("capture-target-unavailable");
}

void CaptureMicrophone(telestudio::LectureRecorder& recorder,
                       const std::shared_ptr<telestudio::media::IMonotonicClock>& clock,
                       int seconds) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 96000;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) throw std::runtime_error("microphone-unavailable");
    HWAVEIN input = nullptr;
    if (waveInOpen(&input, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event), 0,
                   CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(event);
        throw std::runtime_error("microphone-unavailable");
    }
    std::array<std::int16_t, 960> samples{};
    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(samples.data());
    header.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(std::int16_t));
    if (waveInPrepareHeader(input, &header, sizeof(header)) != MMSYSERR_NOERROR ||
        waveInAddBuffer(input, &header, sizeof(header)) != MMSYSERR_NOERROR ||
        waveInStart(input) != MMSYSERR_NOERROR) {
        waveInClose(input); CloseHandle(event); throw std::runtime_error("microphone-unavailable");
    }
    std::int32_t peak = 0;
    std::int64_t firstTimestamp = -1;
    for (int block = 0; block != seconds * 50; ++block) {
        ResetEvent(event);
        if (WaitForSingleObject(event, 1500) != WAIT_OBJECT_0 || (header.dwFlags & WHDR_DONE) == 0) {
            waveInReset(input); waveInUnprepareHeader(input, &header, sizeof(header)); waveInClose(input);
            CloseHandle(event); throw std::runtime_error("microphone-capture-failed");
        }
        for (const auto sample : samples) peak = std::max(peak, std::abs(static_cast<int>(sample)));
        const auto now = clock->Now100ns();
        if (firstTimestamp < 0) firstTimestamp = std::max<std::int64_t>(0, now - 200'000);
        recorder.PushMicrophone({std::vector<std::int16_t>(samples.begin(), samples.end()), 48000, 1,
                                 firstTimestamp + static_cast<std::int64_t>(block) * 200'000, 200'000,
                                 telestudio::media::AudioTapKind::Microphone, 1, block == 0});
        header.dwBytesRecorded = 0;
        if (block + 1 != seconds * 50 && waveInAddBuffer(input, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            waveInReset(input); waveInUnprepareHeader(input, &header, sizeof(header)); waveInClose(input);
            CloseHandle(event); throw std::runtime_error("microphone-capture-failed");
        }
    }
    waveInStop(input);
    waveInReset(input);
    waveInUnprepareHeader(input, &header, sizeof(header));
    waveInClose(input);
    CloseHandle(event);
    if (peak == 0) throw std::runtime_error("microphone-silent");
}

struct ReopenEvidence {
    int videoSamples = 0;
    int audioSamples = 0;
    bool cleanPoint = false;
    bool monotonic = true;
};

ReopenEvidence ReopenRecording(const std::filesystem::path& path, bool expectAudio) {
    winrt::com_ptr<IMFSourceReader> reader;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) throw std::runtime_error("reopen-failed");
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { CoUninitialize(); throw std::runtime_error("reopen-failed"); }
    ReopenEvidence evidence;
    try {
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put()))) throw std::runtime_error("reopen-failed");
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        const DWORD streamCount = expectAudio ? 2u : 1u;
        struct StreamEvidence { bool video = false; bool eos = false; LONGLONG last = -1; };
        std::vector<StreamEvidence> streams(streamCount);
        bool foundVideo = false;
        bool foundAudio = false;
        for (DWORD stream = 0; stream < streamCount; ++stream) {
            winrt::com_ptr<IMFMediaType> type;
            if (FAILED(reader->GetNativeMediaType(stream, 0, type.put()))) throw std::runtime_error("stream-missing");
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            const bool videoStream = subtype == MFVideoFormat_H264;
            const bool audioStream = subtype == MFAudioFormat_AAC;
            if ((!videoStream && !audioStream) || (audioStream && !expectAudio) ||
                (videoStream && std::exchange(foundVideo, true)) ||
                (audioStream && std::exchange(foundAudio, true))) throw std::runtime_error("stream-format");
            streams[stream].video = videoStream;
            reader->SetStreamSelection(stream, TRUE);
        }
        if (!foundVideo || foundAudio != expectAudio) throw std::runtime_error("stream-missing");
        DWORD remaining = streamCount;
        while (remaining != 0) {
            DWORD actual = 0, flags = 0;
            LONGLONG time = 0;
            winrt::com_ptr<IMFSample> sample;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &actual, &flags, &time,
                                          sample.put())) || actual >= streamCount)
                throw std::runtime_error("reopen-read");
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                if (!streams[actual].eos) { streams[actual].eos = true; --remaining; }
                continue;
            }
            if (!sample) continue;
            LONGLONG duration = 0;
            if (time < 0 || time < streams[actual].last ||
                FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) evidence.monotonic = false;
            streams[actual].last = time;
            if (streams[actual].video) {
                ++evidence.videoSamples;
                UINT32 clean = FALSE;
                if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean)
                    evidence.cleanPoint = true;
            } else ++evidence.audioSamples;
        }
    } catch (...) {
        reader = nullptr; MFShutdown(); CoUninitialize(); throw;
    }
    reader = nullptr;
    MFShutdown();
    CoUninitialize();
    return evidence;
}

std::string FileSha256(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("hash-failed");
    std::array<std::uint8_t, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
                                   digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("hash-failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto value : digest) { result += hex[value >> 4]; result += hex[value & 15]; }
    return result;
}

int RunRecordingSmoke(std::string target, int seconds, bool microphone,
                      const std::filesystem::path& output) {
    ChildProcess harness;
    if (target == "HarnessWindow") harness = StartHarnessProcess();
    const std::string source = SelectTarget(target);
    auto clock = std::make_shared<telestudio::media::QpcMonotonicClock>();
    auto worker = telestudio::CreateNativeRecordingWorker({}, output);
    telestudio::LectureRecorder recorder(worker, clock, [](telestudio::media::AudioTapKind) { return true; });
    const auto started = recorder.Start({source, microphone, false});
    if (started.state != telestudio::RecordingState::Starting) throw std::runtime_error("start-rejected");
    for (int retry = 0; retry != 100 && recorder.Snapshot().state == telestudio::RecordingState::Starting; ++retry)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (recorder.Snapshot().state != telestudio::RecordingState::Recording)
        throw std::runtime_error("recording-not-ready");
    if (microphone) CaptureMicrophone(recorder, clock, seconds);
    else std::this_thread::sleep_for(std::chrono::seconds(seconds));
    recorder.Stop();
    for (int retry = 0; retry != 300 && recorder.Snapshot().state == telestudio::RecordingState::Finalizing; ++retry)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto final = recorder.Snapshot();
    if (final.state != telestudio::RecordingState::Completed) throw std::runtime_error("finalize-failed");
    const auto path = output / std::filesystem::path(final.fileName);
    try {
        const auto evidence = ReopenRecording(path, microphone);
        const auto sha = FileSha256(path);
        if (!evidence.monotonic || !evidence.cleanPoint || evidence.videoSamples == 0 ||
            (microphone && evidence.audioSamples == 0)) throw std::runtime_error("reopen-validation");
        std::cout << "duration_seconds=" << seconds << " video_codec=H264 audio_codec="
                  << (microphone ? "AAC" : "none") << " video_samples=" << evidence.videoSamples
                  << " audio_samples=" << evidence.audioSamples << " monotonic=true clean_point=true sha256="
                  << sha << '\n';
        std::filesystem::remove(path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    return EXIT_SUCCESS;
}
}

int main(int argc, char** argv) {
    try {
        using namespace telestudio;
        {
            constexpr std::array states{
                OperationAuthority::PendingStart,
                OperationAuthority::StartingOpen,
                OperationAuthority::PipelineStarted,
                OperationAuthority::WorkerFinished,
                OperationAuthority::CompletedByWorker,
                OperationAuthority::TimedOutByCoordinator};
            int allowedCount = 0;
            for (const auto from : states) {
                for (const auto to : states) {
                    const bool expected =
                        (from == OperationAuthority::PendingStart &&
                         (to == OperationAuthority::StartingOpen || to == OperationAuthority::TimedOutByCoordinator)) ||
                        (from == OperationAuthority::StartingOpen &&
                         (to == OperationAuthority::PipelineStarted || to == OperationAuthority::WorkerFinished ||
                          to == OperationAuthority::TimedOutByCoordinator)) ||
                        (from == OperationAuthority::PipelineStarted &&
                         (to == OperationAuthority::WorkerFinished || to == OperationAuthority::TimedOutByCoordinator)) ||
                        (from == OperationAuthority::WorkerFinished &&
                         (to == OperationAuthority::CompletedByWorker || to == OperationAuthority::TimedOutByCoordinator));
                    Require(IsOperationAuthorityTransitionAllowed(from, to) == expected,
                            "OperationAuthority must reject every unlisted transition");
                    if (expected) ++allowedCount;
                }
            }
            Require(allowedCount == 9, "OperationAuthority transition table must have exactly nine legal edges");
        }
        if (argc == 2 && std::string_view(argv[1]) == "--harness") return RunHarnessWindow();
        if (argc == 9 && std::string_view(argv[1]) == "--smoke") {
            return RunRecordingSmoke(argv[2], std::stoi(argv[4]), std::string_view(argv[6]) == "Microphone",
                                     std::filesystem::path(argv[8]));
        }
        using namespace telestudio;
        auto worker = std::make_shared<FakeWorker>();
        auto clock = std::make_shared<FakeClock>(1'000'000);
        LectureRecorder recorder(worker, clock, [](media::AudioTapKind) { return true; });
        auto start = recorder.Start({"display-1", true, false});
        Require(start.state == RecordingState::Starting, "Start must enter Starting");
        Require(recorder.Start({"display-1", true, false}).error == RecordingError::AlreadyRecording,
                "a live operation must reject another Start");
        worker->PublishReady();
        Require(recorder.Snapshot().state == RecordingState::Recording,
                "worker readiness must enter Recording");
        Require(recorder.Stop().state == RecordingState::Finalizing,
                "Stop must be nonblocking and enter Finalizing");
        Require(recorder.Stop().state == RecordingState::Finalizing,
                "repeated Stop must be idempotent");
        worker->PublishCompleted("EssaStudio-test.mp4");
        Require(recorder.Snapshot().state == RecordingState::Completed,
                "worker completion must enter Completed");
        const auto lifecycle = recorder.DrainNotifications();
        Require(lifecycle.size() == 4 &&
                    lifecycle[0].snapshot.state == RecordingState::Starting &&
                    lifecycle[1].snapshot.state == RecordingState::Recording &&
                    lifecycle[2].snapshot.state == RecordingState::Finalizing &&
                    lifecycle[3].snapshot.state == RecordingState::Completed,
                "recorder lifecycle must be Starting, Recording, Finalizing, Completed");
        Require(recorder.Snapshot().fileName == "EssaStudio-test.mp4", "only basename is public");
        Require(IsSafeRecordingBasename("EssaStudio-test.mp4") &&
                    !IsSafeRecordingBasename("../secret.mp4") &&
                    !IsSafeRecordingBasename("CON.mp4"),
                "basename policy must reject paths and DOS devices");
        {
            auto pauseWorker = std::make_shared<FakeWorker>();
            auto pauseClock = std::make_shared<FakeClock>(10'000'000);
            LectureRecorder pausedRecorder(pauseWorker,pauseClock,[](media::AudioTapKind){return true;});
            Require(pausedRecorder.Start({"display-pause",true,false}).state==RecordingState::Starting,"pause fixture starts");
            pauseWorker->PublishReady();pausedRecorder.Snapshot();
            pauseClock->value_+=20'000'000;
            Require(pausedRecorder.Pause().paused && pauseWorker->paused,"pause must acknowledge worker gating");
            const auto heldTime=pausedRecorder.Snapshot().elapsedMs;
            pauseClock->value_+=50'000'000;
            Require(pausedRecorder.Snapshot().elapsedMs==heldTime,"paused elapsed must remain frozen");
            Require(!pausedRecorder.Resume().paused && !pauseWorker->paused,"resume must acknowledge worker ungating");
            pauseClock->value_+=10'000'000;
            Require(pausedRecorder.Snapshot().elapsedMs==heldTime+1000,"resume excludes the five-second pause");
            pausedRecorder.Pause();const auto stopped=pausedRecorder.Stop();
            Require(stopped.state==RecordingState::Finalizing&&!stopped.paused,"stop while paused finalizes the same file");
            pauseClock->value_+=20'000'000;
            Require(pausedRecorder.Snapshot().elapsedMs==stopped.elapsedMs,"finalization must not append wall-clock duration");
            pauseWorker->PublishCompleted("MyStudio-pause.mp4");
            Require(pausedRecorder.Snapshot().state==RecordingState::Completed,"paused recording remains completable");
            Require(!pausedRecorder.Pause().paused,"completed recording cannot become paused");
        }
        auto failureWorker = std::make_shared<FakeWorker>();
        LectureRecorder failure(failureWorker, clock, [](media::AudioTapKind) { return true; });
        Require(failure.Start({"display-2", false, false}).state == RecordingState::Starting,
                "a second deterministic operation must start");
        failureWorker->PublishFailed(RecordingError::SinkFailed, "EssaStudio-recover.part");
        Require(failure.Snapshot().state == RecordingState::Failed,
                "active worker failure must enter Failed");
        const auto notifications = failure.DrainNotifications();
        Require(notifications.size() == 2 &&
                    notifications[0].kind == RecordingNotificationKind::State &&
                    notifications[1].kind == RecordingNotificationKind::Error &&
                    notifications[1].error == RecordingError::SinkFailed &&
                    notifications[1].snapshot.state == RecordingState::Failed &&
                    notifications[1].snapshot.fileName ==
                        "EssaStudio-recover.part",
                "active failure must publish one terminal Failed error with its recoverable basename");
        auto timeoutWorker = std::make_shared<FakeWorker>();
        LectureRecorder timeout(timeoutWorker, clock, [](media::AudioTapKind) { return true; });
        timeout.Start({"display-3", false, false});
        timeoutWorker->waitOutcome = RecordingWaitOutcome::TimedOutByCoordinator;
        timeoutWorker->quiesced = false;
        const auto timeoutResult = timeout.Shutdown(std::chrono::milliseconds(0));
        Require(timeoutResult.error == RecordingError::FinalizeTimeout,
                "explicit process shutdown alone may surface finalize timeout");
        auto committedWorker = std::make_shared<FakeWorker>();
        LectureRecorder committed(committedWorker, clock, [](media::AudioTapKind) { return true; });
        committed.Start({"display-committed", false, false});
        committedWorker->waitOutcome = RecordingWaitOutcome::CompletionCommittedNotQuiesced;
        committedWorker->quiesced = false;
        const auto committedBegan = std::chrono::steady_clock::now();
        const auto committedResult = committed.Shutdown(std::chrono::milliseconds(0));
        const auto committedNotifications = committed.DrainNotifications();
        Require(std::chrono::steady_clock::now() - committedBegan < std::chrono::milliseconds(100) &&
                    committedResult.error == RecordingError::None &&
                    committedResult.state == RecordingState::Finalizing &&
                    std::none_of(committedNotifications.begin(), committedNotifications.end(),
                        [](const auto& item) {
                            return item.kind == RecordingNotificationKind::Error ||
                                   item.snapshot.state == RecordingState::Failed;
                        }),
                "completion-committed shutdown must be bounded without fabricating FinalizeTimeout/Failed");
        auto noWaitWorker = std::make_shared<FakeWorker>();
        {
            LectureRecorder ordinary(noWaitWorker, clock, [](media::AudioTapKind) { return true; });
            ordinary.Start({"display-4", false, false});
        }
        Require(noWaitWorker->waitCalls == 0,
                "ordinary destruction/call teardown must enqueue Stop without process wait");

        const auto hookRoot = std::filesystem::current_path() / "lecture-recorder-hook-test";
        std::error_code cleanupError;
        std::filesystem::remove_all(hookRoot, cleanupError);
        std::filesystem::create_directories(hookRoot);
        {
            struct RestoreRecordingEnvironment {
                std::wstring previous;
                bool present = false;
                RestoreRecordingEnvironment() {
                    const DWORD size=GetEnvironmentVariableW(L"ESSASTUDIO_RECORDING_DIRECTORY",nullptr,0);
                    if(size) {previous.resize(size);const auto count=GetEnvironmentVariableW(L"ESSASTUDIO_RECORDING_DIRECTORY",previous.data(),size);previous.resize(count);present=true;}
                }
                ~RestoreRecordingEnvironment() {SetEnvironmentVariableW(L"ESSASTUDIO_RECORDING_DIRECTORY",present?previous.c_str():nullptr);}
            } restoreEnvironment;
            const auto selected=hookRoot / L"\u062a\u0633\u062c\u064a\u0644-selected";
            std::filesystem::create_directories(selected);
            Require(SetEnvironmentVariableW(L"ESSASTUDIO_RECORDING_DIRECTORY",selected.c_str()),"recording environment set");
            auto called=std::make_shared<std::atomic<bool>>(false);
            auto hooks=std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline=[called,selected](const std::filesystem::path& part) {
                Require(part.parent_path()==std::filesystem::canonical(selected),"Unicode selected recording folder must be used");
                called->store(true);
                std::ofstream(part,std::ios::binary).put('x');
                return RecordingError::None;
            };
            auto worker=CreateNativeRecordingWorker({}, {}, hooks);
            Require(worker->Start({"synthetic",false,false},1,0)==RecordingError::None,"environment recording starts");
            Require(worker->WaitQuiesced(std::chrono::seconds(2))==RecordingWaitOutcome::Quiesced && called->load(),"environment directory pipeline completes");
            const auto update=worker->Poll();
            Require(update && update->kind==RecordingWorkerUpdateKind::Completed,"selected directory recording completed");
            for(const auto& invalid : {std::filesystem::path(L"relative-recordings"),hookRoot/L"not-created"}) {
                Require(SetEnvironmentVariableW(L"ESSASTUDIO_RECORDING_DIRECTORY",invalid.c_str()),"invalid recording environment set");
                called->store(false);
                auto rejected=CreateNativeRecordingWorker({}, {}, hooks);
                Require(rejected->Start({"synthetic",false,false},1,0)==RecordingError::None,"invalid path processed asynchronously");
                Require(rejected->WaitQuiesced(std::chrono::seconds(2))==RecordingWaitOutcome::Quiesced,"invalid folder failure is bounded");
                const auto failure=rejected->Poll();
                Require(failure && failure->kind==RecordingWorkerUpdateKind::Failed && failure->error==RecordingError::SinkFailed && !called->load(),"invalid selected folder must fail without fallback");
            }
            Require(!std::filesystem::exists(hookRoot/L"not-created"),"missing selected folder must not be created");
        }
        const auto CountFiles = [](const std::filesystem::path& directory, std::string_view extension) {
            std::size_t count = 0;
            if (std::filesystem::exists(directory)) {
                for (const auto& entry : std::filesystem::directory_iterator(directory))
                    if (entry.is_regular_file() && entry.path().extension() == extension) ++count;
            }
            return count;
        };
        {
            const auto directory = hookRoot / "deadline";
            auto renameGate = std::make_shared<ManualGate>();
            auto renameAttempts = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                return RecordingError::None;
            };
            hooks->beforeRenameAuthority = [renameGate] { renameGate->EnterAndWait(); };
            hooks->afterRenameAttempt = [renameAttempts] { ++*renameAttempts; };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None,
                    "native synthetic worker must start asynchronously");
            Require(renameGate->WaitEntered(), "coordinator must reach the injected rename gate");
            auto wait = std::async(std::launch::async, [&] {
                return native->WaitQuiesced(std::chrono::milliseconds(20));
            });
            const auto waitStatus = wait.wait_for(std::chrono::milliseconds(200));
            RecordingWaitOutcome waitResult = RecordingWaitOutcome::Quiesced;
            if (waitStatus == std::future_status::ready) waitResult = wait.get();
            Require(!native->Push(RecorderBlock(0, media::AudioTapKind::Microphone)),
                    "audio admission must close after worker completion while rename is pending");
            renameGate->Release();
            if (waitStatus != std::future_status::ready) waitResult = wait.get();
            Require(waitStatus == std::future_status::ready &&
                        waitResult == RecordingWaitOutcome::TimedOutByCoordinator,
                    "blocked rename must return timeout within the original absolute deadline");
            const auto update = native->Poll();
            Require(update && update->kind == RecordingWorkerUpdateKind::Failed &&
                        update->error == RecordingError::FinalizeTimeout,
                    "rename deadline must publish one typed timeout winner");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Require(renameAttempts->load() == 0 && CountFiles(directory, ".mp4") == 0 &&
                        !native->Poll().has_value(),
                    "timeout winner must prevent late rename and terminal publication");
        }
        {
            const auto directory = hookRoot / "completion-authority";
            auto moveGate = std::make_shared<ManualGate>();
            auto renameAttempts = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                return RecordingError::None;
            };
            hooks->beforeRename = [moveGate] { moveGate->EnterAndWait(); };
            hooks->afterRenameAttempt = [renameAttempts] { ++*renameAttempts; };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        moveGate->WaitEntered(),
                    "coordinator must expose the post-CAS pre-MoveFile completion seam");
            auto wait = std::async(std::launch::async, [&] {
                return native->WaitQuiesced(std::chrono::milliseconds(20));
            });
            const auto waitStatus = wait.wait_for(std::chrono::milliseconds(200));
            RecordingWaitOutcome waitResult = RecordingWaitOutcome::Quiesced;
            if (waitStatus == std::future_status::ready) waitResult = wait.get();
            const bool noTimeoutPublication = !native->Poll().has_value();
            const bool noEarlyRename = CountFiles(directory, ".mp4") == 0 && renameAttempts->load() == 0;
            moveGate->Release();
            if (waitStatus != std::future_status::ready) waitResult = wait.get();
            Require(waitStatus == std::future_status::ready &&
                        waitResult == RecordingWaitOutcome::CompletionCommittedNotQuiesced &&
                        noTimeoutPublication && noEarlyRename,
                    "completion authority must remain non-stealable while Wait stays deadline-bounded");
            Require(native->WaitQuiesced(std::chrono::seconds(1)) == RecordingWaitOutcome::Quiesced,
                    "releasing a completion-owned MoveFile seam must allow normal quiescence");
            const auto update = native->Poll();
            Require(update && update->kind == RecordingWorkerUpdateKind::Completed &&
                        renameAttempts->load() == 1 && CountFiles(directory, ".mp4") == 1 &&
                        !native->Poll().has_value(),
                    "completion winner must publish exactly once after its synchronous rename finishes");
        }
        {
            auto gate=std::make_shared<ManualGate>();
            auto hooks=std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline=[gate](const std::filesystem::path& part){std::ofstream(part,std::ios::binary).put('x');gate->EnterAndWait();return RecordingError::None;};
            auto native=CreateNativeRecordingWorker({},hookRoot/"paused-admission",hooks);
            Require(native->Start({"synthetic",true,false},1,0)==RecordingError::None&&gate->WaitEntered(),"pause admission fixture reaches pipeline");
            Require(native->SetPaused(true,20'000'000),"running pipeline accepts pause");
            Require(!native->Push(RecorderBlock(40'000'000,media::AudioTapKind::Microphone)),"paused pipeline rejects PCM rather than recording silence");
            Require(native->SetPaused(false,70'000'000),"same pipeline resumes");
            Require(!native->Push(RecorderBlock(60'000'000,media::AudioTapKind::Microphone)),"late paused PCM cannot cross resume boundary");
            Require(native->Push(RecorderBlock(78'000'000,media::AudioTapKind::Microphone)),"new resumed PCM is accepted");
            native->Stop();gate->Release();
            Require(native->WaitQuiesced(std::chrono::seconds(2))==RecordingWaitOutcome::Quiesced,"paused worker remains quiesceable");
        }
        {
            const auto directory = hookRoot / "success";
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        native->WaitQuiesced(std::chrono::seconds(1)) == RecordingWaitOutcome::Quiesced,
                    "rename publication before the same deadline must succeed");
            const auto update = native->Poll();
            Require(update && update->kind == RecordingWorkerUpdateKind::Completed &&
                        CountFiles(directory, ".mp4") == 1,
                    "successful deadline interleaving must publish exactly one completed file");
        }
        for (int iteration = 0; iteration != 12; ++iteration) {
            const auto directory = hookRoot / ("terminal-cas-race-" + std::to_string(iteration));
            auto transitionBarrier = std::make_shared<std::barrier<>>(2);
            auto moveGate = std::make_shared<ManualGate>();
            auto renameAttempts = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                return RecordingError::None;
            };
            hooks->beforeRenameAuthority = [transitionBarrier] { transitionBarrier->arrive_and_wait(); };
            hooks->beforeTimeoutAuthority = [transitionBarrier] { transitionBarrier->arrive_and_wait(); };
            hooks->beforeRename = [moveGate] { moveGate->EnterAndWait(); };
            hooks->afterRenameAttempt = [renameAttempts] { ++*renameAttempts; };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", true, false}, 1, 0) == RecordingError::None,
                    "terminal CAS race worker must start");
            auto wait = std::async(std::launch::async, [&] {
                return native->WaitQuiesced(std::chrono::milliseconds(0));
            });
            const auto outcome = wait.get();
            const bool completionWon = moveGate->WaitEntered(std::chrono::milliseconds(50));
            Require(!native->Push(RecorderBlock(0, media::AudioTapKind::Microphone)),
                    "either terminal CAS winner must close admission");
            if (outcome == RecordingWaitOutcome::TimedOutByCoordinator) {
                const auto update = native->Poll();
                Require(!completionWon && update && update->error == RecordingError::FinalizeTimeout &&
                            native->WaitQuiesced(std::chrono::milliseconds(0)) ==
                                RecordingWaitOutcome::TimedOutByCoordinator &&
                            renameAttempts->load() == 0 && !native->Poll().has_value(),
                        "timeout CAS winner must remain unique and forbid rename forever");
            } else {
                Require(outcome == RecordingWaitOutcome::CompletionCommittedNotQuiesced &&
                            completionWon && !native->Poll().has_value() &&
                            native->WaitQuiesced(std::chrono::milliseconds(0)) ==
                                RecordingWaitOutcome::CompletionCommittedNotQuiesced,
                        "completion CAS winner must remain committed without fabricated timeout");
                moveGate->Release();
                Require(native->WaitQuiesced(std::chrono::seconds(1)) == RecordingWaitOutcome::Quiesced,
                        "completion race winner must resolve rename after release");
                const auto update = native->Poll();
                Require(update && update->kind == RecordingWorkerUpdateKind::Completed &&
                            renameAttempts->load() == 1 && !native->Poll().has_value(),
                        "completion CAS winner must publish one rename result");
            }
        }
        {
            const auto directory = hookRoot / "pre-pipeline-audio";
            auto authorityGate = std::make_shared<ManualGate>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->afterPipelineAuthority = [authorityGate] { authorityGate->EnterAndWait(); };
            hooks->runPipeline = [](const std::filesystem::path&) {
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", true, true}, 1, 0) == RecordingError::None &&
                        authorityGate->WaitEntered(),
                    "pre-pipeline fixture must pause while the sink is still opening");
            Require(!native->Push(RecorderBlock(0, media::AudioTapKind::Microphone)) &&
                        !native->Push(RecorderBlock(0, media::AudioTapKind::Room)),
                    "audio must not enter the bounded recording queue before the pipeline is ready");
            authorityGate->Release();
            native->Stop();
            (void)native->WaitQuiesced(std::chrono::seconds(1));
        }
        {
            const auto directory = hookRoot / "single-source-bound";
            auto pipelineGate = std::make_shared<ManualGate>();
            auto renameCalls = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [pipelineGate](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                pipelineGate->EnterAndWait();
                return RecordingError::None;
            };
            hooks->beforeRename = [renameCalls] { ++*renameCalls; };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            const auto began = std::chrono::steady_clock::now();
            Require(native->Start({"synthetic", true, true}, 1, 0) == RecordingError::None &&
                        std::chrono::steady_clock::now() - began < std::chrono::milliseconds(100),
                    "blocked pipeline setup must not block Start");
            Require(pipelineGate->WaitEntered(), "synthetic pipeline must reach its blocked stage");
            for (int index = 0; index != 25; ++index)
                Require(native->Push(RecorderBlock(index * 200'000ll, media::AudioTapKind::Microphone)),
                        "the first 25 microphone blocks must be admitted");
            Require(!native->Push(RecorderBlock(5'000'000, media::AudioTapKind::Microphone)),
                    "the 26th microphone block must fail without blocking");
            const auto stopped = std::chrono::steady_clock::now();
            native->Stop();
            Require(std::chrono::steady_clock::now() - stopped < std::chrono::milliseconds(100),
                    "Stop must remain nonblocking while a worker stage is blocked");
            Require(native->WaitQuiesced(std::chrono::milliseconds(0)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator,
                    "the coordinator timeout must win while the worker remains blocked");
            const auto timeoutUpdate = native->Poll();
            Require(timeoutUpdate && timeoutUpdate->kind == RecordingWorkerUpdateKind::Failed &&
                        timeoutUpdate->error == RecordingError::FinalizeTimeout,
                    "timeout winner must publish exactly the typed failure");
            pipelineGate->Release();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Require(renameCalls->load() == 0 && !native->Poll().has_value() &&
                        CountFiles(directory, ".part") == 1,
                    "a late worker must have no rename or terminal publication authority");
        }
        {
            const auto directory = hookRoot / "dual-source-bound";
            auto pipelineGate = std::make_shared<ManualGate>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [pipelineGate](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                pipelineGate->EnterAndWait();
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", true, true}, 1, 0) == RecordingError::None &&
                        pipelineGate->WaitEntered(), "dual-source admission hook must block");
            for (int index = 0; index != 25; ++index) {
                Require(native->Push(RecorderBlock(index * 200'000ll, media::AudioTapKind::Microphone)) &&
                            native->Push(RecorderBlock(index * 200'000ll, media::AudioTapKind::Room)),
                        "25 paired timestamps must preserve a 500 ms bound per source");
            }
            Require(!native->Push(RecorderBlock(5'000'000, media::AudioTapKind::Microphone)),
                    "the 26th microphone block must be rejected");
            Require(native->WaitQuiesced(std::chrono::milliseconds(0)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator, "dual-source timeout must win");
            pipelineGate->Release();
        }
        {
            const auto directory = hookRoot / "peer-loss-bound";
            auto pipelineGate = std::make_shared<ManualGate>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [pipelineGate](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                pipelineGate->EnterAndWait();
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", true, true}, 1, 0) == RecordingError::None &&
                        pipelineGate->WaitEntered(), "peer-loss admission hook must block");
            native->TapUnavailable(media::AudioTapKind::Room, 1);
            for (int index = 0; index != 25; ++index)
                Require(native->Push(RecorderBlock(index * 200'000ll, media::AudioTapKind::Microphone)),
                        "remaining source must retain the exact 500 ms bound");
            Require(!native->Push(RecorderBlock(5'000'000, media::AudioTapKind::Microphone)),
                    "peer loss must not extend the remaining source past 500 ms");
            Require(native->WaitQuiesced(std::chrono::milliseconds(0)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator, "peer-loss timeout must win");
            pipelineGate->Release();
        }
        {
            const auto directory = hookRoot / "imbalanced-backfill-bound";
            auto pipelineGate = std::make_shared<ManualGate>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [pipelineGate](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                pipelineGate->EnterAndWait();
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", true, true}, 1, 0) == RecordingError::None &&
                        pipelineGate->WaitEntered(), "imbalanced admission hook must block");
            for (int index = 0; index != 25; ++index)
                Require(native->Push(RecorderBlock(index * 200'000ll, media::AudioTapKind::Microphone)),
                        "25 first-source timestamps must fill its 500 ms capacity");
            for (int index = 0; index != 25; ++index)
                Require(native->Push(RecorderBlock(index * 200'000ll, media::AudioTapKind::Room)),
                        "matching peer backfill must retain its independent 500 ms capacity");
            Require(!native->Push(RecorderBlock(5'000'000, media::AudioTapKind::Room)),
                    "the 26th room block must overflow after imbalanced backfill");
            Require(native->WaitQuiesced(std::chrono::milliseconds(0)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator,
                    "imbalanced admission timeout must remain bounded");
            pipelineGate->Release();
        }
        {
            const auto directory = hookRoot / "zero-part-timeout";
            auto pipelineGate = std::make_shared<ManualGate>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [pipelineGate](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary);
                pipelineGate->EnterAndWait();
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", true, false}, 1, 0) == RecordingError::None &&
                        pipelineGate->WaitEntered() && CountFiles(directory, ".part") == 1,
                    "never-returning worker must expose its synthetic zero-byte part before timeout");
            Require(native->WaitQuiesced(std::chrono::milliseconds(20)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator &&
                        CountFiles(directory, ".part") == 0,
                    "timeout authority must best-effort delete a zero-byte part immediately");
            Require(!native->Push(RecorderBlock(0, media::AudioTapKind::Microphone)),
                    "timeout authority must close admission before a blocked worker returns");
            pipelineGate->Release();
        }
        {
            const auto directory = hookRoot / "pre-path-timeout";
            auto pathGate = std::make_shared<ManualGate>();
            auto pipelineGate = std::make_shared<ManualGate>();
            auto pipelineCalls = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->beforePathPublication = [pathGate] { pathGate->EnterAndWait(); };
            hooks->runPipeline = [pipelineGate, pipelineCalls](const std::filesystem::path& part) {
                ++*pipelineCalls;
                std::ofstream(part, std::ios::binary);
                pipelineGate->EnterAndWait();
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        pathGate->WaitEntered(),
                    "worker must expose the pre-path-publication interleaving");
            Require(native->WaitQuiesced(std::chrono::milliseconds(20)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator,
                    "timeout before path publication must remain bounded");
            const auto timeoutUpdate = native->Poll();
            Require(timeoutUpdate && timeoutUpdate->error == RecordingError::FinalizeTimeout,
                    "pre-path timeout must publish exactly one timeout result");
            pathGate->Release();
            const bool pipelineEntered = pipelineGate->WaitEntered(std::chrono::milliseconds(200));
            const bool noLatePart = pipelineCalls->load() == 0 &&
                                    CountFiles(directory, ".part") == 0;
            if (pipelineEntered) pipelineGate->Release();
            Require(!pipelineEntered && noLatePart && !native->Poll().has_value(),
                    "a timed-out worker must not start a late pipeline or create a zero-byte part");
        }
        {
            const auto directory = hookRoot / "pre-pipeline-timeout";
            auto authorityGate = std::make_shared<ManualGate>();
            auto pipelineGate = std::make_shared<ManualGate>();
            auto pipelineCalls = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->beforePipelineAuthority = [authorityGate] { authorityGate->EnterAndWait(); };
            hooks->runPipeline = [pipelineGate, pipelineCalls](const std::filesystem::path& part) {
                ++*pipelineCalls;
                std::ofstream(part, std::ios::binary);
                pipelineGate->EnterAndWait();
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        authorityGate->WaitEntered(),
                    "worker must expose the final pre-pipeline authority window");
            Require(native->WaitQuiesced(std::chrono::milliseconds(20)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator,
                    "timeout must win while pipeline start remains pending");
            const auto timeoutUpdate = native->Poll();
            authorityGate->Release();
            const bool pipelineEntered = pipelineGate->WaitEntered(std::chrono::milliseconds(200));
            const bool noLatePart = pipelineCalls->load() == 0 && CountFiles(directory, ".part") == 0;
            if (pipelineEntered) pipelineGate->Release();
            Require(timeoutUpdate && timeoutUpdate->error == RecordingError::FinalizeTimeout &&
                        !pipelineEntered && noLatePart && !native->Poll().has_value(),
                    "pending pipeline timeout winner must forbid open/file creation and late results");
        }
        {
            const auto directory = hookRoot / "timeout-before-start-cas";
            auto workerGate = std::make_shared<ManualGate>();
            auto timeoutGate = std::make_shared<ManualGate>();
            auto opens = std::make_shared<std::atomic<int>>(0);
            auto history = std::make_shared<std::vector<std::pair<OperationAuthority, OperationAuthority>>>();
            auto historyMutex = std::make_shared<std::mutex>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->beforePipelineAuthority = [workerGate] { workerGate->EnterAndWait(); };
            hooks->afterTimeoutAuthority = [timeoutGate] { timeoutGate->EnterAndWait(); };
            hooks->openPipeline = [opens](const std::filesystem::path& part) {
                ++*opens;
                std::ofstream(part, std::ios::binary);
            };
            hooks->runPipeline = [](const std::filesystem::path&) { return RecordingError::None; };
            hooks->onAuthorityTransition = [history, historyMutex](OperationAuthority from,
                                                                   OperationAuthority to) {
                std::lock_guard lock(*historyMutex);
                history->emplace_back(from, to);
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        workerGate->WaitEntered(),
                    "timeout-before-start fixture must pause before the sole start CAS");
            auto waiting = std::async(std::launch::async, [&] {
                return native->WaitQuiesced(std::chrono::milliseconds(1));
            });
            Require(timeoutGate->WaitEntered(),
                    "timeout winner must expose its post-authority state before the worker resumes");
            workerGate->Release();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            timeoutGate->Release();
            std::vector<std::pair<OperationAuthority, OperationAuthority>> observedHistory;
            {
                std::lock_guard lock(*historyMutex);
                observedHistory = *history;
            }
            Require(waiting.get() == RecordingWaitOutcome::TimedOutByCoordinator &&
                        opens->load() == 0 && CountFiles(directory, ".part") == 0 &&
                        CountFiles(directory, ".mp4") == 0 &&
                        observedHistory == std::vector<std::pair<OperationAuthority, OperationAuthority>>{
                            {OperationAuthority::PendingStart, OperationAuthority::TimedOutByCoordinator}},
                    "timeout from PendingStart must make every later Open invocation impossible");
        }
        {
            const auto directory = hookRoot / "revoked-before-terminal-race";
            auto workerGate = std::make_shared<ManualGate>();
            auto revokedGate = std::make_shared<ManualGate>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->beforePipelineAuthority = [workerGate] { workerGate->EnterAndWait(); };
            hooks->afterPipelineRevocation = [revokedGate] { revokedGate->EnterAndWait(); };
            hooks->runPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        workerGate->WaitEntered(),
                    "split-authority fixture must pause the worker before start ownership");
            auto waiting = std::async(std::launch::async, [&] {
                return native->WaitQuiesced(std::chrono::milliseconds(1));
            });
            Require(revokedGate->WaitEntered(),
                    "waiter must expose the post-revocation/pre-terminal-CAS window");
            workerGate->Release();
            const auto quiesceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!native->IsQuiesced() && std::chrono::steady_clock::now() < quiesceDeadline)
                std::this_thread::yield();
            revokedGate->Release();
            Require(waiting.get() == RecordingWaitOutcome::TimedOutByCoordinator,
                    "a successful pending-start revocation must reserve coherent timeout terminal ownership");
            const auto update = native->Poll();
            Require(update && update->kind == RecordingWorkerUpdateKind::Failed &&
                        update->error == RecordingError::FinalizeTimeout &&
                        CountFiles(directory, ".part") == 0 && CountFiles(directory, ".mp4") == 0 &&
                        !native->Poll().has_value(),
                    "revocation race must publish exactly one timeout and never open or rename");
        }
        {
            const auto directory = hookRoot / "pipeline-started-timeout";
            auto startedGate = std::make_shared<ManualGate>();
            auto openCalls = std::make_shared<std::atomic<int>>(0);
            auto pipelineCalls = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->openPipeline = [startedGate, openCalls](const std::filesystem::path& part) {
                ++*openCalls;
                std::ofstream(part, std::ios::binary);
                startedGate->EnterAndWait();
            };
            hooks->runPipeline = [pipelineCalls](const std::filesystem::path&) {
                ++*pipelineCalls;
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        startedGate->WaitEntered() && CountFiles(directory, ".part") == 1,
                    "PipelineStarted must mean the part/open commit already exists");
            Require(native->WaitQuiesced(std::chrono::milliseconds(20)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator &&
                        CountFiles(directory, ".part") == 0,
                    "timeout after PipelineStarted must clean the existing zero part without false continuation");
            const auto timeoutUpdate = native->Poll();
            startedGate->Release();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Require(timeoutUpdate && timeoutUpdate->error == RecordingError::FinalizeTimeout &&
                        openCalls->load() == 1 && pipelineCalls->load() == 0 &&
                        CountFiles(directory, ".part") == 0 &&
                        CountFiles(directory, ".mp4") == 0 && !native->Poll().has_value(),
                    "PipelineStarted timeout must not reopen a deleted part or publish a late result");
        }
        {
            const auto directory = hookRoot / "post-open-phase-timeout";
            auto phaseGate = std::make_shared<ManualGate>();
            auto pipelineCalls = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->openPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary);
            };
            hooks->afterPipelineStarted = [phaseGate] { phaseGate->EnterAndWait(); };
            hooks->runPipeline = [pipelineCalls](const std::filesystem::path&) {
                ++*pipelineCalls;
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        phaseGate->WaitEntered(),
                    "post-Open fixture must reach PipelineStarted before timeout");
            Require(native->WaitQuiesced(std::chrono::milliseconds(20)) ==
                        RecordingWaitOutcome::TimedOutByCoordinator,
                    "PipelineStarted timeout must return by the hard deadline");
            const auto timeout = native->Poll();
            phaseGate->Release();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Require(timeout && timeout->error == RecordingError::FinalizeTimeout &&
                        pipelineCalls->load() == 0 && CountFiles(directory, ".part") == 0 &&
                        CountFiles(directory, ".mp4") == 0 && !native->Poll().has_value(),
                    "post-Open timeout must abandon pipeline/finalize/rename/publication");
        }
        {
            for (int iteration = 0; iteration != 16; ++iteration) {
                const auto directory = hookRoot / ("simultaneous-start-timeout-" + std::to_string(iteration));
                auto race = std::make_shared<std::barrier<>>(2);
                auto grantGate = std::make_shared<ManualGate>();
                auto grants = std::make_shared<std::atomic<int>>(0);
                auto revocations = std::make_shared<std::atomic<int>>(0);
                auto opens = std::make_shared<std::atomic<int>>(0);
                auto pipelineCalls = std::make_shared<std::atomic<int>>(0);
                auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
                hooks->beforePipelineAuthority = [race] { race->arrive_and_wait(); };
                hooks->beforeTimeoutAuthority = [race] { race->arrive_and_wait(); };
                hooks->afterPipelineAuthority = [grantGate, grants] {
                    ++*grants;
                    grantGate->EnterAndWait();
                };
                hooks->afterPipelineRevocation = [revocations] { ++*revocations; };
                hooks->openPipeline = [opens](const std::filesystem::path& part) {
                    ++*opens;
                    { std::ofstream(part, std::ios::binary); }
                };
                hooks->runPipeline = [pipelineCalls](const std::filesystem::path&) {
                    ++*pipelineCalls;
                    return RecordingError::None;
                };
                auto native = CreateNativeRecordingWorker({}, directory, hooks);
                Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None,
                        "simultaneous start/timeout fixture must start");
                auto waiting = std::async(std::launch::async, [&] {
                    return native->WaitQuiesced(std::chrono::milliseconds(1));
                });
                Require(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
                            waiting.get() == RecordingWaitOutcome::TimedOutByCoordinator,
                        "simultaneous start/timeout race must remain bounded with timeout ownership");
                const auto authorityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (grants->load() + revocations->load() == 0 &&
                       std::chrono::steady_clock::now() < authorityDeadline) std::this_thread::yield();
                const bool grantEntered = grants->load() == 1 &&
                    grantGate->WaitEntered(std::chrono::seconds(1));
                if (grantEntered) grantGate->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const auto update = native->Poll();
                const auto late = native->Poll();
                if (!(grants->load() + revocations->load() == 1 &&
                      opens->load() == 0 && pipelineCalls->load() == 0 &&
                      update && update->error == RecordingError::FinalizeTimeout &&
                      CountFiles(directory, ".part") == 0 && CountFiles(directory, ".mp4") == 0 &&
                      !late.has_value())) {
                    throw std::runtime_error("simultaneous grant/revoke mismatch iteration=" +
                        std::to_string(iteration) + " grants=" + std::to_string(grants->load()) +
                        " revocations=" + std::to_string(revocations->load()) +
                        " opens=" + std::to_string(opens->load()) +
                        " pipeline=" + std::to_string(pipelineCalls->load()) +
                        " update=" + std::to_string(update.has_value()) +
                        " late=" + std::to_string(late.has_value()));
                }
            }
        }
        {
            for (const bool nonempty : {false, true}) {
                const auto directory = hookRoot /
                    (nonempty ? "open-failure-nonempty" : "open-failure-zero");
                auto opens = std::make_shared<std::atomic<int>>(0);
                auto pipelines = std::make_shared<std::atomic<int>>(0);
                auto renames = std::make_shared<std::atomic<int>>(0);
                auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
                hooks->openPipeline = [opens, nonempty](const std::filesystem::path& part) {
                    ++*opens;
                    std::ofstream stream(part, std::ios::binary);
                    if (nonempty) stream.write("part", 4);
                    stream.close();
                    throw std::runtime_error("injected sink open failure");
                };
                hooks->runPipeline = [pipelines](const std::filesystem::path&) {
                    ++*pipelines;
                    return RecordingError::None;
                };
                hooks->renameFile = [renames](const std::filesystem::path&,
                                               const std::filesystem::path&) {
                    ++*renames;
                    return false;
                };
                auto native = CreateNativeRecordingWorker({}, directory, hooks);
                Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                            native->WaitQuiesced(std::chrono::seconds(1)) == RecordingWaitOutcome::Quiesced,
                        "injected sink Open failure must quiesce before the deadline");
                const auto update = native->Poll();
                Require(opens->load() == 1 && pipelines->load() == 0 && renames->load() == 0 &&
                            update && update->kind == RecordingWorkerUpdateKind::Failed &&
                            update->error == RecordingError::SinkFailed &&
                            (nonempty ? IsSafeRecordingBasename(update->fileName, ".part")
                                      : update->fileName.empty()) &&
                            CountFiles(directory, ".part") == (nonempty ? 1u : 0u) &&
                            CountFiles(directory, ".mp4") == 0 && !native->Poll().has_value(),
                        "sink Open failure must publish one SinkFailed, never run/finalize/rename, and apply part policy");
            }
        }
        {
            for (const bool nonempty : {false, true}) {
                const auto directory = hookRoot /
                    (nonempty ? "timed-out-open-throw-nonempty" : "timed-out-open-throw-zero");
                auto openGate = std::make_shared<ManualGate>();
                auto pipelines = std::make_shared<std::atomic<int>>(0);
                auto renames = std::make_shared<std::atomic<int>>(0);
                auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
                hooks->openPipeline = [openGate, nonempty](const std::filesystem::path& part) {
                    std::ofstream stream(part, std::ios::binary);
                    if (nonempty) stream.write("part", 4);
                    stream.close();
                    openGate->EnterAndWait();
                    throw std::runtime_error("late injected sink open failure");
                };
                hooks->runPipeline = [pipelines](const std::filesystem::path&) {
                    ++*pipelines;
                    return RecordingError::None;
                };
                hooks->renameFile = [renames](const std::filesystem::path&,
                                               const std::filesystem::path&) {
                    ++*renames;
                    return false;
                };
                auto native = CreateNativeRecordingWorker({}, directory, hooks);
                Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                            openGate->WaitEntered(),
                        "late Open-failure fixture must block after creating its part");
                Require(native->WaitQuiesced(std::chrono::milliseconds(20)) ==
                            RecordingWaitOutcome::TimedOutByCoordinator,
                        "timeout must remain bounded while injected Open is blocked");
                const auto timeout = native->Poll();
                openGate->Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                Require(timeout && timeout->error == RecordingError::FinalizeTimeout &&
                            (nonempty ? IsSafeRecordingBasename(timeout->fileName, ".part")
                                      : timeout->fileName.empty()) &&
                            pipelines->load() == 0 && renames->load() == 0 &&
                            CountFiles(directory, ".part") == (nonempty ? 1u : 0u) &&
                            CountFiles(directory, ".mp4") == 0 && !native->Poll().has_value(),
                        "timeout-winning late Open throw must apply part policy and publish no second result");
            }
        }
        {
            const auto directory = hookRoot / "rename-collision";
            auto originalFinal = std::make_shared<std::filesystem::path>();
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [originalFinal](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).put('x');
                *originalFinal = part;
                originalFinal->replace_extension(".mp4");
                std::ofstream(*originalFinal, std::ios::binary).write("keep", 4);
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        native->WaitQuiesced(std::chrono::seconds(1)) == RecordingWaitOutcome::Quiesced,
                    "collision fixture must reach quiescence");
            const auto update = native->Poll();
            std::ifstream original(*originalFinal, std::ios::binary);
            std::string sentinel(4, '\0');
            original.read(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
            Require(update && update->kind == RecordingWorkerUpdateKind::Completed &&
                        update->fileName != originalFinal->filename().string() &&
                        IsSafeRecordingBasename(update->fileName, ".mp4") && sentinel == "keep" &&
                        CountFiles(directory, ".mp4") == 2,
                    "rename collision must select the next suffix without overwriting the existing MP4");
        }
        {
            const auto directory = hookRoot / "rename-suffixed-collision";
            std::filesystem::create_directories(directory);
            const std::string stem = "EssaStudio-20300101-010203";
            const auto base = directory / (stem + ".mp4");
            const auto lateOne = directory / (stem + "-1.mp4");
            const auto lateTwo = directory / (stem + "-2.mp4");
            std::ofstream(base, std::ios::binary).write("base", 4);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->makeRecordingStem = [stem] { return stem; };
            hooks->beforePathPublication = [lateOne, lateTwo] {
                std::ofstream(lateOne, std::ios::binary).write("one1", 4);
                std::ofstream(lateTwo, std::ios::binary).write("two2", 4);
            };
            hooks->runPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).write("new!", 4);
                return RecordingError::None;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        native->WaitQuiesced(std::chrono::seconds(1)) == RecordingWaitOutcome::Quiesced,
                    "pre-suffixed collision fixture must quiesce");
            const auto update = native->Poll();
            auto ReadFour = [](const std::filesystem::path& path) {
                std::ifstream stream(path, std::ios::binary);
                std::string bytes(4, '\0');
                stream.read(bytes.data(), 4);
                return bytes;
            };
            Require(update && update->kind == RecordingWorkerUpdateKind::Completed &&
                        update->fileName == stem + "-3.mp4" &&
                        ReadFour(base) == "base" && ReadFour(lateOne) == "one1" &&
                        ReadFour(lateTwo) == "two2" &&
                        ReadFour(directory / update->fileName) == "new!",
                    "late collisions after an initial suffix must advance one canonical numeric sequence");
        }
        {
            const auto directory = hookRoot / "rename-failure";
            auto renameAttempts = std::make_shared<std::atomic<int>>(0);
            auto hooks = std::make_shared<NativeRecordingWorkerTestHooks>();
            hooks->runPipeline = [](const std::filesystem::path& part) {
                std::ofstream(part, std::ios::binary).write("part", 4);
                return RecordingError::None;
            };
            hooks->renameFile = [renameAttempts](const std::filesystem::path&,
                                                  const std::filesystem::path&) {
                ++*renameAttempts;
                SetLastError(ERROR_ACCESS_DENIED);
                return false;
            };
            auto native = CreateNativeRecordingWorker({}, directory, hooks);
            Require(native->Start({"synthetic", false, false}, 1, 0) == RecordingError::None &&
                        native->WaitQuiesced(std::chrono::seconds(1)) == RecordingWaitOutcome::Quiesced,
                    "forced rename failure must still quiesce");
            const auto update = native->Poll();
            Require(update && update->kind == RecordingWorkerUpdateKind::Failed &&
                        update->error == RecordingError::RenameFailed &&
                        IsSafeRecordingBasename(update->fileName, ".part") &&
                        renameAttempts->load() == 1 && CountFiles(directory, ".part") == 1 &&
                        CountFiles(directory, ".mp4") == 0 &&
                        std::filesystem::file_size(directory / update->fileName) == 4 &&
                        !native->Poll().has_value(),
                    "rename failure must retain/report one validated non-empty part and no timeout/final MP4");
        }
        std::filesystem::remove_all(hookRoot, cleanupError);
        std::cout << "lecture_recorder_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        if (argc > 1) std::cerr << "recording_smoke: failed\n";
        else std::cerr << "lecture_recorder_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
