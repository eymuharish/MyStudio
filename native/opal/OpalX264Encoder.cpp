#include "telestudio/media/IVideoEncoder.h"
#include "telestudio/media/detail/EncoderPtsSequence.h"

#include <x264.h>

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
} // namespace telestudio::detail

namespace telestudio {
namespace {

void ValidateSettings(const EncoderSettings& settings, VideoCodec codec) {
    if (settings.codec != codec || settings.width <= 0 || settings.height <= 0 ||
        settings.width % 2 != 0 || settings.height % 2 != 0 || settings.fps <= 0 ||
        settings.bitrateKbps <= 0) {
        throw std::invalid_argument("Invalid encoder settings");
    }
}

class X264Encoder final : public IVideoEncoder {
public:
    explicit X264Encoder(const std::filesystem::path& dll, unsigned maxNalBytes)
        : maxNalBytes_(maxNalBytes), library_(detail::OpenExplicitLibrary(dll, { L"libx264-165.dll", L"libx264.dll", L"x264-165.dll", L"x264.dll" })) {
        try {
            open_ = reinterpret_cast<OpenFn>(detail::RequireExport(library_, "x264_encoder_open_165"));
            preset_ = reinterpret_cast<PresetFn>(detail::RequireExport(library_, "x264_param_default_preset"));
            profile_ = reinterpret_cast<ProfileFn>(detail::RequireExport(library_, "x264_param_apply_profile"));
            encode_ = reinterpret_cast<EncodeFn>(detail::RequireExport(library_, "x264_encoder_encode"));
            reconfig_ = reinterpret_cast<ReconfigFn>(detail::RequireExport(library_, "x264_encoder_reconfig"));
            close_ = reinterpret_cast<CloseFn>(detail::RequireExport(library_, "x264_encoder_close"));
        } catch (...) {
            detail::CloseLibrary(library_);
            library_ = nullptr;
            throw;
        }
    }

    ~X264Encoder() override {
        CloseEncoder();
        detail::CloseLibrary(library_);
    }

    void Open(const EncoderSettings& settings) override {
        ValidateSettings(settings, VideoCodec::H264);
        x264_param_t params = BuildParams(settings);
        auto* replacement = open_(&params);
        if (replacement == nullptr) {
            throw std::runtime_error("x264_encoder_open_165 rejected the requested settings");
        }
        if (encoder_ != nullptr) close_(encoder_);
        encoder_ = replacement;
        settings_ = settings;
        forceNextIdr_ = true;
    }

    EncodedAccessUnit Encode(const I420Frame& frame, bool forceIdr) override {
        ValidateFrame(frame);
        x264_picture_t input{};
        x264_picture_t output{};
        input.i_type = (forceIdr || forceNextIdr_) ? X264_TYPE_IDR : X264_TYPE_AUTO;
        input.i_pts = ptsSequence_.Next();
        input.img.i_csp = X264_CSP_I420;
        input.img.i_plane = 3;
        input.img.i_stride[0] = frame.strideY;
        input.img.i_stride[1] = frame.strideU;
        input.img.i_stride[2] = frame.strideV;
        input.img.plane[0] = const_cast<uint8_t*>(frame.y);
        input.img.plane[1] = const_cast<uint8_t*>(frame.u);
        input.img.plane[2] = const_cast<uint8_t*>(frame.v);

        x264_nal_t* nals = nullptr;
        int nalCount = 0;
        const int bytes = encode_(encoder_, &nals, &nalCount, &input, &output);
        if (bytes < 0) {
            throw std::runtime_error("x264_encoder_encode failed");
        }
        EncodedAccessUnit unit { {}, frame.timestamp100ns, output.b_keyframe != 0 };
        for (int i = 0; i < nalCount; ++i) {
            if (nals[i].i_payload <= 0 || nals[i].p_payload == nullptr) {
                throw std::runtime_error("x264 returned an empty NAL");
            }
            unit.annexB.insert(unit.annexB.end(), nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
        }
        forceNextIdr_ = false;
        return unit;
    }

    void Reconfigure(const EncoderSettings& settings) override {
        ValidateSettings(settings, VideoCodec::H264);
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
        x264_param_t params = BuildParams(settings);
        if (reconfig_(encoder_, &params) < 0) {
            throw std::runtime_error("x264_encoder_reconfig failed");
        }
        settings_ = settings;
    }

private:
    using OpenFn = x264_t* (__cdecl*)(x264_param_t*);
    using PresetFn = int (__cdecl*)(x264_param_t*, const char*, const char*);
    using ProfileFn = int (__cdecl*)(x264_param_t*, const char*);
    using EncodeFn = int (__cdecl*)(x264_t*, x264_nal_t**, int*, x264_picture_t*, x264_picture_t*);
    using ReconfigFn = int (__cdecl*)(x264_t*, x264_param_t*);
    using CloseFn = void (__cdecl*)(x264_t*);

    x264_param_t BuildParams(const EncoderSettings& settings) const {
        x264_param_t params{};
        if (preset_(&params, "veryfast", "zerolatency") < 0) {
            throw std::runtime_error("x264 rejected the veryfast/zerolatency preset");
        }
        params.i_width = settings.width;
        params.i_height = settings.height;
        params.i_csp = X264_CSP_I420;
        params.i_bitdepth = 8;
        params.i_fps_num = static_cast<uint32_t>(settings.fps);
        params.i_fps_den = 1;
        params.i_timebase_num = 1;
        params.i_timebase_den = static_cast<uint32_t>(settings.fps);
        params.i_keyint_max = settings.keyintFrames > 0 ? std::min(settings.keyintFrames, settings.fps * 10) : settings.fps * 2;
        params.i_slice_max_size = maxNalBytes_;
        params.i_keyint_min = params.i_keyint_max;
        params.b_intra_refresh = 0;
        params.i_bframe = 0;
        params.i_sync_lookahead = 0;
        params.rc.i_lookahead = 0;
        params.rc.i_rc_method = X264_RC_ABR;
        params.rc.i_bitrate = settings.bitrateKbps;
        params.rc.i_vbv_max_bitrate = settings.bitrateKbps;
        params.rc.i_vbv_buffer_size = settings.bitrateKbps;
        params.b_repeat_headers = 1;
        params.b_annexb = 1;
        if (profile_(&params, settings.highProfile ? "high" : "baseline") < 0) {
            throw std::runtime_error("x264 rejected the requested profile");
        }
        return params;
    }

    void ValidateFrame(const I420Frame& frame) const {
        if (encoder_ == nullptr || frame.y == nullptr || frame.u == nullptr || frame.v == nullptr ||
            frame.width != settings_.width || frame.height != settings_.height ||
            frame.strideY < frame.width || frame.strideU < frame.width / 2 || frame.strideV < frame.width / 2) {
            throw std::invalid_argument("I420 frame does not match the open x264 encoder");
        }
    }

    void CloseEncoder() noexcept {
        if (encoder_ != nullptr) {
            close_(encoder_);
            encoder_ = nullptr;
            ptsSequence_.Reset();
        }
    }

    unsigned maxNalBytes_ = 0;
    detail::LibraryHandle library_ = nullptr;
    OpenFn open_ = nullptr;
    PresetFn preset_ = nullptr;
    ProfileFn profile_ = nullptr;
    EncodeFn encode_ = nullptr;
    ReconfigFn reconfig_ = nullptr;
    CloseFn close_ = nullptr;
    x264_t* encoder_ = nullptr;
    detail::EncoderPtsSequence ptsSequence_;
    EncoderSettings settings_ {};
    bool forceNextIdr_ = true;
};

} // namespace

std::unique_ptr<IVideoEncoder> CreateOpalX264Encoder(const std::filesystem::path& dll, unsigned maxNalBytes) {
    return std::make_unique<X264Encoder>(dll, maxNalBytes);
}

} // namespace telestudio
