#include "telestudio/media/CodecPolicy.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        using namespace telestudio;

        const Protocol protocol = Protocol::H323;
        const VideoSource source = VideoSource::Desktop;
        Require(protocol == Protocol::H323, "Protocol must be shared by engine and media");
        Require(source == VideoSource::Desktop, "VideoSource must be shared by engine and media");

        const RemoteCapabilities capable{true, true, true, true, 1920, 1080, 10000};

        const auto sx20Auto = ResolveMediaFormat(
            EndpointModel::Sx20, Protocol::Sip, VideoSource::Camera, QualityMode::Auto, capable);
        Require(sx20Auto.codec == VideoCodec::H264, "SX20 Auto must use H.264");
        Require(sx20Auto.width == 1920 && sx20Auto.height == 1080, "SX20 Auto must select 1080p");
        Require(sx20Auto.fps == 30, "SX20 Auto must select 30 fps");
        Require(sx20Auto.bitrateKbps == 2500, "SX20 Auto must select 2500 kbps");
        Require(sx20Auto.highProfile, "SX20 Auto must select High Profile when supported");

        const auto sx20Maximum = ResolveMediaFormat(
            EndpointModel::Sx20, Protocol::Sip, VideoSource::Camera, QualityMode::Maximum, capable);
        Require(sx20Maximum.codec == VideoCodec::H264, "SX20 Maximum must use H.264");
        Require(sx20Maximum.width == 1920 && sx20Maximum.height == 1080, "SX20 Maximum must select 1080p");
        Require(sx20Maximum.fps == 60, "SX20 Maximum must select 60 fps when supported");
        Require(sx20Maximum.bitrateKbps == 6000, "SX20 Maximum must select 6000 kbps");
        const RemoteCapabilities sx20Capped{true, true, true, true, 1920, 1080, 3500};
        const auto sx20CappedMaximum = ResolveMediaFormat(
            EndpointModel::Sx20, Protocol::Sip, VideoSource::Camera, QualityMode::Maximum, sx20Capped);
        Require(sx20CappedMaximum.bitrateKbps == 3500, "Remote bitrate must cap SX20 Maximum");

        const auto sonySip = ResolveMediaFormat(
            EndpointModel::PcsXg77, Protocol::Sip, VideoSource::Camera, QualityMode::Maximum, capable);
        Require(sonySip.codec == VideoCodec::H264, "Sony SIP must use H.264");
        Require(sonySip.width == 1280 && sonySip.height == 720, "Sony SIP must be capped at 720p");
        Require(sonySip.fps == 30, "Sony SIP must be capped at 30 fps");
        Require(sonySip.bitrateKbps == 2048, "Sony SIP must be capped at 2048 kbps");

        const auto sonyH323Maximum = ResolveMediaFormat(
            EndpointModel::PcsXg77, Protocol::H323, VideoSource::Camera, QualityMode::Maximum, capable);
        Require(sonyH323Maximum.bitrateKbps <= 2048,
                "Sony H.323 Maximum must be capped at the configured receive bitrate");

        const RemoteCapabilities sonyNoHigh{true, false, true, true, 1920, 1080, 10000};
        for (const auto model : {EndpointModel::PcsXg77, EndpointModel::PcsXg80}) {
            const auto sony = ResolveMediaFormat(
                model, Protocol::Sip, VideoSource::Camera, QualityMode::Maximum, sonyNoHigh);
            Require(sony.codec == VideoCodec::H264, "Sony models must not select H.265");
            Require(!sony.highProfile, "Sony high profile must follow H.264 capability");
        }

        const auto generic = ResolveMediaFormat(
            EndpointModel::Generic, Protocol::Sip, VideoSource::Camera, QualityMode::Auto, capable);
        Require(generic.codec == VideoCodec::H264, "Generic peers must prefer H.264 when available");
        Require(generic.width == 1280 && generic.height == 720 && generic.fps == 30 && generic.bitrateKbps == 2000,
                "Generic peers must use the conservative default");

        const auto manual = ResolveMediaFormat(EndpointModel::Generic, Protocol::Sip,
            VideoSource::Camera, QualityMode::Auto, capable,
            {VideoResolution::Sd, 1500});
        Require(manual.width == 640 && manual.height == 480 && manual.bitrateKbps == 1500,
                "manual video preferences must override auto defaults");
        const auto peerLimited = ResolveMediaFormat(EndpointModel::Generic, Protocol::Sip,
            VideoSource::Camera, QualityMode::Auto,
            {true, true, false, false, 1920, 1080, 64}, {VideoResolution::Hd, 128});
        Require(peerLimited.bitrateKbps == 64,
                "peer bitrate below manual minimum remains an applied peer limit");

        const RemoteCapabilities h265Only{false, false, true, false, 1920, 1080, 10000};
        for (const auto model : {EndpointModel::Generic, EndpointModel::Auto}) {
            const auto sipH265 = ResolveMediaFormat(
                model, Protocol::Sip, VideoSource::Camera, QualityMode::Auto, h265Only);
            Require(sipH265.codec == VideoCodec::H265,
                    "Generic and Auto SIP peers may fall back to H.265");

            bool h323Rejected = false;
            try {
                (void)ResolveMediaFormat(
                    model, Protocol::H323, VideoSource::Camera, QualityMode::Auto, h265Only);
            } catch (const std::invalid_argument&) {
                h323Rejected = true;
            }
            Require(h323Rejected, "H.265-only H.323 peers must be rejected");
        }

        const RemoteCapabilities h264AndH265{true, false, true, false, 1920, 1080, 10000};
        const auto genericH323 = ResolveMediaFormat(
            EndpointModel::Generic, Protocol::H323, VideoSource::Camera, QualityMode::Auto, h264AndH265);
        Require(genericH323.codec == VideoCodec::H264,
                "Generic H.323 peers must choose H.264 when H.264 and H.265 are available");
        for (const auto model : {EndpointModel::Sx20, EndpointModel::PcsXg77,
                                 EndpointModel::PcsXg80}) {
            bool rejected = false;
            try {
                (void)ResolveMediaFormat(model, Protocol::Sip, VideoSource::Camera, QualityMode::Auto, h265Only);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            Require(rejected, "Non-generic H.265-only peers must be rejected");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
