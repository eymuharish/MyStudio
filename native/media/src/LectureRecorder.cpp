#include "telestudio/media/LectureRecorder.h"

#include "telestudio/media/DesktopCaptureSource.h"
#include "telestudio/media/IVideoEncoder.h"
#include "telestudio/media/MediaFoundationMp4Sink.h"
#include "telestudio/media/RecordingAudioMixer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShlObj_core.h>
#include <KnownFolders.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <thread>
#include <utility>

namespace telestudio {

bool IsOperationAuthorityTransitionAllowed(OperationAuthority from,
                                           OperationAuthority to) noexcept {
    switch (from) {
    case OperationAuthority::PendingStart:
        return to == OperationAuthority::StartingOpen ||
               to == OperationAuthority::TimedOutByCoordinator;
    case OperationAuthority::StartingOpen:
        return to == OperationAuthority::PipelineStarted ||
               to == OperationAuthority::WorkerFinished ||
               to == OperationAuthority::TimedOutByCoordinator;
    case OperationAuthority::PipelineStarted:
        return to == OperationAuthority::WorkerFinished ||
               to == OperationAuthority::TimedOutByCoordinator;
    case OperationAuthority::WorkerFinished:
        return to == OperationAuthority::CompletedByWorker ||
               to == OperationAuthority::TimedOutByCoordinator;
    case OperationAuthority::CompletedByWorker:
    case OperationAuthority::TimedOutByCoordinator:
        return false;
    }
    return false;
}

namespace {

bool StrictUtf8ScalarCount(std::string_view value, std::size_t& count) noexcept {
    count = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 :
                             (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 0;
        if (!length || index + length > value.size()) return false;
        std::uint32_t scalar = first & (length == 1 ? 0x7f : length == 2 ? 0x1f : length == 3 ? 0x0f : 7);
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((length == 2 && scalar < 0x80) || (length == 3 && scalar < 0x800) ||
            (length == 4 && scalar < 0x10000) || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff) || scalar == 0) return false;
        ++count;
        index += length;
    }
    return true;
}

std::wstring ToWide(std::string_view value) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw std::runtime_error("invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), length) != length) throw std::runtime_error("invalid UTF-8");
    return result;
}

std::filesystem::path DefaultRecordingDirectory() {
    constexpr auto setting = L"ESSASTUDIO_RECORDING_DIRECTORY";
    SetLastError(ERROR_SUCCESS);
    const DWORD needed = GetEnvironmentVariableW(setting, nullptr, 0);
    if (needed != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        if (needed <= 1) throw std::runtime_error("recording directory is empty");
        std::wstring value(needed, L'\0');
        const DWORD copied = GetEnvironmentVariableW(setting, value.data(), needed);
        if (copied == 0 || copied >= needed)
            throw std::runtime_error("recording directory changed or is unavailable");
        value.resize(copied);
        const std::filesystem::path selected(value);
        if (!selected.is_absolute() || !std::filesystem::is_directory(selected))
            throw std::runtime_error("recording directory must be an existing absolute directory");
        HANDLE directory = CreateFileW(selected.c_str(), FILE_ADD_FILE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (directory == INVALID_HANDLE_VALUE)
            throw std::runtime_error("recording directory is not writable");
        CloseHandle(directory);
        return std::filesystem::canonical(selected);
    }
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &raw))) {
        throw std::runtime_error("videos folder unavailable");
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    path /= L"EssaStudio";
    std::filesystem::create_directories(path);
    return std::filesystem::weakly_canonical(path);
}

std::string TimestampStem() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char result[64]{};
    std::snprintf(result, sizeof(result), "MyStudio-%04u%02u%02u-%02u%02u%02u",
                  time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return result;
}

struct NativeOperation {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<OwnedCaptureFrame> video;
    std::deque<media::OwnedAudioBlock> audio;
    std::deque<media::AudioTapKind> unavailable;
    std::deque<RecordingWorkerUpdate> updates;
    std::atomic<OperationAuthority> authority{OperationAuthority::PendingStart};
    bool stop = false;
    bool paused = false;
    std::int64_t pauseStarted = 0;
    std::int64_t pausedDuration = 0;
    std::int64_t minimumInputTimestamp = 0;
    std::int64_t lastMicrophoneTimestamp = -1;
    std::int64_t lastRoomTimestamp = -1;
    std::uint64_t timelineRevision = 0;
    bool workerDone = false;
    bool quiesced = false;
    bool captureFailed = false;
    bool videoDiscontinuity = false;
    std::map<std::int64_t, std::uint8_t> audioSlots;
    std::size_t pendingMicrophoneAudio = 0;
    std::size_t pendingRoomAudio = 0;
    std::uint8_t unavailableAudio = 0;
    RecordingError workerError = RecordingError::None;
    std::filesystem::path partPath;
    std::filesystem::path finalPath;
    std::string recordingStem;
    int recordingSuffix = 0;
    RecordingOptions options;
    std::uint64_t generation = 0;
    std::int64_t epoch = 0;
};

bool AcceptsInput(OperationAuthority value) noexcept {
    return value == OperationAuthority::PendingStart ||
           value == OperationAuthority::StartingOpen ||
           value == OperationAuthority::PipelineStarted;
}

void NotifyAuthorityTransition(
    const std::shared_ptr<const NativeRecordingWorkerTestHooks>& hooks,
    OperationAuthority from, OperationAuthority to) noexcept {
    if (!hooks || !hooks->onAuthorityTransition) return;
    try { hooks->onAuthorityTransition(from, to); }
    catch (...) {}
}

bool TransitionAuthority(
    const std::shared_ptr<NativeOperation>& operation,
    OperationAuthority& expected, OperationAuthority desired,
    const std::shared_ptr<const NativeRecordingWorkerTestHooks>& hooks) noexcept {
    if (!IsOperationAuthorityTransitionAllowed(expected, desired)) return false;
    const auto from = expected;
    if (!operation->authority.compare_exchange_strong(expected, desired)) return false;
    NotifyAuthorityTransition(hooks, from, desired);
    return true;
}

class NativeRecordingWorker final : public IRecordingWorker {
public:
    static constexpr std::size_t kMaxPendingAudioBlocksPerSource = 25;
    NativeRecordingWorker(std::filesystem::path x264, std::filesystem::path output,
                          std::shared_ptr<const NativeRecordingWorkerTestHooks> hooks,
                          std::optional<media::ValidatedRecordingOutputDirectory>
                              validatedOutput)
        : x264_(std::move(x264)), output_(std::move(output)),
          hooks_(std::move(hooks)), validatedOutput_(std::move(validatedOutput)) {}

    RecordingError Start(const RecordingOptions& options, std::uint64_t generation,
                         std::int64_t epoch) noexcept override {
        try {
            std::lock_guard owner(ownerMutex_);
            if (operation_) {
                std::lock_guard operationLock(operation_->mutex);
                if (!operation_->quiesced) return RecordingError::AlreadyRecording;
            }
            if (validatedOutput_ &&
                (!validatedOutput_->ValidateBeforeUse() ||
                 validatedOutput_->Directory() != output_)) {
                return RecordingError::SinkFailed;
            }
            auto operation = std::make_shared<NativeOperation>();
            operation->options = options;
            operation->generation = generation;
            operation->epoch = epoch;
            operation_ = operation;
            std::thread([operation, x264 = x264_, output = output_, hooks = hooks_,
                         validatedOutput = validatedOutput_] {
                if (validatedOutput &&
                    !validatedOutput->ValidateBeforeUse()) {
                    std::lock_guard lock(operation->mutex);
                    operation->updates.push_back(
                        {RecordingWorkerUpdateKind::Failed,
                         RecordingError::SinkFailed, {}});
                    operation->quiesced = true;
                    operation->changed.notify_all();
                    return;
                }
                RunWorker(operation, x264, output, hooks);
            }).detach();
            std::thread([operation, hooks = hooks_] { RunCoordinator(operation, hooks); }).detach();
            return RecordingError::None;
        } catch (...) {
            return RecordingError::SinkFailed;
        }
    }

    bool Push(media::OwnedAudioBlock block) noexcept override {
        std::shared_ptr<NativeOperation> operation;
        { std::lock_guard owner(ownerMutex_); operation = operation_; }
        if (!operation) return false;
        std::lock_guard lock(operation->mutex);
        const auto sourceMask = AudioSourceMask(block.source);
        const bool expected = (block.source == media::AudioTapKind::Microphone &&
                               operation->options.includeMicrophone) ||
                              (block.source == media::AudioTapKind::Room &&
                               operation->options.includeRoomAudio);
        const bool valid = expected && block.callGeneration == operation->generation &&
                           block.channels == 1 &&
                           (block.sampleRateHz == 8000 || block.sampleRateHz == 16000 ||
                            block.sampleRateHz == 32000 || block.sampleRateHz == 48000) &&
                           block.duration100ns == 200'000 &&
                           block.samples.size() == static_cast<std::size_t>(block.sampleRateHz / 50);
        if (!valid || operation->paused || block.timestamp100ns < operation->minimumInputTimestamp || operation->stop || operation->workerDone || operation->quiesced ||
            operation->authority.load() != OperationAuthority::PipelineStarted ||
            (operation->unavailableAudio & sourceMask) != 0) {
            return false;
        }
        const auto relative = std::max<std::int64_t>(0, block.timestamp100ns-operation->epoch-operation->pausedDuration);
        block.timestamp100ns = operation->epoch + (relative/200'000)*200'000;
        auto& lastTimestamp = block.source == media::AudioTapKind::Microphone ? operation->lastMicrophoneTimestamp : operation->lastRoomTimestamp;
        if(block.timestamp100ns<=lastTimestamp)return false;
        lastTimestamp=block.timestamp100ns;
        auto slot = operation->audioSlots.find(block.timestamp100ns);
        if (slot != operation->audioSlots.end() && (slot->second & sourceMask) != 0) return false;
        auto& pendingSource = block.source == media::AudioTapKind::Microphone
            ? operation->pendingMicrophoneAudio : operation->pendingRoomAudio;
        if (pendingSource >= kMaxPendingAudioBlocksPerSource) {
            operation->workerError = RecordingError::AudioOverflow;
            operation->stop = true;
            operation->changed.notify_all();
            return false;
        }
        if (slot == operation->audioSlots.end())
            operation->audioSlots.emplace(block.timestamp100ns, sourceMask);
        else
            slot->second |= sourceMask;
        ++pendingSource;
        operation->audio.push_back(std::move(block));
        operation->changed.notify_all();
        return true;
    }

    void TapUnavailable(media::AudioTapKind source, std::uint64_t generation) noexcept override {
        std::shared_ptr<NativeOperation> operation;
        { std::lock_guard owner(ownerMutex_); operation = operation_; }
        if (!operation) return;
        std::lock_guard lock(operation->mutex);
        if (operation->generation != generation || operation->stop || operation->workerDone ||
            !AcceptsInput(operation->authority.load())) return;
        const auto sourceMask = AudioSourceMask(source);
        if ((operation->unavailableAudio & sourceMask) != 0) return;
        operation->unavailableAudio |= sourceMask;
        operation->unavailable.push_back(source);
        operation->changed.notify_all();
    }

    bool SetPaused(bool paused, std::int64_t now) noexcept override {
        std::shared_ptr<NativeOperation> operation;
        { std::lock_guard owner(ownerMutex_); operation=operation_; }
        if(!operation)return false;
        std::lock_guard lock(operation->mutex);
        if(operation->stop||operation->workerDone||operation->quiesced||operation->authority.load()!=OperationAuthority::PipelineStarted)return false;
        if(operation->paused==paused)return true;
        if(paused)operation->pauseStarted=now;
        else {operation->pausedDuration+=std::max<std::int64_t>(0,now-operation->pauseStarted);operation->minimumInputTimestamp=now;}
        operation->paused=paused;
        operation->video.clear();operation->audio.clear();operation->audioSlots.clear();
        operation->pendingMicrophoneAudio=operation->pendingRoomAudio=0;
        operation->videoDiscontinuity=true;++operation->timelineRevision;
        operation->changed.notify_all();return true;
    }

    void Stop() noexcept override {
        std::shared_ptr<NativeOperation> operation;
        { std::lock_guard owner(ownerMutex_); operation = operation_; }
        if (!operation) return;
        std::lock_guard lock(operation->mutex);
        operation->stop = true;
        operation->changed.notify_all();
    }

    std::optional<RecordingWorkerUpdate> Poll() noexcept override {
        std::shared_ptr<NativeOperation> operation;
        { std::lock_guard owner(ownerMutex_); operation = operation_; }
        if (!operation) return std::nullopt;
        std::lock_guard lock(operation->mutex);
        if (operation->updates.empty()) return std::nullopt;
        auto result = std::move(operation->updates.front());
        operation->updates.pop_front();
        return result;
    }

    RecordingWaitOutcome WaitQuiesced(std::chrono::milliseconds timeout) noexcept override {
        std::shared_ptr<NativeOperation> operation;
        { std::lock_guard owner(ownerMutex_); operation = operation_; }
        if (!operation) return RecordingWaitOutcome::Quiesced;
        std::unique_lock lock(operation->mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
        if (operation->changed.wait_until(lock, deadline, [&] { return operation->quiesced; }))
            return RecordingWaitOutcome::Quiesced;
        lock.unlock();
        auto observed = operation->authority.load();
        if (observed != OperationAuthority::CompletedByWorker &&
            observed != OperationAuthority::TimedOutByCoordinator &&
            hooks_ && hooks_->beforeTimeoutAuthority) {
            try { hooks_->beforeTimeoutAuthority(); }
            catch (...) {}
        }
        bool timeoutWon = false;
        OperationAuthority timedOutFrom = observed;
        lock.lock();
        while (observed != OperationAuthority::CompletedByWorker &&
               observed != OperationAuthority::TimedOutByCoordinator) {
            timedOutFrom = observed;
            if (TransitionAuthority(operation, observed, OperationAuthority::TimedOutByCoordinator,
                                    hooks_)) {
                timeoutWon = true;
                break;
            }
        }
        lock.unlock();
        if (timeoutWon) {
            if (hooks_ && hooks_->afterTimeoutAuthority) {
                try { hooks_->afterTimeoutAuthority(); }
                catch (...) {}
            }
            if (timedOutFrom == OperationAuthority::PendingStart &&
                hooks_ && hooks_->afterPipelineRevocation) {
                try { hooks_->afterPipelineRevocation(); }
                catch (...) {}
            }
            lock.lock();
            operation->stop = true;
            CleanupZeroBytePart(*operation);
            operation->updates.push_back({RecordingWorkerUpdateKind::Failed,
                                          RecordingError::FinalizeTimeout,
                                          RecoverablePart(*operation)});
            operation->changed.notify_all();
            return RecordingWaitOutcome::TimedOutByCoordinator;
        }
        if (observed == OperationAuthority::CompletedByWorker) {
            lock.lock();
            if (operation->quiesced) return RecordingWaitOutcome::Quiesced;
        }
        return observed == OperationAuthority::CompletedByWorker
            ? RecordingWaitOutcome::CompletionCommittedNotQuiesced
            : RecordingWaitOutcome::TimedOutByCoordinator;
    }

    bool IsQuiesced() const noexcept override {
        std::shared_ptr<NativeOperation> operation;
        { std::lock_guard owner(ownerMutex_); operation = operation_; }
        if (!operation) return true;
        std::lock_guard lock(operation->mutex);
        return operation->quiesced;
    }

private:
    mutable std::mutex ownerMutex_;
    std::shared_ptr<NativeOperation> operation_;
    std::filesystem::path x264_;
    std::filesystem::path output_;
    std::shared_ptr<const NativeRecordingWorkerTestHooks> hooks_;
    std::optional<media::ValidatedRecordingOutputDirectory> validatedOutput_;

    static constexpr std::uint8_t AudioSourceMask(media::AudioTapKind source) noexcept {
        return source == media::AudioTapKind::Microphone ? 1u : 2u;
    }

    static void ReleaseAudioSlotLocked(NativeOperation& operation,
                                       std::int64_t timestamp100ns) noexcept {
        const auto slot = operation.audioSlots.find(timestamp100ns);
        if (slot == operation.audioSlots.end()) return;
        if ((slot->second & 1u) != 0 && operation.pendingMicrophoneAudio != 0)
            --operation.pendingMicrophoneAudio;
        if ((slot->second & 2u) != 0 && operation.pendingRoomAudio != 0)
            --operation.pendingRoomAudio;
        operation.audioSlots.erase(slot);
    }

    static void CleanupZeroBytePart(const NativeOperation& operation) noexcept {
        try {
            if (!operation.partPath.empty() && std::filesystem::exists(operation.partPath) &&
                std::filesystem::file_size(operation.partPath) == 0) {
                std::filesystem::remove(operation.partPath);
            }
        } catch (...) {}
    }

    static std::string RecoverablePart(const NativeOperation& operation) noexcept {
        try {
            if (std::filesystem::exists(operation.partPath) && std::filesystem::file_size(operation.partPath) > 0) {
                const auto name = operation.partPath.filename().u8string();
                const std::string result(name.begin(), name.end());
                return IsSafeRecordingBasename(result, ".part") ? result : std::string{};
            }
        } catch (...) {}
        return {};
    }

    static void RunWorker(const std::shared_ptr<NativeOperation>& operation,
                          const std::filesystem::path& configuredX264,
                          const std::filesystem::path& configuredOutput,
                          const std::shared_ptr<const NativeRecordingWorkerTestHooks>& hooks) noexcept {
        RecordingError error = RecordingError::None;
        try {
            if ((!hooks || !hooks->runPipeline) &&
                !DesktopCaptureSource::ValidateCaptureItem(operation->options.sourceId)) {
                error = RecordingError::InvalidTarget;
                throw std::runtime_error("invalid target");
            }
            const auto directory = configuredOutput.empty() ? DefaultRecordingDirectory() : configuredOutput;
            std::filesystem::create_directories(directory);
            const std::string stem = hooks && hooks->makeRecordingStem
                ? hooks->makeRecordingStem() : TimestampStem();
            for (int suffix = 0; suffix != 10000; ++suffix) {
                const std::string candidate = stem + (suffix == 0 ? "" : "-" + std::to_string(suffix));
                const auto part = directory / ToWide(candidate + ".part");
                const auto final = directory / ToWide(candidate + ".mp4");
                if (!std::filesystem::exists(part) && !std::filesystem::exists(final)) {
                    if (hooks && hooks->beforePathPublication) hooks->beforePathPublication();
                    std::lock_guard lock(operation->mutex);
                    operation->partPath = part;
                    operation->finalPath = final;
                    operation->recordingStem = stem;
                    operation->recordingSuffix = suffix;
                    break;
                }
            }
            if (operation->partPath.empty()) {
                error = RecordingError::RenameFailed;
                throw std::runtime_error("no final candidate");
            }
            if (hooks && hooks->runPipeline) {
                if (hooks->beforePipelineAuthority) hooks->beforePipelineAuthority();
                OperationAuthority expectedStart = OperationAuthority::PendingStart;
                if (!TransitionAuthority(operation, expectedStart, OperationAuthority::StartingOpen,
                                         hooks)) {
                    error = RecordingError::FinalizeTimeout;
                    throw std::runtime_error("pipeline start authority unavailable");
                }
                if (hooks->afterPipelineAuthority) hooks->afterPipelineAuthority();
                if (operation->authority.load() != OperationAuthority::StartingOpen) {
                    error = RecordingError::FinalizeTimeout;
                    throw std::runtime_error("pipeline timed out before open");
                }
                if (hooks->openPipeline) {
                    hooks->openPipeline(operation->partPath);
                } else {
                    std::ofstream placeholder(operation->partPath, std::ios::binary);
                    if (!placeholder) throw std::runtime_error("synthetic pipeline open failed");
                }
                OperationAuthority expectedOpen = OperationAuthority::StartingOpen;
                if (!TransitionAuthority(operation, expectedOpen, OperationAuthority::PipelineStarted,
                                         hooks)) {
                    CleanupZeroBytePart(*operation);
                    error = RecordingError::FinalizeTimeout;
                    throw std::runtime_error("pipeline open completed after timeout");
                }
                if (hooks->afterPipelineStarted) hooks->afterPipelineStarted();
                if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                    CleanupZeroBytePart(*operation);
                    error = RecordingError::FinalizeTimeout;
                    throw std::runtime_error("pipeline timed out before execution");
                }
                error = hooks->runPipeline(operation->partPath);
                if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                    CleanupZeroBytePart(*operation);
                    error = RecordingError::FinalizeTimeout;
                    throw std::runtime_error("pipeline completed after timeout");
                }
                std::lock_guard lock(operation->mutex);
                if (operation->workerError != RecordingError::None) error = operation->workerError;
            } else {
            DesktopCaptureSource capture;
            if (hooks && hooks->beforePipelineAuthority) hooks->beforePipelineAuthority();
            OperationAuthority expectedStart = OperationAuthority::PendingStart;
            if (!TransitionAuthority(operation, expectedStart, OperationAuthority::StartingOpen,
                                     hooks)) {
                error = RecordingError::FinalizeTimeout;
                throw std::runtime_error("pipeline start authority unavailable");
            }
            if (hooks && hooks->afterPipelineAuthority) hooks->afterPipelineAuthority();
            if (operation->authority.load() != OperationAuthority::StartingOpen) {
                error = RecordingError::FinalizeTimeout;
                throw std::runtime_error("capture start after timeout");
            }
            capture.StartOwned(operation->options.sourceId, 30, 1280, 720,
                [operation](OwnedCaptureFrame frame) {
                    std::lock_guard lock(operation->mutex);
                    if (operation->stop || operation->workerDone || operation->paused || frame.timestamp100ns < operation->minimumInputTimestamp) return;
                    frame.timestamp100ns=std::max(operation->epoch,frame.timestamp100ns-operation->pausedDuration);
                    if (operation->video.size() == 3) {
                        operation->video.pop_front();
                        operation->videoDiscontinuity = true;
                    }
                    operation->video.push_back(std::move(frame));
                    operation->changed.notify_all();
                },
                [operation](const std::string&) {
                    std::lock_guard lock(operation->mutex);
                    operation->captureFailed = true;
                    operation->stop = true;
                    operation->changed.notify_all();
                });

            std::filesystem::path x264 = configuredX264;
#ifdef TELESTUDIO_CODEC_BIN_DIR
            if (x264.empty()) x264 = std::filesystem::path(TELESTUDIO_CODEC_BIN_DIR) / L"libx264-165.dll";
#endif
            error = RecordingError::EncoderFailed;
            auto encoder = CreateX264Encoder(x264);
            EncoderSettings encoderSettings{VideoCodec::H264, 1280, 720, 30, 4000, true, 60};
            encoder->Open(encoderSettings);
            RecordingAudioMixer mixer(operation->options.includeMicrophone,
                                      operation->options.includeRoomAudio,
                                      operation->generation, operation->epoch);
            MediaFoundationMp4Sink sink;

            OwnedCaptureFrame first;
            {
                std::unique_lock lock(operation->mutex);
                if (!operation->changed.wait_for(lock, std::chrono::seconds(5), [&] {
                        return !operation->video.empty() || operation->captureFailed || operation->stop;
                    }) || operation->video.empty()) {
                    error = RecordingError::CaptureFailed;
                    throw std::runtime_error("capture bootstrap failed");
                }
                first = std::move(operation->video.front()); operation->video.pop_front();
            }
            auto bootstrap = encoder->Encode(first.pixels->View(), true);
            const auto parsed = ParseH264Bootstrap(bootstrap.annexB);
            if (!parsed.valid || parsed.profileIdc != 100) throw std::runtime_error("invalid x264 bootstrap");
            RecordingVideoFormat videoFormat{1280, 720, 30, 4000, true, parsed.sequenceHeader};
            error = RecordingError::SinkFailed;
            if (operation->authority.load() != OperationAuthority::StartingOpen) {
                error = RecordingError::FinalizeTimeout;
                throw std::runtime_error("sink open started after timeout");
            }
            sink.Open(operation->partPath, videoFormat,
                      operation->options.includeMicrophone || operation->options.includeRoomAudio);
            OperationAuthority expectedOpen = OperationAuthority::StartingOpen;
            if (!TransitionAuthority(operation, expectedOpen, OperationAuthority::PipelineStarted,
                                     hooks)) {
                error = RecordingError::FinalizeTimeout;
                throw std::runtime_error("sink open completed after timeout");
            }
            if (hooks && hooks->afterPipelineStarted) hooks->afterPipelineStarted();
            const auto firstTime = first.timestamp100ns <= operation->epoch ? 0 : first.timestamp100ns - operation->epoch;
            if (hooks && hooks->beforeVideoWrite) hooks->beforeVideoWrite();
            if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                error = RecordingError::FinalizeTimeout;
                throw std::runtime_error("first video write after timeout");
            }
            sink.WriteVideo({std::move(bootstrap.annexB), firstTime, 333'333, true, false});
            error = RecordingError::None;
            {
                std::lock_guard lock(operation->mutex);
                if (operation->authority.load() == OperationAuthority::PipelineStarted) {
                    operation->updates.push_back({RecordingWorkerUpdateKind::Ready});
                    operation->changed.notify_all();
                }
            }

            std::uint64_t timelineRevision=0;
            while (true) {
                OwnedCaptureFrame frame;
                std::vector<media::OwnedAudioBlock> audioBatch;
                std::optional<media::AudioTapKind> unavailable;
                bool discontinuity = false;
                bool shouldStop = false;
                bool resetMixer = false;
                {
                    std::unique_lock lock(operation->mutex);
                    operation->changed.wait_for(lock, std::chrono::milliseconds(20), [&] {
                        return operation->stop || !operation->video.empty() || !operation->audio.empty() ||
                               !operation->unavailable.empty();
                    });
                    if (!operation->video.empty()) { frame = std::move(operation->video.front()); operation->video.pop_front(); }
                    while (!operation->audio.empty()) {
                        const auto inputTimestamp = operation->audio.front().timestamp100ns;
                        audioBatch.push_back(std::move(operation->audio.front()));
                        operation->audio.pop_front();
                        ReleaseAudioSlotLocked(*operation, inputTimestamp);
                    }
                    if (!operation->unavailable.empty()) {
                        unavailable = operation->unavailable.front();
                        operation->unavailable.pop_front();
                    }
                    if(timelineRevision!=operation->timelineRevision){timelineRevision=operation->timelineRevision;resetMixer=true;}
                    discontinuity = frame.pixels ? std::exchange(operation->videoDiscontinuity, false) : false;
                    shouldStop = operation->stop && operation->video.empty() && operation->audio.empty();
                }
                if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                    error = RecordingError::FinalizeTimeout;
                    break;
                }
                if(resetMixer)mixer.ResetTimeline();
                if (frame.pixels) {
                    auto encoded = encoder->Encode(frame.pixels->View(), discontinuity);
                    const auto time = frame.timestamp100ns <= operation->epoch ? 0 : frame.timestamp100ns - operation->epoch;
                    if (hooks && hooks->beforeVideoWrite) hooks->beforeVideoWrite();
                    if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                        error = RecordingError::FinalizeTimeout;
                        break;
                    }
                    sink.WriteVideo({std::move(encoded.annexB), time, 333'333, encoded.keyFrame, discontinuity});
                }
                bool audioFailed = false;
                for (auto& audio : audioBatch) {
                    auto mixed = mixer.Push(std::move(audio));
                    if (mixed.status == AudioMixStatus::Overflow || mixed.status == AudioMixStatus::TimingLost) {
                        error = mixed.status == AudioMixStatus::Overflow ? RecordingError::AudioOverflow
                                                                        : RecordingError::SinkFailed;
                        shouldStop = true;
                        audioFailed = true;
                        break;
                    }
                    for (auto& sample : mixed.blocks) {
                        if (hooks && hooks->beforeAudioWrite) hooks->beforeAudioWrite();
                        if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                            error = RecordingError::FinalizeTimeout;
                            shouldStop = true;
                            audioFailed = true;
                            break;
                        }
                        sink.WriteAudio({std::move(sample.samples), sample.timestamp100ns,
                                         sample.duration100ns, sample.discontinuity});
                    }
                    if (error == RecordingError::FinalizeTimeout) {
                        audioFailed = true;
                        break;
                    }
                    {
                        std::lock_guard lock(operation->mutex);
                        for (const auto timestamp : mixed.releasedInputTimestamps100ns)
                            ReleaseAudioSlotLocked(*operation, timestamp);
                        operation->changed.notify_all();
                    }
                }
                if (audioFailed) break;
                if (unavailable) {
                    auto mixed = mixer.MarkUnavailable(*unavailable);
                    {
                        std::lock_guard lock(operation->mutex);
                        if (operation->authority.load() == OperationAuthority::PipelineStarted) {
                        operation->updates.push_back({RecordingWorkerUpdateKind::Warning,
                            *unavailable == media::AudioTapKind::Microphone
                                ? RecordingError::MicrophoneUnavailable : RecordingError::RoomAudioUnavailable, {}});
                        }
                    }
                    for (auto& sample : mixed.blocks) {
                        if (hooks && hooks->beforeAudioWrite) hooks->beforeAudioWrite();
                        if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                            error = RecordingError::FinalizeTimeout;
                            shouldStop = true;
                            break;
                        }
                        sink.WriteAudio({std::move(sample.samples), sample.timestamp100ns,
                                         sample.duration100ns, sample.discontinuity});
                    }
                    if (error == RecordingError::FinalizeTimeout) break;
                    {
                        std::lock_guard lock(operation->mutex);
                        for (const auto timestamp : mixed.releasedInputTimestamps100ns)
                            ReleaseAudioSlotLocked(*operation, timestamp);
                        operation->changed.notify_all();
                    }
                }
                if (shouldStop) {
                    std::vector<MixedAudioBlock> retained;
                    std::vector<std::int64_t> released;
                    if (operation->options.includeMicrophone) {
                        auto drained = mixer.MarkUnavailable(media::AudioTapKind::Microphone);
                        for (auto& block : drained.blocks) retained.push_back(std::move(block));
                        released.insert(released.end(), drained.releasedInputTimestamps100ns.begin(),
                                        drained.releasedInputTimestamps100ns.end());
                    }
                    if (operation->options.includeRoomAudio) {
                        auto drained = mixer.MarkUnavailable(media::AudioTapKind::Room);
                        for (auto& block : drained.blocks) retained.push_back(std::move(block));
                        released.insert(released.end(), drained.releasedInputTimestamps100ns.begin(),
                                        drained.releasedInputTimestamps100ns.end());
                    }
                    std::sort(retained.begin(), retained.end(), [](const auto& left, const auto& right) {
                        return left.timestamp100ns < right.timestamp100ns;
                    });
                    for (auto& sample : retained) {
                        if (hooks && hooks->beforeAudioWrite) hooks->beforeAudioWrite();
                        if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                            error = RecordingError::FinalizeTimeout;
                            break;
                        }
                        sink.WriteAudio({std::move(sample.samples), sample.timestamp100ns,
                                         sample.duration100ns, sample.discontinuity});
                    }
                    if (error == RecordingError::FinalizeTimeout) break;
                    {
                        std::lock_guard lock(operation->mutex);
                        for (const auto timestamp : released) operation->audioSlots.erase(timestamp);
                        operation->changed.notify_all();
                    }
                    break;
                }
            }
            capture.Stop();
            {
                std::lock_guard lock(operation->mutex);
                if (operation->captureFailed) error = RecordingError::CaptureFailed;
                if (operation->workerError != RecordingError::None) error = operation->workerError;
            }
            if (error == RecordingError::None &&
                operation->authority.load() == OperationAuthority::PipelineStarted) {
                if (hooks && hooks->beforeFinalize) hooks->beforeFinalize();
                if (operation->authority.load() != OperationAuthority::PipelineStarted) {
                    error = RecordingError::FinalizeTimeout;
                } else {
                error = RecordingError::FinalizeFailed;
                sink.Finalize();
                error = RecordingError::None;
                }
            }
            }
        } catch (...) {
            if (error == RecordingError::None) error = RecordingError::SinkFailed;
        }
        {
            std::lock_guard lock(operation->mutex);
            operation->stop = true;
            operation->audio.clear();
            operation->audioSlots.clear();
            operation->video.clear();
            operation->workerError = error;
        }
        auto phase = operation->authority.load();
        if (phase == OperationAuthority::PendingStart) {
            TransitionAuthority(operation, phase, OperationAuthority::StartingOpen, hooks);
            phase = operation->authority.load();
        }
        if (phase == OperationAuthority::StartingOpen ||
            phase == OperationAuthority::PipelineStarted) {
            TransitionAuthority(operation, phase, OperationAuthority::WorkerFinished, hooks);
        } else if (phase == OperationAuthority::TimedOutByCoordinator) {
            CleanupZeroBytePart(*operation);
        }
        {
            std::lock_guard lock(operation->mutex);
            operation->workerDone = true;
            operation->changed.notify_all();
        }
    }

    static void RunCoordinator(
        const std::shared_ptr<NativeOperation>& operation,
        const std::shared_ptr<const NativeRecordingWorkerTestHooks>& hooks) noexcept {
        {
            std::unique_lock lock(operation->mutex);
            operation->changed.wait(lock, [&] { return operation->workerDone; });
        }
        RecordingError workerError;
        {
            std::lock_guard lock(operation->mutex);
            workerError = operation->workerError;
        }
        if (hooks && hooks->beforeRenameAuthority) {
            try { hooks->beforeRenameAuthority(); }
            catch (...) { workerError = RecordingError::RenameFailed; }
        }
        OperationAuthority expected = OperationAuthority::WorkerFinished;
        if (!TransitionAuthority(operation, expected, OperationAuthority::CompletedByWorker, hooks)) {
            CleanupZeroBytePart(*operation);
            return;
        }
        RecordingWorkerUpdate update;
        if (workerError == RecordingError::None) {
            int chosenSuffix = operation->recordingSuffix;
            auto finalForSuffix = [&](int suffix) {
                const auto candidate = operation->recordingStem +
                    (suffix == 0 ? "" : "-" + std::to_string(suffix));
                return operation->finalPath.parent_path() / ToWide(candidate + ".mp4");
            };
            std::filesystem::path chosen = finalForSuffix(chosenSuffix);
            bool renamed = false;
            bool renameHookFailed = false;
            if (hooks && hooks->beforeRename) {
                try { hooks->beforeRename(); }
                catch (...) { renameHookFailed = true; }
            }
            for (int suffix = 0; suffix != 10000; ++suffix) {
                if (renameHookFailed) break;
                const bool moved = hooks && hooks->renameFile
                    ? hooks->renameFile(operation->partPath, chosen)
                    : MoveFileExW(operation->partPath.c_str(), chosen.c_str(), 0) != FALSE;
                if (hooks && hooks->afterRenameAttempt) {
                    try { hooks->afterRenameAttempt(); }
                    catch (...) { renameHookFailed = true; }
                }
                if (moved) { renamed = true; break; }
                if (renameHookFailed) break;
                if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
                chosen = finalForSuffix(++chosenSuffix);
            }
            if (renamed) {
                const auto name = chosen.filename().u8string();
                const std::string basename(name.begin(), name.end());
                update = IsSafeRecordingBasename(basename, ".mp4")
                    ? RecordingWorkerUpdate{RecordingWorkerUpdateKind::Completed, RecordingError::None, basename}
                    : RecordingWorkerUpdate{RecordingWorkerUpdateKind::Failed, RecordingError::RenameFailed,
                                            RecoverablePart(*operation)};
            } else {
                update = {RecordingWorkerUpdateKind::Failed, RecordingError::RenameFailed, RecoverablePart(*operation)};
            }
        } else {
            CleanupZeroBytePart(*operation);
            update = {RecordingWorkerUpdateKind::Failed, workerError, RecoverablePart(*operation)};
        }
        std::lock_guard lock(operation->mutex);
        operation->updates.push_back(std::move(update));
        operation->quiesced = true;
        operation->changed.notify_all();
    }
};
}

bool IsSafeRecordingBasename(std::string_view value, std::string_view extension) {
    if (value.empty() || value == "." || value == ".." || value.back() == ' ' || value.back() == '.' ||
        value.find_first_of("/\\:*?\"<>|") != std::string_view::npos) return false;
    std::size_t scalars = 0;
    if (!StrictUtf8ScalarCount(value, scalars) || scalars > 255) return false;
    for (const unsigned char ch : value) if (ch < 0x20 || ch == 0x7f) return false;
    if (!extension.empty() && (value.size() < extension.size() ||
        !std::equal(extension.rbegin(), extension.rend(), value.rbegin(),
                    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                               std::tolower(static_cast<unsigned char>(b)); }))) return false;
    const auto dot = value.find('.');
    std::string stem(value.substr(0, dot));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        stem[3] >= '1' && stem[3] <= '9') return false;
    return true;
}

std::shared_ptr<IRecordingWorker> CreateNativeRecordingWorker(std::filesystem::path x264,
                                                               std::filesystem::path output,
                                                               std::shared_ptr<const NativeRecordingWorkerTestHooks> hooks,
                                                               std::optional<media::ValidatedRecordingOutputDirectory> validatedOutput) {
    return std::make_shared<NativeRecordingWorker>(std::move(x264), std::move(output),
                                                    std::move(hooks),
                                                    std::move(validatedOutput));
}

LectureRecorder::LectureRecorder(std::shared_ptr<IRecordingWorker> worker,
                                 std::shared_ptr<media::IMonotonicClock> clock,
                                 TapReadiness readiness)
    : worker_(std::move(worker)), clock_(std::move(clock)), readiness_(std::move(readiness)) {
    if (!worker_ || !clock_ || !readiness_) throw std::invalid_argument("recorder dependencies required");
}

LectureRecorder::~LectureRecorder() { (void)Stop(); }

void LectureRecorder::PollLocked() noexcept {
    while (const auto update = worker_->Poll()) {
        if (update->kind == RecordingWorkerUpdateKind::Ready && snapshot_.state == RecordingState::Starting) {
            snapshot_.state = RecordingState::Recording;
            notifications_.push_back({RecordingNotificationKind::State, RecordingError::None, snapshot_});
        } else if (update->kind == RecordingWorkerUpdateKind::Completed &&
                   snapshot_.state == RecordingState::Finalizing &&
                   IsSafeRecordingBasename(update->fileName, ".mp4")) {
            snapshot_.state = RecordingState::Completed;
            snapshot_.paused = false;
            snapshot_.fileName = update->fileName;
            notifications_.push_back({RecordingNotificationKind::State, RecordingError::None, snapshot_});
        } else if (update->kind == RecordingWorkerUpdateKind::Failed &&
                   snapshot_.state != RecordingState::Idle && snapshot_.state != RecordingState::Completed) {
            lastError_ = update->error;
            snapshot_.state = RecordingState::Failed;
            snapshot_.paused = false;
            snapshot_.fileName = IsSafeRecordingBasename(update->fileName, ".part") ? update->fileName : "";
            notifications_.push_back({RecordingNotificationKind::Error, update->error, snapshot_});
        } else if (update->kind == RecordingWorkerUpdateKind::Warning) {
            notifications_.push_back({RecordingNotificationKind::Warning, update->error, snapshot_});
        }
    }
    if (snapshot_.state == RecordingState::Recording) {
        const auto now = snapshot_.paused ? pauseStarted_ : clock_->Now100ns();
        const auto elapsed = now > epoch_+pausedDuration_ ? (now - epoch_-pausedDuration_) / 10'000 : 0;
        lastElapsed_ = std::max(lastElapsed_, elapsed);
        snapshot_.elapsedMs = lastElapsed_;
        if(!snapshot_.paused&&lastElapsed_-notifiedElapsed_>=1000){notifiedElapsed_=lastElapsed_;notifications_.push_back({RecordingNotificationKind::State,RecordingError::None,snapshot_});}
    }
}

RecordingResult LectureRecorder::ResultLocked(RecordingError error) const {
    RecordingResult result;
    static_cast<RecordingSnapshot&>(result) = snapshot_;
    result.error = error == RecordingError::None && snapshot_.state == RecordingState::Failed
        ? lastError_ : error;
    return result;
}

RecordingResult LectureRecorder::Start(RecordingOptions options) noexcept {
    std::lock_guard lock(mutex_);
    PollLocked();
    if ((snapshot_.state == RecordingState::Starting || snapshot_.state == RecordingState::Recording ||
         snapshot_.state == RecordingState::Finalizing) || !worker_->IsQuiesced()) {
        return ResultLocked(RecordingError::AlreadyRecording);
    }
    if (options.sourceId.empty()) return ResultLocked(RecordingError::InvalidTarget);
    if (options.includeMicrophone && !readiness_(media::AudioTapKind::Microphone))
        return ResultLocked(RecordingError::MicrophoneUnavailable);
    if (options.includeRoomAudio && !readiness_(media::AudioTapKind::Room))
        return ResultLocked(RecordingError::RoomAudioUnavailable);
    epoch_ = clock_->Now100ns();
    const auto generation = ++generation_;
    const RecordingError error = worker_->Start(options, generation, epoch_);
    if (error != RecordingError::None) return ResultLocked(error);
    snapshot_ = {RecordingState::Starting, options.includeMicrophone, options.includeRoomAudio, 0, {}};
    lastElapsed_ = 0;
    pauseStarted_=pausedDuration_=notifiedElapsed_=0;
    lastError_ = RecordingError::None;
    notifications_.push_back({RecordingNotificationKind::State, RecordingError::None, snapshot_});
    return ResultLocked();
}

RecordingResult LectureRecorder::PushMicrophone(media::OwnedAudioBlock block) noexcept {
    std::lock_guard lock(mutex_); PollLocked();
    if ((snapshot_.state == RecordingState::Starting || snapshot_.state == RecordingState::Recording) &&
        snapshot_.micIncluded && !snapshot_.paused) worker_->Push(std::move(block));
    return ResultLocked();
}

RecordingResult LectureRecorder::PushRoomAudio(media::OwnedAudioBlock block) noexcept {
    std::lock_guard lock(mutex_); PollLocked();
    if ((snapshot_.state == RecordingState::Starting || snapshot_.state == RecordingState::Recording) &&
        snapshot_.roomIncluded && !snapshot_.paused) worker_->Push(std::move(block));
    return ResultLocked();
}

RecordingResult LectureRecorder::MicrophoneUnavailable() noexcept {
    std::lock_guard lock(mutex_); PollLocked();
    if ((snapshot_.state == RecordingState::Starting || snapshot_.state == RecordingState::Recording) &&
        snapshot_.micIncluded) worker_->TapUnavailable(media::AudioTapKind::Microphone, generation_);
    return ResultLocked();
}

RecordingResult LectureRecorder::RoomAudioUnavailable() noexcept {
    std::lock_guard lock(mutex_); PollLocked();
    if ((snapshot_.state == RecordingState::Starting || snapshot_.state == RecordingState::Recording) &&
        snapshot_.roomIncluded) worker_->TapUnavailable(media::AudioTapKind::Room, generation_);
    return ResultLocked();
}

RecordingResult LectureRecorder::Pause() noexcept {
    std::lock_guard lock(mutex_);PollLocked();
    if(snapshot_.state!=RecordingState::Recording)return ResultLocked(RecordingError::CaptureFailed);
    if(!snapshot_.paused){const auto now=clock_->Now100ns();if(!worker_->SetPaused(true,now))return ResultLocked(RecordingError::CaptureFailed);pauseStarted_=now;snapshot_.paused=true;}
    notifications_.push_back({RecordingNotificationKind::State,RecordingError::None,snapshot_});return ResultLocked();
}
RecordingResult LectureRecorder::Resume() noexcept {
    std::lock_guard lock(mutex_);PollLocked();
    if(snapshot_.state!=RecordingState::Recording)return ResultLocked(RecordingError::CaptureFailed);
    if(snapshot_.paused){const auto now=clock_->Now100ns();if(!worker_->SetPaused(false,now))return ResultLocked(RecordingError::CaptureFailed);pausedDuration_+=std::max<std::int64_t>(0,now-pauseStarted_);snapshot_.paused=false;}
    notifications_.push_back({RecordingNotificationKind::State,RecordingError::None,snapshot_});return ResultLocked();
}

RecordingResult LectureRecorder::Stop() noexcept {
    std::lock_guard lock(mutex_); PollLocked();
    if (snapshot_.state == RecordingState::Starting || snapshot_.state == RecordingState::Recording) {
        snapshot_.state = RecordingState::Finalizing;
        snapshot_.paused=false;
        snapshot_.fileName.clear();
        notifications_.push_back({RecordingNotificationKind::State, RecordingError::None, snapshot_});
        worker_->Stop();
    }
    return ResultLocked();
}

std::uint64_t LectureRecorder::RecordingGeneration() const noexcept {
    std::lock_guard lock(mutex_);return generation_;
}

RecordingSnapshot LectureRecorder::Snapshot() noexcept {
    std::lock_guard lock(mutex_); PollLocked(); return snapshot_;
}

std::vector<RecordingNotification> LectureRecorder::DrainNotifications() noexcept {
    std::lock_guard lock(mutex_);
    PollLocked();
    auto result = std::move(notifications_);
    notifications_.clear();
    return result;
}

RecordingResult LectureRecorder::Shutdown(std::chrono::milliseconds timeout) noexcept {
    (void)Stop();
    const auto outcome = worker_->WaitQuiesced(timeout);
    if (outcome == RecordingWaitOutcome::TimedOutByCoordinator) {
        std::lock_guard lock(mutex_);
        PollLocked();
        return ResultLocked(lastError_ == RecordingError::None ? RecordingError::FinalizeTimeout : lastError_);
    }
    std::lock_guard lock(mutex_); PollLocked(); return ResultLocked(lastError_);
}

} // namespace telestudio
