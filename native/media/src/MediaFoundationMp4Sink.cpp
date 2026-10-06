#include "telestudio/media/MediaFoundationMp4Sink.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <ShlObj_core.h>
#include <KnownFolders.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace telestudio {
namespace {
struct AnnexBNalu {
    std::span<const std::uint8_t> payload;
    std::size_t prefixOffset = 0;
    std::size_t prefixLength = 0;
    std::size_t endOffset = 0;
};

std::size_t PrefixLengthAt(std::span<const std::uint8_t> data, std::size_t offset) noexcept {
    if (offset + 4 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 0 && data[offset + 3] == 1) return 4;
    if (offset + 3 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 1) return 3;
    return 0;
}

bool ParseAnnexB(std::span<const std::uint8_t> data, std::vector<AnnexBNalu>& result) {
    if (data.empty() || PrefixLengthAt(data, 0) == 0) return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto prefix = PrefixLengthAt(data, offset);
        if (!prefix) return false;
        const auto nalStart = offset + prefix;
        std::size_t next = nalStart;
        while (next < data.size() && PrefixLengthAt(data, next) == 0) ++next;
        if (nalStart >= next) return false;
        const auto payload = data.subspan(nalStart, next - nalStart);
        const auto header = payload.front();
        const auto type = header & 0x1f;
        if ((header & 0x80) != 0 || type == 0 || type >= 24 ||
            type == 17 || type == 18 || type == 22 || type == 23) return false;
        result.push_back({payload, offset, prefix, next});
        offset = next;
    }
    return !result.empty();
}

bool ToRbsp(std::span<const std::uint8_t> ebsp, std::vector<std::uint8_t>& rbsp) {
    int zeros = 0;
    for (std::size_t index = 0; index < ebsp.size(); ++index) {
        const auto byte = ebsp[index];
        if (zeros >= 2 && byte == 3) {
            if (index + 1 >= ebsp.size() || ebsp[index + 1] > 3) return false;
            zeros = 0;
            continue;
        }
        if (zeros >= 2 && byte <= 2) return false;
        rbsp.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return !rbsp.empty();
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool ReadBit(std::uint32_t& value) noexcept {
        if (bit_ >= bytes_.size() * 8) return false;
        value = (bytes_[bit_ / 8] >> (7 - bit_ % 8)) & 1u;
        ++bit_;
        return true;
    }
    bool ReadUe(std::uint32_t& value) noexcept {
        std::uint32_t bit = 0;
        std::uint32_t zeros = 0;
        while (true) {
            if (!ReadBit(bit)) return false;
            if (bit) break;
            if (++zeros > 30) return false;
        }
        std::uint32_t suffix = 0;
        for (std::uint32_t index = 0; index < zeros; ++index) {
            if (!ReadBit(bit)) return false;
            suffix = (suffix << 1) | bit;
        }
        value = ((1u << zeros) - 1u) + suffix;
        return true;
    }
    bool ReadBits(std::uint32_t count, std::uint32_t& value) noexcept {
        if (count > 32) return false;
        value = 0;
        std::uint32_t bit = 0;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!ReadBit(bit)) return false;
            value = (value << 1) | bit;
        }
        return true;
    }
    bool ReadSe(std::int32_t& value) noexcept {
        std::uint32_t encoded = 0;
        if (!ReadUe(encoded) || encoded > 0x7fffffffu) return false;
        value = (encoded & 1u) ? static_cast<std::int32_t>((encoded + 1u) / 2u)
                               : -static_cast<std::int32_t>(encoded / 2u);
        return true;
    }
    bool HasMoreRbspData() const noexcept {
        const auto total = bytes_.size() * 8;
        if (bit_ >= total) return false;
        auto BitAt = [&](std::size_t position) {
            return (bytes_[position / 8] >> (7 - position % 8)) & 1u;
        };
        if (BitAt(bit_) != 1) return true;
        for (std::size_t position = bit_ + 1; position < total; ++position)
            if (BitAt(position) != 0) return true;
        return false;
    }
    bool ReadRbspTrailingBits() noexcept {
        std::uint32_t bit = 0;
        if (!ReadBit(bit) || bit != 1) return false;
        while ((bit_ & 7u) != 0) {
            if (!ReadBit(bit) || bit != 0) return false;
        }
        return bit_ == bytes_.size() * 8;
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t bit_ = 0;
};

bool ParseHrd(BitReader& bits) {
    std::uint32_t count = 0, value = 0;
    if (!bits.ReadUe(count) || count > 31 || !bits.ReadBits(4, value) ||
        !bits.ReadBits(4, value)) return false;
    for (std::uint32_t index = 0; index <= count; ++index) {
        if (!bits.ReadUe(value) || value > 0x00ffffffu ||
            !bits.ReadUe(value) || value > 0x00ffffffu || !bits.ReadBit(value)) return false;
    }
    return bits.ReadBits(5, value) && bits.ReadBits(5, value) &&
           bits.ReadBits(5, value) && bits.ReadBits(5, value);
}

bool ParseVui(BitReader& bits) {
    std::uint32_t flag = 0, value = 0;
    if (!bits.ReadBit(flag)) return false;
    if (flag) {
        if (!bits.ReadBits(8, value)) return false;
        if (value == 255 && (!bits.ReadBits(16, value) || !bits.ReadBits(16, value))) return false;
    }
    if (!bits.ReadBit(flag) || (flag && !bits.ReadBit(value))) return false;
    if (!bits.ReadBit(flag)) return false;
    if (flag) {
        if (!bits.ReadBits(3, value) || !bits.ReadBit(value) || !bits.ReadBit(flag)) return false;
        if (flag && (!bits.ReadBits(8, value) || !bits.ReadBits(8, value) ||
                     !bits.ReadBits(8, value))) return false;
    }
    if (!bits.ReadBit(flag)) return false;
    if (flag && (!bits.ReadUe(value) || value > 5 || !bits.ReadUe(value) || value > 5)) return false;
    if (!bits.ReadBit(flag)) return false;
    if (flag) {
        std::uint32_t numUnitsInTick = 0, timeScale = 0;
        if (!bits.ReadBits(32, numUnitsInTick) || !bits.ReadBits(32, timeScale) ||
            numUnitsInTick == 0 || timeScale == 0 || !bits.ReadBit(value)) return false;
    }
    std::uint32_t nalHrd = 0, vclHrd = 0;
    if (!bits.ReadBit(nalHrd) || (nalHrd && !ParseHrd(bits)) ||
        !bits.ReadBit(vclHrd) || (vclHrd && !ParseHrd(bits))) return false;
    if ((nalHrd || vclHrd) && !bits.ReadBit(value)) return false;
    if (!bits.ReadBit(value) || !bits.ReadBit(flag)) return false;
    if (flag) {
        if (!bits.ReadBit(value) || !bits.ReadUe(value) || value > 16 ||
            !bits.ReadUe(value) || value > 16 || !bits.ReadUe(value) || value > 16 ||
            !bits.ReadUe(value) || value > 16 || !bits.ReadUe(value) || value > 16 ||
            !bits.ReadUe(value) || value > 16) return false;
    }
    return true;
}

bool ParseSps(std::span<const std::uint8_t> nal, int& profileIdc, std::uint32_t& spsId) {
    if (nal.size() < 5) return false;
    std::vector<std::uint8_t> rbsp;
    if (!ToRbsp(nal.subspan(1), rbsp)) return false;
    BitReader bits(rbsp);
    std::uint32_t profile = 0, constraints = 0, level = 0;
    if (!bits.ReadBits(8, profile) || !bits.ReadBits(8, constraints) ||
        !bits.ReadBits(8, level) || (constraints & 0x03u) != 0 ||
        !bits.ReadUe(spsId) || spsId > 31 || (profile != 66 && profile != 100)) return false;
    if (profile == 100) {
        std::uint32_t chromaFormat = 0, value = 0;
        if (!bits.ReadUe(chromaFormat) || chromaFormat > 3) return false;
        if (chromaFormat == 3 && !bits.ReadBit(value)) return false;
        if (!bits.ReadUe(value) || value > 6 || !bits.ReadUe(value) || value > 6 ||
            !bits.ReadBit(value) || !bits.ReadBit(value) || value != 0) return false;
    }
    std::uint32_t value = 0;
    if (!bits.ReadUe(value) || value > 12) return false;
    std::uint32_t pocType = 0;
    if (!bits.ReadUe(pocType) || pocType > 2) return false;
    if (pocType == 0) {
        if (!bits.ReadUe(value) || value > 12) return false;
    } else if (pocType == 1) {
        std::int32_t signedValue = 0;
        if (!bits.ReadBit(value) || !bits.ReadSe(signedValue) || !bits.ReadSe(signedValue) ||
            !bits.ReadUe(value) || value > 255) return false;
        for (std::uint32_t index = 0; index < value; ++index)
            if (!bits.ReadSe(signedValue)) return false;
    }
    if (!bits.ReadUe(value) || value > 16 || !bits.ReadBit(value) ||
        !bits.ReadUe(value) || !bits.ReadUe(value)) return false;
    std::uint32_t frameMbsOnly = 0;
    if (!bits.ReadBit(frameMbsOnly)) return false;
    if (!frameMbsOnly && !bits.ReadBit(value)) return false;
    if (!bits.ReadBit(value)) return false;
    std::uint32_t cropping = 0;
    if (!bits.ReadBit(cropping)) return false;
    if (cropping) {
        for (int index = 0; index != 4; ++index) if (!bits.ReadUe(value)) return false;
    }
    std::uint32_t vuiPresent = 0;
    if (!bits.ReadBit(vuiPresent) || (vuiPresent && !ParseVui(bits)) ||
        !bits.ReadRbspTrailingBits()) return false;
    profileIdc = static_cast<int>(profile);
    return true;
}

bool ParsePps(std::span<const std::uint8_t> nal, std::uint32_t& ppsId,
              std::uint32_t& referencedSps, bool& hasExtension) {
    if (nal.size() < 2) return false;
    std::vector<std::uint8_t> rbsp;
    if (!ToRbsp(nal.subspan(1), rbsp)) return false;
    BitReader bits(rbsp);
    std::uint32_t value = 0;
    std::int32_t signedValue = 0;
    if (!bits.ReadUe(ppsId) || ppsId > 255 || !bits.ReadUe(referencedSps) ||
        referencedSps > 31 || !bits.ReadBit(value) || !bits.ReadBit(value) ||
        !bits.ReadUe(value) || value != 0 || !bits.ReadUe(value) || value > 31 ||
        !bits.ReadUe(value) || value > 31 || !bits.ReadBit(value) ||
        !bits.ReadBits(2, value) || value > 2 || !bits.ReadSe(signedValue) ||
        !bits.ReadSe(signedValue) || !bits.ReadSe(signedValue) ||
        !bits.ReadBit(value) || !bits.ReadBit(value) || !bits.ReadBit(value)) return false;
    hasExtension = bits.HasMoreRbspData();
    if (hasExtension) {
        std::uint32_t scalingMatrix = 0;
        if (!bits.ReadBit(value) || !bits.ReadBit(scalingMatrix) || scalingMatrix != 0 ||
            !bits.ReadSe(signedValue)) return false;
    }
    return bits.ReadRbspTrailingBits();
}

struct H264Analysis {
    bool valid = false;
    bool hasVcl = false;
    bool hasIdr = false;
    bool hasSps = false;
    bool hasPps = false;
    int profileIdc = 0;
    std::uint32_t ppsId = 0;
    std::vector<std::uint8_t> spsPayload;
    std::vector<std::uint8_t> ppsPayload;
    std::vector<std::uint8_t> sequenceHeader;
};

H264Analysis AnalyzeH264(std::span<const std::uint8_t> data, bool requireBootstrap,
                         std::optional<std::uint32_t> admittedPps = std::nullopt,
                         std::span<const std::uint8_t> admittedSpsPayload = {},
                         std::span<const std::uint8_t> admittedPpsPayload = {}) {
    H264Analysis result;
    std::vector<AnnexBNalu> nalus;
    if (!ParseAnnexB(data, nalus)) return result;
    std::vector<std::uint8_t> spsBytes;
    std::vector<std::uint8_t> ppsBytes;
    std::uint32_t spsId = 0;
    std::uint32_t ppsId = 0;
    std::uint32_t ppsSpsId = 0;
    bool ppsHasExtension = false;
    bool seenVcl = false;
    bool seenAud = false;
    for (const auto& record : nalus) {
        const auto nal = record.payload;
        const int type = nal.front() & 0x1f;
        if (type == 7) {
            if (seenVcl) return {};
            std::uint32_t parsedId = 0;
            int parsedProfile = 0;
            if (!ParseSps(nal, parsedProfile, parsedId)) return {};
            const std::vector<std::uint8_t> bytes(nal.begin(), nal.end());
            if (result.hasSps) {
                if (parsedId != spsId || bytes != spsBytes) return {};
                continue;
            }
            result.hasSps = true;
            result.profileIdc = parsedProfile;
            spsId = parsedId;
            spsBytes = bytes;
            result.spsPayload = bytes;
            result.sequenceHeader.insert(result.sequenceHeader.end(),
                data.begin() + record.prefixOffset, data.begin() + record.endOffset);
        } else if (type == 8) {
            if (seenVcl) return {};
            std::uint32_t parsedPpsId = 0;
            std::uint32_t referencedSps = 0;
            bool parsedHasExtension = false;
            if (!ParsePps(nal, parsedPpsId, referencedSps, parsedHasExtension)) return {};
            const std::vector<std::uint8_t> bytes(nal.begin(), nal.end());
            if (result.hasPps) {
                if (parsedPpsId != ppsId || referencedSps != ppsSpsId || bytes != ppsBytes) return {};
                continue;
            }
            result.hasPps = true;
            result.ppsId = parsedPpsId;
            ppsId = parsedPpsId;
            ppsSpsId = referencedSps;
            ppsHasExtension = parsedHasExtension;
            ppsBytes = bytes;
            result.ppsPayload = bytes;
            result.sequenceHeader.insert(result.sequenceHeader.end(),
                data.begin() + record.prefixOffset, data.begin() + record.endOffset);
        } else if (type == 9) {
            if (seenVcl || seenAud) return {};
            seenAud = true;
        } else if (type == 1 || type == 5) {
            std::vector<std::uint8_t> rbsp;
            if (!ToRbsp(nal.subspan(1), rbsp)) return {};
            BitReader bits(rbsp);
            std::uint32_t firstMb = 0;
            std::uint32_t sliceType = 0;
            std::uint32_t slicePpsId = 0;
            if (!bits.ReadUe(firstMb) || !bits.ReadUe(sliceType) || !bits.ReadUe(slicePpsId) ||
                sliceType > 9 ||
                sliceType % 5 == 1 || (type == 5 && sliceType % 5 != 2)) return {};
            const auto expectedPps = result.hasPps ? std::optional<std::uint32_t>(ppsId) : admittedPps;
            if (!expectedPps || slicePpsId != *expectedPps) return {};
            if (!seenVcl && firstMb != 0) return {};
            if (seenVcl && firstMb == 0) return {};
            seenVcl = true;
            result.hasVcl = true;
            result.hasIdr = result.hasIdr || type == 5;
        } else if (type == 2 || type == 3 || type == 4 || type == 19) {
            return {};
        }
    }
    if (!result.hasVcl) return {};
    if (result.hasPps && (!result.hasSps || ppsSpsId != spsId)) return {};
    if (result.hasPps && result.profileIdc == 100 && !ppsHasExtension) return {};
    if (result.hasPps && admittedPps && ppsId != *admittedPps) return {};
    if (result.hasSps && (!admittedSpsPayload.empty() || !admittedPpsPayload.empty())) {
        if (admittedSpsPayload.empty() || admittedPpsPayload.empty() ||
            !std::ranges::equal(result.spsPayload, admittedSpsPayload) ||
            !std::ranges::equal(result.ppsPayload, admittedPpsPayload)) return {};
    }
    if (result.hasSps != result.hasPps) return {};
    if (requireBootstrap && (!result.hasSps || !result.hasPps || !result.hasIdr)) return {};
    result.valid = true;
    return result;
}

void Check(HRESULT value, const char* message) {
    if (FAILED(value)) throw std::runtime_error(message);
}

winrt::com_ptr<IMFMediaType> NewMediaType() {
    winrt::com_ptr<IMFMediaType> type;
    Check(MFCreateMediaType(type.put()), "MFCreateMediaType failed");
    return type;
}

winrt::com_ptr<IMFSample> MakeSample(const void* bytes, std::size_t byteCount,
                                     std::int64_t timestamp, std::int64_t duration) {
    if (byteCount > MAXDWORD || timestamp < 0 || duration <= 0) throw std::invalid_argument("invalid sample");
    winrt::com_ptr<IMFMediaBuffer> buffer;
    Check(MFCreateMemoryBuffer(static_cast<DWORD>(byteCount), buffer.put()), "MFCreateMemoryBuffer failed");
    BYTE* destination = nullptr;
    Check(buffer->Lock(&destination, nullptr, nullptr), "IMFMediaBuffer::Lock failed");
    std::memcpy(destination, bytes, byteCount);
    buffer->Unlock();
    Check(buffer->SetCurrentLength(static_cast<DWORD>(byteCount)), "SetCurrentLength failed");
    winrt::com_ptr<IMFSample> sample;
    Check(MFCreateSample(sample.put()), "MFCreateSample failed");
    Check(sample->AddBuffer(buffer.get()), "AddBuffer failed");
    Check(sample->SetSampleTime(timestamp), "SetSampleTime failed");
    Check(sample->SetSampleDuration(duration), "SetSampleDuration failed");
    return sample;
}
}

H264BootstrapInfo ParseH264Bootstrap(std::span<const std::uint8_t> annexB) noexcept {
    try {
        const auto analyzed = AnalyzeH264(annexB, true);
        return {analyzed.valid, analyzed.hasIdr, analyzed.profileIdc,
                std::move(analyzed.sequenceHeader)};
    } catch (...) {
        return {};
    }
}

namespace {
class ProductionMfSinkBackend final : public IMfSinkBackend {
public:
    void Startup() override {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx failed");
        comInitialized_ = true;
        try {
            Check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup failed");
            mfStarted_ = true;
        } catch (...) {
            ReleaseAndShutdown();
            throw;
        }
    }

    void Create(const std::filesystem::path& partPath, const MfSinkPlan& plan) override {
        winrt::com_ptr<IMFAttributes> attributes;
        Check(MFCreateAttributes(attributes.put(), 2), "MFCreateAttributes failed");
        Check(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING,
                                    plan.disableThrottling ? TRUE : FALSE), "disable throttling failed");
        if (!plan.mpeg4Container) throw std::invalid_argument("MPEG-4 container required");
        Check(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4),
              "MP4 container attribute failed");
        Check(MFCreateSinkWriterFromURL(partPath.c_str(), nullptr, attributes.get(), writer_.put()),
              "MFCreateSinkWriterFromURL failed");
    }

    void AddVideoStream(const MfSinkPlan& plan) override {
        auto type = VideoType(plan.video);
        Check(writer_->AddStream(type.get(), &videoStream_), "video AddStream failed");
    }

    void SetVideoInput(const MfSinkPlan& plan) override {
        auto type = VideoType(plan.video);
        Check(writer_->SetInputMediaType(videoStream_, type.get(), nullptr), "video SetInputMediaType failed");
    }

    void AddAudioStream(const MfSinkPlan& plan) override {
        auto output = NewMediaType();
        Check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "audio major failed");
        Check(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC), "AAC subtype failed");
        Check(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, plan.audioBitsPerSample), "AAC bits failed");
        Check(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, plan.audioSampleRate), "AAC rate failed");
        Check(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, plan.audioChannels), "AAC channels failed");
        Check(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, plan.audioAverageBytesPerSecond), "AAC bytes failed");
        Check(output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, plan.audioBlockAlignment), "AAC alignment failed");
        Check(output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, plan.aacPayloadType), "AAC payload failed");
        Check(output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, plan.aacProfileLevel), "AAC profile failed");
        Check(writer_->AddStream(output.get(), &audioStream_), "audio AddStream failed");
    }

    void SetAudioInput(const MfSinkPlan& plan) override {
        auto input = NewMediaType();
        Check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "PCM major failed");
        Check(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "PCM subtype failed");
        Check(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, plan.audioBitsPerSample), "PCM bits failed");
        Check(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, plan.audioSampleRate), "PCM rate failed");
        Check(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, plan.audioChannels), "PCM channels failed");
        Check(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, plan.pcmBlockAlignment), "PCM alignment failed");
        Check(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, plan.pcmAverageBytesPerSecond), "PCM bytes failed");
        Check(input->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE), "PCM fixed failed");
        Check(input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE), "PCM independent failed");
        Check(writer_->SetInputMediaType(audioStream_, input.get(), nullptr), "audio SetInputMediaType failed");
    }

    void BeginWriting() override { Check(writer_->BeginWriting(), "BeginWriting failed"); }
    void WriteVideo(const RecordingVideoSample& value) override {
        auto sample = MakeSample(value.annexB.data(), value.annexB.size(), value.timestamp100ns,
                                 value.duration100ns);
        if (value.cleanPoint) Check(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE), "clean point failed");
        if (value.discontinuity) Check(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE), "discontinuity failed");
        Check(writer_->WriteSample(videoStream_, sample.get()), "video WriteSample failed");
    }
    void WriteAudio(const RecordingAudioSample& value) override {
        auto sample = MakeSample(value.pcm.data(), value.pcm.size() * sizeof(std::int16_t),
                                 value.timestamp100ns, value.duration100ns);
        if (value.discontinuity) Check(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE),
                                      "audio discontinuity failed");
        Check(writer_->WriteSample(audioStream_, sample.get()), "audio WriteSample failed");
    }
    void Finalize() override { Check(writer_->Finalize(), "Finalize failed"); }
    void ReleaseAndShutdown() noexcept override {
        writer_ = nullptr;
        if (mfStarted_) { MFShutdown(); mfStarted_ = false; }
        if (comInitialized_) { CoUninitialize(); comInitialized_ = false; }
    }

private:
    winrt::com_ptr<IMFSinkWriter> writer_;
    DWORD videoStream_ = 0;
    DWORD audioStream_ = 0;
    bool comInitialized_ = false;
    bool mfStarted_ = false;

    static winrt::com_ptr<IMFMediaType> VideoType(const RecordingVideoFormat& video) {
        auto type = NewMediaType();
        Check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "video major type failed");
        Check(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "video subtype failed");
        Check(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, video.width, video.height), "frame size failed");
        Check(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, video.fps, 1), "frame rate failed");
        Check(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "pixel aspect failed");
        Check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "progressive failed");
        Check(type->SetUINT32(MF_MT_AVG_BITRATE, video.bitrateKbps * 1000u), "bitrate failed");
        Check(type->SetUINT32(MF_MT_MPEG2_PROFILE, video.highProfile ? 100u : 66u), "profile failed");
        Check(type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, video.sequenceHeader.data(),
                            static_cast<UINT32>(video.sequenceHeader.size())), "sequence header failed");
        Check(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE), "independent flag failed");
        return type;
    }
};
}

struct MediaFoundationMp4Sink::Impl {
    MfSinkBackendFactory factory;
    std::unique_ptr<IMfSinkBackend> backend;
    winrt::com_ptr<IMFSinkWriter> writer;
    bool includeAudio = false;
    bool everOpened = false;
    bool active = false;
    bool finalized = false;
    std::int64_t lastVideo = -1;
    std::int64_t lastAudio = -1;
    std::optional<std::uint32_t> admittedPps;
    std::vector<std::uint8_t> admittedSpsPayload;
    std::vector<std::uint8_t> admittedPpsPayload;

    void CloseRuntime() noexcept {
        if (backend) backend->ReleaseAndShutdown();
        backend.reset();
        active = false;
    }
};

MediaFoundationMp4Sink::MediaFoundationMp4Sink(MfSinkBackendFactory factory)
    : impl_(std::make_unique<Impl>()) {
    impl_->factory = factory ? std::move(factory) : [] { return std::make_unique<ProductionMfSinkBackend>(); };
}
MediaFoundationMp4Sink::~MediaFoundationMp4Sink() { impl_->CloseRuntime(); }

void MediaFoundationMp4Sink::Open(const std::filesystem::path& partPath,
                                  const RecordingVideoFormat& video, bool includeAudio) {
    if (impl_->everOpened || partPath.empty() || video.width <= 0 || video.height <= 0 ||
        (video.width & 1) || (video.height & 1) || video.fps <= 0 || video.fps > 30 ||
        video.bitrateKbps <= 0 || video.sequenceHeader.empty()) {
        throw std::invalid_argument("invalid MP4 sink configuration");
    }
    std::vector<std::uint8_t> probe = video.sequenceHeader;
    const std::uint8_t idr[] = {0,0,0,1,0x65,0xb8};
    probe.insert(probe.end(), std::begin(idr), std::end(idr));
    const auto parsed = AnalyzeH264(probe, true);
    if (!parsed.valid || parsed.profileIdc != (video.highProfile ? 100 : 66)) {
        throw std::invalid_argument("H.264 sequence header/profile mismatch");
    }
    impl_->admittedPps = parsed.ppsId;
    impl_->admittedSpsPayload = parsed.spsPayload;
    impl_->admittedPpsPayload = parsed.ppsPayload;
    impl_->everOpened = true;
    impl_->backend = impl_->factory();
    if (!impl_->backend) throw std::runtime_error("MF backend factory failed");
    const MfSinkPlan plan{video, includeAudio};
    try {
        impl_->backend->Startup();
        impl_->backend->Create(partPath, plan);
        impl_->backend->AddVideoStream(plan);
        impl_->backend->SetVideoInput(plan);
        if (includeAudio) {
            impl_->backend->AddAudioStream(plan);
            impl_->backend->SetAudioInput(plan);
        }
        impl_->backend->BeginWriting();
        impl_->includeAudio = includeAudio;
        impl_->active = true;
    } catch (...) {
        impl_->CloseRuntime();
        throw;
    }
}

void MediaFoundationMp4Sink::WriteVideo(const RecordingVideoSample& value) {
    if (!impl_->active) throw std::invalid_argument("invalid video sample");
    const auto analyzed = AnalyzeH264(value.annexB, false, impl_->admittedPps,
                                      impl_->admittedSpsPayload, impl_->admittedPpsPayload);
    if (!analyzed.valid || value.timestamp100ns < impl_->lastVideo ||
        value.duration100ns <= 0 || value.cleanPoint != analyzed.hasIdr) {
        impl_->CloseRuntime();
        throw std::invalid_argument("invalid video sample");
    }
    try {
        impl_->backend->WriteVideo(value);
    } catch (...) {
        impl_->CloseRuntime();
        throw;
    }
    impl_->lastVideo = value.timestamp100ns;
}

void MediaFoundationMp4Sink::WriteAudio(const RecordingAudioSample& value) {
    if (!impl_->active) throw std::invalid_argument("invalid audio sample");
    if (!impl_->includeAudio || value.pcm.size() != 960 ||
        value.timestamp100ns < impl_->lastAudio || value.duration100ns != 200'000) {
        impl_->CloseRuntime();
        throw std::invalid_argument("invalid audio sample");
    }
    try {
        impl_->backend->WriteAudio(value);
    } catch (...) {
        impl_->CloseRuntime();
        throw;
    }
    impl_->lastAudio = value.timestamp100ns;
}

void MediaFoundationMp4Sink::Finalize() {
    if (!impl_->active || impl_->finalized) throw std::logic_error("sink is not open");
    try {
        impl_->backend->Finalize();
        impl_->finalized = true;
        impl_->CloseRuntime();
    } catch (...) {
        impl_->CloseRuntime();
        throw;
    }
}

} // namespace telestudio
