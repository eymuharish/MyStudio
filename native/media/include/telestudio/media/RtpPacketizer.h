#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace telestudio {

struct RtpPayload {
    std::vector<uint8_t> bytes;
    bool marker = false;
};

enum class RtpDepacketizeStatus {
    Complete,
    PacketLoss,
    Malformed,
    UnsupportedPacketization,
    TooLarge
};

struct RtpPayloadView {
    std::span<const uint8_t> bytes;
    bool missing = false;
};

struct RtpAccessUnit {
    RtpDepacketizeStatus status;
    std::vector<uint8_t> annexB;
    bool randomAccess;
};

std::vector<RtpPayload> PacketizeH264(std::span<const uint8_t> annexB, size_t maxPayload = 1200);
std::vector<RtpPayload> PacketizeH265(std::span<const uint8_t> annexB, size_t maxPayload = 1200);
RtpAccessUnit DepacketizeH264(
    std::span<const RtpPayloadView> payloads,
    size_t maxAccessUnitBytes = 8 * 1024 * 1024);
RtpAccessUnit DepacketizeH265(
    std::span<const RtpPayloadView> payloads,
    size_t maxAccessUnitBytes = 8 * 1024 * 1024);

} // namespace telestudio
