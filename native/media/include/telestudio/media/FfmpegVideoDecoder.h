#pragma once

#include "telestudio/media/MediaTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct ID3D11Device;

namespace telestudio {

struct FfmpegRuntimePaths {
    std::filesystem::path binDirectory;
};

enum class DecoderPreference {
    PreferD3d11,
    SoftwareOnly,
};

enum class DecodeStatus {
    Produced,
    NeedMoreInput,
    AwaitingRandomAccess,
    DecodeError,
};

struct DecodedBgraFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    int64_t timestamp100ns = 0;
    std::vector<uint8_t> bytes;
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::NeedMoreInput;
    std::vector<DecodedBgraFrame> frames;
    bool keyframeRequested = false;
    bool usedSoftware = false;
};

namespace detail {

enum class BackendResultKind {
    Produced,
    NeedMoreInput,
    ParserNoOutput,
    ReceiveEagain,
    BitstreamError,
    HardwareFailure,
};

struct BackendDecodeResult {
    BackendResultKind kind = BackendResultKind::NeedMoreInput;
    std::vector<DecodedBgraFrame> frames;
};

class IDecoderBackend {
public:
    virtual ~IDecoderBackend() = default;
    virtual BackendDecodeResult Decode(std::span<const uint8_t> accessUnit, int64_t timestamp100ns) = 0;
    virtual ID3D11Device* MediaDevice() const noexcept = 0;
};

class IDecoderBackendFactory {
public:
    virtual ~IDecoderBackendFactory() = default;
    virtual bool IsAvailable(VideoCodec codec) const noexcept = 0;
    virtual bool IsD3d11Available(VideoCodec codec) const noexcept = 0;
    virtual std::unique_ptr<IDecoderBackend> Open(VideoCodec codec, int width, int height, bool hardware) = 0;
};

class IHardwareFramePipeline {
public:
    virtual ~IHardwareFramePipeline() = default;
    virtual bool TransferToSoftware() = 0;
    virtual std::optional<DecodedBgraFrame> ScaleSoftwareToBgra() = 0;
    virtual void UnrefSoftware() noexcept = 0;
    virtual void UnrefHardware() noexcept = 0;
};

BackendDecodeResult RunHardwareFramePipeline(IHardwareFramePipeline& pipeline);

struct FfmpegRuntimeContract {
    unsigned avutilMajor = 0;
    unsigned avcodecMajor = 0;
    unsigned swscaleMajor = 0;
    std::string packagePath;
    std::vector<std::string> missingSymbols;
};

void ValidateFfmpegRuntimeContract(const FfmpegRuntimeContract& contract);

} // namespace detail

class FfmpegVideoDecoder {
public:
    FfmpegVideoDecoder();
    explicit FfmpegVideoDecoder(FfmpegRuntimePaths paths,
                                DecoderPreference preference = DecoderPreference::PreferD3d11);
    explicit FfmpegVideoDecoder(std::shared_ptr<detail::IDecoderBackendFactory> factory,
                                DecoderPreference preference = DecoderPreference::PreferD3d11);
    ~FfmpegVideoDecoder();
    FfmpegVideoDecoder(FfmpegVideoDecoder&&) noexcept;
    FfmpegVideoDecoder& operator=(FfmpegVideoDecoder&&) noexcept;
    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    static bool IsAvailable(VideoCodec codec);
    static bool IsAvailable(VideoCodec codec, const FfmpegRuntimePaths& paths);
    static bool IsD3d11Available(VideoCodec codec);
    static bool IsD3d11Available(VideoCodec codec, const FfmpegRuntimePaths& paths);
    bool IsD3d11Available() const noexcept;

    void Open(VideoCodec codec, int width, int height);
    DecodeResult Decode(std::span<const uint8_t> accessUnit, int64_t timestamp100ns);
    ID3D11Device* MediaDevice() const noexcept; // borrowed until reopen, fallback, or destruction

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace telestudio
