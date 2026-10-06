#include "telestudio/media/ConsoleEventWriter.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>

namespace {
using namespace telestudio::media;

std::size_t passed = 0;

[[noreturn]] void Fail(const std::string& scenario) {
  std::cerr << "FAIL: " << scenario << '\n';
  std::exit(EXIT_FAILURE);
}

void ExpectJson(const std::optional<std::string>& actual,
                const std::string& expected, const char* scenario) {
  if (!actual || *actual != expected || actual->find('\n') != std::string::npos) {
    Fail(scenario);
  }
  ++passed;
}

void ExpectRejected(const std::optional<std::string>& actual,
                    const char* scenario) {
  if (actual) Fail(scenario);
  ++passed;
}

void ExpectAccepted(const std::optional<std::string>& actual,
                    const char* scenario) {
  if (!actual || actual->find('\n') != std::string::npos) Fail(scenario);
  ++passed;
}

EventPrefix Prefix(std::int64_t sequence = 1,
                   ConsoleProtocol protocol = ConsoleProtocol::Sip) {
  return {sequence, protocol};
}

std::string Utf8(const char8_t* value) {
  return reinterpret_cast<const char*>(value);
}

std::string RepeatBytes(std::string_view scalar, std::size_t count) {
  std::string result;
  for (std::size_t index = 0; index < count; ++index) result += scalar;
  return result;
}

std::string RecordingJson(std::int64_t sequence, const char* state, bool mic,
                          bool room, std::int64_t elapsed,
                          const std::string& file) {
  return std::string("{\"schema\":1,\"type\":\"recording-state\",\"sequence\":") +
         std::to_string(sequence) +
         ",\"protocol\":\"sip\",\"state\":\"" + state +
         "\",\"micIncluded\":" + (mic ? "true" : "false") +
         ",\"roomIncluded\":" + (room ? "true" : "false") +
         ",\"elapsedMs\":" + std::to_string(elapsed) +
         ",\"fileName\":\"" + file + "\",\"paused\":false}";
}
}  // namespace

int main() {
  ExpectJson(WriteRecordingStateV1({Prefix(200),RecordingState::Recording,true,false,1234,"",true}),
    R"({"schema":1,"type":"recording-state","sequence":200,"protocol":"sip","state":"recording","micIncluded":true,"roomIncluded":false,"elapsedMs":1234,"fileName":"","paused":true})","paused recording schema");
  ExpectRejected(WriteRecordingStateV1({Prefix(201),RecordingState::Completed,true,false,1234,"MyStudio.mp4",true}),"terminal recording cannot remain paused");

  ExpectJson(WriteMediaReadyV1({Prefix(20), false, false, false, false, false, false}),
             R"({"schema":1,"type":"media-ready","sequence":20,"protocol":"sip","sourceControl":false,"muteControl":false,"stats":false,"remoteSurface":false,"audioControl":false,"recording":false})",
             "media-ready all false");
  ExpectJson(WriteMediaReadyV1({Prefix(21), true, true, true, true, true, true}),
             R"({"schema":1,"type":"media-ready","sequence":21,"protocol":"sip","sourceControl":true,"muteControl":true,"stats":true,"remoteSurface":true,"audioControl":true,"recording":true})",
             "media-ready all true");
  ExpectJson(WriteMediaReadyV1({Prefix(), true, false, true, false, true, true}),
             R"({"schema":1,"type":"media-ready","sequence":1,"protocol":"sip","sourceControl":true,"muteControl":false,"stats":true,"remoteSurface":false,"audioControl":true,"recording":true})",
             "media-ready exact order");
  ExpectJson(WriteMediaReadyV1({Prefix(std::numeric_limits<std::int64_t>::max(),
                                      ConsoleProtocol::H323),
                                false, true, false, true, false, false}),
             R"({"schema":1,"type":"media-ready","sequence":9223372036854775807,"protocol":"h323","sourceControl":false,"muteControl":true,"stats":false,"remoteSurface":true,"audioControl":false,"recording":false})",
             "media-ready maximum sequence and H.323");

  ExpectJson(WriteSourceChangedV1({Prefix(2), MediaSourceName::Camera}),
             R"({"schema":1,"type":"source-changed","sequence":2,"protocol":"sip","source":"camera"})",
             "camera source");
  ExpectJson(WriteSourceChangedV1({Prefix(3), MediaSourceName::Desktop}),
             R"({"schema":1,"type":"source-changed","sequence":3,"protocol":"sip","source":"desktop"})",
             "desktop source");
  ExpectJson(WriteMuteChangedV1({Prefix(4), true}),
             R"({"schema":1,"type":"mute-changed","sequence":4,"protocol":"sip","muted":true})",
             "mute changed");
  ExpectJson(WriteMuteChangedV1({Prefix(40), false}),
             R"({"schema":1,"type":"mute-changed","sequence":40,"protocol":"sip","muted":false})",
             "mute false exact record");

  ExpectJson(WriteMediaStatsV1({Prefix(5), MediaCodecName::H264,
                                MediaSourceName::Camera, 1, 1, 1, 0, 0.0, 0.0,
                                0.0, 0}),
             R"({"schema":1,"type":"media-stats","sequence":5,"protocol":"sip","codec":"h264","source":"camera","width":1,"height":1,"fps":1,"bitrateKbps":0,"lossPercent":0,"jitterMs":0,"oneWayLatencyMs":0,"droppedFrames":0})",
             "minimum media stats");
  ExpectJson(WriteMediaStatsV1({Prefix(6, ConsoleProtocol::H323),
                                MediaCodecName::H265, MediaSourceName::Desktop,
                                8192, 8192, 240, 100000, 100.0, 600000.0,
                                600000.0, std::numeric_limits<std::int64_t>::max()}),
             R"({"schema":1,"type":"media-stats","sequence":6,"protocol":"h323","codec":"h265","source":"desktop","width":8192,"height":8192,"fps":240,"bitrateKbps":100000,"lossPercent":100,"jitterMs":600000,"oneWayLatencyMs":600000,"droppedFrames":9223372036854775807})",
             "maximum media stats");
  ExpectJson(WriteMediaStatsV1({Prefix(60), MediaCodecName::H264,
                                MediaSourceName::Desktop, 1920, 1080, 30, 4096,
                                1.25, 2.5, 3.75, 9}),
             R"({"schema":1,"type":"media-stats","sequence":60,"protocol":"sip","codec":"h264","source":"desktop","width":1920,"height":1080,"fps":30,"bitrateKbps":4096,"lossPercent":1.25,"jitterMs":2.5,"oneWayLatencyMs":3.75,"droppedFrames":9})",
             "fractional locale-independent media stats");

  const MediaStatsV1 valid_stats{Prefix(), MediaCodecName::H264,
                                  MediaSourceName::Camera, 1920, 1080, 30,
                                  4000, 1.25, 2.5, 3.75, 4};
  auto ExpectBadStats = [&](MediaStatsV1 value, const char* name) {
    ExpectRejected(WriteMediaStatsV1(value), name);
  };
  auto bad_stats = valid_stats;
  bad_stats.width = 0; ExpectBadStats(bad_stats, "stats width below minimum");
  bad_stats = valid_stats; bad_stats.width = 8193;
  ExpectBadStats(bad_stats, "stats width above maximum");
  bad_stats = valid_stats; bad_stats.height = 0;
  ExpectBadStats(bad_stats, "stats height below minimum");
  bad_stats = valid_stats; bad_stats.height = 8193;
  ExpectBadStats(bad_stats, "stats height above maximum");
  bad_stats = valid_stats; bad_stats.fps = 0;
  ExpectBadStats(bad_stats, "stats fps below minimum");
  bad_stats = valid_stats; bad_stats.fps = 241;
  ExpectBadStats(bad_stats, "stats fps above maximum");
  bad_stats = valid_stats; bad_stats.bitrateKbps = -1;
  ExpectBadStats(bad_stats, "stats bitrate below minimum");
  bad_stats = valid_stats; bad_stats.bitrateKbps = 100001;
  ExpectBadStats(bad_stats, "stats bitrate above maximum");
  bad_stats = valid_stats; bad_stats.lossPercent = -0.01;
  ExpectBadStats(bad_stats, "stats loss below minimum");
  bad_stats = valid_stats; bad_stats.lossPercent = 100.01;
  ExpectBadStats(bad_stats, "stats loss above maximum");
  bad_stats = valid_stats; bad_stats.jitterMs = -0.01;
  ExpectBadStats(bad_stats, "stats jitter below minimum");
  bad_stats = valid_stats; bad_stats.jitterMs = 600000.01;
  ExpectBadStats(bad_stats, "stats jitter above maximum");
  bad_stats = valid_stats; bad_stats.oneWayLatencyMs = -0.01;
  ExpectBadStats(bad_stats, "stats latency below minimum");
  bad_stats = valid_stats; bad_stats.oneWayLatencyMs = 600000.01;
  ExpectBadStats(bad_stats, "stats latency above maximum");
  bad_stats = valid_stats; bad_stats.droppedFrames = -1;
  ExpectBadStats(bad_stats, "stats dropped frames below minimum");
  bad_stats = valid_stats; bad_stats.source = static_cast<MediaSourceName>(99);
  ExpectBadStats(bad_stats, "stats invalid source enum");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  bad_stats = valid_stats; bad_stats.lossPercent = nan;
  ExpectBadStats(bad_stats, "stats loss NaN");
  bad_stats = valid_stats; bad_stats.lossPercent = infinity;
  ExpectBadStats(bad_stats, "stats loss positive infinity");
  bad_stats = valid_stats; bad_stats.lossPercent = -infinity;
  ExpectBadStats(bad_stats, "stats loss negative infinity");
  bad_stats = valid_stats; bad_stats.jitterMs = nan;
  ExpectBadStats(bad_stats, "stats jitter NaN");
  bad_stats = valid_stats; bad_stats.jitterMs = infinity;
  ExpectBadStats(bad_stats, "stats jitter infinity");
  bad_stats = valid_stats; bad_stats.oneWayLatencyMs = nan;
  ExpectBadStats(bad_stats, "stats latency NaN");
  bad_stats = valid_stats; bad_stats.oneWayLatencyMs = infinity;
  ExpectBadStats(bad_stats, "stats latency infinity");

  ExpectJson(WriteMediaErrorV1({Prefix(7), "capture",
                                Utf8(u8"display \"gone\" / path\\x\nسلام")}),
             Utf8(u8R"({"schema":1,"type":"media-error","sequence":7,"protocol":"sip","code":"capture","detail":"display \"gone\" \/ path\\x\nسلام"})"),
             "media error escaping and UTF-8");
  ExpectJson(WriteMediaErrorV1({Prefix(70), "1-code", "\b\f\r\t\x01"}),
             R"({"schema":1,"type":"media-error","sequence":70,"protocol":"sip","code":"1-code","detail":"\b\f\r\t\u0001"})",
             "numeric error-code prefix and named control escapes");
  ExpectJson(WriteMediaErrorV1({Prefix(75), "a", ""}),
             R"({"schema":1,"type":"media-error","sequence":75,"protocol":"sip","code":"a","detail":""})",
             "minimum error code");
  const std::string emoji("\xf0\x9f\x98\x80", 4);
  ExpectJson(WriteMediaErrorV1({Prefix(76), "capture", emoji}),
             std::string(R"({"schema":1,"type":"media-error","sequence":76,"protocol":"sip","code":"capture","detail":")") +
                 emoji + "\"}",
             "supplementary scalar copied unchanged");
  const std::string max_code = "a" + std::string(63, '9');
  ExpectJson(WriteMediaErrorV1({Prefix(71), max_code, ""}),
             std::string(R"({"schema":1,"type":"media-error","sequence":71,"protocol":"sip","code":")") +
                 max_code + R"(","detail":""})",
             "64 byte error code boundary");
  const std::string detail_2048(2048, 'a');
  ExpectJson(WriteMediaErrorV1({Prefix(72), "capture", detail_2048}),
             std::string(R"({"schema":1,"type":"media-error","sequence":72,"protocol":"sip","code":"capture","detail":")") +
                 detail_2048 + "\"}",
             "2048 ASCII scalar detail boundary");
  const std::string unicode_detail_2048 =
      RepeatBytes(std::string("\xd8\xb3", 2), 2048);
  const auto unicode_detail_json =
      WriteMediaErrorV1({Prefix(73), "capture", unicode_detail_2048});
  if (!unicode_detail_json) Fail("2048 multibyte scalar detail was rejected");
  ExpectJson(unicode_detail_json,
             std::string(R"({"schema":1,"type":"media-error","sequence":73,"protocol":"sip","code":"capture","detail":")") +
                 unicode_detail_2048 + "\"}",
             "2048 multibyte scalar detail exact JSON");
  const std::string supplementary_detail_2048 = RepeatBytes(emoji, 2048);
  ExpectJson(WriteMediaErrorV1(
                 {Prefix(77), "capture", supplementary_detail_2048}),
             std::string(R"({"schema":1,"type":"media-error","sequence":77,"protocol":"sip","code":"capture","detail":")") +
                 supplementary_detail_2048 + "\"}",
             "2048 supplementary scalar detail boundary");
  ExpectRejected(WriteMediaErrorV1(
                     {Prefix(), "capture", RepeatBytes(emoji, 2049)}),
                 "2049 supplementary scalar detail rejected");
  const std::string all_controls("\0\x02\x03\x04\x05\x06\x07\x0b\x0e\x1f", 10);
  ExpectJson(WriteMediaErrorV1({Prefix(74), "capture", all_controls}),
             R"({"schema":1,"type":"media-error","sequence":74,"protocol":"sip","code":"capture","detail":"\u0000\u0002\u0003\u0004\u0005\u0006\u0007\u000B\u000E\u001F"})",
             "remaining C0 controls escape as uppercase unicode");

  const std::string invalid_codes[] = {
      "", "-bad", "Bad", "bad_code", "bad.code", "bad/part",
      "a" + std::string(64, '0'), std::string("\xc3\xa9", 2)};
  for (const auto& code : invalid_codes) {
    ExpectRejected(WriteMediaErrorV1({Prefix(), code, "safe"}),
                   "invalid error-code matrix");
  }
  ExpectRejected(WriteMediaErrorV1(
                     {Prefix(), "capture",
                      RepeatBytes(std::string("\xd8\xb3", 2), 2049)}),
                 "2049 multibyte scalar detail rejected");
  const std::string malformed_utf8[] = {
      std::string("\x80", 1), std::string("\xc2", 1),
      std::string("\xe2\x82", 2), std::string("\xf0\x9f\x92", 3),
      std::string("\xc0\xaf", 2), std::string("\xe0\x80\xaf", 3),
      std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
      std::string("\xff", 1)};
  for (const auto& detail : malformed_utf8) {
    ExpectRejected(WriteMediaErrorV1({Prefix(), "capture", detail}),
                   "malformed UTF-8 detail matrix");
  }
  ExpectJson(WriteRemoteSurfaceV1({Prefix(8), "1", 1, 8192, "bgra8-unorm",
                                   "18446744073709551615"}),
             R"({"schema":1,"type":"remote-surface","sequence":8,"protocol":"sip","hostHandle":"1","width":1,"height":8192,"format":"bgra8-unorm","epoch":"18446744073709551615"})",
             "remote surface exact order");
  ExpectJson(WriteRemoteSurfaceV1({Prefix(80), "18446744073709551615", 8192, 1,
                                   "bgra8-unorm", "1"}),
             R"({"schema":1,"type":"remote-surface","sequence":80,"protocol":"sip","hostHandle":"18446744073709551615","width":8192,"height":1,"format":"bgra8-unorm","epoch":"1"})",
             "remote surface opposite boundaries");
  const std::string invalid_unsigned[] = {
      "", "0", "00", "01", "-1", "+1", " 1", "1 ", "1x", "1.0", "1e1",
      Utf8(u8"١"), "18446744073709551616",
      "99999999999999999999999999999999999999999999999999"};
  for (const auto& text : invalid_unsigned) {
    ExpectRejected(WriteRemoteSurfaceV1(
                       {Prefix(), text, 1, 1, "bgra8-unorm", "1"}),
                   "invalid canonical host handle");
    ExpectRejected(WriteRemoteSurfaceV1(
                       {Prefix(), "1", 1, 1, "bgra8-unorm", text}),
                   "invalid canonical epoch");
  }
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "1", 0, 1, "bgra8-unorm", "1"}),
                 "surface width below minimum");
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "1", 8193, 1, "bgra8-unorm", "1"}),
                 "surface width above maximum");
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "1", 1, 0, "bgra8-unorm", "1"}),
                 "surface height below minimum");
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "1", 1, 8193, "bgra8-unorm", "1"}),
                 "surface height above maximum");
  ExpectJson(WriteAudioControlChangedV1({Prefix(9), 0, 150, false}),
             R"({"schema":1,"type":"audio-control-changed","sequence":9,"protocol":"sip","roomVolume":0,"micGain":150,"noiseIsolation":false})",
             "audio controls boundaries");
  ExpectJson(WriteAudioControlChangedV1({Prefix(90), 100, 0, true}),
             R"({"schema":1,"type":"audio-control-changed","sequence":90,"protocol":"sip","roomVolume":100,"micGain":0,"noiseIsolation":true})",
             "opposite audio control boundaries");
  ExpectRejected(WriteAudioControlChangedV1({Prefix(), -1, 0, false}),
                 "room volume below minimum");
  ExpectRejected(WriteAudioControlChangedV1({Prefix(), 101, 0, false}),
                 "room volume above maximum");
  ExpectRejected(WriteAudioControlChangedV1({Prefix(), 0, -1, false}),
                 "microphone gain below minimum");
  ExpectRejected(WriteAudioControlChangedV1({Prefix(), 0, 151, false}),
                 "microphone gain above maximum");

  ExpectJson(WriteAudioCodecStateV1(
                 {Prefix(91, ConsoleProtocol::H323), AudioCodecName::AacLd,
                  AudioCodecName::AacLd, 48000, 2, 128, true, false}),
             R"({"schema":1,"type":"audio-codec-state","sequence":91,"protocol":"h323","requested":"aac-ld","negotiated":"aac-ld","sampleRate":48000,"channels":2,"bitrateKbps":128,"sendActive":true,"receiveActive":false})",
             "AAC-LD send codec state");
  ExpectJson(WriteAudioCodecStateV1(
                 {Prefix(92), AudioCodecName::AacLc, AudioCodecName::G722,
                  16000, 1, 64, false, true}),
             R"({"schema":1,"type":"audio-codec-state","sequence":92,"protocol":"sip","requested":"aac-lc","negotiated":"g722","sampleRate":16000,"channels":1,"bitrateKbps":64,"sendActive":false,"receiveActive":true})",
             "G.722 receive fallback codec state");
  const AudioCodecStateV1 valid_audio_codec{
      Prefix(), AudioCodecName::AacLd, AudioCodecName::AacLc,
      48000, 2, 192, true, true};
  auto bad_audio_codec = valid_audio_codec;
  bad_audio_codec.sampleRate = 7999;
  ExpectRejected(WriteAudioCodecStateV1(bad_audio_codec),
                 "audio codec sample rate below minimum");
  bad_audio_codec = valid_audio_codec; bad_audio_codec.channels = 3;
  ExpectRejected(WriteAudioCodecStateV1(bad_audio_codec),
                 "audio codec channels above maximum");
  bad_audio_codec = valid_audio_codec; bad_audio_codec.bitrateKbps = 0;
  ExpectRejected(WriteAudioCodecStateV1(bad_audio_codec),
                 "audio codec bitrate below minimum");
  bad_audio_codec = valid_audio_codec;
  bad_audio_codec.sendActive = false; bad_audio_codec.receiveActive = false;
  ExpectRejected(WriteAudioCodecStateV1(bad_audio_codec),
                 "audio codec inactive state rejected");
  bad_audio_codec = valid_audio_codec;
  bad_audio_codec.negotiated = static_cast<AudioCodecName>(99);
  ExpectRejected(WriteAudioCodecStateV1(bad_audio_codec),
                 "invalid audio codec enum rejected");

  const std::string arabic_file = Utf8(u8"محاضرة.mp4");
  ExpectJson(WriteRecordingStateV1(
                 {Prefix(10), RecordingState::Idle, false, false, 0, ""}),
             RecordingJson(10, "idle", false, false, 0, ""),
             "idle exact record");
  const struct {
    RecordingState state;
    const char* name;
    std::int64_t elapsed;
    const std::string* file;
  } recording_states[] = {
      {RecordingState::Starting, "starting", 0, nullptr},
      {RecordingState::Recording, "recording", 1, nullptr},
      {RecordingState::Finalizing, "finalizing", 2, nullptr},
      {RecordingState::Completed, "completed", 3, &arabic_file},
  };
  const std::string empty_file;
  std::int64_t recording_sequence = 100;
  for (const auto& value : recording_states) {
    for (unsigned flags = 0; flags < 4; ++flags) {
      const bool mic = (flags & 1U) != 0;
      const bool room = (flags & 2U) != 0;
      const std::string& file = value.file == nullptr ? empty_file : *value.file;
      ExpectJson(WriteRecordingStateV1({Prefix(recording_sequence), value.state,
                                        mic, room, value.elapsed, file}),
                 RecordingJson(recording_sequence, value.name, mic, room,
                               value.elapsed, file),
                 "recording state flag cross-product exact JSON");
      ++recording_sequence;
    }
  }
  for (unsigned flags = 0; flags < 4; ++flags) {
    const bool mic = (flags & 1U) != 0;
    const bool room = (flags & 2U) != 0;
    ExpectJson(WriteRecordingStateV1({Prefix(recording_sequence),
                                      RecordingState::Failed, mic, room, 4, ""}),
               RecordingJson(recording_sequence, "failed", mic, room, 4, ""),
               "failed empty filename flag cross-product");
    ++recording_sequence;
    ExpectJson(WriteRecordingStateV1({Prefix(recording_sequence),
                                      RecordingState::Failed, mic, room, 4,
                                      "lecture.part"}),
               RecordingJson(recording_sequence, "failed", mic, room, 4,
                             "lecture.part"),
               "failed retained part flag cross-product");
    ++recording_sequence;
  }
  const std::int64_t elapsed_boundaries[] = {
      0, std::numeric_limits<std::int64_t>::max()};
  const struct {
    RecordingState state;
    const char* name;
  } active_states[] = {{RecordingState::Recording, "recording"},
                       {RecordingState::Finalizing, "finalizing"}};
  for (const auto& active : active_states) {
    for (const auto elapsed : elapsed_boundaries) {
      for (unsigned flags = 0; flags < 4; ++flags) {
        const bool mic = (flags & 1U) != 0;
        const bool room = (flags & 2U) != 0;
        ExpectJson(WriteRecordingStateV1({Prefix(recording_sequence), active.state,
                                          mic, room, elapsed, ""}),
                   RecordingJson(recording_sequence, active.name, mic, room,
                                 elapsed, ""),
                   "active recording elapsed and flag boundary cross-product");
        ++recording_sequence;
      }
    }
  }
  for (const auto elapsed : elapsed_boundaries) {
    for (unsigned flags = 0; flags < 4; ++flags) {
      const bool mic = (flags & 1U) != 0;
      const bool room = (flags & 2U) != 0;
      ExpectJson(WriteRecordingStateV1({Prefix(recording_sequence),
                                        RecordingState::Completed, mic, room,
                                        elapsed, "lecture.mp4"}),
                 RecordingJson(recording_sequence, "completed", mic, room,
                               elapsed, "lecture.mp4"),
                 "completed elapsed and flag boundary cross-product");
      ++recording_sequence;
      for (const std::string file : {std::string(), std::string("lecture.part")}) {
        ExpectJson(WriteRecordingStateV1({Prefix(recording_sequence),
                                          RecordingState::Failed, mic, room,
                                          elapsed, file}),
                   RecordingJson(recording_sequence, "failed", mic, room,
                                 elapsed, file),
                   "failed elapsed flag and file boundary cross-product");
        ++recording_sequence;
      }
    }
  }
  ExpectJson(WriteRecordingStateV1({Prefix(16), RecordingState::Completed,
                                    false, false,
                                    std::numeric_limits<std::int64_t>::max(),
                                    ".mp4"}),
             R"({"schema":1,"type":"recording-state","sequence":16,"protocol":"sip","state":"completed","micIncluded":false,"roomIncluded":false,"elapsedMs":9223372036854775807,"fileName":".mp4","paused":false})",
             "recording completed maximum elapsed and suffix-only basename");
  ExpectJson(WriteRecordingErrorV1({Prefix(17), "finalize-failed", "safe detail",
                                    true, ".part"}),
             R"({"schema":1,"type":"recording-error","sequence":17,"protocol":"sip","code":"finalize-failed","detail":"safe detail","recoverablePart":true,"fileName":".part"})",
             "recoverable recording error");
  ExpectJson(WriteRecordingErrorV1({Prefix(18), "capture", "", false, ""}),
             R"({"schema":1,"type":"recording-error","sequence":18,"protocol":"sip","code":"capture","detail":"","recoverablePart":false,"fileName":""})",
             "non-recoverable recording error");

  const std::string compatibility = WriteMediaError("capture", "display \"gone\"");
  if (compatibility !=
      R"({"schema":1,"type":"media-error","code":"capture","detail":"display \"gone\""})") {
    Fail("exact compatibility fixture");
  }
  ++passed;

  ExpectRejected(WriteMediaReadyV1({Prefix(0), true, true, true, true, true, true}),
                 "sequence zero");
  ExpectRejected(WriteSourceChangedV1({Prefix(-1), MediaSourceName::Camera}),
                 "negative sequence");
  ExpectRejected(WriteMuteChangedV1({Prefix(1, static_cast<ConsoleProtocol>(99)), false}),
                 "invalid protocol");
  ExpectRejected(WriteSourceChangedV1({Prefix(), static_cast<MediaSourceName>(99)}),
                 "invalid source enum");
  ExpectRejected(WriteMediaStatsV1({Prefix(), static_cast<MediaCodecName>(99),
                                    MediaSourceName::Camera, 1, 1, 1, 0, 0, 0, 0, 0}),
                 "invalid codec enum");
  ExpectRejected(WriteMediaStatsV1({Prefix(), MediaCodecName::H264,
                                    MediaSourceName::Camera, 0, 8193, 0, -1,
                                    -1, -1, -1, -1}),
                 "invalid stats ranges");
  ExpectRejected(WriteMediaStatsV1({Prefix(), MediaCodecName::H264,
                                    MediaSourceName::Camera, 1, 1, 1, 0,
                                    std::numeric_limits<double>::infinity(), 0, 0, 0}),
                 "non-finite stats");
  ExpectRejected(WriteMediaErrorV1({Prefix(), "Bad_code", "safe"}),
                 "invalid error code");
  ExpectRejected(WriteMediaErrorV1({Prefix(), "capture", std::string(2049, 'a')}),
                 "detail scalar limit");
  ExpectRejected(WriteMediaErrorV1({Prefix(), "capture", std::string("\xc0\xaf", 2)}),
                 "invalid detail UTF-8");
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "0", 1, 1, "bgra8-unorm", "1"}),
                 "zero handle");
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "01", 1, 1, "bgra8-unorm", "1"}),
                 "noncanonical handle");
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "1", 1, 1, "rgba8", "1"}),
                 "invalid surface format");
  ExpectRejected(WriteRemoteSurfaceV1({Prefix(), "1", 8193, 1, "bgra8-unorm", "1"}),
                 "invalid surface dimension");
  ExpectRejected(WriteAudioControlChangedV1({Prefix(), -1, 151, true}),
                 "invalid audio ranges");

  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Idle, true, false, 0, ""}),
                 "idle inclusion flags");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Idle, false, true, 0, ""}),
                 "idle room inclusion flag");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Idle, true, true, 0, ""}),
                 "idle both inclusion flags");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Idle, false, false, 1, ""}),
                 "idle elapsed");
  ExpectRejected(WriteRecordingStateV1(
                     {Prefix(), RecordingState::Idle, false, false,
                      std::numeric_limits<std::int64_t>::max(), ""}),
                 "idle maximum elapsed rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Idle, false, false, 0, "x.part"}),
                 "idle filename must be empty");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Starting, true, true, 1, ""}),
                 "starting elapsed");
  ExpectRejected(WriteRecordingStateV1(
                     {Prefix(), RecordingState::Starting, true, true,
                      std::numeric_limits<std::int64_t>::max(), ""}),
                 "starting maximum elapsed rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Starting, true, true, 0, "x.part"}),
                 "starting filename must be empty");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Recording, true, true, 1, "x.mp4"}),
                 "recording filename must be empty");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Finalizing, true, true, 1, "x.part"}),
                 "finalizing filename must be empty");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Completed, true, true, 1, ""}),
                 "completed filename required");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Completed, true, true, 1, "x.part"}),
                 "completed mp4 suffix");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Completed, true, true, 1, "x.MP4"}),
                 "completed uppercase suffix rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Completed, true, true, 1, "x.mP4"}),
                 "completed mixed-case suffix rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Completed, true, true, 1, "x.video"}),
                 "completed unrelated suffix rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Failed, true, true, 1, "x.mp4"}),
                 "failed part suffix");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Failed, true, true, 1, "x.PART"}),
                 "failed uppercase suffix rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Failed, true, true, 1, "x.pArt"}),
                 "failed mixed-case suffix rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Failed, true, true, 1, "x.tmp"}),
                 "failed unrelated suffix rejected");
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Failed, true, true, -1, ""}),
                 "negative elapsed");
  const RecordingState all_states[] = {
      RecordingState::Idle, RecordingState::Starting, RecordingState::Recording,
      RecordingState::Finalizing, RecordingState::Completed, RecordingState::Failed};
  for (const auto state : all_states) {
    ExpectRejected(WriteRecordingStateV1(
                       {Prefix(), state, false, false, -1,
                        state == RecordingState::Completed ? "x.mp4" : ""}),
                   "negative elapsed rejected for every state");
  }
  ExpectRejected(WriteRecordingStateV1(
                     {Prefix(), static_cast<RecordingState>(99), false, false, 0, ""}),
                 "invalid recording state enum");

  const char* invalid_names[] = {
      "C:\\secret\\lecture.mp4", "../lecture.mp4", "/root.mp4",
      "\\\\server\\share.mp4", "C:.mp4", "CON.txt.mp4", "con.MP4.mp4",
      "PrN.mp4", "aux.note.mp4", "NUL.mp4", "com1.mp4", "COM9.note.mp4",
      "lpt1.mp4", "LPT9.note.mp4", "lecture?.mp4", "lecture.mp4.",
      "lecture.mp4 ", ".", ".."};
  for (const char* name : invalid_names) {
    ExpectRejected(WriteRecordingStateV1(
                       {Prefix(), RecordingState::Completed, true, true, 1, name}),
                   "unsafe Windows basename");
  }
  ExpectRejected(WriteRecordingStateV1(
                     {Prefix(), RecordingState::Completed, true, true, 1,
                      std::string(252, 'a') + ".mp4"}),
                 "filename scalar limit");
  ExpectJson(WriteRecordingStateV1(
                 {Prefix(19), RecordingState::Completed, true, false, 1,
                  std::string(251, 'a') + ".mp4"}),
             std::string(R"({"schema":1,"type":"recording-state","sequence":19,"protocol":"sip","state":"completed","micIncluded":true,"roomIncluded":false,"elapsedMs":1,"fileName":")") +
                 std::string(251, 'a') + ".mp4\",\"paused\":false}",
             "255 scalar filename");
  const std::string unicode_255_name =
      RepeatBytes(std::string("\xc3\xa9", 2), 251) + ".mp4";
  ExpectJson(WriteRecordingStateV1({Prefix(190), RecordingState::Completed,
                                    false, true, 1, unicode_255_name}),
             RecordingJson(190, "completed", false, true, 1,
                           unicode_255_name),
             "255 Unicode scalar filename");
  const std::string unicode_256_name =
      RepeatBytes(std::string("\xc3\xa9", 2), 252) + ".mp4";
  ExpectRejected(WriteRecordingStateV1({Prefix(), RecordingState::Completed,
                                        false, false, 1, unicode_256_name}),
                 "256 Unicode scalar filename rejected");
  const std::string supplementary_255_name = RepeatBytes(emoji, 251) + ".mp4";
  ExpectJson(WriteRecordingStateV1({Prefix(191), RecordingState::Completed,
                                    true, true, 1, supplementary_255_name}),
             RecordingJson(191, "completed", true, true, 1,
                           supplementary_255_name),
             "255 supplementary scalar filename");
  ExpectRejected(WriteRecordingStateV1(
                     {Prefix(), RecordingState::Completed, true, true, 1,
                      RepeatBytes(emoji, 252) + ".mp4"}),
                 "256 supplementary scalar filename rejected");
  const std::string valid_names[] = {
      " lecture.mp4", "lecture.final.v2.mp4", arabic_file, "COM0.mp4", "COM10.mp4",
      "LPT0.mp4", "LPT10.mp4", "CONSOLE.mp4", "AUXiliary.mp4",
      "NULled.mp4", "PRNter.mp4"};
  for (const auto& name : valid_names) {
    ExpectAccepted(WriteRecordingStateV1(
                       {Prefix(), RecordingState::Completed, true, false, 1, name}),
                   "valid basename and DOS boundary matrix");
  }
  const char reserved[] = {'/', '\\', ':', '*', '?', '"', '<', '>', '|'};
  for (const char character : reserved) {
    std::string name = "lecture";
    name.push_back(character);
    name += ".mp4";
    ExpectRejected(WriteRecordingStateV1(
                       {Prefix(), RecordingState::Completed, false, false, 1, name}),
                   "reserved basename character matrix");
  }
  const unsigned char controls[] = {0x00, 0x01, 0x1f, 0x7f};
  for (const unsigned char character : controls) {
    std::string name = "lecture";
    name.push_back(static_cast<char>(character));
    name += ".mp4";
    ExpectRejected(WriteRecordingStateV1(
                       {Prefix(), RecordingState::Completed, false, false, 1, name}),
                   "control and DEL basename matrix");
  }
  ExpectRejected(WriteRecordingStateV1(
                     {Prefix(), RecordingState::Completed, false, false, 1,
                      std::string("\xc0\xaf", 2) + ".mp4"}),
                 "invalid UTF-8 basename");
  ExpectRejected(WriteRecordingErrorV1({Prefix(), "capture", "safe", true, ""}),
                 "recoverable part requires filename");
  ExpectRejected(WriteRecordingErrorV1({Prefix(), "capture", "safe", false, "x.part"}),
                 "non-recoverable forbids filename");
  ExpectRejected(WriteRecordingErrorV1({Prefix(), "capture", "safe", true, "x.mp4"}),
                 "recoverable requires part suffix");
  ExpectRejected(WriteRecordingErrorV1({Prefix(), "capture", "safe", true,
                                        "C:\\Users\\essam\\secret.part"}),
                 "sensitive full path rejected without echo");
  ExpectAccepted(WriteRecordingErrorV1(
                     {Prefix(), max_code, detail_2048, false, ""}),
                 "recording error maximum code and detail");
  const std::string recording_error_detail =
      std::string("quote\" slash/ back\\ line\n") +
      std::string("\xd8\xb3", 2) + emoji;
  ExpectJson(WriteRecordingErrorV1({Prefix(192), "a", recording_error_detail,
                                    true, " lecture.part"}),
             std::string(R"({"schema":1,"type":"recording-error","sequence":192,"protocol":"sip","code":"a","detail":"quote\" slash\/ back\\ line\n)") +
                 std::string("\xd8\xb3", 2) + emoji +
                 R"(","recoverablePart":true,"fileName":" lecture.part"})",
             "recording error exact escaping Arabic supplementary and leading space");
  const std::string unicode_255_part =
      RepeatBytes(std::string("\xc3\xa9", 2), 250) + ".part";
  ExpectAccepted(WriteRecordingErrorV1(
                     {Prefix(), "capture", unicode_detail_2048, true,
                      unicode_255_part}),
                 "recording error Unicode scalar boundaries");
  ExpectRejected(WriteRecordingErrorV1(
                     {Prefix(), "capture", "safe", true,
                      RepeatBytes(std::string("\xc3\xa9", 2), 251) + ".part"}),
                 "recording error 256 scalar basename");
  const std::string supplementary_255_part = RepeatBytes(emoji, 250) + ".part";
  ExpectAccepted(WriteRecordingErrorV1(
                     {Prefix(), "capture", supplementary_detail_2048, true,
                      supplementary_255_part}),
                 "recording error supplementary scalar boundaries");
  ExpectRejected(WriteRecordingErrorV1(
                     {Prefix(), "capture", RepeatBytes(emoji, 2049), false, ""}),
                 "recording error 2049 supplementary detail");
  ExpectRejected(WriteRecordingErrorV1(
                     {Prefix(), "capture", "safe", true,
                      RepeatBytes(emoji, 251) + ".part"}),
                 "recording error 256 supplementary basename");
  for (const auto& code : invalid_codes) {
    ExpectRejected(WriteRecordingErrorV1({Prefix(), code, "safe", false, ""}),
                   "recording error invalid code matrix");
  }
  ExpectRejected(WriteRecordingErrorV1(
                     {Prefix(), "capture", std::string(2049, 'a'), false, ""}),
                 "recording error detail above scalar limit");
  for (const auto& detail : malformed_utf8) {
    ExpectRejected(WriteRecordingErrorV1(
                       {Prefix(), "capture", detail, false, ""}),
                   "recording error malformed UTF-8 matrix");
  }
  const std::string invalid_part_names[] = {
      "CON.part", "prn.note.part", "AUX.part", "nul.part", "COM1.part",
      "com9.note.part", "LPT1.part", "lpt9.note.part", "x?.part",
      "x.part.", "x.part ", "..\\x.part", "C:\\x.part"};
  for (const auto& name : invalid_part_names) {
    ExpectRejected(WriteRecordingErrorV1(
                       {Prefix(), "capture", "safe", true, name}),
                   "recording error invalid basename matrix");
  }
  for (const char character : reserved) {
    std::string name = "lecture";
    name.push_back(character);
    name += ".part";
    ExpectRejected(WriteRecordingErrorV1(
                       {Prefix(), "capture", "safe", true, name}),
                   "recording error reserved basename character matrix");
  }
  for (const unsigned char character : controls) {
    std::string name = "lecture";
    name.push_back(static_cast<char>(character));
    name += ".part";
    ExpectRejected(WriteRecordingErrorV1(
                       {Prefix(), "capture", "safe", true, name}),
                   "recording error control and DEL basename matrix");
  }
  ExpectRejected(WriteRecordingErrorV1(
                     {Prefix(), "capture", "safe", true,
                      std::string("\xc0\xaf", 2) + ".part"}),
                 "recording error invalid UTF-8 basename");
  const std::string valid_part_names[] = {
      ".part", " leading.part", "lecture.final.part", "COM0.part", "COM10.part", "LPT0.part",
      "LPT10.part", "CONSOLE.part", "AUXiliary.part"};
  for (const auto& name : valid_part_names) {
    ExpectAccepted(WriteRecordingErrorV1(
                       {Prefix(), "capture", "safe", true, name}),
                   "recording error valid basename matrix");
  }

  if (!WriteMediaError("capture", std::string("\xc0\xaf", 2)).empty() ||
      !WriteMediaError("Bad", "safe").empty()) {
    Fail("compatibility formatter rejects invalid input without diagnostic");
  }
  ++passed;

  std::cout << passed << " console event writer cases passed\n";
  return EXIT_SUCCESS;
}
