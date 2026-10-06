#pragma once
#include <opal/mediafmt.h>
namespace studio::opal {
inline void ConfigureVideoFormat(OpalMediaFormat&format,unsigned width,unsigned height,unsigned bitrate,unsigned fps){
 format.SetOptionInteger(OpalVideoFormat::FrameWidthOption(),width);format.SetOptionInteger(OpalVideoFormat::FrameHeightOption(),height);
 format.SetOptionInteger(OpalVideoFormat::MaxRxFrameWidthOption(),width);format.SetOptionInteger(OpalVideoFormat::MaxRxFrameHeightOption(),height);
 format.SetOptionInteger(OpalMediaFormat::TargetBitRateOption(),bitrate*1000);format.SetOptionInteger(OpalMediaFormat::MaxBitRateOption(),bitrate*1000);format.SetOptionInteger(OpalMediaFormat::FrameTimeOption(),90000/fps);
}
}
