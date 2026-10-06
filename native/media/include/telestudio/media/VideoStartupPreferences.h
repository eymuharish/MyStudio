#pragma once
#include "MediaTypes.h"
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace telestudio {
// An absent pair preserves defaults. A partial/invalid pair never partially applies.
// Source-enable state is deliberately not part of startup configuration.
inline std::optional<VideoPreferences> ParseVideoStartupPreferences(const char* resolution,const char* bitrate) {
    if(!resolution && !bitrate) return std::nullopt;
    if(!resolution || !bitrate) throw std::invalid_argument("Incomplete video startup preferences");
    VideoPreferences result;
    const std::string_view value(resolution), rate(bitrate);
    if(value=="auto") result.resolution=VideoResolution::Auto;
    else if(value=="SD") result.resolution=VideoResolution::Sd;
    else if(value=="HD") result.resolution=VideoResolution::Hd;
    else if(value=="FHD") result.resolution=VideoResolution::Fhd;
    else throw std::invalid_argument("Invalid startup video resolution");
    if(rate!="auto") {
        int parsed=0;
        const auto converted=std::from_chars(rate.data(),rate.data()+rate.size(),parsed);
        if(converted.ec!=std::errc{} || converted.ptr!=rate.data()+rate.size() || parsed<128 || parsed>20000)
            throw std::invalid_argument("Invalid startup video bitrate");
        result.bitrateKbps=parsed;
    }
    return result;
}
}
