"""Regression gate for Test-OpalDevice orchestration, entirely simulated.

Run: python scripts/TestOpalDeviceAuditTests.py
All process creation, probes, catalog lookup, clocks and resource reads are replaced
before run() executes. Fake .exe files are inert placeholders, never launched.
These checks validate the tester; they do not establish physical media quality.
Temporary reports and fake recordings are removed automatically.
"""
if not __debug__:
 raise RuntimeError('Run this regression gate without -O; its assertions must remain enabled')

import base64, copy, json, queue, runpy, sys, tempfile, time as realtime
from pathlib import Path
from types import SimpleNamespace as NS
m=runpy.run_path(str(Path(__file__).with_name('Test-OpalDevice.py')));g=m['run'].__globals__
for duration,enabled,redials in ((2100,False,0),(1800,True,0),(2161,True,0),(2100,True,1)):
 try:m['validate_stability_duration'](duration,enabled,redials)
 except ValueError:pass
 else:raise AssertionError('Unexpected unbounded/implicit long call authorization')
m['validate_stability_duration'](2100,True,0)
m['validate_stability_duration'](45,False,0)
print('PASS: explicit long-duration bounds; ordinary call limits and single-call constraint retained')
class Stream:
 def __init__(self):self.q=queue.Queue()
 def __iter__(self):return self
 def __next__(self):
  value=self.q.get()
  if value is None:raise StopIteration
  return value+'\n'
 def readline(self):return next(self)
class Clock:
 def __init__(self):self.now=0;self.worker=None
 def monotonic(self):return self.now
 def sleep(self,seconds):
  self.now+=seconds
  if self.worker and self.worker.active:self.worker.health()
  if self.worker:
   w=self.worker
   if getattr(w,'pending_bye',None) is not None and self.now>=w.pending_bye:
    w.stdout.q.put('call 1: DISCONNECTED (EndedByLocalUser)');w.pending_bye=None
   if getattr(w,'pending_hold',None) is not None and self.now>=w.pending_hold[0]:
    w.emit(type='call-hold-changed',held=w.pending_hold[1]);w.pending_hold=None
   if w.recording and ((not w.paused and not (w.dialog==stuck_record_dialog and getattr(w,'resumed_once',False))) or (w.paused and w.dialog==advance_paused_dialog)):
    w.record_elapsed+=round(seconds*1000)
    if w.record_elapsed-getattr(w,'record_notice',0)>=1000:w.record_notice=w.record_elapsed;w.recording_state()
  if self.worker and self.worker.active and remote_early_clear and self.now-self.worker.call_start>=10:
   self.worker.active=False;self.worker.stdout.q.put('call 1: DISCONNECTED (EndedByRemoteUser)')
  realtime.sleep(.001)
clock=Clock();processes=[];lifetimes=[];skip_hangup_dialog=-1;change_audio_dialog=-1;stall_media_dialog=-1;missing_sdp_dialog=-1;append_sdp_dialog=-1;fail_microphone_dialog=-1;environments=[];bad_video_dialog=-1;stuck_record_dialog=-1;advance_paused_dialog=-1;suppress_playback_dialog=-1;bad_decode=False;bye_delay=16;exit_on_hangup=-1;failed_handshake=False;remote_early_clear=False
hold_delay=20;suppress_hold_on=False;suppress_hold_off=False
class FakeProcess:
 def __init__(self,argv,**kwargs):
  self.pid=24680;self.returncode=None;self.stdout=Stream();self.stderr=Stream();self.stdin=self
  self.fixture=str(argv[0]).endswith('fixture.exe');self.active=False;self.dialog=-1;self.recording=False;self.paused=False;self.record_elapsed=0;self.records=0
  self.record_dir=Path(kwargs.get('env',{}).get('ESSASTUDIO_RECORDING_DIRECTORY','.'))
  if self.fixture:self.stdout.q.put(json.dumps({'selectedHandle':1234,'decoyHandle':5678}));lifetimes.append(argv[1])
  else:clock.worker=self;self.stdout.q.put('Listening SIP on 15180');self.stdout.q.put('Listening H.323 on 17820')
  processes.append(self)
  if not self.fixture:environments.append(kwargs['env'])
 def emit(self,**event):self.stdout.q.put(json.dumps(event))
 def health(self):
  elapsed=clock.now-self.call_start
  if self.dialog==stall_media_dialog:elapsed=min(elapsed,2)
  self.emit(type='opal-connection-health',capturedSamples=0 if self.dialog==fail_microphone_dialog else int(elapsed*48000),decodedAudioSamples=int(elapsed*48000),decodedNonzeroSamples=int(elapsed*48000),deviceSubmittedNonzeroSamples=int(elapsed*48000),sentFrames=int(elapsed*30),receivedFrames=int(elapsed*20))
 def recording_state(self,state='recording'):self.emit(type='recording-state',state=state,paused=self.paused,elapsedMs=self.record_elapsed,fileName=f'recording-{self.records}.mp4')
 def finish_recording(self):
  if not self.recording:return
  (self.record_dir/f'recording-{self.records}.mp4').write_bytes(b'fake'+b'0'*15000);self.recording_state('completed');self.recording=False
 def write(self,line):
  line=line.strip()
  if line=='quit':self.returncode=0;self.stdout.q.put(None);self.stderr.q.put(None);return
  if self.fixture:return
  if line.startswith('call '):
   if self.active:self.emit(type='media-error',code='call-busy');return
   self.dialog+=1;self.active=True;self.call_start=clock.now
   sdp='m=audio 48000 RTP/AVP 96\nm=video 48002 RTP/AVP 97\na=content:main\nm=video 48004 RTP/AVP 98\na=content:slides\nm=application 0 UDP/BFCP *\n'
   if self.dialog==missing_sdp_dialog:sdp=sdp.replace('m=application 0 UDP/BFCP *\n','')
   if self.dialog==append_sdp_dialog:sdp+='m=video 49004 RTP/AVP 99\n'
   self.emit(type='opal-sdp-offer',protocol='sip',sdp=sdp.replace('\n','  '));self.stdout.q.put('call 1: DISCONNECTED (EndedByNoAnswer)' if failed_handshake else 'call 1: CONFIRMED (connected)')
   if failed_handshake:self.active=False
   if self.dialog==fail_microphone_dialog:self.emit(type='media-error',code='capture-device-failed')
   rate=32000 if self.dialog==change_audio_dialog else 48000
   for d in ('send','receive'):self.emit(type='audio-direction-state',direction=d,codec='AAC-LC',sampleRate=rate,channels=2,bitrateKbps=192,payloadType=96+self.dialog)
   self.emit(type='audio-codec-state',negotiated='AAC-LC',sampleRate=rate,channels=2,bitrateKbps=192);self.health()
   # Initial false is stale by the later resume command. It cannot prove OFF.
   self.emit(type='call-hold-changed',held=False)
  elif line.startswith('hold '):
   held=line=='hold on'
   if held and suppress_hold_on:
    # A fresh opposite state cannot acknowledge the requested true state.
    self.emit(type='call-hold-changed',held=False)
   elif not held and suppress_hold_off:pass
   else:self.pending_hold=(clock.now+hold_delay,held)
  elif line=='hangup':
   self.active=False
   if self.dialog!=skip_hangup_dialog:self.pending_bye=clock.now+bye_delay
   if self.dialog==exit_on_hangup:self.returncode=1;self.stdout.q.put(None);self.stderr.q.put(None)
   self.finish_recording()
  elif line.startswith('desktop '):
   active=line.startswith('desktop on ');self.emit(type='video-state',desktopRequested=active,desktopActive=active,appliedWidth=1920 if active and self.dialog!=bad_video_dialog else 0,appliedHeight=1080 if active and self.dialog!=bad_video_dialog else 0)
  elif line.startswith('record start '):self.records+=1;self.recording=True;self.paused=False;self.record_elapsed=2000;self.record_notice=2000;self.resumed_once=False;self.recording_state()
  elif line=='record pause':self.paused=True;self.recording_state()
  elif line=='record resume':self.paused=False;self.resumed_once=True;self.recording_state()
  elif line=='record stop':self.finish_recording()
 def flush(self):pass
 def poll(self):return self.returncode
 def wait(self,seconds):return self.returncode
 def kill(self):self.returncode=-1;self.stdout.q.put(None);self.stderr.q.put(None)
def probe(argv,**kwargs):
 metadata={'result':'PASS','pid':24680,'nonzeroSamples':48000,'peak':500,'videoWidth':1920,'videoHeight':1080,'videoSamples':20,'audioSamples':20,'audioNonzeroPcmSamples':48000}
 if argv[1]=='playback' and clock.worker.dialog==suppress_playback_dialog:return NS(returncode=0)
 if argv[1]=='recording-pattern' and bad_decode:metadata['audioNonzeroPcmSamples']=0
 Path(argv[-1]).write_text(json.dumps(metadata));return NS(returncode=0)
def capture(handle):return 'esw1.'+base64.urlsafe_b64encode(b'v001'+handle.to_bytes(8,'little')).decode().rstrip('=')
helper=NS(catalog=lambda worker:[(2,capture(1234)),(2,capture(5678)),(1,'monitor-owned')])
g['time']=clock;g['subprocess']=NS(Popen=FakeProcess,run=probe,PIPE=-1,TimeoutExpired=TimeoutError)
g['importlib']=NS(util=NS(spec_from_file_location=lambda *a:NS(loader=NS(exec_module=lambda module:None)),module_from_spec=lambda spec:helper))

g['process_resources']=lambda pid:dict(privateBytes=50000000,workingSetBytes=40000000,peakWorkingSetBytes=60000000,handleCount=100)

with tempfile.TemporaryDirectory(prefix='opal-hold-offline-') as folder:
 root=Path(folder);worker=root/'worker.exe';worker.write_bytes(b'fake-no-executable')
 args=NS(target='10.136.51.11',seconds=90,exercise_recording=False,share_multiple_sources=False,share_fixture=None,redial_cycles=0,
  playback_probe=None,output=root/'proof',protocol='sip',aac='on',connection_mode='vpn',worker=worker,sip_port=15180,h323_port=17820,rtp_port=45400,
  debugger=None,interface='127.0.0.1',prefer_stereo=True,verify_second_call_rejection=False,exercise_controls=True,sharing_cycles=1,
  real_microphone=False,verify_sdp_shape=False)
 for kind in ('delayed-hold-resume','missing-hold-confirmation','stale-false-before-resume'):
  suppress_hold_on=kind=='missing-hold-confirmation';suppress_hold_off=kind=='stale-false-before-resume'
  args.output=root/kind
  code=m['run'](args)
  value=json.loads((args.output/'pcs-xg77-10.136.51.11-sip-aac-on.json').read_text())
  commands=json.loads((args.output/'pcs-xg77-10.136.51.11-sip-aac-on-commands.json').read_text())
  assert value['workerExitCode']==0 and not value.get('forcedOwnedCleanup') and not value.get('cleanupError'),value
  assert sum(c['command']=='hangup' for c in commands)==1 and commands[-1]['command']=='quit',commands
  assert 'DISCONNECTED (EndedByLocalUser)' in (args.output/'pcs-xg77-10.136.51.11-sip-aac-on.log').read_text()
  if kind=='delayed-hold-resume':
   assert code==0 and value.get('callHoldResumeConfirmed'),value.get('error')
   on=next(c['seconds'] for c in commands if c['command']=='hold on')
   off=next(c['seconds'] for c in commands if c['command']=='hold off')
   assert off-on>=20 and value['dialogs'][0]['cleanHangupConfirmed'],commands
  else:
   assert code==1 and value['result']=='FAIL' and not value.get('callHoldResumeConfirmed'),value
   request=next(c for c in reversed(commands) if c['command'].startswith('hold '))
   hangup=next(c for c in commands if c['command']=='hangup')
   assert 45<=hangup['seconds']-request['seconds']<45.2,commands
  print('PASS: '+kind+' with fresh native confirmation or bounded failure/confirmed cleanup')
suppress_hold_on=suppress_hold_off=False
if '--hold-only' in sys.argv:
 print('PASS: 3 offline hold scenarios; no physical device or media proof')
 raise SystemExit(0)
processes.clear();lifetimes.clear();environments.clear()
clock.now=0;clock.worker=None

with tempfile.TemporaryDirectory(prefix='opal-audit-offline-') as folder:
 root=Path(folder)
 for name in ('worker.exe','fixture.exe','probe.exe'):(root/name).write_bytes(b'fake-no-executable')
 args=NS(target='10.136.51.11',seconds=90,exercise_recording=True,share_multiple_sources=True,share_fixture=root/'fixture.exe',redial_cycles=2,
  playback_probe=root/'probe.exe',output=root/'proof',protocol='sip',aac='on',connection_mode='vpn',worker=root/'worker.exe',sip_port=15180,h323_port=17820,rtp_port=45400,
  debugger=None,interface='127.0.0.1',prefer_stereo=True,verify_second_call_rejection=True,exercise_controls=False,sharing_cycles=3,
  real_microphone=False,verify_sdp_shape=True)
 def execute(a):
  code=m['run'](a)
  value=json.loads((a.output/'pcs-xg77-10.136.51.11-sip-aac-on.json').read_text())
  return code,value
 code,result=execute(args)
 assert code==0,result.get('error')
 assert len([p for p in processes if not p.fixture])==1
 assert [d['requiredConnectedSeconds'] for d in result['dialogs']]==[90,45,45]
 assert all(d['cleanHangupConfirmed'] and d['windowsProcessPlaybackVerified'] for d in result['dialogs'])
 assert len(result['recordings'])==4 and all(r['decodedMediaVerified'] for r in result['recordings'])
 assert all(d['pausedRecordingCompletedOnHangup'] for d in result['dialogs'][1:])
 assert result['recordingPauseResumeCycles']==8
 boundaries=[sample for sample in result['processResourceSamples'] if sample['phase']!='connected-periodic']
 assert len(boundaries)==12 and not result['memoryLeakAbsenceVerified']
 assert any(sample['phase']=='connected-periodic' for sample in result['processResourceSamples'])
 assert all(d['localSdpShapeVerified'] for d in result['dialogs'])
 assert result['workerExitCode']==0 and not result.get('forcedOwnedCleanup')
 print('PASS: synthetic90+45+45/onePID/16sBYEs/fresh playback each dialog/4decoded files/8resumes/frozen paused hangup12resource phases/flattened SDP')
 real=copy.copy(args);real.real_microphone=True;real.output=root/'real-microphone'
 code,result=execute(real)
 assert code==0,result.get('error')
 assert all(d['actualMicrophoneCaptureVerified'] for d in result['dialogs'])
 assert environments[-1]['ESSASTUDIO_TEST_MODE']=='0' and environments[-1]['ESSASTUDIO_DIAGNOSTIC_LOG']=='1'
 print('PASS: real-mic option checks per-dialog capture via fake data')
 for failure in ('stale-hangup','changed-format','stalled-media','missing-sdp','appended-sdp','failed-microphone','zero-video-size','stuck-record-resume','record-advances-paused','stale-playback','bad-recording-decode','native-exit-during-hangup'):
  skip_hangup_dialog=2 if failure=='stale-hangup' else -1
  change_audio_dialog=1 if failure=='changed-format' else -1
  stall_media_dialog=1 if failure=='stalled-media' else -1
  missing_sdp_dialog=1 if failure=='missing-sdp' else -1
  append_sdp_dialog=1 if failure=='appended-sdp' else -1
  fail_microphone_dialog=1 if failure=='failed-microphone' else -1
  bad_video_dialog=1 if failure=='zero-video-size' else -1
  stuck_record_dialog=1 if failure=='stuck-record-resume' else -1
  advance_paused_dialog=1 if failure=='record-advances-paused' else -1
  suppress_playback_dialog=1 if failure=='stale-playback' else -1
  bad_decode=failure=='bad-recording-decode'
  exit_on_hangup=2 if failure=='native-exit-during-hangup' else -1
  bad=copy.copy(args);bad.output=root/failure;bad.real_microphone=failure=='failed-microphone'
  if failure=='stale-playback':
   bad.output.mkdir();(bad.output/'pcs-xg77-10.136.51.11-sip-aac-on-windows-playback-dialog-1.json').write_text(json.dumps({'result':'PASS','pid':24680,'nonzeroSamples':48000,'peak':500}))
  code,value=execute(bad)
  assert code==1 and value['result']=='FAIL',(failure,value.get('error'))
  if failure=='native-exit-during-hangup':assert value['workerExitCode']==1 and not value.get('forcedOwnedCleanup'),value
  if failure=='stale-hangup':assert value.get('cleanupError') and value.get('forcedOwnedCleanup'),value
  print('PASS: rejects '+failure+' => '+value.get('error',''))
 print('PASS: all14 fake-only audit scenarios')

# Reset fault injection before the independently bounded initial-call scenarios.
skip_hangup_dialog=change_audio_dialog=stall_media_dialog=missing_sdp_dialog=append_sdp_dialog=fail_microphone_dialog=-1
bad_video_dialog=stuck_record_dialog=advance_paused_dialog=suppress_playback_dialog=exit_on_hangup=-1
bad_decode=False

with tempfile.TemporaryDirectory(prefix='opal-clear-offline-') as folder:
 root=Path(folder);worker=root/'worker.exe';worker.write_bytes(b'fake-no-executable')
 args=NS(target='10.136.51.11',seconds=45,exercise_recording=False,share_multiple_sources=False,share_fixture=None,redial_cycles=0,
  playback_probe=None,output=root/'proof',protocol='sip',aac='on',connection_mode='vpn',worker=worker,sip_port=15180,h323_port=17820,rtp_port=45400,
  debugger=None,interface='127.0.0.1',prefer_stereo=True,verify_second_call_rejection=False,exercise_controls=False,sharing_cycles=1,
  real_microphone=False,verify_sdp_shape=False)
 for kind in ('failed-handshake','early-remote-clear'):
  failed_handshake=kind=='failed-handshake';remote_early_clear=kind=='early-remote-clear'
  args.output=root/kind
  assert m['run'](args)==1
  value=json.loads((args.output/'pcs-xg77-10.136.51.11-sip-aac-on.json').read_text())
  commands=json.loads((args.output/'pcs-xg77-10.136.51.11-sip-aac-on-commands.json').read_text())
  assert value['workerExitCode']==0 and not value.get('forcedOwnedCleanup'),value.get('error')
  assert value['callAlreadyCleared'] and not any(c['command']=='hangup' for c in commands)
  assert value['totalSeconds']<15,value['totalSeconds']
  print('PASS: '+kind+' fails promptly; no redundant hangup, no forced kill, primary reason preserved')

print('PASS: 19 offline simulation scenarios plus long-duration bounds; no physical device or media proof')
