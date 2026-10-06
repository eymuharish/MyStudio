#include "telestudio/media/FfmpegVideoDecoder.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "Missing decoder smoke fixture: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void DecodeFixture(const telestudio::FfmpegRuntimePaths& runtime, telestudio::VideoCodec codec,
                   const std::filesystem::path& path, int64_t timestamp) {
    telestudio::FfmpegVideoDecoder decoder(runtime, telestudio::DecoderPreference::SoftwareOnly);
    decoder.Open(codec, 1280, 720);
    const auto result = decoder.Decode(Read(path), timestamp);
    Require(result.status == telestudio::DecodeStatus::Produced, "Pinned FFmpeg software decoder must produce a frame");
    Require(result.usedSoftware, "Software-only decode must report its backend");
    Require(!result.keyframeRequested, "Healthy software decode must not request a keyframe");
    Require(!result.frames.empty(), "DecodeResult must retain all frames from this call");
    int64_t previous = -1;
    for (const auto& frame : result.frames) {
        Require(frame.width == 1280 && frame.height == 720 && frame.stride == 1280 * 4, "Decoded BGRA geometry must be owned and exact");
        Require(frame.bytes.size() == static_cast<size_t>(frame.stride) * frame.height, "Decoded BGRA byte ownership must cover every row");
        Require(frame.timestamp100ns >= previous, "DecodeResult frames must preserve timestamp order");
        previous = frame.timestamp100ns;
    }
    std::cout << (codec == telestudio::VideoCodec::H264 ? "H264" : "HEVC")
              << " software_frames=" << result.frames.size() << " first_ts=" << result.frames.front().timestamp100ns << '\n';
}

void DecodePreferred(const telestudio::FfmpegRuntimePaths& runtime, telestudio::VideoCodec codec,
                     const std::filesystem::path& path, int64_t timestamp) {
    telestudio::FfmpegVideoDecoder decoder(runtime, telestudio::DecoderPreference::PreferD3d11);
    decoder.Open(codec, 1280, 720);
    Require(decoder.MediaDevice() != nullptr, "D3D11-capable decoder must expose its borrowed media device for the shared surface");
    const auto corrupt = decoder.Decode({}, timestamp - 1);
    Require(corrupt.status == telestudio::DecodeStatus::DecodeError && !corrupt.keyframeRequested,
            "Corrupt input must not cause backend churn or a keyframe request");
    const auto result = decoder.Decode(Read(path), timestamp);
    std::cout << (codec == telestudio::VideoCodec::H264 ? "H264" : "HEVC")
              << " preferred_status=" << static_cast<int>(result.status)
              << " used_software=" << result.usedSoftware
              << " requested=" << result.keyframeRequested
              << " raw_frames=" << result.frames.size() << '\n';
    if (result.status == telestudio::DecodeStatus::DecodeError) {
        Require(!result.usedSoftware && !result.keyframeRequested && decoder.IsD3d11Available(),
                "Send/receive bitstream error must not cause backend churn even for random-access input");
        std::cout << (codec == telestudio::VideoCodec::H264 ? "H264" : "HEVC")
                  << " preferred_backend=d3d11-bitstream-error-no-churn\n";
        return;
    }
    Require(result.status == telestudio::DecodeStatus::Produced && !result.frames.empty(),
            "Preferred decoder must either produce through D3D11VA or its controlled software fallback");
    if (result.usedSoftware) Require(result.keyframeRequested, "D3D11VA fallback must request exactly one keyframe on transition");
    Require(decoder.IsD3d11Available() == !result.usedSoftware,
            "Decoder-instance D3D11 availability must become false after sticky software fallback");
    std::cout << (codec == telestudio::VideoCodec::H264 ? "H264" : "HEVC")
              << " preferred_backend=" << (result.usedSoftware ? "software-fallback" : "d3d11va")
              << " frames=" << result.frames.size() << '\n';
}
}

int main() {
    try {
        const telestudio::FfmpegRuntimePaths runtime { TELESTUDIO_CODEC_BIN_DIR };
        Require(telestudio::FfmpegVideoDecoder::IsAvailable(telestudio::VideoCodec::H264, runtime), "Pinned H.264 decoder must bind");
        Require(telestudio::FfmpegVideoDecoder::IsAvailable(telestudio::VideoCodec::H265, runtime), "Pinned HEVC decoder must bind");
        Require(!telestudio::FfmpegVideoDecoder::IsAvailable(telestudio::VideoCodec::H264, {runtime.binDirectory / "missing"}), "Missing explicit runtime must be rejected without PATH fallback");
        DecodeFixture(runtime, telestudio::VideoCodec::H264, "work/evidence/media/h264-smoke.264", 9'000'000);
        DecodeFixture(runtime, telestudio::VideoCodec::H265, "work/evidence/media/h265-smoke.265", 10'000'000);
        DecodePreferred(runtime, telestudio::VideoCodec::H264, "work/evidence/media/h264-smoke.264", 11'000'000);
        DecodePreferred(runtime, telestudio::VideoCodec::H265, "work/evidence/media/h265-smoke.265", 12'000'000);
        std::cout << "D3D11VA H264=" << telestudio::FfmpegVideoDecoder::IsD3d11Available(telestudio::VideoCodec::H264, runtime)
                  << " HEVC=" << telestudio::FfmpegVideoDecoder::IsD3d11Available(telestudio::VideoCodec::H265, runtime) << '\n';
        std::cout << "media_ffmpeg_decoder_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "media_ffmpeg_decoder_tests: " << error.what() << '\n';
        return 1;
    }
}
