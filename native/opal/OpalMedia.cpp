#include "OpalMedia.h"
#include "OpalEchoCanceller.h"
#include "SharedRecordingGate.h"
#include <codec/opalplugin.h>
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>
#include <iostream>
#include <map>
using namespace telestudio;
using namespace telestudio::media;
namespace studio::opal {
namespace {
std::string quote(std::string_view s){std::string o="\"";for(unsigned char c:s){if(c=='"'||c=='\\')o+='\\';o+=c<32?' ':c;}return o+'"';}
UINT device(bool input){const char*id=std::getenv(input?"ESSASTUDIO_MICROPHONE_ID":"ESSASTUDIO_SPEAKER_ID");if(!id||!*id)return WAVE_MAPPER;int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,id,-1,nullptr,0);if(!length)throw std::runtime_error("Invalid audio endpoint ID");std::wstring expected(length,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,id,-1,expected.data(),length);expected.resize(length-1);for(UINT i=0,count=input?waveInGetNumDevs():waveOutGetNumDevs();i<count;++i){ULONG bytes=0;auto rc=input?waveInMessage(reinterpret_cast<HWAVEIN>(UINT_PTR(i)),DRV_QUERYFUNCTIONINSTANCEIDSIZE,reinterpret_cast<DWORD_PTR>(&bytes),0):waveOutMessage(reinterpret_cast<HWAVEOUT>(UINT_PTR(i)),DRV_QUERYFUNCTIONINSTANCEIDSIZE,reinterpret_cast<DWORD_PTR>(&bytes),0);if(rc||bytes<2||bytes>65536)continue;std::vector<wchar_t>value(bytes/2+1);rc=input?waveInMessage(reinterpret_cast<HWAVEIN>(UINT_PTR(i)),DRV_QUERYFUNCTIONINSTANCEID,reinterpret_cast<DWORD_PTR>(value.data()),bytes):waveOutMessage(reinterpret_cast<HWAVEOUT>(UINT_PTR(i)),DRV_QUERYFUNCTIONINSTANCEID,reinterpret_cast<DWORD_PTR>(value.data()),bytes);if(!rc&&expected==value.data())return i;}throw std::runtime_error("Selected audio endpoint unavailable");}
class RecordingWorker final:public ISharedRecordingWorker {
 std::shared_ptr<ISharedRecordingWorker> worker;std::atomic<uint64_t> g{0};
public:explicit RecordingWorker(std::shared_ptr<ISharedRecordingWorker>w):worker(std::move(w)){}
 RecordingError Start(const RecordingOptions&o,uint64_t gen,int64_t epoch)noexcept override{g=gen;return worker->Start(o,gen,epoch);}
 bool PushVideo(std::shared_ptr<const ComposedVideoFrame> pixels,int64_t timestamp)noexcept override{return worker->PushVideo(std::move(pixels),timestamp);}
 bool Push(OwnedAudioBlock b)noexcept override{b.callGeneration=g;return worker->Push(std::move(b));}
 void TapUnavailable(AudioTapKind s,uint64_t gen)noexcept override{worker->TapUnavailable(s,gen);}
 bool SetPaused(bool p,int64_t now)noexcept override{return worker->SetPaused(p,now);}
 void Stop()noexcept override{worker->Stop();}
 std::optional<RecordingWorkerUpdate>Poll()noexcept override{return worker->Poll();}
 RecordingWaitOutcome WaitQuiesced(std::chrono::milliseconds t)noexcept override{return worker->WaitQuiesced(t);}
 bool IsQuiesced()const noexcept override{return worker->IsQuiesced();}
};
class EvenDesktopCapture final:public IDesktopCaptureSource {
 DesktopCaptureSource capture;int width,height;
public:
 EvenDesktopCapture(int w,int h):width(w),height(h){}
 void Start(std::string_view id,int fps,FrameCallback frame,CaptureErrorCallback error)override{capture.StartOwned(id,fps,width,height,[frame=std::move(frame)](OwnedCaptureFrame value){frame(value.pixels->View());},std::move(error));}
 void Stop()override{capture.Stop();}
};
class RecordingDesktopCapture final:public IDesktopCaptureSource {
 std::shared_ptr<IDesktopCaptureSource> capture;std::function<void(I420Frame)> tap;std::function<void()> failed;
public:
 RecordingDesktopCapture(std::shared_ptr<IDesktopCaptureSource>source,std::function<void(I420Frame)> observe,std::function<void()> error):capture(std::move(source)),tap(std::move(observe)),failed(std::move(error)){}
 void Start(std::string_view id,int fps,FrameCallback frame,CaptureErrorCallback error)override{capture->Start(id,fps,[tap=tap,frame=std::move(frame)](I420Frame value){tap(value);frame(value);},[failed=failed,error=std::move(error)](const std::string&detail){failed();if(error)error(detail);});}
 void Stop()override{capture->Stop();}
};
std::filesystem::path bin(){std::wstring s(32768,L'\0');auto n=GetModuleFileNameW(nullptr,s.data(),DWORD(s.size()));s.resize(n);return std::filesystem::path(s).parent_path();}
}
struct StudioMedia::Impl {
 EchoCanceller echo;
 HWAVEIN input=nullptr;HWAVEOUT output=nullptr;
 std::array<WAVEHDR,8>inHeaders{},outHeaders{};
 std::array<std::array<int16_t,960>,8>inBuffers{},outBuffers{};
 std::atomic<bool>inputFailed{false},outputFailed{false};
 bool systemPending=false;std::optional<bool>systemState;std::string systemError;
 unsigned systemRetries=0;std::chrono::steady_clock::time_point systemRetryAt{};
 QpcMonotonicClock clock;AudioBlockFramer inputFramer{clock},outputFramer{clock};
 std::unique_ptr<ProtocolAudioRateBridge>sendBridge,receiveBridge;
 std::deque<int16_t> sendQueue;uint64_t samplePosition=0;
 std::mutex videoMutex;std::optional<ComposedVideoFrame>latest,contentLatest;
 ComposedVideoSender::Completion latestDone,contentDone;
 struct FrameAck {ComposedVideoSender::Completion done;std::shared_ptr<const ComposedVideoFrame> recordPixels;uint64_t revision=0;};
 std::map<uint64_t,FrameAck> awaiting;
 std::shared_ptr<const ComposedVideoFrame> rawMain,rawContent,latestRecord,contentRecord;
 SharedRecordingGate sharedRecording;std::chrono::steady_clock::time_point lastSharedAccepted;bool sharedOutputStalled=false;std::atomic<bool> pauseWhenReady{false};bool recordingStopping=false;
 struct PendingRecording {RecordingOptions options;uint64_t generation,revision;std::chrono::steady_clock::time_point deadline;};
 std::optional<PendingRecording> pendingRecording;
 std::optional<RecordingSnapshot> waitingRecording;
 std::deque<std::pair<std::string,std::string>> recordingEvents;
 std::atomic<bool>cameraWanted{false},desktopWanted{false};
 std::atomic<bool> separateMode{false},separateWanted{false};std::optional<DeliveredVideoState>contentState;
 std::optional<DeliveredVideoState>lastState;uint64_t videoRevision=0;double tone=440;
 void Close(){if(input){waveInStop(input);waveInReset(input);for(auto&h:inHeaders)waveInUnprepareHeader(input,&h,sizeof(h));waveInClose(input);input=nullptr;}if(output){waveOutReset(output);for(auto&h:outHeaders)if(h.dwFlags&WHDR_PREPARED)waveOutUnprepareHeader(output,&h,sizeof(h));waveOutClose(output);output=nullptr;}}
};
StudioMedia::StudioMedia(ComposedVideoSource::DesktopFactory desktop,std::function<void(I420Frame)> recordedInput,IProcessLoopbackBackend* loopback):source({},RecordingDesktopFactory(false,desktop)),contentSource({},RecordingDesktopFactory(true,desktop)),mixer(std::make_unique<OutboundAudioMixer>()),systemAudio(loopback),impl(std::make_unique<Impl>()){
 auto*value=std::getenv("ESSASTUDIO_TEST_MODE");testMode=value&&std::string(value)=="1";
 if(auto*frequency=std::getenv("ESSASTUDIO_TEST_TONE_HZ")){impl->tone=std::stod(frequency);if(!std::isfinite(impl->tone)||impl->tone<100||impl->tone>2000)throw std::runtime_error("Invalid test frequency");}
 if(auto host=SurfaceHostFromEnvironment()){localSurface=std::make_unique<VideoSurfacePublisher>(*host);remoteSurface=std::make_unique<VideoSurfacePublisher>(*host);}
 auto*testOutput=std::getenv("ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR");auto override=ResolveTestRecordingOutputOverride(testOutput?std::optional<std::string_view>{"1"}:std::nullopt,testOutput?std::optional<std::filesystem::path>{testOutput}:std::nullopt,std::filesystem::temp_directory_path());if(testOutput&&!testMode)throw std::runtime_error("Recording override requires explicit test mode");
 recordingWorker=std::make_shared<RecordingWorker>(CreateOpalSharedRecordingWorker(bin()/L"libx264-165.dll",override?override->Directory():std::filesystem::path{},override,std::move(recordedInput)));
 recorder=std::make_shared<LectureRecorder>(recordingWorker,std::make_shared<QpcMonotonicClock>(),[this](auto kind){return generation.load()!=0&&(kind==AudioTapKind::Microphone?captureSamples.load()>0:playbackSamples.load()>0);});
}
ComposedVideoSource::DesktopFactory StudioMedia::RecordingDesktopFactory(bool content,ComposedVideoSource::DesktopFactory factory){
 return [this,content,factory=std::move(factory)](){std::shared_ptr<IDesktopCaptureSource> capture=factory?factory():std::make_shared<EvenDesktopCapture>(1920,1080);if(!capture)return std::shared_ptr<IDesktopCaptureSource>{};return std::shared_ptr<IDesktopCaptureSource>(std::make_shared<RecordingDesktopCapture>(std::move(capture),[this,content](I420Frame frame){
  auto pixels=std::make_shared<ComposedVideoFrame>(ComposeVideoFrame(&frame,nullptr,{1920,1080},frame.timestamp100ns));std::scoped_lock lock(impl->videoMutex);(content?impl->rawContent:impl->rawMain)=std::move(pixels);
 },[this](){std::scoped_lock lock(impl->videoMutex);impl->sharedRecording.Select(impl->sharedRecording.Source());impl->rawMain.reset();impl->rawContent.reset();impl->latestRecord.reset();impl->contentRecord.reset();PauseSharedRecording();}));};
}
bool StudioMedia::SharedRecordingSource(std::string_view id)const{std::scoped_lock lock(impl->videoMutex);return generation&&impl->sharedRecording.CanRecord(id)&&std::chrono::steady_clock::now()-impl->lastSharedAccepted<std::chrono::seconds(2);}
RecordingResult StudioMedia::StartRecording(RecordingOptions options){
 std::scoped_lock lock(impl->videoMutex);RecordingResult result;
 if(impl->pendingRecording){static_cast<RecordingSnapshot&>(result)=*impl->waitingRecording;result.error=RecordingError::AlreadyRecording;return result;}
 if(!generation||!impl->sharedRecording.CanRecord(options.sourceId)||std::chrono::steady_clock::now()-impl->lastSharedAccepted>=std::chrono::seconds(2)){result.error=RecordingError::InvalidTarget;return result;}
 result=recorder->Start(options);
 if(result.error==RecordingError::MicrophoneUnavailable||result.error==RecordingError::RoomAudioUnavailable){
  // Connected/video-ready may precede the first decoded PCM block. Keep this
  // request bound to its call and explicit source; do not open another capture.
  impl->pendingRecording=Impl::PendingRecording{options,generation.load(),impl->sharedRecording.Revision(),std::chrono::steady_clock::now()+std::chrono::seconds(3)};
  impl->waitingRecording=RecordingSnapshot{RecordingState::Starting,options.includeMicrophone,options.includeRoomAudio,0,{}};
  static_cast<RecordingSnapshot&>(result)=*impl->waitingRecording;result.error=RecordingError::None;
 }else if(result.error==RecordingError::None)impl->waitingRecording.reset();
 if(result.error==RecordingError::None){impl->pauseWhenReady=false;impl->recordingStopping=false;}return result;
}
RecordingSnapshot StudioMedia::RecordingStatus(){std::scoped_lock lock(impl->videoMutex);return impl->waitingRecording?*impl->waitingRecording:recorder->Snapshot();}
// Called with videoMutex held. Cancellation is terminal even though no MP4 was
// opened, so the managed Starting state cannot remain stranded or finalize forever.
void StudioMedia::CancelPendingRecording(std::string code,std::string detail){
 if(!impl->pendingRecording)return;
 impl->pendingRecording.reset();impl->waitingRecording->state=RecordingState::Failed;
 auto snapshot=*impl->waitingRecording;
 impl->recordingEvents.emplace_back("recording-error","\"code\":"+quote(code)+",\"detail\":"+quote(detail)+",\"recoverablePart\":false,\"fileName\":\"\"");
 impl->recordingEvents.emplace_back("recording-state","\"state\":\"failed\",\"micIncluded\":"+std::string(snapshot.micIncluded?"true":"false")+",\"roomIncluded\":"+(snapshot.roomIncluded?"true":"false")+",\"elapsedMs\":0,\"fileName\":\"\",\"paused\":false");
}
void StudioMedia::StopRecording(){std::scoped_lock lock(impl->videoMutex);if(impl->pendingRecording)CancelPendingRecording("recording-start-cancelled","Recording stopped before requested audio became ready");else recorder->Stop();}
void StudioMedia::PumpPendingRecording(const Emit& emit){
 std::deque<std::pair<std::string,std::string>> events;
 {std::scoped_lock lock(impl->videoMutex);
  if(impl->pendingRecording){auto request=*impl->pendingRecording;const auto now=std::chrono::steady_clock::now();
   if(request.generation!=generation||request.revision!=impl->sharedRecording.Revision())CancelPendingRecording("recording-start-cancelled","Call or shared source changed before requested audio became ready");
   else if(now>=request.deadline)CancelPendingRecording("recording-start-timeout","Requested microphone/room PCM or acknowledged shared frames did not become ready within 3 seconds");
   else if(impl->sharedRecording.CanRecord(request.options.sourceId)&&now-impl->lastSharedAccepted<std::chrono::seconds(2)&&(!request.options.includeMicrophone||captureSamples>0)&&(!request.options.includeRoomAudio||playbackSamples>0)){
    auto result=recorder->Start(request.options);
    if(result.error==RecordingError::None){impl->pendingRecording.reset();impl->waitingRecording.reset();impl->pauseWhenReady=false;}
    else if(result.error!=RecordingError::MicrophoneUnavailable&&result.error!=RecordingError::RoomAudioUnavailable)CancelPendingRecording("recording-start-rejected","Recording worker rejected deferred start: code "+std::to_string(int(result.error)));
   }
  }
  events.swap(impl->recordingEvents);
 }
 for(auto&[type,fields]:events)emit(type,fields);
}
void StudioMedia::PauseSharedRecording(){impl->pauseWhenReady=true;auto state=recorder->Snapshot();if(state.state==RecordingState::Recording&&!state.paused)recorder->Pause();}
RecordingResult StudioMedia::ResumeRecording(){std::scoped_lock lock(impl->videoMutex);if(!generation||!impl->sharedRecording.CanRecord(impl->sharedRecording.Source())||std::chrono::steady_clock::now()-impl->lastSharedAccepted>=std::chrono::seconds(2)){RecordingResult result;result.error=RecordingError::InvalidTarget;return result;}auto result=recorder->Resume();if(result.error==RecordingError::None){auto revision=impl->sharedRecording.Select(impl->sharedRecording.Source());impl->sharedRecording.Confirm(revision,true);impl->pauseWhenReady=false;}return result;}
StudioMedia::~StudioMedia(){Stop();recorder->Shutdown();}
bool StudioMedia::SetEchoCancellation(bool enabled){std::scoped_lock lock(audioMutex);impl->echo.Enable(enabled);return !enabled||impl->echo.Available();}
bool StudioMedia::EchoAvailable()const{return impl->echo.Available();}
bool StudioMedia::EchoEnabled()const{return impl->echo.Enabled();}
void StudioMedia::Start(uint64_t g){Stop();impl->echo.Reset();generation=g;captureSamples=playbackSamples=sentFrames=receivedFrames=sentContentFrames=receivedContentFrames=decodedNonzeroSamples=deviceSubmittedNonzeroSamples=0;impl->cameraWanted=impl->desktopWanted=false;impl->samplePosition=0;impl->sendQueue.clear();impl->sendBridge.reset();impl->receiveBridge.reset();impl->lastState.reset();impl->contentState.reset();impl->separateMode=impl->separateWanted=false;impl->videoRevision=0;appliedWidth=appliedHeight=appliedBitrateKbps=0;
 mixer=std::make_unique<OutboundAudioMixer>();if(!mixer->SetSendBufferMs(sendBufferMs))throw std::runtime_error("Invalid outbound audio buffer");mixer->SetVoicePriorityMix(25);mixer->SetMicrophoneMuted(micMuted);mixer->SetSystemAudioMuted(!systemEnabled);
 WAVEFORMATEX format{WAVE_FORMAT_PCM,1,48000,96000,2,16,0};
 if(!testMode){if(waveInOpen(&impl->input,device(true),&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR)impl->inputFailed=true;else{for(size_t i=0;i<8;++i){auto&h=impl->inHeaders[i];h={};h.lpData=reinterpret_cast<LPSTR>(impl->inBuffers[i].data());h.dwBufferLength=1920;waveInPrepareHeader(impl->input,&h,sizeof(h));waveInAddBuffer(impl->input,&h,sizeof(h));}waveInStart(impl->input);}}
 if(waveOutOpen(&impl->output,device(false),&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR)impl->outputFailed=true;
 if(localSurface)localSurface->Start(g);if(remoteSurface)remoteSurface->Start(g);
 source.StartAsync(g,settings,fps,[this](uint64_t gen,I420Frame frame,ComposedVideoSender::Completion done){if(gen!=generation){done(false);return;}auto copy=ComposeVideoFrame(&frame,nullptr,{frame.width,frame.height},frame.timestamp100ns);std::scoped_lock lock(impl->videoMutex);// Superseded capture frames were never submitted to an encoder.
 impl->latestDone={};impl->latest=std::move(copy);impl->latestRecord=impl->desktopWanted&&!impl->separateWanted?impl->rawMain:nullptr;impl->latestDone=std::move(done);});
 source.Camera(false,true);source.Desktop(false,true);
 if(systemEnabled)SystemAudio(true);
}
void StudioMedia::Stop(){{std::scoped_lock lock(impl->videoMutex);CancelPendingRecording("recording-start-cancelled","Call ended before requested audio became ready");}impl->recordingStopping=true;generation=0;source.Stop();contentSource.Stop();systemAudio.Stop();std::scoped_lock lock(audioMutex);impl->Close();impl->inputFramer.Reset();impl->outputFramer.Reset();impl->sendBridge.reset();impl->receiveBridge.reset();std::deque<int16_t>{}.swap(impl->sendQueue);mixer=std::make_unique<OutboundAudioMixer>();mixer->SetVoicePriorityMix(25);if(localSurface)localSurface->Stop();if(remoteSurface)remoteSurface->Stop();{std::scoped_lock videoLock(impl->videoMutex);impl->latest.reset();impl->contentLatest.reset();if(impl->latestDone)impl->latestDone(false);if(impl->contentDone)impl->contentDone(false);for(auto&entry:impl->awaiting)entry.second.done(false);impl->awaiting.clear();impl->latestDone={};impl->contentDone={};impl->rawMain.reset();impl->rawContent.reset();impl->latestRecord.reset();impl->contentRecord.reset();impl->sharedRecording.Select({});}recorder->Stop();}
void StudioMedia::Camera(bool on,std::string id){impl->cameraWanted=on;source.Camera(on,generation!=0,std::move(id));}
void StudioMedia::Desktop(bool on,std::string id,bool separate){bool sameSourceModeChange=false;{std::scoped_lock lock(impl->videoMutex);sameSourceModeChange=on&&impl->desktopWanted&&impl->separateMode.load()!=separate&&impl->sharedRecording.Source()==id;}impl->separateMode=separate;impl->separateWanted=separate&&on;impl->desktopWanted=on;contentSource.Stop();impl->contentState.reset();if(!on)source.Desktop(false,generation!=0);{std::scoped_lock lock(impl->videoMutex);if(!sameSourceModeChange){CancelPendingRecording("recording-start-cancelled","Shared source stopped or changed before requested audio became ready");impl->sharedRecording.Select(on?id:std::string{});}impl->sharedOutputStalled=false;if(!on||!separate){impl->contentLatest.reset();impl->contentDone={};}if(!on)impl->latest.reset();impl->rawMain.reset();impl->rawContent.reset();impl->latestRecord.reset();impl->contentRecord.reset();}if(!sameSourceModeChange)PauseSharedRecording();if(on)source.Desktop(true,generation!=0,id);if(!separate||!on)return;contentSource.StartAsync(generation,settings,fps,[this](uint64_t gen,I420Frame f,ComposedVideoSender::Completion done){if(gen!=generation){done(false);return;}auto copied=ComposeVideoFrame(&f,nullptr,{f.width,f.height},f.timestamp100ns);std::scoped_lock lock(impl->videoMutex);impl->contentDone={};impl->contentLatest=std::move(copied);impl->contentRecord=impl->separateWanted?impl->rawContent:nullptr;impl->contentDone=std::move(done);});contentSource.Camera(false,true);contentSource.Desktop(true,true,std::move(id));}
void StudioMedia::Configure(VideoCompositionSettings value,int rate){settings=value;fps=rate;source.Configure(value,rate);contentSource.Configure(value,rate);}
void StudioMedia::Ack(bool local,SurfaceAcknowledgement kind,uint64_t epoch){auto*p=local?localSurface.get():remoteSurface.get();if(p)p->Acknowledge(kind,epoch);}
bool StudioMedia::SystemAudio(bool on){systemEnabled=on;impl->systemPending=true;impl->systemRetries=0;impl->systemRetryAt={};impl->systemError.clear();if(!on){mixer->SetSystemAudioMuted(true);systemAudio.Stop();return true;}if(systemAudio.IsRunning()||systemAudio.IsStarting())return true;systemAudio.Stop();mixer->SetSystemAudioMuted(false);uint32_t pid=GetCurrentProcessId();if(auto*v=std::getenv("ESSASTUDIO_SYSTEM_AUDIO_EXCLUDE_PID"))pid=std::stoul(v);return systemAudio.Start(pid,[this](auto&&b){std::scoped_lock lock(audioMutex);if(!generation)return;b.callGeneration=generation;audio.ObserveSystemAudio(b);mixer->PushSystemAudio(b);});}
bool StudioMedia::ReadAudio(void*data,size_t size,size_t&length,int rate,int channels){std::scoped_lock lock(audioMutex);length=0;if(!generation||rate<8000||rate>96000||channels<1||channels>2)return false;
 if(!impl->sendBridge||impl->sendBridge->NegotiatedRateHz()!=rate){impl->sendBridge=std::make_unique<ProtocolAudioRateBridge>(rate);impl->sendQueue.clear();}
 std::vector<int16_t> captured;
 if(testMode&&impl->sendQueue.size()<size/2/channels){auto need=size/2/channels-impl->sendQueue.size();captured.resize(960*((need+rate/50-1)/(rate/50)));for(auto&s:captured)s=int16_t(7000*std::sin(6.283185307179586*impl->tone*double(impl->samplePosition++)/48000));}
 else if(impl->input){for(auto&h:impl->inHeaders)if(h.dwFlags&WHDR_DONE){auto*p=reinterpret_cast<int16_t*>(h.lpData);captured.insert(captured.end(),p,p+h.dwBytesRecorded/2);h.dwBytesRecorded=0;waveInAddBuffer(impl->input,&h,sizeof(h));}}
 bool actualCapture=!captured.empty();if(captured.empty()&&impl->sendQueue.empty())captured.resize(960,0);
 if(!captured.empty())for(auto&block:impl->inputFramer.Push(captured,48000,AudioTapKind::Microphone,generation)){if(actualCapture)impl->echo.Capture(block.samples);audio.ProcessMicrophone(block);if(actualCapture)captureSamples+=block.samples.size();recorder->PushMicrophone(block);mixer->PushMicrophone(block);auto mixed=mixer->TakeMixedBlock();std::vector<int16_t>out(rate/50);bool discontinuity=false;if(impl->sendBridge->FromMixer(mixed,out,discontinuity))impl->sendQueue.insert(impl->sendQueue.end(),out.begin(),out.end());}
 auto*out=static_cast<int16_t*>(data);size_t samples=size/2/channels;for(size_t i=0;i<samples;++i){auto v=impl->sendQueue.empty()?0:impl->sendQueue.front();if(!impl->sendQueue.empty())impl->sendQueue.pop_front();for(int c=0;c<channels;++c)out[i*channels+c]=v;}length=samples*channels*2;if(impl->sendQueue.size()>size_t(rate/5))impl->sendQueue.clear();return true;
}
bool StudioMedia::WriteAudio(const void*data,size_t length,size_t&written,int rate,int channels){std::scoped_lock lock(audioMutex);written=0;if(!generation||rate<8000||rate>96000||channels<1||channels>2)return false;if(length==0)return true;if(!data)return false;auto*p=static_cast<const int16_t*>(data);std::vector<int16_t>mono(length/2/channels);for(size_t i=0;i<mono.size();++i){int sum=0;for(int c=0;c<channels;++c){sum+=p[i*channels+c];}mono[i]=int16_t(sum/channels);if(mono[i])++decodedNonzeroSamples;}playbackSamples+=mono.size();if(!impl->receiveBridge||impl->receiveBridge->NegotiatedRateHz()!=rate)impl->receiveBridge=std::make_unique<ProtocolAudioRateBridge>(rate);
 for(auto&block:impl->outputFramer.Push(mono,rate,AudioTapKind::Room,generation)){auto output=block;output.samples.resize(960);auto resamplerInput=block;resamplerInput.source=AudioTapKind::Microphone;if(!impl->receiveBridge->ToMixer(resamplerInput,output))continue;output.source=AudioTapKind::Room;audio.ApplyRoomPlayback(output);recorder->PushRoomAudio(output);if(impl->output)for(size_t i=0;i<8;++i){auto&h=impl->outHeaders[i];if(h.dwFlags&WHDR_INQUEUE)continue;if(h.dwFlags&WHDR_PREPARED)waveOutUnprepareHeader(impl->output,&h,sizeof(h));std::copy(output.samples.begin(),output.samples.end(),impl->outBuffers[i].begin());h={};h.lpData=reinterpret_cast<LPSTR>(impl->outBuffers[i].data());h.dwBufferLength=1920;if(waveOutPrepareHeader(impl->output,&h,sizeof(h))==MMSYSERR_NOERROR&&waveOutWrite(impl->output,&h,sizeof(h))==MMSYSERR_NOERROR){impl->echo.Playback(output.samples);for(auto s:output.samples)if(s)++deviceSubmittedNonzeroSamples;}break;}}
 written=length;return true;
}
bool StudioMedia::ReadVideo(std::vector<uint8_t>&data,int width,int height,uint32_t timestamp,bool content){if(!generation)return false;std::optional<ComposedVideoFrame>picture;{std::scoped_lock lock(impl->videoMutex);picture=content?impl->contentLatest:impl->latest;auto&done=content?impl->contentDone:impl->latestDone;if(done){auto key=(uint64_t(content)<<32)|timestamp;if(auto existing=impl->awaiting.find(key);existing!=impl->awaiting.end()){existing->second.done(false);impl->awaiting.erase(existing);}impl->awaiting.emplace(key,Impl::FrameAck{std::move(done),content?impl->contentRecord:impl->latestRecord,impl->sharedRecording.Revision()});done={};while(impl->awaiting.size()>64){auto old=impl->awaiting.begin();old->second.done(false);impl->awaiting.erase(old);}}}
 if(testMode&&!content&&!impl->cameraWanted&&!impl->desktopWanted){ComposedVideoFrame f;f.width=width;f.height=height;f.y.resize(width*height);f.u.resize(width*height/4,80);f.v.resize(width*height/4,170);for(int y=0;y<height;++y)for(int x=0;x<width;++x)f.y[y*width+x]=uint8_t(32+(x+timestamp/360)%180);picture=std::move(f);}
 if(!picture)picture=ComposeVideoFrame(nullptr,nullptr,{width,height},0);auto view=picture->View();if(view.width!=width||view.height!=height)picture=ComposeVideoFrame(&view,nullptr,{width,height},0);
 data.resize(OpalVideoFrameSizeForResolution(width,height));auto*h=reinterpret_cast<PluginCodec_Video_FrameHeader*>(data.data());*h={0,0,unsigned(width),unsigned(height)};auto*p=OpalVideoFrameDataPtr(h);std::memcpy(p,picture->y.data(),picture->y.size());std::memcpy(p+picture->y.size(),picture->u.data(),picture->u.size());std::memcpy(p+picture->y.size()+picture->u.size(),picture->v.data(),picture->v.size());
 // Main/camera and dedicated presentation RTP remain independent. The one
 // local preview follows the selected share, rather than alternating streams.
 if(localSurface&&(impl->separateWanted.load()?content:!content))localSurface->SubmitI420(generation,picture->View());return true;}
void StudioMedia::VideoAccepted(uint32_t timestamp,bool content,bool accepted,int width,int height,int bitrate){std::scoped_lock lock(impl->videoMutex);if(!generation)return;auto key=(uint64_t(content)<<32)|timestamp;if(auto it=impl->awaiting.find(key);it!=impl->awaiting.end()){
 auto acknowledgment=std::move(it->second);impl->awaiting.erase(it);acknowledgment.done(accepted);
 if(acknowledgment.recordPixels&&impl->sharedRecording.Confirm(acknowledgment.revision,accepted)){impl->lastSharedAccepted=std::chrono::steady_clock::now();impl->sharedOutputStalled=false;recordingWorker->PushVideo(std::move(acknowledgment.recordPixels),impl->clock.Now100ns());}
 else if(acknowledgment.recordPixels&&acknowledgment.revision==impl->sharedRecording.Revision()&&!accepted)PauseSharedRecording();
}if(accepted){++sentFrames;if(content)++sentContentFrames;appliedWidth=width;appliedHeight=height;appliedBitrateKbps=bitrate;}}
bool StudioMedia::WriteVideo(const void*data,size_t length,bool content){if(!generation||!data||length<sizeof(PluginCodec_Video_FrameHeader))return false;auto*h=static_cast<const PluginCodec_Video_FrameHeader*>(data);if(h->x||h->y||h->width<2||h->height<2||h->width>4096||h->height>2160||length<OpalVideoFrameSizeForResolution(h->width,h->height))return false;auto*p=OpalVideoFrameDataPtr(h);auto count=h->width*h->height;I420Frame f{p,p+count,p+count+count/4,int(h->width),int(h->width/2),int(h->width/2),int(h->width),int(h->height),0};if(remoteSurface)remoteSurface->SubmitI420(generation,f);++receivedFrames;if(content)++receivedContentFrames;return true;}
void StudioMedia::Pump(const Emit&emit,bool reportDeviceErrors){if(reportDeviceErrors&&impl->inputFailed.exchange(false)){recorder->MicrophoneUnavailable();emit("media-error","\"code\":\"capture-device-failed\",\"detail\":\"Cannot open Windows microphone\"");}if(reportDeviceErrors&&impl->outputFailed.exchange(false))emit("media-error","\"code\":\"playback-device-failed\",\"detail\":\"Cannot open Windows speaker\"");
 if(generation&&!systemAudio.IsStarting()){
  const bool running=systemAudio.IsRunning();const auto error=systemAudio.LastError();
  if(impl->systemPending||!impl->systemState||*impl->systemState!=running){mixer->SetSystemAudioMuted(!running);emit("system-audio-changed","\"enabled\":"+std::string(running?"true":"false"));impl->systemState=running;impl->systemPending=false;}
  if(!running&&systemEnabled&&!error.empty()&&impl->systemError!=error){emit("media-error","\"code\":\"system-audio-unavailable\",\"detail\":"+quote(error));impl->systemError=error;}
  const bool transient=error.find("0x88890004")!=std::string::npos||error.find("0x88890010")!=std::string::npos;
  if(!running&&systemEnabled&&transient&&impl->systemRetries<3){
   const auto now=std::chrono::steady_clock::now();if(impl->systemRetryAt==std::chrono::steady_clock::time_point{})impl->systemRetryAt=now+std::chrono::milliseconds(500);
   else if(now>=impl->systemRetryAt){const auto attempt=impl->systemRetries+1;SystemAudio(true);impl->systemRetries=attempt;}
  }else if(running){impl->systemRetryAt={};impl->systemRetries=0;}
 }
 auto surface=[&](auto*p,bool local){if(!p)return;if(auto v=p->Pump())emit(local?"local-surface":"remote-surface","\"hostHandle\":"+quote(std::to_string(v->hostHandleValue))+",\"width\":"+std::to_string(v->width)+",\"height\":"+std::to_string(v->height)+",\"format\":\"bgra8-unorm\",\"epoch\":"+quote(std::to_string(v->epoch)));};surface(localSurface.get(),true);surface(remoteSurface.get(),false);
 auto mainState=source.TakeState();auto contentState=contentSource.TakeState();if(mainState)impl->lastState=mainState;if(contentState)impl->contentState=contentState;
 bool shareTimedOut=false;{std::scoped_lock lock(impl->videoMutex);if(impl->sharedRecording.CanRecord(impl->sharedRecording.Source())&&std::chrono::steady_clock::now()-impl->lastSharedAccepted>=std::chrono::seconds(2)){impl->sharedRecording.Select(impl->sharedRecording.Source());impl->sharedOutputStalled=true;shareTimedOut=true;PauseSharedRecording();}}
 if((mainState||contentState||shareTimedOut)&&impl->lastState){auto state=*impl->lastState;auto value=state.source;bool accepted=state.outputAccepted;if(impl->separateMode){value.desktopRequested=impl->separateWanted;value.desktopActive=impl->contentState&&impl->contentState->source.desktopActive&&impl->contentState->outputAccepted;value.desktopError=impl->contentState?impl->contentState->source.desktopError:"";}else value.desktopActive=value.desktopActive&&accepted;value.cameraActive=value.cameraActive&&accepted;if(value.cameraRequested&&state.outputFailed&&value.cameraError.empty())value.cameraError="video-output-not-ready";if(value.desktopRequested&&((impl->separateMode&&impl->contentState&&impl->contentState->outputFailed)||(!impl->separateMode&&state.outputFailed))&&value.desktopError.empty())value.desktopError="video-output-not-ready";
  {std::scoped_lock lock(impl->videoMutex);if(impl->sharedOutputStalled&&value.desktopRequested){value.desktopActive=false;value.desktopError="video-output-not-ready";}}
  if(!value.desktopActive){std::scoped_lock lock(impl->videoMutex);if(!impl->sharedRecording.CanRecord(impl->sharedRecording.Source())){impl->sharedRecording.Confirm(impl->sharedRecording.Revision(),false);PauseSharedRecording();}}
  static const char*sizes[]={"small","medium","large"};static const char*positions[]={"top-left","top-right","bottom-left","bottom-right"};
  emit("video-state","\"cameraRequested\":"+std::string(value.cameraRequested?"true":"false")+",\"desktopRequested\":"+(value.desktopRequested?"true":"false")+",\"cameraActive\":"+(value.cameraActive?"true":"false")+",\"desktopActive\":"+(value.desktopActive?"true":"false")+",\"resolution\":"+quote(resolution)+",\"bitrateKbps\":"+std::to_string(bitrateKbps)+",\"appliedWidth\":"+std::to_string(appliedWidth.load())+",\"appliedHeight\":"+std::to_string(appliedHeight.load())+",\"appliedBitrateKbps\":"+std::to_string(appliedBitrateKbps.load())+",\"overlaySize\":"+quote(sizes[int(settings.cameraSize)])+",\"overlayPosition\":"+quote(positions[int(settings.cameraPosition)])+",\"revision\":"+quote(std::to_string(++impl->videoRevision))+",\"cameraError\":"+quote(value.cameraError)+",\"desktopError\":"+quote(value.desktopError));
 }
 PumpPendingRecording(emit);
 if(impl->pauseWhenReady)PauseSharedRecording();
 static const char*states[]={"idle","starting","recording","finalizing","completed","failed"};for(auto&n:recorder->DrainNotifications()){auto&s=n.snapshot;if(impl->recordingStopping&&n.kind==RecordingNotificationKind::State&&(s.state==RecordingState::Starting||s.state==RecordingState::Recording))continue;if(n.kind!=RecordingNotificationKind::State)emit("recording-error","\"code\":"+quote(n.error==RecordingError::MicrophoneUnavailable?"microphone-unavailable":n.error==RecordingError::RoomAudioUnavailable?"room-audio-unavailable":"recording-worker-error")+",\"detail\":"+quote("Recording error "+std::to_string(int(n.error)))+",\"recoverablePart\":"+(s.fileName.ends_with(".part")?"true":"false")+",\"fileName\":"+quote(s.fileName));emit("recording-state","\"state\":"+quote(states[int(s.state)])+",\"micIncluded\":"+(s.micIncluded?"true":"false")+",\"roomIncluded\":"+(s.roomIncluded?"true":"false")+",\"elapsedMs\":"+std::to_string(s.elapsedMs)+",\"fileName\":"+quote(s.fileName)+",\"paused\":"+(s.paused?"true":"false"));}
}
}
