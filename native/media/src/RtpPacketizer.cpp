#include "telestudio/media/RtpPacketizer.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace telestudio {
namespace {

struct NalView { size_t offset; size_t size; };

size_t StartCodeSize(std::span<const uint8_t> bytes, size_t offset) {
    if (offset + 4 <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0 &&
        bytes[offset + 2] == 0 && bytes[offset + 3] == 1) return 4;
    if (offset + 3 <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 1) return 3;
    return 0;
}

std::vector<NalView> ParseAnnexB(std::span<const uint8_t> bytes) {
    if (bytes.empty() || StartCodeSize(bytes, 0) == 0) {
        throw std::invalid_argument("Access unit must begin with a three- or four-byte Annex-B delimiter");
    }
    std::vector<std::pair<size_t, size_t>> starts;
    for (size_t offset = 0; offset < bytes.size();) {
        const size_t prefix = StartCodeSize(bytes, offset);
        if (prefix != 0) {
            starts.emplace_back(offset, prefix);
            offset += prefix;
        } else {
            ++offset;
        }
    }
    std::vector<NalView> nals;
    nals.reserve(starts.size());
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t begin = starts[i].first + starts[i].second;
        const size_t end = i + 1 < starts.size() ? starts[i + 1].first : bytes.size();
        if (begin >= end) throw std::invalid_argument("Annex-B access unit contains an empty NAL");
        nals.push_back({begin, end - begin});
    }
    return nals;
}

void FinishMarkers(std::vector<RtpPayload>& payloads) {
    if (payloads.empty()) throw std::invalid_argument("Access unit has no NAL payloads");
    payloads.back().marker = true;
}

RtpAccessUnit Failure(RtpDepacketizeStatus status) {
    return {status, {}, false};
}

bool HasCapacity(size_t current, size_t additional, size_t maximum) {
    return current <= maximum && additional <= maximum - current;
}

bool AppendNal(
    std::vector<uint8_t>& annexB,
    std::span<const uint8_t> nal,
    size_t maximum) {
    if (!HasCapacity(annexB.size(), 4, maximum) ||
        !HasCapacity(annexB.size() + 4, nal.size(), maximum)) {
        return false;
    }
    annexB.insert(annexB.end(), {0, 0, 0, 1});
    annexB.insert(annexB.end(), nal.begin(), nal.end());
    return true;
}

bool AppendFragment(
    std::vector<uint8_t>& annexB,
    std::span<const uint8_t> fragment,
    size_t maximum) {
    if (!HasCapacity(annexB.size(), fragment.size(), maximum)) return false;
    annexB.insert(annexB.end(), fragment.begin(), fragment.end());
    return true;
}

bool BeginFragmentedNal(
    std::vector<uint8_t>& annexB,
    std::span<const uint8_t> header,
    std::span<const uint8_t> fragment,
    size_t maximum) {
    if (!HasCapacity(annexB.size(), 4, maximum) ||
        !HasCapacity(annexB.size() + 4, header.size(), maximum) ||
        !HasCapacity(annexB.size() + 4 + header.size(), fragment.size(), maximum)) {
        return false;
    }
    annexB.insert(annexB.end(), {0, 0, 0, 1});
    annexB.insert(annexB.end(), header.begin(), header.end());
    annexB.insert(annexB.end(), fragment.begin(), fragment.end());
    return true;
}

} // namespace

std::vector<RtpPayload> PacketizeH264(std::span<const uint8_t> annexB, size_t maxPayload) {
    if (maxPayload == 0) throw std::invalid_argument("H.264 maxPayload must be non-zero");
    const auto nals = ParseAnnexB(annexB);
    std::vector<RtpPayload> result;
    for (const auto& view : nals) {
        const auto nal = annexB.subspan(view.offset, view.size);
        const uint8_t header = nal.front();
        const uint8_t type = header & 0x1f;
        if ((header & 0x80) != 0 || type >= 24) throw std::invalid_argument("Invalid H.264 Annex-B NAL header");
        if (nal.size() <= maxPayload) {
            result.push_back({std::vector<uint8_t>(nal.begin(), nal.end()), false});
            continue;
        }
        if (maxPayload < 3 || nal.size() < 2) throw std::invalid_argument("H.264 FU-A payload limit is too small");
        const size_t capacity = maxPayload - 2;
        size_t offset = 1;
        while (offset < nal.size()) {
            const size_t count = std::min(capacity, nal.size() - offset);
            const bool start = offset == 1;
            const bool end = offset + count == nal.size();
            std::vector<uint8_t> fragment;
            fragment.reserve(count + 2);
            fragment.push_back(static_cast<uint8_t>((header & 0xe0) | 28));
            fragment.push_back(static_cast<uint8_t>((start ? 0x80 : 0) | (end ? 0x40 : 0) | type));
            fragment.insert(fragment.end(), nal.begin() + static_cast<std::ptrdiff_t>(offset),
                            nal.begin() + static_cast<std::ptrdiff_t>(offset + count));
            result.push_back({std::move(fragment), false});
            offset += count;
        }
    }
    FinishMarkers(result);
    return result;
}

std::vector<RtpPayload> PacketizeH265(std::span<const uint8_t> annexB, size_t maxPayload) {
    if (maxPayload == 0) throw std::invalid_argument("H.265 maxPayload must be non-zero");
    const auto nals = ParseAnnexB(annexB);
    std::vector<RtpPayload> result;
    for (const auto& view : nals) {
        const auto nal = annexB.subspan(view.offset, view.size);
        if (nal.size() < 2) throw std::invalid_argument("H.265 NAL header is incomplete");
        const uint8_t type = (nal[0] >> 1) & 0x3f;
        if ((nal[0] & 0x80) != 0 || (nal[1] & 0x07) == 0 || type == 48 || type == 49 || type == 50) {
            throw std::invalid_argument("Invalid H.265 Annex-B NAL header");
        }
        if (nal.size() <= maxPayload) {
            result.push_back({std::vector<uint8_t>(nal.begin(), nal.end()), false});
            continue;
        }
        if (maxPayload < 4) throw std::invalid_argument("H.265 FU payload limit is too small");
        const size_t capacity = maxPayload - 3;
        size_t offset = 2;
        while (offset < nal.size()) {
            const size_t count = std::min(capacity, nal.size() - offset);
            const bool start = offset == 2;
            const bool end = offset + count == nal.size();
            std::vector<uint8_t> fragment;
            fragment.reserve(count + 3);
            fragment.push_back(static_cast<uint8_t>((nal[0] & 0x81) | (49 << 1)));
            fragment.push_back(nal[1]);
            fragment.push_back(static_cast<uint8_t>((start ? 0x80 : 0) | (end ? 0x40 : 0) | type));
            fragment.insert(fragment.end(), nal.begin() + static_cast<std::ptrdiff_t>(offset),
                            nal.begin() + static_cast<std::ptrdiff_t>(offset + count));
            result.push_back({std::move(fragment), false});
            offset += count;
        }
    }
    FinishMarkers(result);
    return result;
}

RtpAccessUnit DepacketizeH264(
    std::span<const RtpPayloadView> payloads,
    size_t maxAccessUnitBytes) {
    if (payloads.empty()) return Failure(RtpDepacketizeStatus::Malformed);

    std::vector<uint8_t> annexB;
    bool randomAccess = false;
    bool fuOpen = false;
    uint8_t fuIndicator = 0;
    uint8_t fuType = 0;

    for (const auto& payloadView : payloads) {
        if (payloadView.missing) return Failure(RtpDepacketizeStatus::PacketLoss);
        const auto payload = payloadView.bytes;
        if (payload.empty()) return Failure(RtpDepacketizeStatus::Malformed);

        const uint8_t indicator = payload[0];
        const uint8_t type = indicator & 0x1f;
        if ((indicator & 0x80) != 0 || type == 0) return Failure(RtpDepacketizeStatus::Malformed);
        if (fuOpen && type != 28) return Failure(RtpDepacketizeStatus::Malformed);

        if (type <= 23) {
            if (!AppendNal(annexB, payload, maxAccessUnitBytes)) return Failure(RtpDepacketizeStatus::TooLarge);
            randomAccess = randomAccess || type == 5;
            continue;
        }

        if (type == 24) {
            size_t offset = 1;
            bool foundNal = false;
            uint8_t maximumNri = 0;
            while (offset < payload.size()) {
                if (payload.size() - offset < 2) return Failure(RtpDepacketizeStatus::Malformed);
                const size_t nalSize = (static_cast<size_t>(payload[offset]) << 8) | payload[offset + 1];
                offset += 2;
                if (nalSize == 0 || nalSize > payload.size() - offset) {
                    return Failure(RtpDepacketizeStatus::Malformed);
                }
                const auto nal = payload.subspan(offset, nalSize);
                const uint8_t nalType = nal[0] & 0x1f;
                if ((nal[0] & 0x80) != 0 || nalType == 0 || nalType > 23) {
                    return Failure(RtpDepacketizeStatus::Malformed);
                }
                if (!AppendNal(annexB, nal, maxAccessUnitBytes)) return Failure(RtpDepacketizeStatus::TooLarge);
                randomAccess = randomAccess || nalType == 5;
                maximumNri = std::max(maximumNri, static_cast<uint8_t>(nal[0] & 0x60));
                foundNal = true;
                offset += nalSize;
            }
            if (!foundNal || (indicator & 0x60) != maximumNri) {
                return Failure(RtpDepacketizeStatus::Malformed);
            }
            continue;
        }

        if (type == 28) {
            if (payload.size() < 3) return Failure(RtpDepacketizeStatus::Malformed);
            const uint8_t fuHeader = payload[1];
            const bool start = (fuHeader & 0x80) != 0;
            const bool end = (fuHeader & 0x40) != 0;
            const uint8_t originalType = fuHeader & 0x1f;
            if ((fuHeader & 0x20) != 0 || (start && end) || originalType == 0 || originalType > 23) {
                return Failure(RtpDepacketizeStatus::Malformed);
            }

            const auto fragment = payload.subspan(2);
            if (start) {
                if (fuOpen) return Failure(RtpDepacketizeStatus::Malformed);
                const uint8_t reconstructed = static_cast<uint8_t>((indicator & 0xe0) | originalType);
                if (!BeginFragmentedNal(annexB, std::span<const uint8_t>(&reconstructed, 1), fragment,
                                        maxAccessUnitBytes)) {
                    return Failure(RtpDepacketizeStatus::TooLarge);
                }
                fuOpen = true;
                fuIndicator = indicator;
                fuType = originalType;
                randomAccess = randomAccess || originalType == 5;
            } else {
                if (!fuOpen || (indicator & 0xe0) != (fuIndicator & 0xe0) || originalType != fuType) {
                    return Failure(RtpDepacketizeStatus::Malformed);
                }
                if (!AppendFragment(annexB, fragment, maxAccessUnitBytes)) return Failure(RtpDepacketizeStatus::TooLarge);
            }
            if (end) fuOpen = false;
            continue;
        }

        return Failure(RtpDepacketizeStatus::UnsupportedPacketization);
    }

    if (fuOpen) return Failure(RtpDepacketizeStatus::PacketLoss);
    return {RtpDepacketizeStatus::Complete, std::move(annexB), randomAccess};
}

RtpAccessUnit DepacketizeH265(
    std::span<const RtpPayloadView> payloads,
    size_t maxAccessUnitBytes) {
    if (payloads.empty()) return Failure(RtpDepacketizeStatus::Malformed);

    std::vector<uint8_t> annexB;
    bool randomAccess = false;
    bool fuOpen = false;
    uint8_t fuLayerHigh = 0;
    uint8_t fuSecondHeader = 0;
    uint8_t fuType = 0;

    for (const auto& payloadView : payloads) {
        if (payloadView.missing) return Failure(RtpDepacketizeStatus::PacketLoss);
        const auto payload = payloadView.bytes;
        if (payload.size() < 2) return Failure(RtpDepacketizeStatus::Malformed);

        const uint8_t first = payload[0];
        const uint8_t second = payload[1];
        const uint8_t type = (first >> 1) & 0x3f;
        if ((first & 0x80) != 0 || (second & 0x07) == 0) return Failure(RtpDepacketizeStatus::Malformed);
        if (fuOpen && type != 49) return Failure(RtpDepacketizeStatus::Malformed);

        if (type <= 47) {
            if (!AppendNal(annexB, payload, maxAccessUnitBytes)) return Failure(RtpDepacketizeStatus::TooLarge);
            randomAccess = randomAccess || (type >= 16 && type <= 23);
            continue;
        }

        if (type == 48) {
            size_t offset = 2;
            size_t nalCount = 0;
            uint8_t minimumLayerId = 63;
            uint8_t minimumTid = 7;
            while (offset < payload.size()) {
                if (payload.size() - offset < 2) return Failure(RtpDepacketizeStatus::Malformed);
                const size_t nalSize = (static_cast<size_t>(payload[offset]) << 8) | payload[offset + 1];
                offset += 2;
                if (nalSize < 2 || nalSize > payload.size() - offset) {
                    return Failure(RtpDepacketizeStatus::Malformed);
                }
                const auto nal = payload.subspan(offset, nalSize);
                const uint8_t nalType = (nal[0] >> 1) & 0x3f;
                if ((nal[0] & 0x80) != 0 || (nal[1] & 0x07) == 0 || nalType > 47) {
                    return Failure(RtpDepacketizeStatus::Malformed);
                }
                if (!AppendNal(annexB, nal, maxAccessUnitBytes)) return Failure(RtpDepacketizeStatus::TooLarge);
                randomAccess = randomAccess || (nalType >= 16 && nalType <= 23);
                const uint8_t layerId = static_cast<uint8_t>(((nal[0] & 0x01) << 5) | (nal[1] >> 3));
                minimumLayerId = std::min(minimumLayerId, layerId);
                minimumTid = std::min(minimumTid, static_cast<uint8_t>(nal[1] & 0x07));
                ++nalCount;
                offset += nalSize;
            }
            const uint8_t apLayerId = static_cast<uint8_t>(((first & 0x01) << 5) | (second >> 3));
            const uint8_t apTid = second & 0x07;
            if (nalCount < 2 || apLayerId != minimumLayerId || apTid != minimumTid) {
                return Failure(RtpDepacketizeStatus::Malformed);
            }
            continue;
        }

        if (type == 49) {
            if (payload.size() < 4) return Failure(RtpDepacketizeStatus::Malformed);
            const uint8_t fuHeader = payload[2];
            const bool start = (fuHeader & 0x80) != 0;
            const bool end = (fuHeader & 0x40) != 0;
            const uint8_t originalType = fuHeader & 0x3f;
            if ((start && end) || originalType > 47) return Failure(RtpDepacketizeStatus::Malformed);

            const auto fragment = payload.subspan(3);
            if (start) {
                if (fuOpen) return Failure(RtpDepacketizeStatus::Malformed);
                const uint8_t reconstructed[2] {
                    static_cast<uint8_t>((first & 0x81) | (originalType << 1)),
                    second
                };
                if (!BeginFragmentedNal(annexB, reconstructed, fragment, maxAccessUnitBytes)) {
                    return Failure(RtpDepacketizeStatus::TooLarge);
                }
                fuOpen = true;
                fuLayerHigh = first & 0x01;
                fuSecondHeader = second;
                fuType = originalType;
                randomAccess = randomAccess || (originalType >= 16 && originalType <= 23);
            } else {
                if (!fuOpen || (first & 0x01) != fuLayerHigh || second != fuSecondHeader || originalType != fuType) {
                    return Failure(RtpDepacketizeStatus::Malformed);
                }
                if (!AppendFragment(annexB, fragment, maxAccessUnitBytes)) return Failure(RtpDepacketizeStatus::TooLarge);
            }
            if (end) fuOpen = false;
            continue;
        }

        if (type == 50) return Failure(RtpDepacketizeStatus::UnsupportedPacketization);
        return Failure(RtpDepacketizeStatus::UnsupportedPacketization);
    }

    if (fuOpen) return Failure(RtpDepacketizeStatus::PacketLoss);
    return {RtpDepacketizeStatus::Complete, std::move(annexB), randomAccess};
}

} // namespace telestudio
