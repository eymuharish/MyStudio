#include "telestudio/media/SystemAudioCapture.h"
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

using Microsoft::WRL::ComPtr;
void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) { std::ostringstream s; s << operation << " failed 0x" << std::hex << hr; throw std::runtime_error(s.str()); }
}
std::string Quoted(const std::string& value) {
    std::string out="\""; for(char c:value) { if(c=='"'||c=='\\')out+='\\'; if(c=='\n')out+="\\n";else out+=c; } return out+'"';
}
int Playback(unsigned long pid, int seconds, std::ostream& output) {
    std::mutex lock; uint64_t samples=0, nonzero=0, blocks=0; double sum=0; int peak=0;
    telestudio::media::SystemAudioCapture capture;
    if(!capture.Start(pid,[&](telestudio::media::OwnedAudioBlock&& block){
        std::lock_guard guard(lock); ++blocks;
        for(auto s:block.samples) { ++samples; if(s)++nonzero; sum+=double(s)*s; peak=std::max(peak,std::abs(int(s))); }
    })) throw std::runtime_error(capture.LastError());
    std::this_thread::sleep_for(std::chrono::seconds(seconds)); capture.Stop();
    std::lock_guard guard(lock);
    bool pass=nonzero>1000 && peak>100;
    output << "{\"mode\":\"included-process-Windows-loopback\",\"pid\":" <<pid<<",\"seconds\":"<<seconds
      <<",\"blocks\":"<<blocks<<",\"samples\":"<<samples<<",\"nonzeroSamples\":"<<nonzero
      <<",\"rms\":"<<(samples?std::sqrt(sum/samples):0)<<",\"peak\":"<<peak
      <<",\"result\":\""<<(pass?"PASS":"NO_NONZERO_PLAYBACK")<<"\"}\n";
    return pass?0:1;
}
struct Decoded { uint64_t samples=0,nonzero=0,pixels=0,cyan=0,magenta=0,pcmCount=0,toneCount=0; double micSin=0,micCos=0,roomSin=0,roomCos=0; UINT32 width=0,height=0; LONGLONG first=-1,last=-1; };
Decoded Decode(const wchar_t* path, bool video) {
    ComPtr<IMFSourceReader> reader; Check(MFCreateSourceReaderFromURL(path,nullptr,&reader),"MFCreateSourceReaderFromURL");
    Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"stream selection");
    DWORD stream=video?MF_SOURCE_READER_FIRST_VIDEO_STREAM:MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    Check(reader->SetStreamSelection(stream,TRUE),"selected stream");
    ComPtr<IMFMediaType> type; Check(MFCreateMediaType(&type),"media type");
    Check(type->SetGUID(MF_MT_MAJOR_TYPE,video?MFMediaType_Video:MFMediaType_Audio),"major type");
    Check(type->SetGUID(MF_MT_SUBTYPE,video?MFVideoFormat_NV12:MFAudioFormat_PCM),"subtype");
    if(!video)Check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),"PCM16");
    Check(reader->SetCurrentMediaType(stream,nullptr,type.Get()),"decoded media type");
    UINT32 width=0,height=0,stride=0;
    if(video) {
        ComPtr<IMFMediaType> decoded;Check(reader->GetCurrentMediaType(stream,&decoded),"decoded dimensions");
        Check(MFGetAttributeSize(decoded.Get(),MF_MT_FRAME_SIZE,&width,&height),"frame dimensions");
        if(FAILED(decoded->GetUINT32(MF_MT_DEFAULT_STRIDE,&stride)))stride=width;
    }
    Decoded stats; stats.width=width; stats.height=height;
    for(unsigned i=0;i<200000;++i) {
        DWORD actual=0,flags=0; LONGLONG timestamp=0; ComPtr<IMFSample> sample;
        Check(reader->ReadSample(stream,0,&actual,&flags,&timestamp,&sample),"decode sample");
        if(sample) {
            if(stats.first<0)stats.first=timestamp; stats.last=timestamp; ++stats.samples;
            ComPtr<IMFMediaBuffer> buffer; Check(sample->ConvertToContiguousBuffer(&buffer),"sample buffer");
            BYTE* bytes=nullptr; DWORD length=0; Check(buffer->Lock(&bytes,nullptr,&length),"lock sample");
            if(video) {
                for(DWORD n=0;n<length;++n)if(bytes[n])++stats.nonzero;
                if(stride>=width&&uint64_t(stride)*height*3/2<=length)for(UINT32 y=0;y<height;y+=8)for(UINT32 x=0;x<width;x+=8) {
                    auto luminance=bytes[y*stride+x];auto uv=uint64_t(stride)*height+(y/2)*stride+(x&~1u);
                    auto u=bytes[uv],v=bytes[uv+1];++stats.pixels;
                    if(luminance>70&&luminance<220&&u>140&&u<220&&v<110)++stats.cyan;
                    if(luminance>40&&luminance<190&&u>170&&v>170)++stats.magenta;
                }
            }
            else { const auto* pcm=reinterpret_cast<const int16_t*>(bytes); for(DWORD n=0;n<length/2;++n) {
                if(pcm[n])++stats.nonzero;
                if(timestamp>=10000000) { const double t=double(stats.pcmCount)/48000, value=pcm[n];
                    stats.micSin+=value*std::sin(2*3.141592653589793*1000*t); stats.micCos+=value*std::cos(2*3.141592653589793*1000*t);
                    stats.roomSin+=value*std::sin(2*3.141592653589793*700*t); stats.roomCos+=value*std::cos(2*3.141592653589793*700*t); ++stats.toneCount; }
                ++stats.pcmCount;
            } }
            buffer->Unlock();
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)return stats;
    }
    throw std::runtime_error("decoder sample limit reached");
}
int Recording(const wchar_t* path,std::ostream& output,bool pattern=false) {
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitializeEx"); Check(MFStartup(MF_VERSION),"MFStartup");
    auto video=Decode(path,true); auto audio=Decode(path,false);
    bool pass=video.samples>=10 && audio.samples>=10 && video.nonzero>1000 && audio.nonzero>1000;
    double cyan=video.pixels?double(video.cyan)/video.pixels:0,magenta=video.pixels?double(video.magenta)/video.pixels:0;
    if(pattern)pass=pass&&cyan>=0.35&&magenta<0.002;
    output <<"{\"mode\":\"MediaFoundation-decoded-recording\",\"videoSamples\":"<<video.samples
      <<",\"videoWidth\":"<<video.width<<",\"videoHeight\":"<<video.height
      <<",\"videoNonzeroBytes\":"<<video.nonzero<<",\"audioSamples\":"<<audio.samples
      <<",\"audioNonzeroPcmSamples\":"<<audio.nonzero<<",\"videoSeconds\":"<<(video.last-video.first)/10000000.0
      <<",\"fixtureMic1000HzAmplitude\":"<<(audio.toneCount?2*std::hypot(audio.micSin,audio.micCos)/audio.toneCount:0)
      <<",\"fixtureRoom700HzAmplitude\":"<<(audio.toneCount?2*std::hypot(audio.roomSin,audio.roomCos)/audio.toneCount:0)
      <<",\"audioSeconds\":"<<(audio.last-audio.first)/10000000.0<<",\"videoFirstSeconds\":"<<video.first/10000000.0
      <<",\"videoLastSeconds\":"<<video.last/10000000.0<<",\"audioFirstSeconds\":"<<audio.first/10000000.0<<",\"audioLastSeconds\":"<<audio.last/10000000.0
      <<",\"patternChecked\":"<<(pattern?"true":"false")<<",\"patternPixels\":"<<video.pixels<<",\"cyanFraction\":"<<cyan<<",\"magentaFraction\":"<<magenta
      <<",\"result\":\""<<(pass?"PASS":(pattern?"WRONG_RECORDING_SOURCE":"EMPTY_MEDIA"))<<"\"}\n";
    MFShutdown(); CoUninitialize(); return pass?0:1;
}
int wmain(int argc,wchar_t** argv) {
    std::ofstream file; int result=2;
    try {
        if(argc<4)throw std::runtime_error("playback PID seconds result.json | recording file.mp4 result.json");
        file.open(argv[argc-1],std::ios::binary); if(!file)throw std::runtime_error("cannot create result file");
        if(std::wstring(argv[1])==L"playback" && argc==5) {
            int seconds=std::stoi(argv[3]); if(seconds<1||seconds>30)throw std::runtime_error("bounded duration required");
            result=Playback(std::stoul(argv[2]),seconds,file);
        } else if(std::wstring(argv[1])==L"recording" && argc==4)result=Recording(argv[2],file);
        else if(std::wstring(argv[1])==L"recording-pattern"&&argc==4)result=Recording(argv[2],file,true);
        else throw std::runtime_error("invalid mode");
    } catch(const std::exception& e) { if(file)file<<"{\"result\":\"ERROR\",\"error\":"<<Quoted(e.what())<<"}\n"; std::cerr<<e.what()<<'\n'; }
    return result;
}
