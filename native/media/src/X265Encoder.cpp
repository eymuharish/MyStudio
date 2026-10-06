#include "telestudio/media/IVideoEncoder.h"
#include "telestudio/media/detail/EncoderPtsSequence.h"

#include <x265.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace telestudio::detail {
struct LoadedLibrary;
using LibraryHandle = LoadedLibrary*;
LibraryHandle OpenExplicitLibrary(const std::filesystem::path&, const std::vector<std::wstring>&);
void CloseLibrary(LibraryHandle) noexcept;
void* RequireExport(LibraryHandle, const char*);

bool IsPinnedX265Api(const x265_api* api) noexcept {
    return api != nullptr && api->api_major_version == X265_MAJOR_VERSION && api->api_build_number == X265_BUILD &&
           api->sizeof_param == sizeof(x265_param) && api->sizeof_picture == sizeof(x265_picture) && api->bit_depth == 8;
}
} // namespace telestudio::detail

namespace telestudio {
namespace {

void ValidateSettings(const EncoderSettings& settings) {
    if (settings.codec != VideoCodec::H265 || settings.width <= 0 || settings.height <= 0 ||
        settings.width % 2 != 0 || settings.height % 2 != 0 || settings.fps <= 0 || settings.bitrateKbps <= 0) {
        throw std::invalid_argument("Invalid encoder settings");
    }
}

class X265Encoder final : public IVideoEncoder {
public:
    explicit X265Encoder(const std::filesystem::path& dll)
        : library_(detail::OpenExplicitLibrary(dll, { L"libx265.dll", L"libx265-216.dll", L"x265.dll", L"x265-216.dll" })) {
        try {
            apiGet_ = reinterpret_cast<ApiGetFn>(detail::RequireExport(library_, "x265_api_get_216"));
            api_ = apiGet_(8);
            if (!detail::IsPinnedX265Api(api_)) {
                throw std::runtime_error("x265 API ABI mismatch (requires major 1, build 216, 8-bit pinned header layout)");
            }
        } catch (...) {
            detail::CloseLibrary(library_);
            library_ = nullptr;
            throw;
        }
    }

    ~X265Encoder() override {
        CloseEncoder();
        detail::CloseLibrary(library_);
    }

    void Open(const EncoderSettings& settings) override {
        ValidateSettings(settings);
        x265_param* params = BuildParams(settings);
        x265_encoder* replacement = nullptr;
        try {
            replacement = api_->encoder_open(params);
            api_->param_free(params);
        } catch (...) {
            api_->param_free(params);
            throw;
        }
        if (replacement == nullptr) {
            throw std::runtime_error("x265 encoder_open rejected the requested settings");
        }
        if (encoder_ != nullptr) api_->encoder_close(encoder_);
        encoder_ = replacement;
        settings_ = settings;
        forceNextIdr_ = true;
    }

    EncodedAccessUnit Encode(const I420Frame& frame, bool forceIdr) override {
        ValidateFrame(frame);
        x265_picture input{};
        x265_picture output{};
        x265_param* pictureParams = BuildParams(settings_);
        api_->picture_init(pictureParams, &input);
        api_->param_free(pictureParams);
        input.pts = ptsSequence_.Next();
        input.sliceType = (forceIdr || forceNextIdr_) ? X265_TYPE_IDR : X265_TYPE_AUTO;
        input.bitDepth = 8;
        input.colorSpace = X265_CSP_I420;
        input.planes[0] = const_cast<uint8_t*>(frame.y);
        input.planes[1] = const_cast<uint8_t*>(frame.u);
        input.planes[2] = const_cast<uint8_t*>(frame.v);
        input.stride[0] = frame.strideY;
        input.stride[1] = frame.strideU;
        input.stride[2] = frame.strideV;

        x265_nal* nals = nullptr;
        uint32_t nalCount = 0;
        const int bytes = api_->encoder_encode(encoder_, &nals, &nalCount, &input, &output);
        if (bytes < 0) {
            throw std::runtime_error("x265 encoder_encode failed");
        }
        EncodedAccessUnit unit { {}, frame.timestamp100ns, output.sliceType == X265_TYPE_IDR || output.sliceType == X265_TYPE_I };
        for (uint32_t i = 0; i < nalCount; ++i) {
            if (nals[i].payload == nullptr || nals[i].sizeBytes == 0) {
                throw std::runtime_error("x265 returned an empty NAL");
            }
            unit.annexB.insert(unit.annexB.end(), nals[i].payload, nals[i].payload + nals[i].sizeBytes);
        }
        forceNextIdr_ = false;
        return unit;
    }

    void Reconfigure(const EncoderSettings& settings) override {
        ValidateSettings(settings);
        if (encoder_ == nullptr) {
            Open(settings);
            return;
        }
        const bool requiresReopen = settings.width != settings_.width || settings.height != settings_.height ||
                                    settings.fps != settings_.fps || settings.highProfile != settings_.highProfile ||
                                    settings.keyintFrames != settings_.keyintFrames;
        if (requiresReopen) {
            Open(settings);
            return;
        }
        x265_param* params = BuildParams(settings);
        const int result = api_->encoder_reconfig(encoder_, params);
        api_->param_free(params);
        if (result < 0) {
            throw std::runtime_error("x265 encoder_reconfig failed");
        }
        settings_ = settings;
    }

private:
    using ApiGetFn = const x265_api* (__cdecl*)(int);

    x265_param* BuildParams(const EncoderSettings& settings) const {
        x265_param* params = api_->param_alloc();
        if (params == nullptr || api_->param_default_preset(params, "veryfast", "zerolatency") < 0) {
            if (params != nullptr) api_->param_free(params);
            throw std::runtime_error("x265 rejected the veryfast/zerolatency preset");
        }
        params->internalBitDepth = 8;
        params->internalCsp = X265_CSP_I420;
        params->sourceWidth = settings.width;
        params->sourceHeight = settings.height;
        params->fpsNum = static_cast<uint32_t>(settings.fps);
        params->fpsDenom = 1;
        params->keyframeMax = settings.keyintFrames > 0 ? std::min(settings.keyintFrames, settings.fps * 2) : settings.fps * 2;
        params->keyframeMin = params->keyframeMax;
        params->bframes = 0;
        params->lookaheadDepth = 0;
        params->bIntraRefresh = 0;
        params->bRepeatHeaders = 1;
        params->bAnnexB = 1;
        params->rc.rateControlMode = X265_RC_ABR;
        params->rc.bitrate = settings.bitrateKbps;
        params->rc.vbvMaxBitrate = settings.bitrateKbps;
        params->rc.vbvBufferSize = settings.bitrateKbps;
        if (api_->param_apply_profile(params, "main") < 0) {
            api_->param_free(params);
            throw std::runtime_error("x265 rejected the Main profile");
        }
        return params;
    }

    void ValidateFrame(const I420Frame& frame) const {
        if (encoder_ == nullptr || frame.y == nullptr || frame.u == nullptr || frame.v == nullptr ||
            frame.width != settings_.width || frame.height != settings_.height ||
            frame.strideY < frame.width || frame.strideU < frame.width / 2 || frame.strideV < frame.width / 2) {
            throw std::invalid_argument("I420 frame does not match the open x265 encoder");
        }
    }

    void CloseEncoder() noexcept {
        if (encoder_ != nullptr) {
            api_->encoder_close(encoder_);
            encoder_ = nullptr;
            ptsSequence_.Reset();
        }
    }

    detail::LibraryHandle library_ = nullptr;
    ApiGetFn apiGet_ = nullptr;
    const x265_api* api_ = nullptr;
    x265_encoder* encoder_ = nullptr;
    detail::EncoderPtsSequence ptsSequence_;
    EncoderSettings settings_ {};
    bool forceNextIdr_ = true;
};

} // namespace

std::unique_ptr<IVideoEncoder> CreateX265Encoder(const std::filesystem::path& dll) {
    return std::make_unique<X265Encoder>(dll);
}

} // namespace telestudio
