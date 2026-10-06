#include "telestudio/media/IVideoEncoder.h"
#include "telestudio/media/detail/EncoderPtsSequence.h"

#include <x265.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace telestudio::detail {
bool IsPinnedX265Api(const x265_api* api) noexcept;
}

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TestFrame {
    int width = 1280;
    int height = 720;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;

    TestFrame() : y(static_cast<size_t>(width) * height),
                  u(static_cast<size_t>(width / 2) * (height / 2)),
                  v(static_cast<size_t>(width / 2) * (height / 2)) {
        constexpr uint8_t yBars[] = { 235, 210, 170, 145, 106, 81, 41, 16 };
        constexpr uint8_t uBars[] = { 128, 16, 166, 54, 202, 90, 240, 128 };
        constexpr uint8_t vBars[] = { 128, 146, 16, 34, 222, 240, 110, 128 };
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; ++column) {
                y[static_cast<size_t>(row) * width + column] = yBars[(column * 8) / width];
            }
        }
        for (int row = 0; row < height / 2; ++row) {
            for (int column = 0; column < width / 2; ++column) {
                const int bar = (column * 16) / width;
                u[static_cast<size_t>(row) * (width / 2) + column] = uBars[bar];
                v[static_cast<size_t>(row) * (width / 2) + column] = vBars[bar];
            }
        }
    }

    telestudio::I420Frame View(int64_t timestamp100ns) const {
        return { y.data(), u.data(), v.data(), width, width / 2, width / 2,
                 width, height, timestamp100ns };
    }
};

std::vector<std::vector<uint8_t>> ReadAnnexBNals(const std::vector<uint8_t>& annexB) {
    const bool beginsWithFour = annexB.size() >= 4 && annexB[0] == 0 && annexB[1] == 0 &&
                                annexB[2] == 0 && annexB[3] == 1;
    const bool beginsWithThree = annexB.size() >= 3 && annexB[0] == 0 && annexB[1] == 0 && annexB[2] == 1;
    Require(beginsWithFour || beginsWithThree, "Access unit must begin with a three- or four-byte Annex-B prefix");

    std::vector<std::pair<size_t, size_t>> starts;
    for (size_t i = 0; i < annexB.size();) {
        const bool four = i + 4 <= annexB.size() && annexB[i] == 0 && annexB[i + 1] == 0 &&
                          annexB[i + 2] == 0 && annexB[i + 3] == 1;
        const bool three = i + 3 <= annexB.size() && annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 1;
        if (four) {
            starts.emplace_back(i, 4);
            i += 4;
        } else if (three) {
            starts.emplace_back(i, 3);
            i += 3;
        } else {
            ++i;
        }
    }
    Require(!starts.empty(), "Annex-B output must contain a start code");
    std::vector<std::vector<uint8_t>> nals;
    for (size_t index = 0; index < starts.size(); ++index) {
        const size_t start = starts[index].first;
        const size_t prefix = starts[index].second;
        const size_t end = index + 1 < starts.size() ? starts[index + 1].first : annexB.size();
        Require(end > start + prefix, "Annex-B output must not contain empty NALs");
        nals.emplace_back(annexB.begin() + static_cast<std::ptrdiff_t>(start + prefix),
                          annexB.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return nals;
}

void RequireAnnexBRejected(const std::vector<uint8_t>& bytes, const char* message) {
    bool rejected = false;
    try {
        (void)ReadAnnexBNals(bytes);
    } catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected, message);
}

void VerifyAnnexBFixtures() {
    const std::vector<uint8_t> threeByte { 0, 0, 1, 0x67, 0, 0, 1, 0x68 };
    const auto threeNals = ReadAnnexBNals(threeByte);
    Require(threeNals.size() == 2 && threeNals[0] == std::vector<uint8_t> { 0x67 } &&
            threeNals[1] == std::vector<uint8_t> { 0x68 }, "Three-byte Annex-B prefixes must be parsed exactly");

    const std::vector<uint8_t> fourByte { 0, 0, 0, 1, 0x67, 0, 0, 0, 1, 0x68 };
    const auto fourNals = ReadAnnexBNals(fourByte);
    Require(fourNals.size() == 2 && fourNals[0] == std::vector<uint8_t> { 0x67 } &&
            fourNals[1] == std::vector<uint8_t> { 0x68 }, "Four-byte Annex-B prefixes must be parsed before three-byte prefixes");

    RequireAnnexBRejected({ 0, 0, 0, 1, 0, 0, 1, 0x67 }, "Empty Annex-B NAL must be rejected");
    RequireAnnexBRejected({ 0, 0, 0, 6, 0x67, 0, 0, 1, 0x68 },
                           "Length-prefixed bytes containing an internal start code must be rejected");
}

void VerifyEncoderPtsSequenceStartsAtZeroAndAdvances() {
    telestudio::detail::EncoderPtsSequence firstOpen;
    Require(firstOpen.Next() == 0 && firstOpen.Next() == 1 && firstOpen.Next() == 2,
            "A new encoder PTS sequence must emit 0, 1, 2");

    telestudio::detail::EncoderPtsSequence reopened;
    Require(reopened.Next() == 0 && reopened.Next() == 1 && reopened.Next() == 2,
            "A reopened encoder PTS sequence must emit 0, 1, 2");
}

void VerifyEncoderPtsSequenceResetRestartsAtZero() {
    telestudio::detail::EncoderPtsSequence sequence;
    Require(sequence.Next() == 0 && sequence.Next() == 1,
            "Encoder PTS sequence must advance before reset");
    sequence.Reset();
    Require(sequence.Next() == 0, "Reset encoder PTS sequence must restart at zero");
}

bool ContainsH264Nal(const std::vector<uint8_t>& annexB, uint8_t type) {
    const auto nals = ReadAnnexBNals(annexB);
    return std::any_of(nals.begin(), nals.end(), [type](const auto& nal) {
        return (nal.front() & 0x1f) == type;
    });
}

uint8_t H264SpsProfileIdc(const std::vector<uint8_t>& annexB) {
    const auto nals = ReadAnnexBNals(annexB);
    for (const auto& nal : nals) {
        if ((nal.front() & 0x1f) == 7) {
            Require(nal.size() >= 2, "H.264 SPS must carry a profile idc");
            return nal[1];
        }
    }
    throw std::runtime_error("H.264 access unit does not contain an SPS");
}

bool ContainsH265Nal(const std::vector<uint8_t>& annexB, uint8_t type) {
    const auto nals = ReadAnnexBNals(annexB);
    return std::any_of(nals.begin(), nals.end(), [type](const auto& nal) {
        return ((nal.front() >> 1) & 0x3f) == type;
    });
}

bool ContainsH265Idr(const std::vector<uint8_t>& annexB) {
    return ContainsH265Nal(annexB, 19) || ContainsH265Nal(annexB, 20);
}

void Append(std::vector<uint8_t>& stream, const telestudio::EncodedAccessUnit& unit) {
    Require(!unit.annexB.empty(), "Encoded access unit must not be empty");
    (void)ReadAnnexBNals(unit.annexB);
    stream.insert(stream.end(), unit.annexB.begin(), unit.annexB.end());
}

void WriteEvidence(const std::filesystem::path& path, const std::vector<uint8_t>& stream) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    Require(static_cast<bool>(output), "Could not create evidence stream: " + path.string());
    output.write(reinterpret_cast<const char*>(stream.data()), static_cast<std::streamsize>(stream.size()));
    Require(static_cast<bool>(output), "Could not write evidence stream: " + path.string());
}

void ExerciseH264(const std::filesystem::path& codecBin, const TestFrame& frame) {
    auto encoder = telestudio::CreateX264Encoder(codecBin);
    const telestudio::EncoderSettings settings { telestudio::VideoCodec::H264, 1280, 720, 30, 2000, true, 0 };
    encoder->Open(settings);
    auto first = encoder->Encode(frame.View(1'000'000), true);
    const auto savedFirst = first.annexB;
    auto second = encoder->Encode(frame.View(1'000'007), false);
    auto third = encoder->Encode(frame.View(1'777'777), false);
    Require(first.timestamp100ns == 1'000'000, "H.264 timestamp must match submitted frame");
    Require(second.timestamp100ns == 1'000'007, "Irregular second H.264 timestamp must match submitted frame");
    Require(third.timestamp100ns == 1'777'777, "Irregular third H.264 timestamp must match submitted frame");
    Require(first.keyFrame, "Forced H.264 IDR must be reported as a keyframe");
    Require(ContainsH264Nal(first.annexB, 7), "H.264 IDR must carry SPS");
    Require(ContainsH264Nal(first.annexB, 8), "H.264 IDR must carry PPS");
    Require(first.annexB == savedFirst, "H.264 returned access unit must own its bytes");
    std::vector<uint8_t> stream;
    Append(stream, first); Append(stream, second); Append(stream, third);
    WriteEvidence("work/evidence/media/h264-smoke.264", stream);

    encoder->Reconfigure({ telestudio::VideoCodec::H264, 1280, 720, 24, 1800, false, 1 });
    const auto sameDimensions = encoder->Encode(frame.View(1'999'999), false);
    const auto keyintOne = encoder->Encode(frame.View(2'332'332), false);
    Require(sameDimensions.timestamp100ns == 1'999'999 && sameDimensions.keyFrame &&
            ContainsH264Nal(sameDimensions.annexB, 5) && ContainsH264Nal(sameDimensions.annexB, 7) &&
            ContainsH264Nal(sameDimensions.annexB, 8),
            "Same-dimension H.264 profile/fps/keyint reconfigure must reopen as IDR with SPS/PPS");
    Require(H264SpsProfileIdc(sameDimensions.annexB) == 66,
            "Same-dimension H.264 profile reconfigure must emit Baseline SPS, not retained High SPS");
    Require(keyintOne.timestamp100ns == 2'332'332 && keyintOne.keyFrame && ContainsH264Nal(keyintOne.annexB, 5) &&
            ContainsH264Nal(keyintOne.annexB, 7) && ContainsH264Nal(keyintOne.annexB, 8),
            "Same-dimension H.264 keyint reconfigure must apply keyint=1");

    encoder->Reconfigure({ telestudio::VideoCodec::H264, 1280, 720, 25, 1800, false, 1 });
    const auto fpsChanged = encoder->Encode(frame.View(2'665'665), false);
    Require(fpsChanged.timestamp100ns == 2'665'665 && fpsChanged.keyFrame && ContainsH264Nal(fpsChanged.annexB, 5) &&
            ContainsH264Nal(fpsChanged.annexB, 7) && ContainsH264Nal(fpsChanged.annexB, 8),
            "Same-dimension H.264 fps reconfigure must reopen as IDR with SPS/PPS");

    encoder->Reconfigure({ telestudio::VideoCodec::H264, 640, 360, 30, 1200, false, 0 });
    TestFrame resized;
    resized.width = 640; resized.height = 360;
    resized.y.resize(640 * 360); resized.u.resize(320 * 180); resized.v.resize(320 * 180);
    std::fill(resized.y.begin(), resized.y.end(), 128);
    std::fill(resized.u.begin(), resized.u.end(), 128);
    std::fill(resized.v.begin(), resized.v.end(), 128);
    const auto reconfigured = encoder->Encode(resized.View(2'999'998), false);
    Require(reconfigured.keyFrame && ContainsH264Nal(reconfigured.annexB, 7) && ContainsH264Nal(reconfigured.annexB, 8),
            "Dimension-reconfigured H.264 output must be IDR with SPS/PPS");
    Require(reconfigured.timestamp100ns == 2'999'998, "Dimension-reconfigured H.264 timestamp must match submitted frame");
}

void ExerciseH265(const std::filesystem::path& codecBin, const TestFrame& frame) {
    auto encoder = telestudio::CreateX265Encoder(codecBin);
    const telestudio::EncoderSettings settings { telestudio::VideoCodec::H265, 1280, 720, 30, 2000, false, 0 };
    encoder->Open(settings);
    auto first = encoder->Encode(frame.View(1'000'000), true);
    const auto savedFirst = first.annexB;
    auto second = encoder->Encode(frame.View(1'000'007), false);
    auto third = encoder->Encode(frame.View(1'777'777), false);
    Require(first.timestamp100ns == 1'000'000, "H.265 timestamp must match submitted frame");
    Require(second.timestamp100ns == 1'000'007, "Irregular second H.265 timestamp must match submitted frame");
    Require(third.timestamp100ns == 1'777'777, "Irregular third H.265 timestamp must match submitted frame");
    Require(first.keyFrame, "Forced H.265 IDR must be reported as a keyframe");
    Require(ContainsH265Nal(first.annexB, 32), "H.265 IDR must carry VPS");
    Require(ContainsH265Nal(first.annexB, 33), "H.265 IDR must carry SPS");
    Require(ContainsH265Nal(first.annexB, 34), "H.265 IDR must carry PPS");
    Require(first.annexB == savedFirst, "H.265 returned access unit must own its bytes");
    std::vector<uint8_t> stream;
    Append(stream, first); Append(stream, second); Append(stream, third);
    WriteEvidence("work/evidence/media/h265-smoke.265", stream);

    encoder->Reconfigure({ telestudio::VideoCodec::H265, 1280, 720, 24, 1800, false, 1 });
    const auto sameDimensions = encoder->Encode(frame.View(1'999'999), false);
    const auto keyintOne = encoder->Encode(frame.View(2'332'332), false);
    Require(sameDimensions.timestamp100ns == 1'999'999 && sameDimensions.keyFrame && ContainsH265Idr(sameDimensions.annexB) &&
            ContainsH265Nal(sameDimensions.annexB, 32) && ContainsH265Nal(sameDimensions.annexB, 33) &&
            ContainsH265Nal(sameDimensions.annexB, 34),
            "Same-dimension H.265 fps/keyint reconfigure must reopen as IDR with VPS/SPS/PPS");
    Require(keyintOne.timestamp100ns == 2'332'332 && keyintOne.keyFrame && ContainsH265Idr(keyintOne.annexB) &&
            ContainsH265Nal(keyintOne.annexB, 32) && ContainsH265Nal(keyintOne.annexB, 33) && ContainsH265Nal(keyintOne.annexB, 34),
            "Same-dimension H.265 keyint reconfigure must apply keyint=1");

    encoder->Reconfigure({ telestudio::VideoCodec::H265, 1280, 720, 25, 1800, false, 1 });
    const auto fpsChanged = encoder->Encode(frame.View(2'665'665), false);
    Require(fpsChanged.timestamp100ns == 2'665'665 && fpsChanged.keyFrame && ContainsH265Idr(fpsChanged.annexB) &&
            ContainsH265Nal(fpsChanged.annexB, 32) && ContainsH265Nal(fpsChanged.annexB, 33) && ContainsH265Nal(fpsChanged.annexB, 34),
            "Same-dimension H.265 fps reconfigure must reopen as IDR with VPS/SPS/PPS");

    encoder->Reconfigure({ telestudio::VideoCodec::H265, 640, 360, 30, 1200, false, 0 });
    TestFrame resized;
    resized.width = 640; resized.height = 360;
    resized.y.resize(640 * 360); resized.u.resize(320 * 180); resized.v.resize(320 * 180);
    std::fill(resized.y.begin(), resized.y.end(), 128);
    std::fill(resized.u.begin(), resized.u.end(), 128);
    std::fill(resized.v.begin(), resized.v.end(), 128);
    const auto reconfigured = encoder->Encode(resized.View(2'999'998), false);
    Require(reconfigured.keyFrame && ContainsH265Nal(reconfigured.annexB, 32) &&
            ContainsH265Nal(reconfigured.annexB, 33) && ContainsH265Nal(reconfigured.annexB, 34),
            "Dimension-reconfigured H.265 output must be IDR with VPS/SPS/PPS");
    Require(reconfigured.timestamp100ns == 2'999'998, "Dimension-reconfigured H.265 timestamp must match submitted frame");
}

} // namespace

int main() {
    try {
        const std::filesystem::path codecBin = TELESTUDIO_CODEC_BIN_DIR;
        bool missingDllRejected = false;
        try { (void)telestudio::CreateX264Encoder(codecBin / "missing-x264.dll"); }
        catch (const std::exception&) { missingDllRejected = true; }
        Require(missingDllRejected, "Missing codec DLL must fail before opening an encoder");

        bool missingExportRejected = false;
        try { (void)telestudio::CreateX264Encoder(codecBin / "libx265.dll"); }
        catch (const std::exception&) { missingExportRejected = true; }
        Require(missingExportRejected, "DLL without the pinned x264 export must be rejected");

        x265_api wrongAbi {};
        wrongAbi.api_major_version = X265_MAJOR_VERSION;
        wrongAbi.api_build_number = X265_BUILD - 1;
        wrongAbi.sizeof_param = sizeof(x265_param);
        wrongAbi.sizeof_picture = sizeof(x265_picture);
        wrongAbi.bit_depth = 8;
        Require(!telestudio::detail::IsPinnedX265Api(&wrongAbi), "x265 ABI build mismatch must be rejected before open");

        VerifyAnnexBFixtures();
        VerifyEncoderPtsSequenceStartsAtZeroAndAdvances();
        VerifyEncoderPtsSequenceResetRestartsAtZero();

        const TestFrame frame;
        ExerciseH264(codecBin, frame);
        ExerciseH265(codecBin, frame);
        std::cout << "media_encoder_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "media_encoder_tests: " << error.what() << '\n';
        return 1;
    }
}
