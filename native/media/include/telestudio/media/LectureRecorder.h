#pragma once

#include "telestudio/media/MonotonicClock.h"
#include "telestudio/media/RecordingOutputOverride.h"
#include "telestudio/media/RecordingTypes.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace telestudio {

enum class RecordingWorkerUpdateKind { Ready, Completed, Failed, Warning };
enum class RecordingWaitOutcome {
    Quiesced,
    TimedOutByCoordinator,
    CompletionCommittedNotQuiesced
};

enum class OperationAuthority {
    PendingStart,
    StartingOpen,
    PipelineStarted,
    WorkerFinished,
    CompletedByWorker,
    TimedOutByCoordinator
};

bool IsOperationAuthorityTransitionAllowed(OperationAuthority from,
                                           OperationAuthority to) noexcept;
struct RecordingWorkerUpdate {
    RecordingWorkerUpdateKind kind = RecordingWorkerUpdateKind::Warning;
    RecordingError error = RecordingError::None;
    std::string fileName;
};

enum class RecordingNotificationKind { Warning, Error, State };
struct RecordingNotification {
    RecordingNotificationKind kind = RecordingNotificationKind::State;
    RecordingError error = RecordingError::None;
    RecordingSnapshot snapshot;
};

class IRecordingWorker {
public:
    virtual ~IRecordingWorker() = default;
    virtual RecordingError Start(const RecordingOptions& options, std::uint64_t generation,
                                 std::int64_t epoch100ns) noexcept = 0;
    virtual bool Push(media::OwnedAudioBlock block) noexcept = 0;
    virtual void TapUnavailable(media::AudioTapKind source, std::uint64_t generation) noexcept = 0;
    virtual bool SetPaused(bool, std::int64_t) noexcept = 0;
    virtual void Stop() noexcept = 0;
    virtual std::optional<RecordingWorkerUpdate> Poll() noexcept = 0;
    virtual RecordingWaitOutcome WaitQuiesced(std::chrono::milliseconds timeout) noexcept = 0;
    virtual bool IsQuiesced() const noexcept = 0;
};

struct NativeRecordingWorkerTestHooks {
    std::function<RecordingError(const std::filesystem::path& partPath)> runPipeline;
    std::function<void()> beforePathPublication;
    std::function<std::string()> makeRecordingStem;
    std::function<void()> beforePipelineAuthority;
    std::function<void()> afterPipelineAuthority;
    std::function<void()> afterPipelineRevocation;
    std::function<void()> afterPipelineStarted;
    std::function<void()> beforeVideoWrite;
    std::function<void()> beforeAudioWrite;
    std::function<void()> beforeFinalize;
    std::function<void(OperationAuthority from, OperationAuthority to)> onAuthorityTransition;
    std::function<void(const std::filesystem::path& partPath)> openPipeline;
    std::function<void()> beforeRenameAuthority;
    std::function<void()> beforeTimeoutAuthority;
    std::function<void()> afterTimeoutAuthority;
    std::function<void()> beforeRename;
    std::function<void()> afterRenameAttempt;
    std::function<bool(const std::filesystem::path& source,
                       const std::filesystem::path& destination)> renameFile;
};

std::shared_ptr<IRecordingWorker> CreateNativeRecordingWorker(
    std::filesystem::path x264Dll = {}, std::filesystem::path outputDirectoryOverride = {},
    std::shared_ptr<const NativeRecordingWorkerTestHooks> testHooks = {},
    std::optional<media::ValidatedRecordingOutputDirectory>
        validatedOutputDirectory = std::nullopt);

class LectureRecorder {
public:
    using TapReadiness = std::function<bool(media::AudioTapKind)>;
    LectureRecorder(std::shared_ptr<IRecordingWorker> worker,
                    std::shared_ptr<media::IMonotonicClock> clock,
                    TapReadiness readiness);
    ~LectureRecorder();

    RecordingResult Start(RecordingOptions options) noexcept;
    RecordingResult PushMicrophone(media::OwnedAudioBlock block) noexcept;
    RecordingResult PushRoomAudio(media::OwnedAudioBlock block) noexcept;
    RecordingResult MicrophoneUnavailable() noexcept;
    RecordingResult RoomAudioUnavailable() noexcept;
    RecordingResult Pause() noexcept;
    RecordingResult Resume() noexcept;
    RecordingResult Stop() noexcept;
    RecordingSnapshot Snapshot() noexcept;
    std::uint64_t RecordingGeneration() const noexcept;
    std::vector<RecordingNotification> DrainNotifications() noexcept;
    RecordingResult Shutdown(std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<IRecordingWorker> worker_;
    std::shared_ptr<media::IMonotonicClock> clock_;
    TapReadiness readiness_;
    RecordingSnapshot snapshot_;
    std::uint64_t generation_ = 0;
    std::int64_t epoch_ = 0;
    std::int64_t lastElapsed_ = 0;
    std::int64_t notifiedElapsed_ = 0;
    std::int64_t pauseStarted_ = 0;
    std::int64_t pausedDuration_ = 0;
    RecordingError lastError_ = RecordingError::None;
    std::vector<RecordingNotification> notifications_;

    void PollLocked() noexcept;
    RecordingResult ResultLocked(RecordingError error = RecordingError::None) const;
};

} // namespace telestudio
