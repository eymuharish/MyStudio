#include "telestudio/media/RtpPacketizer.h"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace telestudio {

RtpAccessUnit DepacketizeH264(
    std::initializer_list<RtpPayloadView> payloads,
    size_t maxAccessUnitBytes = 8 * 1024 * 1024) {
    return DepacketizeH264(
        std::span<const RtpPayloadView>(payloads.begin(), payloads.size()),
        maxAccessUnitBytes);
}

RtpAccessUnit DepacketizeH265(
    std::initializer_list<RtpPayloadView> payloads,
    size_t maxAccessUnitBytes = 8 * 1024 * 1024) {
    return DepacketizeH265(
        std::span<const RtpPayloadView>(payloads.begin(), payloads.size()),
        maxAccessUnitBytes);
}

} // namespace telestudio

namespace {

using telestudio::RtpAccessUnit;
using telestudio::RtpDepacketizeStatus;
using telestudio::RtpPayloadView;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void RequireRejected(Function&& function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, message);
}

void RequireFailure(const RtpAccessUnit& result, RtpDepacketizeStatus status, const char* message) {
    Require(result.status == status, message);
    Require(result.annexB.empty(), "A failed depacketization must not expose partial Annex-B data");
    Require(!result.randomAccess, "A failed depacketization must not claim random access");
}

void RequireComplete(
    const RtpAccessUnit& result,
    const std::vector<uint8_t>& expected,
    bool randomAccess,
    const char* message) {
    Require(result.status == RtpDepacketizeStatus::Complete, message);
    Require(result.annexB == expected, message);
    Require(result.randomAccess == randomAccess, message);
}

std::vector<RtpPayloadView> Views(const std::vector<telestudio::RtpPayload>& payloads) {
    std::vector<RtpPayloadView> views;
    views.reserve(payloads.size());
    for (const auto& payload : payloads) views.push_back({payload.bytes, false});
    return views;
}

void VerifyMarkersAndSize(const std::vector<telestudio::RtpPayload>& payloads, size_t limit) {
    Require(!payloads.empty(), "Packetization must not return empty output");
    size_t markers = 0;
    for (size_t i = 0; i < payloads.size(); ++i) {
        Require(!payloads[i].bytes.empty() && payloads[i].bytes.size() <= limit, "Payload exceeds negotiated limit");
        markers += payloads[i].marker ? 1u : 0u;
        Require(payloads[i].marker == (i + 1 == payloads.size()), "Marker belongs only on final access-unit payload");
    }
    Require(markers == 1, "Exactly one marker is required");
}

void VerifyH264Packetization() {
    const std::vector<uint8_t> mixed {0,0,0,1,0x67,0x42, 0,0,1,0x68,0xce};
    const auto mixedPackets = telestudio::PacketizeH264(mixed, 3);
    Require(mixedPackets.size() == 2 && mixedPackets[0].bytes == std::vector<uint8_t>({0x67,0x42}) &&
            mixedPackets[1].bytes == std::vector<uint8_t>({0x68,0xce}), "Mixed Annex-B prefixes preserve NAL order");
    VerifyMarkersAndSize(mixedPackets, 3);
    RequireComplete(
        telestudio::DepacketizeH264(Views(mixedPackets)),
        {0,0,0,1,0x67,0x42, 0,0,0,1,0x68,0xce}, false,
        "Multiple H.264 single-NAL packets round-trip to canonical Annex-B");

    std::vector<uint8_t> raw(4000, 0x5a);
    raw[0] = 0x65;
    std::vector<uint8_t> annexB {0,0,0,1};
    annexB.insert(annexB.end(), raw.begin(), raw.end());
    const auto fragments = telestudio::PacketizeH264(annexB);
    VerifyMarkersAndSize(fragments, 1200);
    RequireComplete(telestudio::DepacketizeH264(Views(fragments)), annexB, true,
                    "H.264 FU-A must round-trip byte-identically");

    std::vector<uint8_t> boundary {0,0,1,0x61};
    boundary.resize(1203, 0x11);
    Require(telestudio::PacketizeH264(boundary, 1200).size() == 1, "Exact H.264 boundary uses a single-NAL packet");
    RequireRejected([&]{ telestudio::PacketizeH264({}); }, "Empty H.264 input must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH264(std::vector<uint8_t>{0x65}); }, "Missing H.264 delimiter must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH264(std::vector<uint8_t>{0,0,1,0x65,0,0,1}); }, "Trailing delimiter must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH264(std::vector<uint8_t>{0,0,1,0x80}); }, "H.264 forbidden bit must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH264(std::vector<uint8_t>{0,0,1,0x78}); }, "H.264 RTP-only NAL type must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH264(annexB, 2); }, "H.264 FU limit below three must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH264(mixed, 0); }, "Zero H.264 payload limit must be rejected");
}

void VerifyH264Depacketization() {
    const std::vector<uint8_t> single {0x65,0xaa};
    RequireComplete(telestudio::DepacketizeH264({RtpPayloadView{single}}),
                    {0,0,0,1,0x65,0xaa}, true, "H.264 IDR single NAL is random access");

    const std::vector<uint8_t> stap {0x78, 0,2,0x67,0x11, 0,2,0x65,0x22};
    RequireComplete(telestudio::DepacketizeH264({RtpPayloadView{stap}}),
                    {0,0,0,1,0x67,0x11, 0,0,0,1,0x65,0x22}, true,
                    "H.264 STAP-A emits one Annex-B delimiter per NAL");
    const std::vector<uint8_t> stapWrongNri {0x58, 0,2,0x67,0x11, 0,2,0x45,0x22};
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{stapWrongNri}}),
                   RtpDepacketizeStatus::Malformed,
                   "H.264 STAP-A NRI must equal the maximum aggregated NAL NRI");

    const std::vector<uint8_t> fuStart {0x7c,0x85,0xaa};
    const std::vector<uint8_t> fuMiddle {0x7c,0x05,0xbb};
    const std::vector<uint8_t> fuEnd {0x7c,0x45,0xcc};
    RequireComplete(
        telestudio::DepacketizeH264({RtpPayloadView{fuStart}, RtpPayloadView{fuMiddle}, RtpPayloadView{fuEnd}}),
        {0,0,0,1,0x65,0xaa,0xbb,0xcc}, true, "H.264 FU-A reconstructs its original header and payload");

    const std::vector<uint8_t> pStart {0x7c,0x81,0x10};
    const std::vector<uint8_t> pEnd {0x7c,0x41,0x11};
    RequireComplete(
        telestudio::DepacketizeH264({RtpPayloadView{fuStart}, RtpPayloadView{fuEnd},
                                    RtpPayloadView{pStart}, RtpPayloadView{pEnd}}),
        {0,0,0,1,0x65,0xaa,0xcc, 0,0,0,1,0x61,0x10,0x11}, true,
        "Consecutive H.264 fragmented NALs remain separate");

    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{fuStart}, RtpPayloadView{{}, true}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::PacketLoss, "A missing H.264 FU packet reports packet loss");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{fuMiddle}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "H.264 FU without a start is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{fuStart}, RtpPayloadView{fuStart}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "Duplicate H.264 FU start is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{fuStart}, RtpPayloadView{fuEnd}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "Duplicate H.264 FU end is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{fuStart}, RtpPayloadView{single}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "Mixing H.264 packetization inside an FU is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{std::vector<uint8_t>{0x7c,0x85}}}),
                   RtpDepacketizeStatus::Malformed, "An empty H.264 FU fragment is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{std::vector<uint8_t>{24,0,3,0x67,0x11}}}),
                   RtpDepacketizeStatus::Malformed, "An oversized H.264 STAP-A length is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{std::vector<uint8_t>{24,0,0}}}),
                   RtpDepacketizeStatus::Malformed, "A zero H.264 STAP-A length is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{std::vector<uint8_t>{}}}),
                   RtpDepacketizeStatus::Malformed, "An empty H.264 payload is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{std::vector<uint8_t>{0xe5,0x11}}}),
                   RtpDepacketizeStatus::Malformed, "The H.264 forbidden bit is malformed");
    RequireFailure(telestudio::DepacketizeH264({RtpPayloadView{single}}, 5),
                   RtpDepacketizeStatus::TooLarge, "H.264 output above the configured cap is rejected");
    RequireComplete(telestudio::DepacketizeH264({RtpPayloadView{single}}, 6),
                    {0,0,0,1,0x65,0xaa}, true, "H.264 output equal to the configured cap is accepted");
}

void VerifyH265Packetization() {
    const std::vector<uint8_t> small {
        0,0,0,1,static_cast<uint8_t>(32 << 1),0x01,0x11,
        0,0,0,1,static_cast<uint8_t>(1 << 1),0x01,0x22
    };
    const auto smallPackets = telestudio::PacketizeH265(small, 1200);
    VerifyMarkersAndSize(smallPackets, 1200);
    RequireComplete(telestudio::DepacketizeH265(Views(smallPackets)), small, false,
                    "Multiple H.265 single-NAL packets round-trip to canonical Annex-B");

    std::vector<uint8_t> raw(4000, 0xa5);
    raw[0] = static_cast<uint8_t>(19 << 1);
    raw[1] = 0x95;
    std::vector<uint8_t> annexB {0,0,0,1};
    annexB.insert(annexB.end(), raw.begin(), raw.end());
    const auto fragments = telestudio::PacketizeH265(annexB);
    VerifyMarkersAndSize(fragments, 1200);
    RequireComplete(telestudio::DepacketizeH265(Views(fragments)), annexB, true,
                    "H.265 type-49 FU must round-trip byte-identically");

    std::vector<uint8_t> boundary {0,0,0,1,static_cast<uint8_t>(1 << 1),0x01};
    boundary.resize(1204, 0x22);
    Require(telestudio::PacketizeH265(boundary, 1200).size() == 1, "Exact H.265 boundary uses a single-NAL packet");
    RequireRejected([&]{ telestudio::PacketizeH265(std::vector<uint8_t>{0,0,1,0x02}); }, "Incomplete H.265 header must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH265(std::vector<uint8_t>{0,0,1,0x80,0x01}); }, "H.265 forbidden bit must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH265(std::vector<uint8_t>{0,0,1,0x02,0x00}); }, "H.265 zero TID must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH265(std::vector<uint8_t>{0,0,1,static_cast<uint8_t>(49<<1),0x01}); }, "H.265 RTP-only FU must be rejected");
    RequireRejected([&]{ telestudio::PacketizeH265(annexB, 3); }, "H.265 FU limit below four must be rejected");
}

void VerifyH265Depacketization() {
    const std::vector<uint8_t> single {static_cast<uint8_t>(19 << 1),0x01,0xaa};
    RequireComplete(telestudio::DepacketizeH265({RtpPayloadView{single}}),
                    {0,0,0,1,static_cast<uint8_t>(19 << 1),0x01,0xaa}, true,
                    "H.265 IRAP single NAL is random access");

    const std::vector<uint8_t> ap {static_cast<uint8_t>(48 << 1),0x01,
                                   0,3,static_cast<uint8_t>(32 << 1),0x01,0x11,
                                   0,3,static_cast<uint8_t>(16 << 1),0x01,0x22};
    RequireComplete(telestudio::DepacketizeH265({RtpPayloadView{ap}}),
                    {0,0,0,1,static_cast<uint8_t>(32 << 1),0x01,0x11,
                     0,0,0,1,static_cast<uint8_t>(16 << 1),0x01,0x22}, true,
                    "H.265 AP emits one Annex-B delimiter per NAL");
    const std::vector<uint8_t> oneNalAp {
        static_cast<uint8_t>(48 << 1),0x01,
        0,3,static_cast<uint8_t>(32 << 1),0x01,0x11
    };
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{oneNalAp}}),
                   RtpDepacketizeStatus::Malformed, "H.265 AP requires at least two aggregated NALs");
    const std::vector<uint8_t> apWrongLayerId {
        static_cast<uint8_t>(48 << 1),0x11,
        0,2,static_cast<uint8_t>(32 << 1),0x09,
        0,2,static_cast<uint8_t>(33 << 1),0x19
    };
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{apWrongLayerId}}),
                   RtpDepacketizeStatus::Malformed,
                   "H.265 AP LayerId must equal the minimum aggregated NAL LayerId");
    const std::vector<uint8_t> apWrongTid {
        static_cast<uint8_t>(48 << 1),0x02,
        0,2,static_cast<uint8_t>(32 << 1),0x01,
        0,2,static_cast<uint8_t>(33 << 1),0x03
    };
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{apWrongTid}}),
                   RtpDepacketizeStatus::Malformed,
                   "H.265 AP TID must equal the minimum aggregated NAL TID");

    const std::vector<uint8_t> fuStart {static_cast<uint8_t>(49 << 1),0x01,static_cast<uint8_t>(0x80 | 19),0xaa};
    const std::vector<uint8_t> fuMiddle {static_cast<uint8_t>(49 << 1),0x01,19,0xbb};
    const std::vector<uint8_t> fuEnd {static_cast<uint8_t>(49 << 1),0x01,static_cast<uint8_t>(0x40 | 19),0xcc};
    RequireComplete(
        telestudio::DepacketizeH265({RtpPayloadView{fuStart}, RtpPayloadView{fuMiddle}, RtpPayloadView{fuEnd}}),
        {0,0,0,1,static_cast<uint8_t>(19 << 1),0x01,0xaa,0xbb,0xcc}, true,
        "H.265 FU reconstructs its original header and payload");

    const std::vector<uint8_t> pStart {static_cast<uint8_t>(49 << 1),0x01,static_cast<uint8_t>(0x80 | 1),0x10};
    const std::vector<uint8_t> pEnd {static_cast<uint8_t>(49 << 1),0x01,static_cast<uint8_t>(0x40 | 1),0x11};
    RequireComplete(
        telestudio::DepacketizeH265({RtpPayloadView{fuStart}, RtpPayloadView{fuEnd},
                                    RtpPayloadView{pStart}, RtpPayloadView{pEnd}}),
        {0,0,0,1,static_cast<uint8_t>(19 << 1),0x01,0xaa,0xcc,
         0,0,0,1,static_cast<uint8_t>(1 << 1),0x01,0x10,0x11}, true,
        "Consecutive H.265 fragmented NALs remain separate");

    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{fuStart}, RtpPayloadView{{}, true}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::PacketLoss, "A missing H.265 FU packet reports packet loss");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{fuMiddle}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "H.265 FU without a start is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{fuStart}, RtpPayloadView{fuStart}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "Duplicate H.265 FU start is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{fuStart}, RtpPayloadView{fuEnd}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "Duplicate H.265 FU end is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{fuStart}, RtpPayloadView{single}, RtpPayloadView{fuEnd}}),
                   RtpDepacketizeStatus::Malformed, "Mixing H.265 packetization inside an FU is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{std::vector<uint8_t>{static_cast<uint8_t>(49 << 1),0x01,static_cast<uint8_t>(0x80 | 19)}}}),
                   RtpDepacketizeStatus::Malformed, "An empty H.265 FU fragment is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{std::vector<uint8_t>{static_cast<uint8_t>(48 << 1),0x01,0,4,0x40,0x01,0x11}}}),
                   RtpDepacketizeStatus::Malformed, "An oversized H.265 AP length is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{std::vector<uint8_t>{static_cast<uint8_t>(48 << 1),0x01,0,0}}}),
                   RtpDepacketizeStatus::Malformed, "A zero H.265 AP length is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{std::vector<uint8_t>{}}}),
                   RtpDepacketizeStatus::Malformed, "An empty H.265 payload is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{std::vector<uint8_t>{0x80,0x01}}}),
                   RtpDepacketizeStatus::Malformed, "The H.265 forbidden bit is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{std::vector<uint8_t>{0x02,0x00}}}),
                   RtpDepacketizeStatus::Malformed, "H.265 TID zero is malformed");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{std::vector<uint8_t>{static_cast<uint8_t>(50 << 1),0x01,0x00}}}),
                   RtpDepacketizeStatus::UnsupportedPacketization, "H.265 PACI is unsupported");
    RequireFailure(telestudio::DepacketizeH265({RtpPayloadView{single}}, 6),
                   RtpDepacketizeStatus::TooLarge, "H.265 output above the configured cap is rejected");
    RequireComplete(telestudio::DepacketizeH265({RtpPayloadView{single}}, 7),
                    {0,0,0,1,static_cast<uint8_t>(19 << 1),0x01,0xaa}, true,
                    "H.265 output equal to the configured cap is accepted");
}

} // namespace

int main() {
    try {
        VerifyH264Packetization();
        VerifyH264Depacketization();
        VerifyH265Packetization();
        VerifyH265Depacketization();
        std::cout << "media_rtp_packetizer_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "media_rtp_packetizer_tests: " << error.what() << '\n';
        return 1;
    }
}
