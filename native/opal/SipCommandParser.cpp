#include "SipCommandParser.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace telestudio::sip {
namespace {

bool IsStrictUtf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    if (first <= 0x7f) length = 1;
    else if (first >= 0xc2 && first <= 0xdf) length = 2;
    else if (first >= 0xe0 && first <= 0xef) length = 3;
    else if (first >= 0xf0 && first <= 0xf4) length = 4;
    else return false;
    if (index + length > value.size()) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(value[index + offset]);
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
  }
  return true;
}

bool Prepare(std::string_view input, std::string_view& prepared) {
  if (input.size() > 4096) return false;
  if (!input.empty() && input.back() == '\r') input.remove_suffix(1);
  if (!IsStrictUtf8(input)) return false;
  for (const unsigned char value : input) {
    if ((value < 0x20 && value != ' ' && value != '\t') || value == 0x7f ||
        value == '"' || value == '\\') return false;
  }
  while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) input.remove_prefix(1);
  while (!input.empty() && (input.back() == ' ' || input.back() == '\t')) input.remove_suffix(1);
  prepared = input;
  return true;
}

std::vector<std::string_view> Tokens(std::string_view input) {
  std::vector<std::string_view> result;
  while (!input.empty()) {
    const std::size_t end = input.find_first_of(" \t");
    result.push_back(input.substr(0, end));
    if (end == std::string_view::npos) break;
    input.remove_prefix(end);
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) input.remove_prefix(1);
  }
  return result;
}

int Base64UrlValue(char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '-') return 62;
  if (value == '_') return 63;
  return -1;
}

bool DecodeBase64Url(std::string_view encoded, std::string& decoded) {
  decoded.clear();
  if (encoded.empty() || encoded.size() % 4 == 1) return false;
  for (const char value : encoded) if (Base64UrlValue(value) < 0) return false;
  const std::size_t remainder = encoded.size() % 4;
  const int last = Base64UrlValue(encoded.back());
  if ((remainder == 2 && (last & 0x0f) != 0) ||
      (remainder == 3 && (last & 0x03) != 0)) return false;
  std::uint32_t bits = 0;
  unsigned bit_count = 0;
  for (const char value : encoded) {
    bits = (bits << 6) | static_cast<std::uint32_t>(Base64UrlValue(value));
    bit_count += 6;
    if (bit_count >= 8) {
      bit_count -= 8;
      decoded.push_back(static_cast<char>((bits >> bit_count) & 0xff));
    }
  }
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string reencoded;
  bits = 0;
  bit_count = 0;
  for (const unsigned char value : decoded) {
    bits = (bits << 8) | value;
    bit_count += 8;
    while (bit_count >= 6) {
      bit_count -= 6;
      reencoded.push_back(alphabet[(bits >> bit_count) & 0x3f]);
    }
  }
  if (bit_count != 0) reencoded.push_back(alphabet[(bits << (6 - bit_count)) & 0x3f]);
  return reencoded == encoded;
}

bool IsOpaqueId(std::string_view value) {
  std::string decoded;
  return DecodeBase64Url(value, decoded);
}

bool DecodeIdentifier(std::string_view value, std::string& decoded) {
  if (!DecodeBase64Url(value, decoded) || decoded.empty() || decoded.size() > 2048 ||
      !IsStrictUtf8(decoded)) return false;
  for (std::size_t index = 0; index < decoded.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(decoded[index]);
    if (character < 0x20 || character == 0x7f || (character == 0xc2 && index + 1 < decoded.size() && static_cast<unsigned char>(decoded[index + 1]) >= 0x80 && static_cast<unsigned char>(decoded[index + 1]) <= 0x9f)) return false;
  }
  return true;
}

bool IsIdentity(std::string_view value) {
  std::string decoded;
  if (!DecodeBase64Url(value, decoded) || decoded.size() < 2 || decoded.size() > 64) return false;
  for (const unsigned char character : decoded) {
    const bool allowed = (character >= 'A' && character <= 'Z') ||
                         (character >= 'a' && character <= 'z') ||
                         (character >= '0' && character <= '9') || character == ' ' ||
                         character == '.' || character == '\'' || character == '-';
    if (!allowed) return false;
  }
  return true;
}

bool ParseDecimal(std::string_view text, unsigned maximum, unsigned& result) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
  unsigned value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;
    const unsigned digit = static_cast<unsigned>(character - '0');
    if (value > (maximum - digit) / 10) return false;
    value = value * 10 + digit;
  }
  result = value;
  return true;
}

bool ParseSignedBand(std::string_view text, int& result) {
  bool negative = false;
  if (!text.empty() && text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  unsigned magnitude = 0;
  if (!ParseDecimal(text, 12, magnitude) || (negative && magnitude == 0)) {
    return false;
  }
  result = negative ? -static_cast<int>(magnitude)
                    : static_cast<int>(magnitude);
  return true;
}

bool IsOneOf(std::string_view value, std::initializer_list<std::string_view> choices) {
  for (const auto choice : choices) if (value == choice) return true;
  return false;
}
}  // namespace

SipCommand ParseSipCommand(std::string_view input) {
  std::string_view command;
  if (!Prepare(input, command)) return {};
  if (command.empty()) return {SipCommandKind::Empty};
  if (command == "share-mode auto" || command == "share-mode main" || command == "share-mode dual") {
    SipCommand result{SipCommandKind::SharingMode};
    result.argument = std::string(command.substr(11));
    result.transmissionMode = result.argument == "main" ? SharingTransmissionMode::MainOnly :
        result.argument == "dual" ? SharingTransmissionMode::MainAndPresentation : SharingTransmissionMode::Auto;
    return result;
  }
  const auto tokens = Tokens(command);
  if(tokens[0]=="audio-profile") {
    const auto profile=media::ParseAudioNetworkProfile(tokens);if(!profile)return {};
    SipCommand result{SipCommandKind::AudioProfile};result.audioProfile=*profile;return result;
  }
  if(tokens[0]=="network-video") {
    const auto profile=media::ParseNetworkVideoProfile(tokens);if(!profile)return {};
    SipCommand result{SipCommandKind::NetworkVideo};result.networkVideo=*profile;return result;
  }
  if (tokens.size() == 1) {
    if (tokens[0] == "answer") return {SipCommandKind::Answer};
    if (tokens[0] == "reject") return {SipCommandKind::Reject};
    if (tokens[0] == "hangup") return {SipCommandKind::HangUp};
    if (tokens[0] == "quit" || tokens[0] == "exit") return {SipCommandKind::Quit};
  }
  if ((tokens.size() == 2 || tokens.size() == 3) && tokens[0] == "call" &&
      (tokens.size() == 2 || IsOneOf(tokens[2], {"udp", "tcp"}))) {
    return {SipCommandKind::Call, std::string(tokens[1]),
            tokens.size() == 2 ? "udp" : std::string(tokens[2])};
  }
  if (tokens.size() == 2 && tokens[0] == "network-mode" &&
      IsOneOf(tokens[1], {"direct", "external"})) {
    SipCommand result{SipCommandKind::NetworkMode};
    result.networkMode = tokens[1] == "external" ? SipNetworkMode::External
                                                   : SipNetworkMode::Direct;
    return result;
  }
  if (tokens.size() == 2 && tokens[0] == "source" && tokens[1] == "camera")
    return {SipCommandKind::Source, "camera"};
  if (tokens.size() == 3 && tokens[0] == "source" && tokens[1] == "desktop" && IsOpaqueId(tokens[2]))
    return {SipCommandKind::Source, std::string(tokens[2])};
  if (tokens.size() == 3 && tokens[0] == "video-preferences") {
    SipCommand result{SipCommandKind::VideoPreferences};
    if (tokens[1] == "auto") result.videoPreferences.resolution = VideoResolution::Auto;
    else if (tokens[1] == "sd") result.videoPreferences.resolution = VideoResolution::Sd;
    else if (tokens[1] == "hd") result.videoPreferences.resolution = VideoResolution::Hd;
    else if (tokens[1] == "fhd") result.videoPreferences.resolution = VideoResolution::Fhd;
    else return {};
    if (tokens[2] != "auto") { unsigned bitrate = 0; if (!ParseDecimal(tokens[2], 20000, bitrate) || bitrate < 128) return {}; result.videoPreferences.bitrateKbps = bitrate; }
    return result;
  }
  if (tokens.size() >= 2 && tokens[0] == "camera") {
    if (tokens.size() == 2 && tokens[1] == "off") { SipCommand result{SipCommandKind::Camera}; result.enabled = false; return result; }
    if ((tokens.size() == 2 || tokens.size() == 3) && tokens[1] == "on") { SipCommand result{SipCommandKind::Camera}; result.enabled = true; if (tokens.size() == 3 && !DecodeIdentifier(tokens[2], result.deviceId)) return {}; return result; }
    return {};
  }
  if (tokens.size() >= 2 && tokens[0] == "desktop") {
    if (tokens.size() == 2 && tokens[1] == "off") { SipCommand result{SipCommandKind::Desktop}; result.enabled = false; return result; }
    if (tokens.size() == 3 && tokens[1] == "on") { SipCommand result{SipCommandKind::Desktop}; result.enabled = true; if (!DecodeIdentifier(tokens[2], result.deviceId)) return {}; return result; }
    return {};
  }
  if (tokens.size() == 3 && tokens[0] == "camera-overlay") {
    SipCommand result{SipCommandKind::CameraOverlay};
    if (tokens[1] == "small") result.cameraOverlay.size = CameraOverlaySize::Small; else if (tokens[1] == "medium") result.cameraOverlay.size = CameraOverlaySize::Medium; else if (tokens[1] == "large") result.cameraOverlay.size = CameraOverlaySize::Large; else return {};
    if (tokens[2] == "top-left") result.cameraOverlay.position = CameraOverlayPosition::TopLeft; else if (tokens[2] == "top-right") result.cameraOverlay.position = CameraOverlayPosition::TopRight; else if (tokens[2] == "bottom-left") result.cameraOverlay.position = CameraOverlayPosition::BottomLeft; else if (tokens[2] == "bottom-right") result.cameraOverlay.position = CameraOverlayPosition::BottomRight; else return {};
    return result;
  }
  if (tokens.size() == 2 && tokens[0] == "mute" && IsOneOf(tokens[1], {"on", "off"})) {
    SipCommand result{SipCommandKind::Mute, std::string(tokens[1])};
    result.enabled = tokens[1] == "on";
    return result;
  }
  if (tokens.size() == 2 &&
      IsOneOf(tokens[0], {"mic-mute", "system-audio", "room-mute"}) &&
      IsOneOf(tokens[1], {"on", "off"})) {
    SipCommand result;
    result.kind = tokens[0] == "mic-mute"
                      ? SipCommandKind::MicrophoneMute
                      : tokens[0] == "system-audio"
                            ? SipCommandKind::SystemAudio
                            : SipCommandKind::RoomMute;
    result.argument = std::string(tokens[1]);
    result.enabled = tokens[1] == "on";
    return result;
  }
  if ((tokens.size() == 5 || tokens.size() == 6) && tokens[0] == "room-mixer") {
    unsigned master = 0;
    unsigned noiseReduction = 0;
    int bass = 0;
    int voice = 0;
    int treble = 0;
    if (ParseDecimal(tokens[1], 300, master) &&
        ParseSignedBand(tokens[2], bass) &&
        ParseSignedBand(tokens[3], voice) &&
        ParseSignedBand(tokens[4], treble) &&
        (tokens.size() == 5 || ParseDecimal(tokens[5], 30, noiseReduction))) {
      SipCommand result{SipCommandKind::RoomMixer};
      result.roomMixer = {static_cast<int>(master), bass, voice, treble,
                          static_cast<int>(noiseReduction)};
      return result;
    }
  }
  if (tokens.size() == 2 && tokens[0] == "codec" && IsOneOf(tokens[1], {"auto", "h264", "h265"}))
    return {SipCommandKind::Codec, std::string(tokens[1])};
  if (tokens.size() == 2 && tokens[0] == "quality" &&
      IsOneOf(tokens[1], {"auto", "presentation", "motion", "maximum"}))
    return {SipCommandKind::Quality, std::string(tokens[1])};
  if (tokens.size() == 2 && tokens[0] == "endpoint" &&
      IsOneOf(tokens[1], {"auto", "ex90", "sx20", "pcs-xg77", "pcs-xg80", "generic"}))
    return {SipCommandKind::Endpoint, std::string(tokens[1])};
  if (tokens.size() == 2 && tokens[0] == "identity" && IsIdentity(tokens[1]))
    return {SipCommandKind::Identity, std::string(tokens[1])};
  if (tokens.size() == 2 && (tokens[0] == "room-volume" || tokens[0] == "mic-gain")) {
    unsigned value = 0;
    if (ParseDecimal(tokens[1], tokens[0] == "room-volume" ? 100U : 150U, value)) {
      SipCommand result{tokens[0] == "room-volume" ? SipCommandKind::RoomVolume : SipCommandKind::MicGain};
      result.integerArgument = value;
      return result;
    }
  }
  if (tokens.size() == 2 && tokens[0] == "echo-cancellation" && IsOneOf(tokens[1], {"on", "off"})) {
    SipCommand result{SipCommandKind::EchoCancellation};
    result.enabled = tokens[1] == "on";
    return result;
  }
  if (tokens.size() == 2 && tokens[0] == "noise-isolation" && IsOneOf(tokens[1], {"on", "off"})) {
    SipCommand result{SipCommandKind::NoiseIsolation};
    result.enabled = tokens[1] == "on";
    return result;
  }
  if (tokens.size() == 5 && tokens[0] == "record" && tokens[1] == "start" &&
      IsOpaqueId(tokens[2]) && IsOneOf(tokens[3], {"mic-on", "mic-off"}) &&
      IsOneOf(tokens[4], {"room-on", "room-off"})) {
    SipCommand result{SipCommandKind::RecordStart};
    result.recording = {std::string(tokens[2]), tokens[3] == "mic-on", tokens[4] == "room-on"};
    return result;
  }
  if (tokens.size() == 2 && tokens[0] == "record" && tokens[1] == "stop")
    return {SipCommandKind::RecordStop};
  if (tokens.size() == 2 && tokens[0] == "record" && tokens[1] == "pause") return {SipCommandKind::RecordPause};
  if (tokens.size() == 2 && tokens[0] == "record" && tokens[1] == "resume") return {SipCommandKind::RecordResume};
  return {};
}

std::optional<std::uint32_t> ParseSystemAudioExcludePid(
    std::string_view input) noexcept {
  if (input.empty() || input.front() == '0') return std::nullopt;
  std::uint32_t value = 0;
  for (const char character : input) {
    if (character < '0' || character > '9') return std::nullopt;
    const auto digit = static_cast<std::uint32_t>(character - '0');
    if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return value == 0 ? std::nullopt : std::optional<std::uint32_t>{value};
}

}  // namespace telestudio::sip
