#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

namespace telestudio::media {

enum class ConsoleProtocol { Sip, H323 };
enum class MediaCodecName { H264, H265 };
enum class AudioCodecName { AacLd, AacLc, G722 };
enum class MediaSourceName { Camera, Desktop };
enum class RecordingState { Idle, Starting, Recording, Finalizing, Completed, Failed };

struct EventPrefix {
  std::int64_t sequence;
  ConsoleProtocol protocol;
};

struct MediaReadyV1 {
  EventPrefix prefix;
  bool sourceControl;
  bool muteControl;
  bool stats;
  bool remoteSurface;
  bool audioControl;
  bool recording;
  std::optional<bool> videoControl;
};

struct SourceChangedV1 { EventPrefix prefix; MediaSourceName source; };
struct MuteChangedV1 { EventPrefix prefix; bool muted; };

struct MediaStatsV1 {
  EventPrefix prefix;
  MediaCodecName codec;
  MediaSourceName source;
  std::int64_t width;
  std::int64_t height;
  std::int64_t fps;
  std::int64_t bitrateKbps;
  double lossPercent;
  double jitterMs;
  double oneWayLatencyMs;
  std::int64_t droppedFrames;
};

struct MediaErrorV1 {
  EventPrefix prefix;
  std::string code;
  std::string detail;
};

struct RemoteSurfaceV1 {
  EventPrefix prefix;
  std::string hostHandle;
  std::int64_t width;
  std::int64_t height;
  std::string format;
  std::string epoch;
};
using LocalSurfaceV1 = RemoteSurfaceV1;
enum class VideoResolutionName { Auto, Sd, Hd, Fhd };
enum class CameraOverlaySizeName { Small, Medium, Large };
enum class CameraOverlayPositionName { TopLeft, TopRight, BottomLeft, BottomRight };
struct VideoStateV1 {
  EventPrefix prefix; bool cameraRequested; bool desktopRequested; bool cameraActive; bool desktopActive;
  VideoResolutionName resolution; std::optional<std::int64_t> bitrateKbps;
  std::int64_t appliedWidth; std::int64_t appliedHeight; std::int64_t appliedBitrateKbps;
  CameraOverlaySizeName overlaySize; CameraOverlayPositionName overlayPosition;
  std::string revision; std::string cameraError; std::string desktopError;
};

struct AudioControlChangedV1 {
  EventPrefix prefix;
  int roomVolume;
  int micGain;
  bool noiseIsolation;
};

struct AudioCodecStateV1 {
  EventPrefix prefix;
  AudioCodecName requested;
  AudioCodecName negotiated;
  int sampleRate;
  int channels;
  int bitrateKbps;
  bool sendActive;
  bool receiveActive;
};

struct RecordingStateV1 {
  EventPrefix prefix;
  RecordingState state;
  bool micIncluded;
  bool roomIncluded;
  std::int64_t elapsedMs;
  std::string fileName;
  bool paused = false;
};

struct RecordingErrorV1 {
  EventPrefix prefix;
  std::string code;
  std::string detail;
  bool recoverablePart;
  std::string fileName;
};

std::optional<std::string> WriteMediaReadyV1(const MediaReadyV1& value);
std::optional<std::string> WriteSourceChangedV1(const SourceChangedV1& value);
std::optional<std::string> WriteMuteChangedV1(const MuteChangedV1& value);
std::optional<std::string> WriteMediaStatsV1(const MediaStatsV1& value);
std::optional<std::string> WriteMediaErrorV1(const MediaErrorV1& value);
std::optional<std::string> WriteRemoteSurfaceV1(const RemoteSurfaceV1& value);
std::optional<std::string> WriteLocalSurfaceV1(const LocalSurfaceV1& value);
std::optional<std::string> WriteVideoStateV1(const VideoStateV1& value);
std::optional<std::string> WriteAudioControlChangedV1(
    const AudioControlChangedV1& value);
std::optional<std::string> WriteAudioCodecStateV1(
    const AudioCodecStateV1& value);
std::optional<std::string> WriteRecordingStateV1(const RecordingStateV1& value);
std::optional<std::string> WriteRecordingErrorV1(const RecordingErrorV1& value);

std::string WriteMediaError(std::string_view code, std::string_view detail);

}  // namespace telestudio::media
