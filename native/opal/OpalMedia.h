#pragma once
#include <telestudio/media/ComposedVideoSender.h>
#include <telestudio/media/VideoSurfacePublisher.h>
#include <telestudio/media/AudioControlProcessor.h>
#include <telestudio/media/OutboundAudioMixer.h>
#include <telestudio/media/SystemAudioCapture.h>
#include <telestudio/media/ProtocolAudioRateBridge.h>
#include <telestudio/media/AudioBlockFramer.h>
#include <telestudio/media/LectureRecorder.h>
#include "OpalSharedRecordingWorker.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <deque>
namespace studio::opal {
class StudioMedia {
public:
 using Emit=std::function<void(std::string,std::string)>;
 explicit StudioMedia(telestudio::ComposedVideoSource::DesktopFactory desktop={},std::function<void(telestudio::I420Frame)> recordedInput={},telestudio::media::IProcessLoopbackBackend* loopback=nullptr);~StudioMedia();
 void Start(uint64_t generation);void Stop();void Pump(const Emit&,bool reportDeviceErrors=true);
 bool ReadAudio(void*,size_t,size_t&,int,int);bool WriteAudio(const void*,size_t,size_t&,int,int);
 bool SetEchoCancellation(bool);bool EchoAvailable()const;bool EchoEnabled()const;
 bool ReadVideo(std::vector<uint8_t>&,int,int,uint32_t,bool content=false);
 bool WriteVideo(const void*,size_t,bool content=false);
 void VideoAccepted(uint32_t,bool,bool,int,int,int);
 bool SystemAudio(bool);void Ack(bool,telestudio::SurfaceAcknowledgement,uint64_t);
 void Configure(telestudio::VideoCompositionSettings,int=30);
 void Desktop(bool,std::string={},bool separate=false);void Camera(bool,std::string={});
 telestudio::RecordingResult StartRecording(telestudio::RecordingOptions);
 telestudio::RecordingSnapshot RecordingStatus();
 void StopRecording();
 bool SharedRecordingSource(std::string_view)const;
 telestudio::RecordingResult ResumeRecording();
 std::shared_ptr<telestudio::LectureRecorder> recorder;
 telestudio::media::AudioControlProcessor audio;
 std::unique_ptr<telestudio::media::OutboundAudioMixer> mixer;
 telestudio::media::SystemAudioCapture systemAudio;
 telestudio::ComposedVideoSender source,contentSource;
 telestudio::VideoCompositionSettings settings{1920,1080};
 std::atomic<uint64_t> generation{0},captureSamples{0},playbackSamples{0},sentFrames{0},receivedFrames{0},sentContentFrames{0},receivedContentFrames{0},decodedNonzeroSamples{0},deviceSubmittedNonzeroSamples{0};
 std::atomic<int> appliedWidth{0},appliedHeight{0},appliedBitrateKbps{0};
 std::unique_ptr<telestudio::VideoSurfacePublisher> localSurface,remoteSurface;
 std::atomic<bool> micMuted{false};bool systemEnabled=false,testMode=false;
 std::string resolution="auto";unsigned bitrateKbps=4000,sendBufferMs=40;int fps=30;
 std::mutex audioMutex;
private:
 std::shared_ptr<telestudio::ISharedRecordingWorker> recordingWorker;
 telestudio::ComposedVideoSource::DesktopFactory RecordingDesktopFactory(bool,telestudio::ComposedVideoSource::DesktopFactory);
 void PauseSharedRecording();
 void CancelPendingRecording(std::string code,std::string detail);
 void PumpPendingRecording(const Emit&);
 struct Impl;std::unique_ptr<Impl> impl;
};
}
