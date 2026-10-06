#include "telestudio/media/FfmpegVideoDecoder.h"

#include <windows.h>
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace telestudio {

detail::BackendDecodeResult detail::RunHardwareFramePipeline(IHardwareFramePipeline& pipeline) {
    if (!pipeline.TransferToSoftware()) {
        pipeline.UnrefSoftware();
        pipeline.UnrefHardware();
        return {BackendResultKind::HardwareFailure, {}};
    }
    auto frame = pipeline.ScaleSoftwareToBgra();
    pipeline.UnrefSoftware();
    pipeline.UnrefHardware();
    if (!frame) return {BackendResultKind::HardwareFailure, {}};
    std::vector<DecodedBgraFrame> frames;
    frames.push_back(std::move(*frame));
    return {BackendResultKind::Produced, std::move(frames)};
}

void detail::ValidateFfmpegRuntimeContract(const FfmpegRuntimeContract& contract) {
    if (!contract.missingSymbols.empty()) {
        throw std::runtime_error("Pinned FFmpeg runtime " + contract.packagePath +
                                 " is missing required symbol " + contract.missingSymbols.front());
    }
    if (contract.avutilMajor != 61 || contract.avcodecMajor != 63 || contract.swscaleMajor != 10) {
        throw std::runtime_error("Pinned FFmpeg runtime " + contract.packagePath +
                                 " major mismatch; expected avutil=61 avcodec=63 swscale=10, got " +
                                 std::to_string(contract.avutilMajor) + "/" +
                                 std::to_string(contract.avcodecMajor) + "/" +
                                 std::to_string(contract.swscaleMajor));
    }
}

namespace {

#ifndef TELESTUDIO_DEFAULT_CODEC_BIN_DIR
#define TELESTUDIO_DEFAULT_CODEC_BIN_DIR L"work/build/video-codecs/install/bin"
#endif

class Module {
public:
    Module() = default;
    Module(const std::filesystem::path& path, DLL_DIRECTORY_COOKIE cookie) : path_(std::filesystem::absolute(path)) {
        if (cookie == nullptr) throw std::runtime_error("Could not establish scoped DLL directory: " + path_.parent_path().string());
        module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_ == nullptr) throw std::runtime_error("Could not load pinned FFmpeg DLL: " + path_.string());
    }
    ~Module() { if (module_ != nullptr) FreeLibrary(module_); }
    Module(Module&& other) noexcept : module_(std::exchange(other.module_, nullptr)), path_(std::move(other.path_)) {}
    Module& operator=(Module&& other) noexcept {
        if (this != &other) {
            if (module_ != nullptr) FreeLibrary(module_);
            module_ = std::exchange(other.module_, nullptr); path_ = std::move(other.path_);
        }
        return *this;
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    template <typename Function>
    Function Require(const char* name) const {
        const auto symbol = GetProcAddress(module_, name);
        if (symbol == nullptr) throw std::runtime_error("Pinned FFmpeg DLL " + path_.string() + " is missing symbol " + name);
        return reinterpret_cast<Function>(symbol);
    }
    bool Has(const char* name) const noexcept { return GetProcAddress(module_, name) != nullptr; }
private:
    HMODULE module_ = nullptr;
    std::filesystem::path path_;
};

struct FfmpegRuntime {
    using AvcodecVersion = unsigned (*)();
    using AvutilVersion = unsigned (*)();
    using SwscaleVersion = unsigned (*)();
    using FindDecoder = const AVCodec* (*)(AVCodecID);
    using GetHwConfig = const AVCodecHWConfig* (*)(const AVCodec*, int);
    using ParserInit = AVCodecParserContext* (*)(int);
    using ParserParse = int (*)(AVCodecParserContext*, AVCodecContext*, uint8_t**, int*, const uint8_t*, int, int64_t, int64_t, int64_t);
    using ParserClose = void (*)(AVCodecParserContext*);
    using AllocContext = AVCodecContext* (*)(const AVCodec*);
    using OpenCodec = int (*)(AVCodecContext*, const AVCodec*, AVDictionary**);
    using SendPacket = int (*)(AVCodecContext*, const AVPacket*);
    using ReceiveFrame = int (*)(AVCodecContext*, AVFrame*);
    using GetHwFramesParameters = int (*)(AVCodecContext*, AVBufferRef*, AVPixelFormat, AVBufferRef**);
    using FreeContext = void (*)(AVCodecContext**);
    using FlushBuffers = void (*)(AVCodecContext*);
    using PacketAlloc = AVPacket* (*)();
    using PacketUnref = void (*)(AVPacket*);
    using PacketFree = void (*)(AVPacket**);
    using FrameAlloc = AVFrame* (*)();
    using FrameUnref = void (*)(AVFrame*);
    using FrameFree = void (*)(AVFrame**);
    using BufferRef = AVBufferRef* (*)(const AVBufferRef*);
    using BufferUnref = void (*)(AVBufferRef**);
    using HwDeviceCreate = int (*)(AVBufferRef**, AVHWDeviceType, const char*, AVDictionary*, int);
    using HwFrameCtxAlloc = AVBufferRef* (*)(AVBufferRef*);
    using HwFrameCtxInit = int (*)(AVBufferRef*);
    using HwFrameTransfer = int (*)(AVFrame*, const AVFrame*, int);
    using SwsGetContext = SwsContext* (*)(int,int,AVPixelFormat,int,int,AVPixelFormat,int,SwsFilter*,SwsFilter*,const double*);
    using SwsScale = int (*)(SwsContext*, const uint8_t* const[], const int[], int, int, uint8_t* const[], const int[]);
    using SwsFreeContext = void (*)(SwsContext*);

    explicit FfmpegRuntime(const FfmpegRuntimePaths& paths) {
        bin = std::filesystem::absolute(paths.binDirectory);
        if (!std::filesystem::is_directory(bin)) throw std::runtime_error("Pinned FFmpeg directory does not exist: " + bin.string());
        cookie = AddDllDirectory(bin.c_str());
        try {
            avutil = Module(bin / L"avutil-61.dll", cookie);
            avcodec = Module(bin / L"avcodec-63.dll", cookie);
            swscale = Module(bin / L"swscale-10.dll", cookie);

            avutilVersion = avutil.Require<AvutilVersion>("avutil_version");
            avcodecVersion = avcodec.Require<AvcodecVersion>("avcodec_version");
            swscaleVersion = swscale.Require<SwscaleVersion>("swscale_version");
            detail::FfmpegRuntimeContract contract {
                avutilVersion() >> 16, avcodecVersion() >> 16, swscaleVersion() >> 16, bin.string(), {}
            };
            auto check = [&contract](const Module& module, std::initializer_list<const char*> symbols) {
                for (const char* symbol : symbols) if (!module.Has(symbol)) contract.missingSymbols.emplace_back(symbol);
            };
            check(avcodec, {"avcodec_find_decoder", "avcodec_get_hw_config", "av_parser_init", "av_parser_parse2",
                            "av_parser_close", "avcodec_alloc_context3", "avcodec_open2", "avcodec_send_packet",
                            "avcodec_receive_frame", "avcodec_get_hw_frames_parameters", "avcodec_free_context",
                            "avcodec_flush_buffers", "av_packet_alloc", "av_packet_unref", "av_packet_free"});
            check(avutil, {"av_frame_alloc", "av_frame_unref", "av_frame_free", "av_buffer_ref", "av_buffer_unref",
                           "av_hwdevice_ctx_create", "av_hwframe_ctx_alloc", "av_hwframe_ctx_init", "av_hwframe_transfer_data"});
            check(swscale, {"sws_getContext", "sws_scale", "sws_freeContext"});
            detail::ValidateFfmpegRuntimeContract(contract);

            findDecoder = avcodec.Require<FindDecoder>("avcodec_find_decoder");
            getHwConfig = avcodec.Require<GetHwConfig>("avcodec_get_hw_config");
            parserInit = avcodec.Require<ParserInit>("av_parser_init");
            parserParse = avcodec.Require<ParserParse>("av_parser_parse2");
            parserClose = avcodec.Require<ParserClose>("av_parser_close");
            allocContext = avcodec.Require<AllocContext>("avcodec_alloc_context3");
            openCodec = avcodec.Require<OpenCodec>("avcodec_open2");
            sendPacket = avcodec.Require<SendPacket>("avcodec_send_packet");
            receiveFrame = avcodec.Require<ReceiveFrame>("avcodec_receive_frame");
            getHwFramesParameters = avcodec.Require<GetHwFramesParameters>("avcodec_get_hw_frames_parameters");
            freeContext = avcodec.Require<FreeContext>("avcodec_free_context");
            flushBuffers = avcodec.Require<FlushBuffers>("avcodec_flush_buffers");
            packetAlloc = avcodec.Require<PacketAlloc>("av_packet_alloc");
            packetUnref = avcodec.Require<PacketUnref>("av_packet_unref");
            packetFree = avcodec.Require<PacketFree>("av_packet_free");
            frameAlloc = avutil.Require<FrameAlloc>("av_frame_alloc");
            frameUnref = avutil.Require<FrameUnref>("av_frame_unref");
            frameFree = avutil.Require<FrameFree>("av_frame_free");
            bufferRef = avutil.Require<BufferRef>("av_buffer_ref");
            bufferUnref = avutil.Require<BufferUnref>("av_buffer_unref");
            hwDeviceCreate = avutil.Require<HwDeviceCreate>("av_hwdevice_ctx_create");
            hwFrameCtxAlloc = avutil.Require<HwFrameCtxAlloc>("av_hwframe_ctx_alloc");
            hwFrameCtxInit = avutil.Require<HwFrameCtxInit>("av_hwframe_ctx_init");
            hwFrameTransfer = avutil.Require<HwFrameTransfer>("av_hwframe_transfer_data");
            swsGetContext = swscale.Require<SwsGetContext>("sws_getContext");
            swsScale = swscale.Require<SwsScale>("sws_scale");
            swsFreeContext = swscale.Require<SwsFreeContext>("sws_freeContext");
        } catch (...) {
            if (cookie != nullptr) { RemoveDllDirectory(cookie); cookie = nullptr; }
            throw;
        }
    }

    ~FfmpegRuntime() { if (cookie != nullptr) RemoveDllDirectory(cookie); }

    std::filesystem::path bin;
    DLL_DIRECTORY_COOKIE cookie = nullptr;
    Module avutil, avcodec, swscale;
    AvcodecVersion avcodecVersion{}; AvutilVersion avutilVersion{}; SwscaleVersion swscaleVersion{};
    FindDecoder findDecoder{}; GetHwConfig getHwConfig{}; ParserInit parserInit{}; ParserParse parserParse{}; ParserClose parserClose{};
    AllocContext allocContext{}; OpenCodec openCodec{}; SendPacket sendPacket{}; ReceiveFrame receiveFrame{};
    GetHwFramesParameters getHwFramesParameters{};
    FreeContext freeContext{}; FlushBuffers flushBuffers{}; PacketAlloc packetAlloc{}; PacketUnref packetUnref{}; PacketFree packetFree{};
    FrameAlloc frameAlloc{}; FrameUnref frameUnref{}; FrameFree frameFree{}; BufferRef bufferRef{}; BufferUnref bufferUnref{};
    HwDeviceCreate hwDeviceCreate{}; HwFrameCtxAlloc hwFrameCtxAlloc{}; HwFrameCtxInit hwFrameCtxInit{}; HwFrameTransfer hwFrameTransfer{};
    SwsGetContext swsGetContext{}; SwsScale swsScale{}; SwsFreeContext swsFreeContext{};
};

AVCodecID CodecId(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return AV_CODEC_ID_H264;
    case VideoCodec::H265: return AV_CODEC_ID_HEVC;
    }
    throw std::invalid_argument("Unsupported video codec");
}

bool IsSelfContainedRandomAccess(VideoCodec codec, std::span<const uint8_t> bytes) {
    bool vps = false, sps = false, pps = false, random = false;
    size_t offset = 0;
    while (offset < bytes.size()) {
        size_t prefix = 0;
        if (offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, "\0\0\0\1", 4) == 0) prefix = 4;
        else if (offset + 3 <= bytes.size() && std::memcmp(bytes.data() + offset, "\0\0\1", 3) == 0) prefix = 3;
        if (prefix == 0) { ++offset; continue; }
        const size_t nal = offset + prefix;
        if (nal >= bytes.size()) return false;
        if (codec == VideoCodec::H264) {
            const uint8_t type = bytes[nal] & 0x1f;
            random |= type == 5; sps |= type == 7; pps |= type == 8;
        } else if (nal + 1 < bytes.size()) {
            const uint8_t type = (bytes[nal] >> 1) & 0x3f;
            random |= type >= 16 && type <= 23; vps |= type == 32; sps |= type == 33; pps |= type == 34;
        }
        offset = nal + 1;
    }
    return codec == VideoCodec::H264 ? random && sps && pps : random && vps && sps && pps;
}

} // namespace

class FfmpegVideoDecoder::Impl {
public:
    enum class State { Closed, HardwareActive, SoftwareActive, AwaitRandomAccess };
    struct InternalResult { DecodeResult publicResult; bool hardwareFailure = false; };

    Impl(FfmpegRuntimePaths paths, DecoderPreference preference)
        : runtime_(std::make_unique<FfmpegRuntime>(paths)), preference_(preference) {}
    Impl(std::shared_ptr<detail::IDecoderBackendFactory> factory, DecoderPreference preference)
        : preference_(preference), factory_(std::move(factory)) {
        if (!factory_) throw std::invalid_argument("Decoder backend factory is required");
    }
    ~Impl() { CloseBackend(); }

    static AVPixelFormat SelectHardwareFormat(AVCodecContext* context, const AVPixelFormat* formats) {
        auto* self = static_cast<Impl*>(context->opaque);
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == AV_PIX_FMT_D3D11 && self != nullptr && self->ConfigureHardwareFrames(context)) return *format;
        }
        if (self != nullptr) self->formatNegotiationFailed_ = true;
        return AV_PIX_FMT_NONE;
    }

    bool ConfigureHardwareFrames(AVCodecContext* context) {
        if (context->hw_frames_ctx != nullptr) return true;
        AVBufferRef* framesReference = nullptr;
        if (runtime_->getHwFramesParameters(context, hardwareDevice_, AV_PIX_FMT_D3D11, &framesReference) < 0 ||
            framesReference == nullptr) {
            formatNegotiationFailed_ = true;
            return false;
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(framesReference->data);
        frames->format = AV_PIX_FMT_D3D11;
        frames->sw_format = AV_PIX_FMT_NV12;
        auto* d3dFrames = static_cast<AVD3D11VAFramesContext*>(frames->hwctx);
        d3dFrames->BindFlags |= D3D11_BIND_DECODER;
        if (runtime_->hwFrameCtxInit(framesReference) < 0) {
            runtime_->bufferUnref(&framesReference);
            formatNegotiationFailed_ = true;
            return false;
        }
        context->hw_frames_ctx = runtime_->bufferRef(framesReference);
        runtime_->bufferUnref(&framesReference);
        if (context->hw_frames_ctx == nullptr) {
            formatNegotiationFailed_ = true;
            return false;
        }
        return true;
    }

    bool OpenBackend(bool hardware) {
        CloseBackend();
        const AVCodec* decoder = runtime_->findDecoder(CodecId(codec_));
        if (decoder == nullptr) return false;
        context_ = runtime_->allocContext(decoder);
        if (context_ == nullptr) return false;
        context_->width = width_; context_->height = height_;
        context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context_->thread_count = 1;
        context_->pkt_timebase = AVRational{1, 10'000'000};
        if (hardware) {
            if (runtime_->hwDeviceCreate(&hardwareDevice_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0) {
                CloseBackend(); return false;
            }
            context_->hw_device_ctx = runtime_->bufferRef(hardwareDevice_);
            if (context_->hw_device_ctx == nullptr) { CloseBackend(); return false; }
            context_->opaque = this;
            context_->get_format = &SelectHardwareFormat;
        }
        if (runtime_->openCodec(context_, decoder, nullptr) < 0) { CloseBackend(); return false; }
        parser_ = runtime_->parserInit(static_cast<int>(CodecId(codec_)));
        packet_ = runtime_->packetAlloc();
        frame_ = runtime_->frameAlloc();
        transferFrame_ = runtime_->frameAlloc();
        if (parser_ == nullptr || packet_ == nullptr || frame_ == nullptr || transferFrame_ == nullptr) {
            CloseBackend(); return false;
        }
        hardware_ = hardware;
        formatNegotiationFailed_ = false;
        return true;
    }

    void Open(VideoCodec codec, int width, int height) {
        if (width <= 0 || height <= 0) throw std::invalid_argument("Decoder dimensions must be positive");
        codec_ = codec; width_ = width; height_ = height; pendingKeyframeRequest_ = false;
        if (factory_) {
            OpenInjected(codec, width, height);
            return;
        }
        if (preference_ == DecoderPreference::PreferD3d11) {
            if (OpenBackend(true)) { state_ = State::HardwareActive; return; }
            if (!OpenBackend(false)) throw std::runtime_error("Could not open pinned FFmpeg decoder in D3D11VA or software mode");
            state_ = State::AwaitRandomAccess;
            pendingKeyframeRequest_ = true;
            return;
        }
        if (!OpenBackend(false)) throw std::runtime_error("Could not open pinned FFmpeg software decoder");
        state_ = State::SoftwareActive;
    }

    void OpenInjected(VideoCodec codec, int width, int height) {
        injectedBackend_.reset();
        if (!factory_->IsAvailable(codec)) throw std::runtime_error("Injected decoder backend is unavailable");
        if (preference_ == DecoderPreference::PreferD3d11 && factory_->IsD3d11Available(codec)) {
            injectedBackend_ = factory_->Open(codec, width, height, true);
            if (injectedBackend_) { state_ = State::HardwareActive; hardware_ = true; return; }
        }
        injectedBackend_ = factory_->Open(codec, width, height, false);
        if (!injectedBackend_) throw std::runtime_error("Injected software decoder backend could not open");
        hardware_ = false;
        if (preference_ == DecoderPreference::PreferD3d11) {
            state_ = State::AwaitRandomAccess;
            pendingKeyframeRequest_ = true;
        } else {
            state_ = State::SoftwareActive;
        }
    }

    DecodedBgraFrame Convert(const AVFrame* source, int64_t submittedTimestamp) {
        const auto pixelFormat = static_cast<AVPixelFormat>(source->format);
        if (sws_ == nullptr || swsWidth_ != source->width || swsHeight_ != source->height || swsFormat_ != pixelFormat) {
            if (sws_ != nullptr) runtime_->swsFreeContext(sws_);
            sws_ = runtime_->swsGetContext(source->width, source->height, pixelFormat,
                                           source->width, source->height, AV_PIX_FMT_BGRA,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
            swsWidth_ = source->width; swsHeight_ = source->height; swsFormat_ = pixelFormat;
        }
        if (sws_ == nullptr) throw std::runtime_error("sws_getContext could not create BGRA conversion");
        DecodedBgraFrame output;
        output.width = source->width; output.height = source->height; output.stride = source->width * 4;
        output.timestamp100ns = source->best_effort_timestamp == AV_NOPTS_VALUE ? submittedTimestamp : source->best_effort_timestamp;
        output.bytes.resize(static_cast<size_t>(output.stride) * output.height);
        uint8_t* destination[] { output.bytes.data(), nullptr, nullptr, nullptr };
        int destinationStride[] { output.stride, 0, 0, 0 };
        const int rows = runtime_->swsScale(sws_, source->data, source->linesize, 0, source->height, destination, destinationStride);
        if (rows != source->height) throw std::runtime_error("sws_scale did not convert the complete frame");
        return output;
    }

    bool Drain(InternalResult& result, int64_t submittedTimestamp) {
        class ActualHardwarePipeline final : public detail::IHardwareFramePipeline {
        public:
            ActualHardwarePipeline(Impl& owner, int64_t timestamp) : owner_(owner), timestamp_(timestamp) {}
            bool TransferToSoftware() override {
                owner_.runtime_->frameUnref(owner_.transferFrame_);
                return owner_.runtime_->hwFrameTransfer(owner_.transferFrame_, owner_.frame_, 0) >= 0;
            }
            std::optional<DecodedBgraFrame> ScaleSoftwareToBgra() override {
                try { return owner_.Convert(owner_.transferFrame_, timestamp_); }
                catch (...) { return std::nullopt; }
            }
            void UnrefSoftware() noexcept override { owner_.runtime_->frameUnref(owner_.transferFrame_); }
            void UnrefHardware() noexcept override { owner_.runtime_->frameUnref(owner_.frame_); }
        private:
            Impl& owner_;
            int64_t timestamp_;
        };

        for (;;) {
            const int received = runtime_->receiveFrame(context_, frame_);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
            if (received < 0) {
                result.hardwareFailure = hardware_ && formatNegotiationFailed_;
                return false;
            }
            if (frame_->format == AV_PIX_FMT_D3D11) {
                ActualHardwarePipeline pipeline(*this, submittedTimestamp);
                auto converted = detail::RunHardwareFramePipeline(pipeline);
                if (converted.kind == detail::BackendResultKind::HardwareFailure) {
                    result.hardwareFailure = true;
                    return false;
                }
                result.publicResult.frames.push_back(std::move(converted.frames.front()));
                continue;
            }
            try {
                if (hardware_ && formatNegotiationFailed_) {
                    result.hardwareFailure = true;
                    runtime_->frameUnref(frame_);
                    return false;
                }
                result.publicResult.frames.push_back(Convert(frame_, submittedTimestamp));
            } catch (...) {
                // swscale is the final hardware-output conversion stage while
                // this backend is active, so its failure is eligible for the
                // one-way software fallback.  Parser/send/receive failures do
                // not set this flag and therefore never churn the backend.
                result.hardwareFailure = hardware_;
                runtime_->frameUnref(frame_);
                return false;
            }
            runtime_->frameUnref(frame_);
        }
    }

    InternalResult DecodeInternal(std::span<const uint8_t> accessUnit, int64_t timestamp) {
        InternalResult result;
        result.publicResult.usedSoftware = !hardware_;
        if (accessUnit.empty()) { result.publicResult.status = DecodeStatus::DecodeError; return result; }
        std::vector<uint8_t> padded(accessUnit.begin(), accessUnit.end());
        padded.resize(padded.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        const uint8_t* input = padded.data();
        int remaining = static_cast<int>(accessUnit.size());
        bool packetProduced = false;
        while (remaining > 0) {
            uint8_t* packetData = nullptr; int packetSize = 0;
            const int consumed = runtime_->parserParse(parser_, context_, &packetData, &packetSize, input, remaining,
                                                       timestamp, timestamp, 0);
            if (consumed < 0 || (consumed == 0 && packetSize == 0)) {
                result.publicResult.status = consumed < 0 ? DecodeStatus::DecodeError : DecodeStatus::NeedMoreInput;
                return result;
            }
            input += consumed; remaining -= consumed;
            if (packetSize == 0) continue;
            packetProduced = true;
            packet_->data = packetData; packet_->size = packetSize; packet_->pts = timestamp; packet_->dts = timestamp;
            int sent = runtime_->sendPacket(context_, packet_);
            if (sent == AVERROR(EAGAIN)) {
                if (!Drain(result, timestamp)) { result.publicResult.status = DecodeStatus::DecodeError; return result; }
                sent = runtime_->sendPacket(context_, packet_);
            }
            if (sent < 0 || !Drain(result, timestamp)) {
                result.publicResult.status = DecodeStatus::DecodeError;
                return result;
            }
            runtime_->packetUnref(packet_);
        }
        result.publicResult.status = !result.publicResult.frames.empty() ? DecodeStatus::Produced :
                                     (packetProduced ? DecodeStatus::NeedMoreInput : DecodeStatus::NeedMoreInput);
        return result;
    }

    static DecodeResult PublicResult(detail::BackendDecodeResult backend, bool software) {
        DecodeResult result;
        result.usedSoftware = software;
        result.frames = std::move(backend.frames);
        switch (backend.kind) {
        case detail::BackendResultKind::Produced:
            result.status = result.frames.empty() ? DecodeStatus::NeedMoreInput : DecodeStatus::Produced;
            break;
        case detail::BackendResultKind::NeedMoreInput:
        case detail::BackendResultKind::ParserNoOutput:
        case detail::BackendResultKind::ReceiveEagain:
            result.status = DecodeStatus::NeedMoreInput;
            break;
        case detail::BackendResultKind::BitstreamError: result.status = DecodeStatus::DecodeError; break;
        case detail::BackendResultKind::HardwareFailure: result.status = DecodeStatus::DecodeError; break;
        }
        return result;
    }

    DecodeResult DecodeInjected(std::span<const uint8_t> accessUnit, int64_t timestamp) {
        if (accessUnit.empty()) return {DecodeStatus::DecodeError, {}, false, state_ != State::HardwareActive};
        const bool randomAccess = IsSelfContainedRandomAccess(codec_, accessUnit);
        if (state_ == State::AwaitRandomAccess) {
            DecodeResult waiting {DecodeStatus::AwaitingRandomAccess, {},
                                  std::exchange(pendingKeyframeRequest_, false), true};
            if (!randomAccess) return waiting;
            auto result = PublicResult(injectedBackend_->Decode(accessUnit, timestamp), true);
            result.keyframeRequested = waiting.keyframeRequested;
            if (result.status == DecodeStatus::Produced) state_ = State::SoftwareActive;
            else { result.frames.clear(); result.status = DecodeStatus::AwaitingRandomAccess; }
            return result;
        }

        auto backendResult = injectedBackend_->Decode(accessUnit, timestamp);
        if (state_ != State::HardwareActive || backendResult.kind != detail::BackendResultKind::HardwareFailure) {
            return PublicResult(std::move(backendResult), state_ != State::HardwareActive);
        }

        injectedBackend_.reset(); // hardware resources die before software creation/retry
        injectedBackend_ = factory_->Open(codec_, width_, height_, false);
        if (!injectedBackend_) {
            state_ = State::AwaitRandomAccess;
            hardware_ = false;
            return {DecodeStatus::DecodeError, {}, true, true};
        }
        hardware_ = false;
        state_ = State::AwaitRandomAccess;
        auto retry = PublicResult(injectedBackend_->Decode(accessUnit, timestamp), true); // exactly once
        retry.keyframeRequested = true;
        if (randomAccess && retry.status == DecodeStatus::Produced) state_ = State::SoftwareActive;
        else { retry.frames.clear(); retry.status = DecodeStatus::AwaitingRandomAccess; }
        return retry;
    }

    DecodeResult Decode(std::span<const uint8_t> accessUnit, int64_t timestamp) {
        if (state_ == State::Closed) throw std::logic_error("Decoder must be opened before Decode");
        if (factory_) return DecodeInjected(accessUnit, timestamp);
        const bool randomAccess = IsSelfContainedRandomAccess(codec_, accessUnit);
        if (state_ == State::AwaitRandomAccess) {
            DecodeResult waiting;
            waiting.status = DecodeStatus::AwaitingRandomAccess;
            waiting.usedSoftware = true;
            waiting.keyframeRequested = std::exchange(pendingKeyframeRequest_, false);
            if (!randomAccess) return waiting;
            auto attempt = DecodeInternal(accessUnit, timestamp);
            attempt.publicResult.keyframeRequested = waiting.keyframeRequested;
            if (attempt.publicResult.status == DecodeStatus::Produced) state_ = State::SoftwareActive;
            else { attempt.publicResult.frames.clear(); attempt.publicResult.status = DecodeStatus::AwaitingRandomAccess; }
            return std::move(attempt.publicResult);
        }

        auto result = DecodeInternal(accessUnit, timestamp);
        if (state_ != State::HardwareActive || !result.hardwareFailure) return std::move(result.publicResult);

        const std::vector<uint8_t> preserved(accessUnit.begin(), accessUnit.end());
        if (!OpenBackend(false)) {
            result.publicResult.frames.clear(); result.publicResult.status = DecodeStatus::DecodeError;
            result.publicResult.keyframeRequested = true; result.publicResult.usedSoftware = true;
            state_ = State::AwaitRandomAccess; pendingKeyframeRequest_ = false;
            return std::move(result.publicResult);
        }
        state_ = State::AwaitRandomAccess;
        auto retry = DecodeInternal(preserved, timestamp); // exactly one controlled retry
        retry.publicResult.keyframeRequested = true;
        retry.publicResult.usedSoftware = true;
        if (randomAccess && retry.publicResult.status == DecodeStatus::Produced) state_ = State::SoftwareActive;
        else { retry.publicResult.frames.clear(); retry.publicResult.status = DecodeStatus::AwaitingRandomAccess; }
        return std::move(retry.publicResult);
    }

    void CloseBackend() noexcept {
        if (sws_ != nullptr && runtime_) { runtime_->swsFreeContext(sws_); sws_ = nullptr; }
        if (transferFrame_ != nullptr && runtime_) runtime_->frameFree(&transferFrame_);
        if (frame_ != nullptr && runtime_) runtime_->frameFree(&frame_);
        if (packet_ != nullptr && runtime_) runtime_->packetFree(&packet_);
        if (parser_ != nullptr && runtime_) { runtime_->parserClose(parser_); parser_ = nullptr; }
        if (context_ != nullptr && runtime_) runtime_->freeContext(&context_);
        if (hardwareDevice_ != nullptr && runtime_) runtime_->bufferUnref(&hardwareDevice_);
        swsWidth_ = swsHeight_ = 0; swsFormat_ = AV_PIX_FMT_NONE; hardware_ = false;
    }

    ID3D11Device* MediaDevice() const noexcept {
        if (factory_) return injectedBackend_ ? injectedBackend_->MediaDevice() : nullptr;
        if (hardwareDevice_ == nullptr || hardwareDevice_->data == nullptr) return nullptr;
        const auto* deviceContext = reinterpret_cast<const AVHWDeviceContext*>(hardwareDevice_->data);
        if (deviceContext->hwctx == nullptr) return nullptr;
        return static_cast<const AVD3D11VADeviceContext*>(deviceContext->hwctx)->device;
    }

    bool IsD3d11Available() const noexcept { return state_ == State::HardwareActive && hardware_; }

    std::unique_ptr<FfmpegRuntime> runtime_;
    DecoderPreference preference_;
    std::shared_ptr<detail::IDecoderBackendFactory> factory_;
    std::unique_ptr<detail::IDecoderBackend> injectedBackend_;
    VideoCodec codec_ = VideoCodec::H264;
    int width_ = 0, height_ = 0;
    State state_ = State::Closed;
    bool pendingKeyframeRequest_ = false, hardware_ = false, formatNegotiationFailed_ = false;
    AVCodecContext* context_ = nullptr; AVCodecParserContext* parser_ = nullptr; AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr; AVFrame* transferFrame_ = nullptr; AVBufferRef* hardwareDevice_ = nullptr;
    SwsContext* sws_ = nullptr; int swsWidth_ = 0, swsHeight_ = 0; AVPixelFormat swsFormat_ = AV_PIX_FMT_NONE;
};

FfmpegVideoDecoder::FfmpegVideoDecoder()
    : FfmpegVideoDecoder({std::filesystem::path(TELESTUDIO_DEFAULT_CODEC_BIN_DIR)}) {}
FfmpegVideoDecoder::FfmpegVideoDecoder(FfmpegRuntimePaths paths, DecoderPreference preference)
    : impl_(std::make_unique<Impl>(std::move(paths), preference)) {}
FfmpegVideoDecoder::FfmpegVideoDecoder(std::shared_ptr<detail::IDecoderBackendFactory> factory,
                                       DecoderPreference preference)
    : impl_(std::make_unique<Impl>(std::move(factory), preference)) {}
FfmpegVideoDecoder::~FfmpegVideoDecoder() = default;
FfmpegVideoDecoder::FfmpegVideoDecoder(FfmpegVideoDecoder&&) noexcept = default;
FfmpegVideoDecoder& FfmpegVideoDecoder::operator=(FfmpegVideoDecoder&&) noexcept = default;

bool FfmpegVideoDecoder::IsAvailable(VideoCodec codec) {
    return IsAvailable(codec, {std::filesystem::path(TELESTUDIO_DEFAULT_CODEC_BIN_DIR)});
}
bool FfmpegVideoDecoder::IsAvailable(VideoCodec codec, const FfmpegRuntimePaths& paths) {
    try { FfmpegRuntime runtime(paths); return runtime.findDecoder(CodecId(codec)) != nullptr; } catch (...) { return false; }
}
bool FfmpegVideoDecoder::IsD3d11Available(VideoCodec codec) {
    return IsD3d11Available(codec, {std::filesystem::path(TELESTUDIO_DEFAULT_CODEC_BIN_DIR)});
}
bool FfmpegVideoDecoder::IsD3d11Available(VideoCodec codec, const FfmpegRuntimePaths& paths) {
    try {
        auto runtime = std::make_unique<FfmpegRuntime>(paths);
        const AVCodec* decoder = runtime->findDecoder(CodecId(codec));
        if (decoder == nullptr) return false;
        bool codecSupportsD3d11 = false;
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config = runtime->getHwConfig(decoder, index);
            if (config == nullptr) break;
            const bool d3dPixelFormat = config->pix_fmt == AV_PIX_FMT_D3D11 || config->pix_fmt == AV_PIX_FMT_D3D11VA_VLD;
            const bool usableMethod = (config->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX |
                                                           AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
                                                           AV_CODEC_HW_CONFIG_METHOD_INTERNAL)) != 0;
            if (d3dPixelFormat && usableMethod) { codecSupportsD3d11 = true; break; }
        }
        if (!codecSupportsD3d11) return false;
        AVBufferRef* device = nullptr;
        const bool available = runtime->hwDeviceCreate(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0;
        if (device != nullptr) runtime->bufferUnref(&device);
        return available;
    } catch (...) { return false; }
}

void FfmpegVideoDecoder::Open(VideoCodec codec, int width, int height) { impl_->Open(codec, width, height); }
DecodeResult FfmpegVideoDecoder::Decode(std::span<const uint8_t> accessUnit, int64_t timestamp100ns) {
    return impl_->Decode(accessUnit, timestamp100ns);
}
ID3D11Device* FfmpegVideoDecoder::MediaDevice() const noexcept { return impl_->MediaDevice(); }
bool FfmpegVideoDecoder::IsD3d11Available() const noexcept { return impl_->IsD3d11Available(); }

} // namespace telestudio
