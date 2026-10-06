#include "telestudio/media/FfmpegVideoDecoder.h"

#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

std::vector<uint8_t> RandomAccess(telestudio::VideoCodec codec) {
    if (codec == telestudio::VideoCodec::H264) {
        return {0,0,1,0x67,0x42, 0,0,1,0x68,0xce, 0,0,1,0x65,0x01};
    }
    return {0,0,1,static_cast<uint8_t>(32<<1),1, 0,0,1,static_cast<uint8_t>(33<<1),1,
            0,0,1,static_cast<uint8_t>(34<<1),1, 0,0,1,static_cast<uint8_t>(19<<1),1};
}

std::vector<uint8_t> NonKey(telestudio::VideoCodec codec) {
    return codec == telestudio::VideoCodec::H264
        ? std::vector<uint8_t>{0,0,1,0x61,0x01}
        : std::vector<uint8_t>{0,0,1,static_cast<uint8_t>(1<<1),1,0x01};
}

telestudio::DecodedBgraFrame Frame(int marker, int64_t timestamp) {
    return {1, 1, 4, timestamp, std::vector<uint8_t>(4, static_cast<uint8_t>(marker))};
}

struct FakeState {
    std::deque<telestudio::detail::BackendDecodeResult> hardwareResults;
    std::deque<telestudio::detail::BackendDecodeResult> softwareResults;
    int hardwareOpens = 0;
    int softwareOpens = 0;
    int hardwareDecodes = 0;
    int softwareDecodes = 0;
    std::vector<std::string> lifecycle;
};

class FakeBackend final : public telestudio::detail::IDecoderBackend {
public:
    FakeBackend(std::shared_ptr<FakeState> state, bool hardware) : state_(std::move(state)), hardware_(hardware) {}
    ~FakeBackend() override { state_->lifecycle.push_back(hardware_ ? "hardware-backend-destroy" : "software-backend-destroy"); }

    telestudio::detail::BackendDecodeResult Decode(std::span<const uint8_t>, int64_t) override {
        auto& queue = hardware_ ? state_->hardwareResults : state_->softwareResults;
        hardware_ ? ++state_->hardwareDecodes : ++state_->softwareDecodes;
        if (queue.empty()) return {telestudio::detail::BackendResultKind::NeedMoreInput, {}};
        auto result = std::move(queue.front()); queue.pop_front(); return result;
    }
    ID3D11Device* MediaDevice() const noexcept override { return hardware_ ? reinterpret_cast<ID3D11Device*>(1) : nullptr; }

private:
    std::shared_ptr<FakeState> state_;
    bool hardware_;
};

class FakeFactory final : public telestudio::detail::IDecoderBackendFactory {
public:
    explicit FakeFactory(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    ~FakeFactory() override { state_->lifecycle.push_back("factory-destroy"); }
    bool IsAvailable(telestudio::VideoCodec) const noexcept override { return true; }
    bool IsD3d11Available(telestudio::VideoCodec) const noexcept override { return true; }
    std::unique_ptr<telestudio::detail::IDecoderBackend> Open(telestudio::VideoCodec, int, int, bool hardware) override {
        hardware ? ++state_->hardwareOpens : ++state_->softwareOpens;
        return std::make_unique<FakeBackend>(state_, hardware);
    }
private:
    std::shared_ptr<FakeState> state_;
};

telestudio::detail::BackendDecodeResult Result(telestudio::detail::BackendResultKind kind,
                                               std::vector<telestudio::DecodedBgraFrame> frames = {}) {
    return {kind, std::move(frames)};
}

void VerifyCorruptRandomAccessDoesNotFallback(telestudio::VideoCodec codec) {
    auto state = std::make_shared<FakeState>();
    state->hardwareResults.push_back(Result(telestudio::detail::BackendResultKind::BitstreamError));
    state->hardwareResults.push_back(Result(telestudio::detail::BackendResultKind::Produced, {Frame(1, 22)}));
    telestudio::FfmpegVideoDecoder decoder(std::make_shared<FakeFactory>(state));
    decoder.Open(codec, 16, 16);
    const auto corrupt = decoder.Decode(RandomAccess(codec), 11);
    Require(corrupt.status == telestudio::DecodeStatus::DecodeError && !corrupt.keyframeRequested &&
            state->softwareOpens == 0 && state->hardwareOpens == 1,
            "Corrupt random-access input must not trigger backend churn");
    const auto next = decoder.Decode(NonKey(codec), 22);
    Require(next.status == telestudio::DecodeStatus::Produced && !next.usedSoftware,
            "Hardware backend remains active after corrupt input");
}

void VerifyNonKeyHardwareFailure(telestudio::VideoCodec codec) {
    auto state = std::make_shared<FakeState>();
    state->hardwareResults.push_back(Result(telestudio::detail::BackendResultKind::HardwareFailure));
    state->softwareResults.push_back(Result(telestudio::detail::BackendResultKind::Produced, {Frame(2, 30)}));
    state->softwareResults.push_back(Result(telestudio::detail::BackendResultKind::Produced, {Frame(3, 50)}));
    telestudio::FfmpegVideoDecoder decoder(std::make_shared<FakeFactory>(state));
    decoder.Open(codec, 16, 16);
    const auto failed = decoder.Decode(NonKey(codec), 30);
    Require(failed.status == telestudio::DecodeStatus::AwaitingRandomAccess && failed.frames.empty() &&
            failed.keyframeRequested && failed.usedSoftware && state->softwareDecodes == 1 && state->softwareOpens == 1,
            "Non-key hardware failure gets exactly one hidden software retry and one keyframe request");
    const auto dropped = decoder.Decode(NonKey(codec), 40);
    Require(dropped.status == telestudio::DecodeStatus::AwaitingRandomAccess && !dropped.keyframeRequested &&
            state->softwareDecodes == 1 && state->softwareOpens == 1,
            "AwaitRandomAccess drops ordinary input without decode or backend churn");
    const auto resumed = decoder.Decode(RandomAccess(codec), 50);
    Require(resumed.status == telestudio::DecodeStatus::Produced && resumed.frames.size() == 1 &&
            state->softwareDecodes == 2 && state->softwareOpens == 1,
            "Complete random-access input resumes the existing sticky software backend");
}

void VerifyKeyHardwareFailureExactlyOnce(telestudio::VideoCodec codec) {
    auto state = std::make_shared<FakeState>();
    state->hardwareResults.push_back(Result(telestudio::detail::BackendResultKind::HardwareFailure));
    state->softwareResults.push_back(Result(telestudio::detail::BackendResultKind::Produced,
                                            {Frame(4, 60), Frame(5, 61)}));
    telestudio::FfmpegVideoDecoder decoder(std::make_shared<FakeFactory>(state));
    decoder.Open(codec, 16, 16);
    const auto result = decoder.Decode(RandomAccess(codec), 60);
    Require(result.status == telestudio::DecodeStatus::Produced && result.frames.size() == 2 &&
            result.keyframeRequested && result.usedSoftware && state->softwareDecodes == 1 && state->softwareOpens == 1,
            "Self-contained key trigger has exactly one controlled software retry and retains every frame");
    Require(result.frames[0].timestamp100ns == 60 && result.frames[1].timestamp100ns == 61 &&
            result.frames[0].bytes.data() != result.frames[1].bytes.data(),
            "Multi-frame result preserves ordered timestamps and independent BGRA ownership");
}

void VerifyNeedMore(telestudio::VideoCodec codec) {
    auto state = std::make_shared<FakeState>();
    state->hardwareResults.push_back(Result(telestudio::detail::BackendResultKind::ParserNoOutput));
    state->hardwareResults.push_back(Result(telestudio::detail::BackendResultKind::ReceiveEagain));
    auto factory = std::make_shared<FakeFactory>(state);
    {
        telestudio::FfmpegVideoDecoder decoder(factory);
        decoder.Open(codec, 16, 16);
        Require(decoder.Decode(NonKey(codec), 1).status == telestudio::DecodeStatus::NeedMoreInput,
                "Parser no-output is normal NeedMoreInput flow");
        Require(decoder.Decode(NonKey(codec), 2).status == telestudio::DecodeStatus::NeedMoreInput,
                "receive EAGAIN is normal NeedMoreInput flow");
    }
    Require(state->lifecycle == std::vector<std::string>{"hardware-backend-destroy"},
            "Decoder backend is destroyed before its factory/runtime owner");
    factory.reset();
    Require(state->lifecycle == std::vector<std::string>({"hardware-backend-destroy", "factory-destroy"}),
            "Factory/runtime unload occurs after backend destruction");
}

class FakeHardwarePipeline final : public telestudio::detail::IHardwareFramePipeline {
public:
    bool transferSucceeds = true;
    bool scaleSucceeds = true;
    std::vector<std::string> events;
    bool TransferToSoftware() override { events.push_back("transfer"); return transferSucceeds; }
    std::optional<telestudio::DecodedBgraFrame> ScaleSoftwareToBgra() override {
        events.push_back("sws-software");
        if (!scaleSucceeds) return std::nullopt;
        return Frame(9, 90);
    }
    void UnrefSoftware() noexcept override { events.push_back("unref-software"); }
    void UnrefHardware() noexcept override { events.push_back("unref-hardware"); }
};

void VerifyHardwareTransferOrdering() {
    FakeHardwarePipeline pipeline;
    const auto success = telestudio::detail::RunHardwareFramePipeline(pipeline);
    Require(success.kind == telestudio::detail::BackendResultKind::Produced && success.frames.size() == 1 &&
            pipeline.events == std::vector<std::string>({"transfer", "sws-software", "unref-software", "unref-hardware"}),
            "NV12 transfer and swscale complete before the hardware frame reference is released");

    FakeHardwarePipeline failed;
    failed.scaleSucceeds = false;
    const auto failure = telestudio::detail::RunHardwareFramePipeline(failed);
    Require(failure.kind == telestudio::detail::BackendResultKind::HardwareFailure && failure.frames.empty() &&
            failed.events == std::vector<std::string>({"transfer", "sws-software", "unref-software", "unref-hardware"}),
            "Conversion failure releases both frames and is classified as hardware failure");
}

void VerifyRuntimeContractRejection() {
    telestudio::detail::FfmpegRuntimeContract valid {61, 63, 10, "C:/pinned", {}};
    telestudio::detail::ValidateFfmpegRuntimeContract(valid);

    auto wrongMajor = valid; wrongMajor.avcodecMajor = 62;
    bool rejected = false;
    try { telestudio::detail::ValidateFfmpegRuntimeContract(wrongMajor); }
    catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("C:/pinned") != std::string::npos &&
                   std::string(error.what()).find("63") != std::string::npos;
    }
    Require(rejected, "Wrong FFmpeg major is rejected with pinned path and expected version");

    auto missing = valid; missing.missingSymbols = {"avcodec_receive_frame"};
    rejected = false;
    try { telestudio::detail::ValidateFfmpegRuntimeContract(missing); }
    catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("avcodec_receive_frame") != std::string::npos &&
                   std::string(error.what()).find("C:/pinned") != std::string::npos;
    }
    Require(rejected, "Missing FFmpeg function-table symbol is rejected with precise path and symbol");
}

} // namespace

int main() {
    try {
        for (const auto codec : {telestudio::VideoCodec::H264, telestudio::VideoCodec::H265}) {
            VerifyCorruptRandomAccessDoesNotFallback(codec);
            VerifyNonKeyHardwareFailure(codec);
            VerifyKeyHardwareFailureExactlyOnce(codec);
            VerifyNeedMore(codec);
        }
        VerifyHardwareTransferOrdering();
        VerifyRuntimeContractRejection();
        std::cout << "media_decoder_state_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "media_decoder_state_tests: " << error.what() << '\n';
        return 1;
    }
}
