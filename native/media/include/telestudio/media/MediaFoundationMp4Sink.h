#pragma once

#include "telestudio/media/RecordingTypes.h"

#include <filesystem>
#include <memory>
#include <functional>
#include <span>
#include <vector>

namespace telestudio {

struct H264BootstrapInfo {
    bool valid = false;
    bool hasIdr = false;
    int profileIdc = 0;
    std::vector<std::uint8_t> sequenceHeader;
};

H264BootstrapInfo ParseH264Bootstrap(std::span<const std::uint8_t> annexB) noexcept;

struct MfSinkPlan {
    RecordingVideoFormat video;
    bool includeAudio = false;
    bool disableThrottling = true;
    bool mpeg4Container = true;
    int audioSampleRate = 48000;
    int audioChannels = 1;
    int audioBitsPerSample = 16;
    int audioAverageBytesPerSecond = 12000;
    int audioBlockAlignment = 1;
    int aacPayloadType = 0;
    int aacProfileLevel = 0x29;
    int pcmAverageBytesPerSecond = 96000;
    int pcmBlockAlignment = 2;
};

class IMfSinkBackend {
public:
    virtual ~IMfSinkBackend() = default;
    virtual void Startup() = 0;
    virtual void Create(const std::filesystem::path& partPath, const MfSinkPlan& plan) = 0;
    virtual void AddVideoStream(const MfSinkPlan& plan) = 0;
    virtual void SetVideoInput(const MfSinkPlan& plan) = 0;
    virtual void AddAudioStream(const MfSinkPlan& plan) = 0;
    virtual void SetAudioInput(const MfSinkPlan& plan) = 0;
    virtual void BeginWriting() = 0;
    virtual void WriteVideo(const RecordingVideoSample& sample) = 0;
    virtual void WriteAudio(const RecordingAudioSample& sample) = 0;
    virtual void Finalize() = 0;
    virtual void ReleaseAndShutdown() noexcept = 0;
};

using MfSinkBackendFactory = std::function<std::unique_ptr<IMfSinkBackend>()>;

class IRecordingMp4Sink {
public:
    virtual ~IRecordingMp4Sink() = default;
    virtual void Open(const std::filesystem::path& partPath, const RecordingVideoFormat& video,
                      bool includeAudio) = 0;
    virtual void WriteVideo(const RecordingVideoSample& sample) = 0;
    virtual void WriteAudio(const RecordingAudioSample& sample) = 0;
    virtual void Finalize() = 0;
};

class MediaFoundationMp4Sink final : public IRecordingMp4Sink {
public:
    explicit MediaFoundationMp4Sink(MfSinkBackendFactory factory = {});
    ~MediaFoundationMp4Sink() override;
    MediaFoundationMp4Sink(const MediaFoundationMp4Sink&) = delete;
    MediaFoundationMp4Sink& operator=(const MediaFoundationMp4Sink&) = delete;

    void Open(const std::filesystem::path& partPath, const RecordingVideoFormat& video,
              bool includeAudio) override;
    void WriteVideo(const RecordingVideoSample& sample) override;
    void WriteAudio(const RecordingAudioSample& sample) override;
    void Finalize() override;

    static constexpr bool SuccessfulPathUsesFlush() noexcept { return false; }
    static constexpr bool SuccessfulPathUsesNotifyEndOfSegment() noexcept { return false; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace telestudio
