#pragma once

#include "SipNetworkMode.h"
#include "../media/include/telestudio/media/NetworkProfiles.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace telestudio::sip {

enum class SipCommandKind {
  Empty,
  Answer,
  Reject,
  HangUp,
  Call,
  Quit,
  Source,
  SharingMode,
  Mute,
  MicrophoneMute,
  SystemAudio,
  RoomMute,
  RoomMixer,
  Codec,
  Quality,
  Endpoint,
  NetworkMode,
  AudioProfile,
  NetworkVideo,
  Identity,
  RoomVolume,
  MicGain,
  NoiseIsolation,
  EchoCancellation,
  RecordStart,
  RecordStop,
  RecordPause,
  RecordResume,
  VideoPreferences,
  Camera,
  Desktop,
  CameraOverlay,
  Invalid,
};

enum class VideoResolution { Auto, Sd, Hd, Fhd };
enum class SharingTransmissionMode { Auto, MainOnly, MainAndPresentation };
enum class CameraOverlaySize { Small, Medium, Large };
enum class CameraOverlayPosition { TopLeft, TopRight, BottomLeft, BottomRight };
struct VideoPreferencesArguments { VideoResolution resolution = VideoResolution::Auto; std::optional<unsigned> bitrateKbps; };
struct CameraOverlayArguments { CameraOverlaySize size = CameraOverlaySize::Medium; CameraOverlayPosition position = CameraOverlayPosition::BottomRight; };

struct RecordingCommandArguments {
  std::string captureItemId;
  bool includeMicrophone = false;
  bool includeRoomAudio = false;
};

struct RoomMixerCommandArguments {
  int masterPercent = 100;
  int bassDb = 0;
  int voiceDb = 0;
  int trebleDb = 0;
  int noiseReductionDb = 0;
};

struct SipCommand {
  SipCommandKind kind = SipCommandKind::Invalid;
  std::string argument;
  std::string transport;
  unsigned integerArgument = 0;
  bool enabled = false;
  RecordingCommandArguments recording;
  RoomMixerCommandArguments roomMixer;
  VideoPreferencesArguments videoPreferences;
  CameraOverlayArguments cameraOverlay;
  SharingTransmissionMode transmissionMode = SharingTransmissionMode::Auto;
  std::string deviceId;
  SipNetworkMode networkMode = SipNetworkMode::Direct;
  media::AudioNetworkProfile audioProfile;
  media::NetworkVideoProfile networkVideo;
};

SipCommand ParseSipCommand(std::string_view input);
std::optional<std::uint32_t> ParseSystemAudioExcludePid(
    std::string_view input) noexcept;

}  // namespace telestudio::sip
