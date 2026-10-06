#include "telestudio/media/ConsoleEventWriter.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace telestudio::media {
namespace {

bool Utf8ScalarCount(std::string_view value, std::size_t& count) {
  count = 0;
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    if (first <= 0x7f) {
      length = 1;
    } else if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
    } else {
      return false;
    }
    if (index + length > value.size()) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(value[index + offset]);
      if (continuation < 0x80 || continuation > 0xbf) return false;
    }
    if (length == 3) {
      const auto second = static_cast<unsigned char>(value[index + 1]);
      if ((first == 0xe0 && second < 0xa0) ||
          (first == 0xed && second > 0x9f)) return false;
    }
    if (length == 4) {
      const auto second = static_cast<unsigned char>(value[index + 1]);
      if ((first == 0xf0 && second < 0x90) ||
          (first == 0xf4 && second > 0x8f)) return false;
    }
    index += length;
    ++count;
  }
  return true;
}

std::optional<std::string> Escape(std::string_view value,
                                  std::size_t maximum_scalars) {
  std::size_t scalars = 0;
  if (!Utf8ScalarCount(value, scalars) || scalars > maximum_scalars) {
    return std::nullopt;
  }
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char character : value) {
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '/': escaped += "\\/"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (character < 0x20) {
          escaped += "\\u00";
          escaped.push_back(hex[character >> 4]);
          escaped.push_back(hex[character & 0x0f]);
        } else {
          escaped.push_back(static_cast<char>(character));
        }
    }
  }
  return escaped;
}

bool IsErrorCode(std::string_view code) {
  if (code.empty() || code.size() > 64 ||
      !((code.front() >= 'a' && code.front() <= 'z') ||
        (code.front() >= '0' && code.front() <= '9'))) return false;
  for (const char value : code) {
    if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
          value == '-')) return false;
  }
  return true;
}

bool IsCanonicalUnsigned(std::string_view value) {
  if (value.empty() || value.front() < '1' || value.front() > '9') return false;
  std::uint64_t parsed = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') return false;
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed != 0;
}

std::string LowerAscii(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(character >= 'A' && character <= 'Z'
                         ? static_cast<char>(character - 'A' + 'a')
                         : character);
  }
  return result;
}

bool IsWindowsBasename(std::string_view value) {
  std::size_t scalars = 0;
  if (value.empty() || !Utf8ScalarCount(value, scalars) || scalars > 255 ||
      value == "." || value == ".." || value.back() == ' ' ||
      value.back() == '.') return false;
  for (const unsigned char character : value) {
    if (character < 0x20 || character == 0x7f || character == '/' ||
        character == '\\' || character == ':' || character == '*' ||
        character == '?' || character == '"' || character == '<' ||
        character == '>' || character == '|') return false;
  }
  const std::string stem = LowerAscii(value.substr(0, value.find('.')));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
    return false;
  }
  if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) &&
      stem[3] >= '1' && stem[3] <= '9') return false;
  return true;
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

std::optional<std::string_view> ProtocolName(ConsoleProtocol value) {
  switch (value) {
    case ConsoleProtocol::Sip: return "sip";
    case ConsoleProtocol::H323: return "h323";
  }
  return std::nullopt;
}

std::optional<std::string_view> CodecName(MediaCodecName value) {
  switch (value) {
    case MediaCodecName::H264: return "h264";
    case MediaCodecName::H265: return "h265";
  }
  return std::nullopt;
}

std::optional<std::string_view> AudioCodecToken(AudioCodecName value) {
  switch (value) {
    case AudioCodecName::AacLd: return "aac-ld";
    case AudioCodecName::AacLc: return "aac-lc";
    case AudioCodecName::G722: return "g722";
  }
  return std::nullopt;
}

std::optional<std::string_view> SourceName(MediaSourceName value) {
  switch (value) {
    case MediaSourceName::Camera: return "camera";
    case MediaSourceName::Desktop: return "desktop";
  }
  return std::nullopt;
}

std::optional<std::string_view> RecordingStateName(RecordingState value) {
  switch (value) {
    case RecordingState::Idle: return "idle";
    case RecordingState::Starting: return "starting";
    case RecordingState::Recording: return "recording";
    case RecordingState::Finalizing: return "finalizing";
    case RecordingState::Completed: return "completed";
    case RecordingState::Failed: return "failed";
  }
  return std::nullopt;
}

std::optional<std::string> Begin(const EventPrefix& prefix,
                                 std::string_view type) {
  const auto protocol = ProtocolName(prefix.protocol);
  if (prefix.sequence < 1 || !protocol) return std::nullopt;
  return std::string("{\"schema\":1,\"type\":\"") + std::string(type) +
         "\",\"sequence\":" + std::to_string(prefix.sequence) +
         ",\"protocol\":\"" + std::string(*protocol) + "\"";
}

const char* Bool(bool value) { return value ? "true" : "false"; }

std::optional<std::string> JsonNumber(double value) {
  if (!std::isfinite(value)) return std::nullopt;
  if (value == 0.0) return std::string("0");
  char buffer[64]{};
  const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                       std::chars_format::general,
                                       std::numeric_limits<double>::max_digits10);
  if (converted.ec != std::errc{}) return std::nullopt;
  return std::string(buffer, converted.ptr);
}

bool InRange(std::int64_t value, std::int64_t minimum,
             std::int64_t maximum) {
  return value >= minimum && value <= maximum;
}

}  // namespace

std::optional<std::string> WriteMediaReadyV1(const MediaReadyV1& value) {
  auto json = Begin(value.prefix, "media-ready");
  if (!json) return std::nullopt;
  *json += std::string(",\"sourceControl\":") + Bool(value.sourceControl) +
           ",\"muteControl\":" + Bool(value.muteControl) +
           ",\"stats\":" + Bool(value.stats) +
           ",\"remoteSurface\":" + Bool(value.remoteSurface) +
           ",\"audioControl\":" + Bool(value.audioControl) +
           ",\"recording\":" + Bool(value.recording);
  if (value.videoControl) *json += std::string(",\"videoControl\":") + Bool(*value.videoControl);
  *json += "}";
  return json;
}

std::optional<std::string> WriteSourceChangedV1(const SourceChangedV1& value) {
  auto json = Begin(value.prefix, "source-changed");
  const auto source = SourceName(value.source);
  if (!json || !source) return std::nullopt;
  *json += ",\"source\":\"" + std::string(*source) + "\"}";
  return json;
}

std::optional<std::string> WriteMuteChangedV1(const MuteChangedV1& value) {
  auto json = Begin(value.prefix, "mute-changed");
  if (!json) return std::nullopt;
  *json += std::string(",\"muted\":") + Bool(value.muted) + "}";
  return json;
}

std::optional<std::string> WriteMediaStatsV1(const MediaStatsV1& value) {
  auto json = Begin(value.prefix, "media-stats");
  const auto codec = CodecName(value.codec);
  const auto source = SourceName(value.source);
  const auto loss = JsonNumber(value.lossPercent);
  const auto jitter = JsonNumber(value.jitterMs);
  const auto latency = JsonNumber(value.oneWayLatencyMs);
  if (!json || !codec || !source || !loss || !jitter || !latency ||
      !InRange(value.width, 1, 8192) || !InRange(value.height, 1, 8192) ||
      !InRange(value.fps, 1, 240) || !InRange(value.bitrateKbps, 0, 100000) ||
      value.lossPercent < 0 || value.lossPercent > 100 || value.jitterMs < 0 ||
      value.jitterMs > 600000 || value.oneWayLatencyMs < 0 ||
      value.oneWayLatencyMs > 600000 || value.droppedFrames < 0) {
    return std::nullopt;
  }
  *json += ",\"codec\":\"" + std::string(*codec) + "\",\"source\":\"" +
           std::string(*source) + "\",\"width\":" + std::to_string(value.width) +
           ",\"height\":" + std::to_string(value.height) +
           ",\"fps\":" + std::to_string(value.fps) +
           ",\"bitrateKbps\":" + std::to_string(value.bitrateKbps) +
           ",\"lossPercent\":" + *loss + ",\"jitterMs\":" + *jitter +
           ",\"oneWayLatencyMs\":" + *latency +
           ",\"droppedFrames\":" + std::to_string(value.droppedFrames) + "}";
  return json;
}

std::optional<std::string> WriteMediaErrorV1(const MediaErrorV1& value) {
  auto json = Begin(value.prefix, "media-error");
  const auto detail = Escape(value.detail, 2048);
  if (!json || !IsErrorCode(value.code) || !detail) return std::nullopt;
  *json += ",\"code\":\"" + value.code + "\",\"detail\":\"" + *detail + "\"}";
  return json;
}

std::optional<std::string> WriteRemoteSurfaceV1(const RemoteSurfaceV1& value) {
  auto json = Begin(value.prefix, "remote-surface");
  if (!json || !IsCanonicalUnsigned(value.hostHandle) ||
      !IsCanonicalUnsigned(value.epoch) || !InRange(value.width, 1, 8192) ||
      !InRange(value.height, 1, 8192) || value.format != "bgra8-unorm") {
    return std::nullopt;
  }
  *json += ",\"hostHandle\":\"" + value.hostHandle + "\",\"width\":" +
           std::to_string(value.width) + ",\"height\":" +
           std::to_string(value.height) + ",\"format\":\"bgra8-unorm\",\"epoch\":\"" +
           value.epoch + "\"}";
  return json;
}

std::optional<std::string> WriteLocalSurfaceV1(const LocalSurfaceV1& value) {
  auto json = Begin(value.prefix, "local-surface");
  if (!json || !IsCanonicalUnsigned(value.hostHandle) || !IsCanonicalUnsigned(value.epoch) ||
      !InRange(value.width, 1, 8192) || !InRange(value.height, 1, 8192) || value.format != "bgra8-unorm") return std::nullopt;
  *json += ",\"hostHandle\":\"" + value.hostHandle + "\",\"width\":" + std::to_string(value.width) +
      ",\"height\":" + std::to_string(value.height) + ",\"format\":\"bgra8-unorm\",\"epoch\":\"" + value.epoch + "\"}";
  return json;
}

std::optional<std::string> WriteVideoStateV1(const VideoStateV1& value) {
  auto json = Begin(value.prefix, "video-state");
  static constexpr const char* resolution[] = {"auto", "sd", "hd", "fhd"};
  static constexpr const char* sizes[] = {"small", "medium", "large"};
  static constexpr const char* positions[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
  const auto ri = static_cast<unsigned>(value.resolution), si = static_cast<unsigned>(value.overlaySize), pi = static_cast<unsigned>(value.overlayPosition);
  const auto cameraError = Escape(value.cameraError, 2048), desktopError = Escape(value.desktopError, 2048);
  const bool dimensionsValid = (value.appliedWidth == 0 && value.appliedHeight == 0) ||
      (InRange(value.appliedWidth, 2, 8192) && InRange(value.appliedHeight, 2, 8192) && value.appliedWidth % 2 == 0 && value.appliedHeight % 2 == 0);
  if (!json || ri >= 4 || si >= 3 || pi >= 4 || !IsCanonicalUnsigned(value.revision) ||
      (value.bitrateKbps && !InRange(*value.bitrateKbps, 128, 20000)) || !dimensionsValid ||
      !InRange(value.appliedBitrateKbps, 0, 20000) || !cameraError || !desktopError) return std::nullopt;
  *json += std::string(",\"cameraRequested\":") + Bool(value.cameraRequested) + ",\"desktopRequested\":" + Bool(value.desktopRequested) +
      ",\"cameraActive\":" + Bool(value.cameraActive) + ",\"desktopActive\":" + Bool(value.desktopActive) +
      ",\"resolution\":\"" + resolution[ri] + "\",\"bitrateKbps\":" + (value.bitrateKbps ? std::to_string(*value.bitrateKbps) : "null") +
      ",\"appliedWidth\":" + std::to_string(value.appliedWidth) + ",\"appliedHeight\":" + std::to_string(value.appliedHeight) +
      ",\"appliedBitrateKbps\":" + std::to_string(value.appliedBitrateKbps) + ",\"overlaySize\":\"" + sizes[si] +
      "\",\"overlayPosition\":\"" + positions[pi] + "\",\"revision\":\"" + value.revision +
      "\",\"cameraError\":\"" + *cameraError + "\",\"desktopError\":\"" + *desktopError + "\"}";
  return json;
}

std::optional<std::string> WriteAudioControlChangedV1(
    const AudioControlChangedV1& value) {
  auto json = Begin(value.prefix, "audio-control-changed");
  if (!json || value.roomVolume < 0 || value.roomVolume > 100 ||
      value.micGain < 0 || value.micGain > 150) return std::nullopt;
  *json += ",\"roomVolume\":" + std::to_string(value.roomVolume) +
           ",\"micGain\":" + std::to_string(value.micGain) +
           ",\"noiseIsolation\":" + Bool(value.noiseIsolation) + "}";
  return json;
}

std::optional<std::string> WriteAudioCodecStateV1(
    const AudioCodecStateV1& value) {
  auto json = Begin(value.prefix, "audio-codec-state");
  const auto requested = AudioCodecToken(value.requested);
  const auto negotiated = AudioCodecToken(value.negotiated);
  if (!json || !requested || !negotiated ||
      !InRange(value.sampleRate, 8000, 192000) ||
      !InRange(value.channels, 1, 2) ||
      !InRange(value.bitrateKbps, 1, 1024) ||
      (!value.sendActive && !value.receiveActive)) {
    return std::nullopt;
  }
  *json += ",\"requested\":\"" + std::string(*requested) +
           "\",\"negotiated\":\"" + std::string(*negotiated) +
           "\",\"sampleRate\":" + std::to_string(value.sampleRate) +
           ",\"channels\":" + std::to_string(value.channels) +
           ",\"bitrateKbps\":" + std::to_string(value.bitrateKbps) +
           ",\"sendActive\":" + Bool(value.sendActive) +
           ",\"receiveActive\":" + Bool(value.receiveActive) + "}";
  return json;
}

std::optional<std::string> WriteRecordingStateV1(const RecordingStateV1& value) {
  auto json = Begin(value.prefix, "recording-state");
  const auto state = RecordingStateName(value.state);
  if (!json || !state || value.elapsedMs < 0 || (value.paused && value.state != RecordingState::Recording)) return std::nullopt;
  switch (value.state) {
    case RecordingState::Idle:
      if (value.micIncluded || value.roomIncluded || value.elapsedMs != 0 ||
          !value.fileName.empty()) return std::nullopt;
      break;
    case RecordingState::Starting:
      if (value.elapsedMs != 0 || !value.fileName.empty()) return std::nullopt;
      break;
    case RecordingState::Recording:
    case RecordingState::Finalizing:
      if (!value.fileName.empty()) return std::nullopt;
      break;
    case RecordingState::Completed:
      if (!IsWindowsBasename(value.fileName) || !EndsWith(value.fileName, ".mp4"))
        return std::nullopt;
      break;
    case RecordingState::Failed:
      if (!value.fileName.empty() &&
          (!IsWindowsBasename(value.fileName) || !EndsWith(value.fileName, ".part")))
        return std::nullopt;
      break;
  }
  const auto file = Escape(value.fileName, 255);
  if (!file) return std::nullopt;
  *json += ",\"state\":\"" + std::string(*state) +
           "\",\"micIncluded\":" + Bool(value.micIncluded) +
           ",\"roomIncluded\":" + Bool(value.roomIncluded) +
           ",\"elapsedMs\":" + std::to_string(value.elapsedMs) +
           ",\"fileName\":\"" + *file + "\",\"paused\":" + Bool(value.paused) + "}";
  return json;
}

std::optional<std::string> WriteRecordingErrorV1(const RecordingErrorV1& value) {
  auto json = Begin(value.prefix, "recording-error");
  const auto detail = Escape(value.detail, 2048);
  if (!json || !detail || !IsErrorCode(value.code)) return std::nullopt;
  if ((value.recoverablePart &&
       (!IsWindowsBasename(value.fileName) || !EndsWith(value.fileName, ".part"))) ||
      (!value.recoverablePart && !value.fileName.empty())) return std::nullopt;
  const auto file = Escape(value.fileName, 255);
  if (!file) return std::nullopt;
  *json += ",\"code\":\"" + value.code + "\",\"detail\":\"" + *detail +
           "\",\"recoverablePart\":" + Bool(value.recoverablePart) +
           ",\"fileName\":\"" + *file + "\"}";
  return json;
}

std::string WriteMediaError(std::string_view code, std::string_view detail) {
  const auto escaped = Escape(detail, 2048);
  if (!IsErrorCode(code) || !escaped) return {};
  return std::string("{\"schema\":1,\"type\":\"media-error\",\"code\":\"") +
         std::string(code) + "\",\"detail\":\"" + *escaped + "\"}";
}

}  // namespace telestudio::media
