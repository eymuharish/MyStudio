#include "telestudio/media/MediaFoundationMp4Sink.h"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Trace {
    std::vector<std::string> calls;
    std::vector<std::thread::id> threads;
    telestudio::MfSinkPlan plan;
    std::string failAt;
    int releases = 0;
    std::vector<telestudio::RecordingVideoSample> videos;
};
class FakeBackend final : public telestudio::IMfSinkBackend {
public:
    explicit FakeBackend(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {}
    void Call(std::string value) {
        trace_->calls.push_back(value);
        trace_->threads.push_back(std::this_thread::get_id());
        if (trace_->failAt == value) throw std::runtime_error("injected MF failure");
    }
    void Startup() override { Call("startup"); }
    void Create(const std::filesystem::path&, const telestudio::MfSinkPlan& plan) override { trace_->plan = plan; Call("create"); }
    void AddVideoStream(const telestudio::MfSinkPlan&) override { Call("add-video"); }
    void SetVideoInput(const telestudio::MfSinkPlan&) override { Call("set-video"); }
    void AddAudioStream(const telestudio::MfSinkPlan&) override { Call("add-audio"); }
    void SetAudioInput(const telestudio::MfSinkPlan&) override { Call("set-audio"); }
    void BeginWriting() override { Call("begin"); }
    void WriteVideo(const telestudio::RecordingVideoSample& sample) override {
        Call("write-video");
        trace_->videos.push_back(sample);
    }
    void WriteAudio(const telestudio::RecordingAudioSample&) override { Call("write-audio"); }
    void Finalize() override { Call("finalize"); }
    void ReleaseAndShutdown() noexcept override {
        trace_->calls.push_back("release-shutdown");
        trace_->threads.push_back(std::this_thread::get_id());
        ++trace_->releases;
    }
private: std::shared_ptr<Trace> trace_;
};
}

int main() {
    try {
        const std::vector<std::uint8_t> sequenceHeader = {
            0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc4,
            0,0,1,0x68,0xce,0x3c,0x80
        };
        std::vector<std::uint8_t> bootstrap = sequenceHeader;
        bootstrap.insert(bootstrap.end(), {0,0,0,1,0x65,0xb8});
        const auto parsed = telestudio::ParseH264Bootstrap(bootstrap);
        Require(parsed.valid && parsed.hasIdr && parsed.profileIdc == 66,
                "SPS/PPS/IDR bootstrap must be recognized");
        Require(parsed.sequenceHeader == sequenceHeader,
                "accepted SPS/PPS must preserve their original 4/3-byte Annex-B prefixes exactly");
        const std::array<std::uint8_t, 3> ppsStart{0,0,1};
        const auto ppsPrefix = std::search(sequenceHeader.begin() + 4, sequenceHeader.end(),
                                           ppsStart.begin(), ppsStart.end());
        Require(ppsPrefix != sequenceHeader.end(), "fixture must contain a three-byte PPS prefix");
        const std::vector<std::uint8_t> sps(sequenceHeader.begin(), ppsPrefix);
        const std::vector<std::uint8_t> pps(ppsPrefix, sequenceHeader.end());
        const std::vector<std::uint8_t> idr = {0,0,0,1,0x65,0xb8};
        const std::vector<std::uint8_t> truncatedIdr = {0,0,0,1,0x65,0xb0};
        const std::vector<std::uint8_t> pSlice = {0,0,1,0x41,0xe0};
        auto Joined = [](std::initializer_list<std::vector<std::uint8_t>> pieces) {
            std::vector<std::uint8_t> value;
            for (const auto& piece : pieces) value.insert(value.end(), piece.begin(), piece.end());
            return value;
        };
        const std::vector<std::uint8_t> validMinimalVuiSps = {
            0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc8,0x02};
        Require(telestudio::ParseH264Bootstrap(Joined({validMinimalVuiSps, pps, idr})).valid,
                "a structurally complete bounded VUI SPS must remain accepted");
        const std::vector<std::uint8_t> strictBaselineVuiSps = {
            0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x01,0x40,0x16,0xe8,0x40,0x00,
            0x00,0x03,0x00,0x40,0x00,0x00,0x0f,0x23,0xc6,0x0c,0xa8};
        const std::vector<std::uint8_t> strictHighVuiSps = {
            0,0,0,1,0x67,0x64,0x00,0x1f,0xac,0xb4,0x02,0x80,0x2d,0xd0,0x80,
            0x00,0x00,0x03,0x00,0x80,0x00,0x00,0x1e,0x47,0x8c,0x19,0x50};
        const std::vector<std::uint8_t> highPps = {
            0,0,0,1,0x68,0xef,0x3c,0xb0};
        const std::vector<std::uint8_t> alternateBaselinePps = {
            0,0,1,0x68,0xce,0x3e,0x80};
        const auto strictBaseline =
            telestudio::ParseH264Bootstrap(Joined({strictBaselineVuiSps, pps, idr}));
        const auto strictHigh =
            telestudio::ParseH264Bootstrap(Joined({strictHighVuiSps, highPps, idr}));
        Require(strictBaseline.valid && strictBaseline.profileIdc == 66,
                "strict x264 Baseline VUI SPS must be accepted");
        Require(strictHigh.valid && strictHigh.profileIdc == 100,
                "pinned real x264 High VUI SPS/PPS must be accepted");
        Require(telestudio::ParseH264Bootstrap(Joined({sps, alternateBaselinePps, idr})).valid,
                "byte-different same-ID PPS continuity fixture must be structurally valid");
        const std::vector<std::uint8_t> validMinimalHrdSps = {
            0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x01,0x40,0x16,0xe8,0x30,0x0d,
            0x7b,0xdf,0x01};
        Require(telestudio::ParseH264Bootstrap(Joined({validMinimalHrdSps, pps, idr})).valid,
                "bounded minimal NAL-HRD SPS must be accepted");
        Require(!telestudio::ParseH264Bootstrap(Joined({pps, idr})).valid,
                "missing SPS must fail");
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, idr})).valid,
                "missing PPS must fail");
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, pps, pSlice})).valid,
                "missing IDR must fail");
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, pps, truncatedIdr})).valid,
                "slice truncated before pic_parameter_set_id must fail");
        const std::vector<std::uint8_t> truncatedAfterFirstMb = {0,0,0,1,0x65,0x80};
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, pps, truncatedAfterFirstMb})).valid,
                "slice truncated after first_mb_in_slice must fail");
        const std::vector<std::uint8_t> unknownPpsIdr = {0,0,0,1,0x65,0xb4};
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, pps, unknownPpsIdr})).valid,
                "slice pic_parameter_set_id must reference an admitted PPS");
        const std::vector<std::uint8_t> truncatedSps = {0,0,0,1,0x67,0x42,0xc0,0x1f,0x80};
        const std::vector<std::uint8_t> truncatedPps = {0,0,1,0x68,0xc0};
        const std::vector<std::uint8_t> missingVuiPayloadSps = {
            0,0,0,1,0x67,0x42,0xc0,0x1f,0xfb,0xd0};
        const std::vector<std::uint8_t> missingTrailingBitsPps = {
            0,0,1,0x68,0xce,0x3c};
        Require(!telestudio::ParseH264Bootstrap(Joined({truncatedSps, pps, idr})).valid,
                "SPS truncated after seq_parameter_set_id must fail");
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, truncatedPps, idr})).valid,
                "PPS truncated after its IDs must fail");
        Require(!telestudio::ParseH264Bootstrap(Joined({missingVuiPayloadSps, pps, idr})).valid,
                "SPS declaring VUI without its payload must fail closed");
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, missingTrailingBitsPps, idr})).valid,
                "PPS without rbsp_stop_one_bit must fail closed");
        const std::vector<std::vector<std::uint8_t>> invalidStrictSps = {
            {0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc0},
            {0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc5},
            {0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc4,0x80},
            {0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc8,0x40,0x00},
            {0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc8,0x30},
            {0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x02,0x80,0x2d,0xc8,0x06},
            {0,0,0,1,0x67,0x64,0x00,0x1f,0xac,0xb4,0x02,0x80,0x2d,0xd0,0x80,
             0x00,0x00,0x03,0x00,0x80,0x00,0x00,0x1e,0x47,0x8c,0x19}
        };
        for (const auto& invalidSps : invalidStrictSps)
            Require(!telestudio::ParseH264Bootstrap(Joined({invalidSps, pps, idr})).valid,
                    "malformed VUI or SPS rbsp trailing bits must fail closed");
        const std::vector<std::vector<std::uint8_t>> invalidStrictPps = {
            {0,0,1,0x68,0xce,0x3c},
            {0,0,1,0x68,0xce,0x3c,0x00},
            {0,0,1,0x68,0xce,0x3c,0xc0},
            {0,0,1,0x68,0xce,0x3c,0x80,0x80},
            {0,0,0,1,0x68,0xef,0x3c,0x80}
        };
        for (std::size_t index = 0; index < invalidStrictPps.size(); ++index) {
            const auto& linkedSps = index == 4 ? strictHighVuiSps : sps;
            if (telestudio::ParseH264Bootstrap(Joined({linkedSps, invalidStrictPps[index], idr})).valid)
                throw std::runtime_error("malformed PPS accepted at vector " + std::to_string(index));
        }
        auto forbidden = bootstrap;
        forbidden[4] |= 0x80;
        Require(!telestudio::ParseH264Bootstrap(forbidden).valid,
                "forbidden_zero_bit must fail closed");
        auto trailingStartCode = bootstrap;
        trailingStartCode.insert(trailingStartCode.end(), {0,0,1});
        Require(!telestudio::ParseH264Bootstrap(trailingStartCode).valid,
                "trailing empty NAL/start code must fail");
        auto conflictingSps = sps;
        conflictingSps.back() ^= 1;
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, conflictingSps, pps, idr})).valid,
                "conflicting SPS must fail");
        const std::vector<std::uint8_t> wrongSpsPps = {0,0,1,0x68,0xa0};
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, wrongSpsPps, idr})).valid,
                "PPS referencing a missing SPS id must fail");
        const std::vector<std::uint8_t> secondAu = {0,0,1,0x41,0xe0};
        Require(!telestudio::ParseH264Bootstrap(Joined({bootstrap, secondAu})).valid,
                "a second first_mb_in_slice=0 VCL must fail as multiple AUs");
        const std::vector<std::uint8_t> laterSlice = {0,0,1,0x41,0x58};
        Require(telestudio::ParseH264Bootstrap(Joined({bootstrap, laterSlice})).valid,
                "a later nonzero first_mb_in_slice may remain in the same AU");
        const std::vector<std::uint8_t> aud = {0,0,1,0x09,0xf0};
        Require(!telestudio::ParseH264Bootstrap(Joined({bootstrap, aud})).valid,
                "AUD after VCL must fail as a second AU boundary");
        const std::vector<std::uint8_t> bSlice = {0,0,1,0x41,0xa8};
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, pps, idr, bSlice})).valid,
                "B-slice input must fail closed");
        Require(!telestudio::ParseH264Bootstrap({{0x67,0x42,0xc0,0x1f,0x80}}).valid,
                "Annex-B without an initial delimiter must fail");
        Require(!telestudio::ParseH264Bootstrap(
                    {{0,0,1,0,0,1,0x65,0xb0}}).valid,
                "an empty NAL between delimiters must fail");
        auto reservedNal = bootstrap;
        reservedNal.insert(reservedNal.end(), {0,0,1,0x78,0x80});
        Require(!telestudio::ParseH264Bootstrap(reservedNal).valid,
                "reserved/invalid NAL type must fail");
        Require(telestudio::ParseH264Bootstrap(Joined({sps, sps, pps, pps, idr})).valid,
                "byte-identical duplicate parameter sets must deduplicate deterministically");
        auto conflictingPps = pps;
        conflictingPps.back() = 0xd0;
        Require(!telestudio::ParseH264Bootstrap(Joined({sps, pps, conflictingPps, idr})).valid,
                "conflicting PPS must fail");
        Require(telestudio::ParseH264Bootstrap(Joined({aud, bootstrap})).valid,
                "one AUD before VCL may introduce the single accepted AU");
        Require(!telestudio::ParseH264Bootstrap(Joined({aud, aud, bootstrap})).valid,
                "AUD plus AUD plus bootstrap must fail as duplicate AU delimiters");
        Require(!telestudio::ParseH264Bootstrap(Joined({aud, sps, aud, pps, idr})).valid,
                "an AUD repeated inside one bootstrap must fail regardless of placement");
        for (const std::uint8_t type : {17u, 18u, 22u, 23u}) {
            auto withReserved = bootstrap;
            withReserved.insert(withReserved.end(), {0,0,1,static_cast<std::uint8_t>(0x60u | type),0x80});
            Require(!telestudio::ParseH264Bootstrap(withReserved).valid,
                    "reserved NAL types 17/18/22/23 must fail closed");
        }
        for (const std::uint8_t header : {0x42u, 0x43u, 0x44u, 0x53u}) {
            auto withUnsupportedVcl = bootstrap;
            withUnsupportedVcl.insert(withUnsupportedVcl.end(), {0,0,1,header,0xa8});
            Require(!telestudio::ParseH264Bootstrap(withUnsupportedVcl).valid,
                    "unsupported VCL types 2/3/4/19 must fail closed instead of being ignored");
        }
        auto malformedEscapeSps = sps;
        malformedEscapeSps.insert(malformedEscapeSps.end(), {0,0,3});
        Require(!telestudio::ParseH264Bootstrap(Joined({malformedEscapeSps, pps, idr})).valid,
                "truncated emulation-prevention sequence must fail");
        const std::vector<std::uint8_t> malformedEscapeSlice = {0,0,1,0x41,0,0,3};
        Require(!telestudio::ParseH264Bootstrap(Joined({bootstrap, malformedEscapeSlice})).valid,
                "malformed emulation-prevention data in a VCL RBSP must fail");
        auto unsupportedProfile = sps;
        unsupportedProfile[5] = 77;
        Require(!telestudio::ParseH264Bootstrap(Joined({unsupportedProfile, pps, idr})).valid,
                "unsupported SPS profile must fail closed");
        Require(telestudio::MediaFoundationMp4Sink::SuccessfulPathUsesFlush() == false &&
                    telestudio::MediaFoundationMp4Sink::SuccessfulPathUsesNotifyEndOfSegment() == false,
                "successful finalization must call neither Flush nor NotifyEndOfSegment");
        auto trace = std::make_shared<Trace>();
        telestudio::MediaFoundationMp4Sink sink([trace] { return std::make_unique<FakeBackend>(trace); });
        sink.Open(L"synthetic.part", {320, 180, 30, 1000, false, parsed.sequenceHeader}, true);
        sink.WriteVideo({bootstrap, 0, 333'333, true, false});
        sink.WriteAudio({std::vector<std::int16_t>(960, 1), 0, 200'000, false});
        sink.Finalize();
        const std::vector<std::string> exact = {"startup", "create", "add-video", "set-video",
            "add-audio", "set-audio", "begin", "write-video", "write-audio", "finalize",
            "release-shutdown"};
        Require(trace->calls == exact, "injected MF seam must observe the exact success order");
        Require(std::all_of(trace->threads.begin(), trace->threads.end(), [&](auto id) {
                    return id == std::this_thread::get_id();
                }), "all MF/COM/writer operations must stay on one worker thread");
        Require(trace->plan.disableThrottling && trace->plan.mpeg4Container &&
                    trace->plan.video.width == 320 && trace->plan.video.height == 180 &&
                    trace->plan.video.fps == 30 && trace->plan.video.bitrateKbps == 1000 &&
                    trace->plan.audioSampleRate == 48000 && trace->plan.audioChannels == 1 &&
                    trace->plan.audioBitsPerSample == 16 && trace->plan.audioAverageBytesPerSecond == 12000 &&
                    trace->plan.aacProfileLevel == 0x29 && trace->plan.pcmAverageBytesPerSecond == 96000,
                "MF seam must carry every frozen video/AAC/PCM/container attribute");
        Require(std::find(trace->calls.begin(), trace->calls.end(), "flush") == trace->calls.end() &&
                    std::find(trace->calls.begin(), trace->calls.end(), "notify-end") == trace->calls.end(),
                "executed success path must observe zero Flush/Notify calls");

        for (const auto& accepted : std::vector<std::pair<telestudio::H264BootstrapInfo, bool>>{
                 {strictBaseline, false}, {strictHigh, true}}) {
            auto acceptedTrace = std::make_shared<Trace>();
            telestudio::MediaFoundationMp4Sink acceptedSink(
                [acceptedTrace] { return std::make_unique<FakeBackend>(acceptedTrace); });
            acceptedSink.Open(L"strict-profile.part",
                {1280, 720, 30, 2500, accepted.second, accepted.first.sequenceHeader}, false);
            acceptedSink.Finalize();
            Require(acceptedTrace->plan.video.highProfile == accepted.second &&
                        acceptedTrace->releases == 1,
                    "strict Baseline/High sequence headers must configure matching MF profiles");
        }

        auto mismatchTrace = std::make_shared<Trace>();
        bool profileMismatchRejected = false;
        try {
            telestudio::MediaFoundationMp4Sink mismatch(
                [mismatchTrace] { return std::make_unique<FakeBackend>(mismatchTrace); });
            mismatch.Open(L"profile-mismatch.part", {320, 180, 30, 1000, true,
                                                       parsed.sequenceHeader}, false);
        } catch (const std::invalid_argument&) { profileMismatchRejected = true; }
        Require(profileMismatchRejected && mismatchTrace->calls.empty(),
                "SPS/settings profile mismatch must fail before any backend call");
        std::vector<std::vector<std::uint8_t>> invalidHeaders{
            Joined({missingVuiPayloadSps, pps}), Joined({sps, missingTrailingBitsPps})};
        for (const auto& invalidSps : invalidStrictSps)
            invalidHeaders.push_back(Joined({invalidSps, pps}));
        for (std::size_t index = 0; index < invalidStrictPps.size(); ++index)
            invalidHeaders.push_back(Joined({index == 4 ? strictHighVuiSps : sps,
                                             invalidStrictPps[index]}));
        for (const auto& invalidHeader : invalidHeaders) {
            auto invalidHeaderTrace = std::make_shared<Trace>();
            bool invalidHeaderRejected = false;
            try {
                telestudio::MediaFoundationMp4Sink invalidHeaderSink(
                    [invalidHeaderTrace] { return std::make_unique<FakeBackend>(invalidHeaderTrace); });
                invalidHeaderSink.Open(L"invalid-sequence-header.part",
                    {320, 180, 30, 1000, false, invalidHeader}, false);
            } catch (const std::invalid_argument&) { invalidHeaderRejected = true; }
            Require(invalidHeaderRejected && invalidHeaderTrace->calls.empty(),
                    "truncated SPS/PPS sequence header must fail before backend Startup/Create");
        }

        for (const auto& validAu : std::vector<std::vector<std::uint8_t>>{
                 pSlice, Joined({pSlice, laterSlice})}) {
            auto validTrace = std::make_shared<Trace>();
            telestudio::MediaFoundationMp4Sink valid(
                [validTrace] { return std::make_unique<FakeBackend>(validTrace); });
            valid.Open(L"synthetic-valid-au.part", {320, 180, 30, 1000, false,
                                                     parsed.sequenceHeader}, false);
            valid.WriteVideo({validAu, 0, 333'333, false, false});
            valid.Finalize();
            Require(std::count(validTrace->calls.begin(), validTrace->calls.end(), "write-video") == 1,
                    "one valid P or same-picture multi-slice AU must reach the backend unchanged");
        }

        std::vector<std::uint8_t> spsWithThreeBytePrefix{0,0,1};
        spsWithThreeBytePrefix.insert(spsWithThreeBytePrefix.end(), sps.begin() + 4, sps.end());
        std::vector<std::uint8_t> ppsWithFourBytePrefix{0,0,0,1};
        ppsWithFourBytePrefix.insert(ppsWithFourBytePrefix.end(), pps.begin() + 3, pps.end());
        const auto repeatedHeadersDifferentPrefixes =
            Joined({spsWithThreeBytePrefix, ppsWithFourBytePrefix, idr});
        auto repeatedTrace = std::make_shared<Trace>();
        telestudio::MediaFoundationMp4Sink repeated(
            [repeatedTrace] { return std::make_unique<FakeBackend>(repeatedTrace); });
        repeated.Open(L"synthetic-repeated-headers.part",
                      {320, 180, 30, 1000, false, parsed.sequenceHeader}, false);
        repeated.WriteVideo({repeatedHeadersDifferentPrefixes, 0, 333'333, true, false});
        repeated.Finalize();
        Require(repeatedTrace->videos.size() == 1 &&
                    repeatedTrace->videos.front().annexB == repeatedHeadersDifferentPrefixes,
                "byte-identical admitted SPS/PPS must ignore delimiter width and forward unchanged");

        const std::vector<std::uint8_t> reserved17 = {0,0,1,0x71,0x80};
        const std::vector<std::uint8_t> partitionB = {0,0,1,0x42,0xa8};
        for (const auto& invalidAu : std::vector<std::pair<std::vector<std::uint8_t>, bool>>{
                  {{0,0,1,0x65}, true},
                  {truncatedAfterFirstMb, true},
                  {truncatedIdr, true},
                  {unknownPpsIdr, true},
                  {Joined({bootstrap, secondAu}), true},
                  {bSlice, false},
                  {Joined({pSlice, reserved17}), false},
                  {Joined({pSlice, partitionB}), false},
                  {Joined({aud, aud, pSlice}), false},
                  {malformedEscapeSlice, false},
                  {Joined({missingVuiPayloadSps, pps, idr}), true},
                  {Joined({sps, missingTrailingBitsPps, idr}), true},
                  {Joined({strictHighVuiSps, highPps, idr}), true},
                  {Joined({sps, alternateBaselinePps, idr}), true},
                  {sequenceHeader, false},
                  {pSlice, true}}) {
            auto invalidTrace = std::make_shared<Trace>();
            telestudio::MediaFoundationMp4Sink invalid(
                [invalidTrace] { return std::make_unique<FakeBackend>(invalidTrace); });
            invalid.Open(L"synthetic-invalid-au.part", {320, 180, 30, 1000, false,
                                                         parsed.sequenceHeader}, false);
            bool rejected = false;
            try { invalid.WriteVideo({invalidAu.first, 0, 333'333, invalidAu.second, false}); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected && invalidTrace->releases == 1 &&
                        invalidTrace->calls.back() == "release-shutdown" &&
                        std::find(invalidTrace->calls.begin(), invalidTrace->calls.end(), "write-video") ==
                            invalidTrace->calls.end(),
                    "malformed/multiple/B-slice AU must close before any backend write");
            const auto callsAfterRejection = invalidTrace->calls.size();
            bool laterVideoRejected = false;
            bool laterAudioRejected = false;
            bool laterFinalizeRejected = false;
            try { invalid.WriteVideo({pSlice, 333'333, 333'333, false, false}); }
            catch (const std::invalid_argument&) { laterVideoRejected = true; }
            try { invalid.WriteAudio({std::vector<std::int16_t>(960, 1), 0, 200'000, false}); }
            catch (const std::invalid_argument&) { laterAudioRejected = true; }
            try { invalid.Finalize(); }
            catch (const std::logic_error&) { laterFinalizeRejected = true; }
            Require(laterVideoRejected && laterAudioRejected && laterFinalizeRejected &&
                        invalidTrace->calls.size() == callsAfterRejection,
                    "each rejected H.264 vector must terminally forbid backend continuation");
        }

        for (const bool invalidVideo : {true, false}) {
            auto invalidTrace = std::make_shared<Trace>();
            telestudio::MediaFoundationMp4Sink invalid(
                [invalidTrace] { return std::make_unique<FakeBackend>(invalidTrace); });
            invalid.Open(L"synthetic-invalid.part", {320, 180, 30, 1000, false,
                                                      parsed.sequenceHeader}, true);
            bool rejected = false;
            try {
                if (invalidVideo)
                    invalid.WriteVideo({bootstrap, 0, 333'333, false, false});
                else
                    invalid.WriteAudio({std::vector<std::int16_t>(959, 1), 0, 200'000, false});
            } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected && invalidTrace->releases == 1 &&
                        invalidTrace->calls.back() == "release-shutdown",
                    "every invalid post-Open sample must close MF immediately");
            const auto callsAfterInvalid = invalidTrace->calls.size();
            bool videoContinuationRejected = false;
            bool audioContinuationRejected = false;
            bool finalizeContinuationRejected = false;
            try { invalid.WriteVideo({bootstrap, 333'333, 333'333, true, false}); }
            catch (const std::invalid_argument&) { videoContinuationRejected = true; }
            try { invalid.WriteAudio({std::vector<std::int16_t>(960, 1), 200'000, 200'000, false}); }
            catch (const std::invalid_argument&) { audioContinuationRejected = true; }
            try { invalid.Finalize(); }
            catch (const std::logic_error&) { finalizeContinuationRejected = true; }
            Require(videoContinuationRejected && audioContinuationRejected &&
                        finalizeContinuationRejected &&
                        invalidTrace->calls.size() == callsAfterInvalid,
                    "invalid sample failure must terminally reject continuation without backend calls");
            Require(std::all_of(invalidTrace->threads.begin(), invalidTrace->threads.end(),
                        [&](auto id) { return id == std::this_thread::get_id(); }),
                    "invalid sample cleanup must remain on the owning worker thread");
        }

        for (const std::string stage : {"startup", "create", "add-video", "set-video",
                                        "add-audio", "set-audio", "begin"}) {
            auto failedTrace = std::make_shared<Trace>();
            failedTrace->failAt = stage;
            bool rejected = false;
            {
                telestudio::MediaFoundationMp4Sink failed(
                    [failedTrace] { return std::make_unique<FakeBackend>(failedTrace); });
                try {
                    failed.Open(L"synthetic.part", {320, 180, 30, 1000, false,
                                                     parsed.sequenceHeader}, true);
                } catch (const std::runtime_error&) { rejected = true; }
            }
            Require(rejected && failedTrace->calls.back() == "release-shutdown" &&
                        failedTrace->releases == 1,
                    "every MF Open failure must release/shutdown exactly once");
            Require(std::count(failedTrace->calls.begin(), failedTrace->calls.end(), stage) == 1,
                    "the injected failing stage must execute exactly once");
        }
        for (const std::string stage : {"write-video", "write-audio", "finalize"}) {
            auto failedTrace = std::make_shared<Trace>();
            failedTrace->failAt = stage;
            bool rejected = false;
            {
                telestudio::MediaFoundationMp4Sink failed(
                    [failedTrace] { return std::make_unique<FakeBackend>(failedTrace); });
                failed.Open(L"synthetic.part", {320, 180, 30, 1000, false,
                                                 parsed.sequenceHeader}, true);
                try {
                    if (stage == "write-video")
                        failed.WriteVideo({bootstrap, 0, 333'333, true, false});
                    else if (stage == "write-audio")
                        failed.WriteAudio({std::vector<std::int16_t>(960, 1), 0, 200'000, false});
                    else
                        failed.Finalize();
                } catch (const std::runtime_error&) { rejected = true; }
                Require(rejected && failedTrace->calls.back() == "release-shutdown" &&
                            failedTrace->releases == 1,
                        "write/finalize failure must release/shutdown immediately");
                const auto callsAfterFailure = failedTrace->calls.size();
                bool videoContinuationRejected = false;
                bool audioContinuationRejected = false;
                bool finalizeContinuationRejected = false;
                try { failed.WriteVideo({bootstrap, 333'333, 333'333, true, false}); }
                catch (const std::invalid_argument&) { videoContinuationRejected = true; }
                try { failed.WriteAudio({std::vector<std::int16_t>(960, 1), 200'000, 200'000, false}); }
                catch (const std::invalid_argument&) { audioContinuationRejected = true; }
                try { failed.Finalize(); }
                catch (const std::logic_error&) { finalizeContinuationRejected = true; }
                Require(videoContinuationRejected && audioContinuationRejected &&
                            finalizeContinuationRejected &&
                            failedTrace->calls.size() == callsAfterFailure,
                        "failed MF sink must terminally reject every continuation");
                Require(std::all_of(failedTrace->threads.begin(), failedTrace->threads.end(),
                            [&](auto id) { return id == std::this_thread::get_id(); }),
                        "failure cleanup must run immediately on the owning worker thread");
            }
            Require(rejected && failedTrace->calls.back() == "release-shutdown" &&
                        failedTrace->releases == 1,
                    "destruction after a failed MF write/finalize must not clean up twice");
        }
        bool reused = false;
        try { sink.Open(L"again.part", {320, 180, 30, 1000, false, parsed.sequenceHeader}, false); }
        catch (const std::invalid_argument&) { reused = true; }
        Require(reused, "the MF sink lifecycle must be explicitly single-use");
        std::cout << "media_foundation_mp4_sink_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "media_foundation_mp4_sink_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
