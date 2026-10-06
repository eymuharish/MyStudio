"""Real OPAL/RTP integration; explicit localhost endpoints only.

Run after staging the compiled worker with SDK, G722, AAC and video DLLs.
No physical calls are made. All spawned process handles are owned and closed.
"""
import argparse, base64, ctypes, json, os, queue, subprocess, tempfile, threading, time
from pathlib import Path

def check(value, message):
    if not value:
        raise AssertionError(message)

class Worker:
    def __init__(self, exe, sip, h323, rtp, auto, tone, debugger=None):
        self.lines=[]; self.stdout_lines=[]; self.stderr_lines=[]; self.events=[]; self.incoming=queue.Queue()
        self.recording_dir=Path(tempfile.mkdtemp(prefix="MyStudio-opal-recording-")); env=os.environ.copy(); env.update(ESSASTUDIO_SIP_TRANSPORT='udp',ESSASTUDIO_SIP_SRTP='disabled',ESSASTUDIO_MICROPHONE_ID='',ESSASTUDIO_SPEAKER_ID='',ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR=str(self.recording_dir),ESSASTUDIO_TEST_MODE='1',ESSASTUDIO_TEST_TONE_HZ=str(tone))
        self.process=subprocess.Popen(([str(debugger)] if debugger else [])+[str(exe),'--sip-port',str(sip),'--h323-port',str(h323),'--rtp-port',str(rtp),'--interface','127.0.0.1']+(['--test-loopback-autoanswer'] if auto else []),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',errors='replace',env=env,creationflags=0x08000000)
        for stream in (self.process.stdout,self.process.stderr):
            threading.Thread(target=self.read,args=(stream,stream is self.process.stdout),daemon=True).start()
    def read(self, stream, is_stdout=False):
        for line in stream:
            line=line.rstrip(); (self.stdout_lines if is_stdout else self.stderr_lines).append(line); self.lines.append(line); self.incoming.put(line)
            if line.startswith('{'):
                try: self.events.append(json.loads(line))
                except json.JSONDecodeError: pass
    def send(self, command):
        self.process.stdin.write(command+'\n'); self.process.stdin.flush()
    def await_(self, predicate, timeout=25):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if predicate(): return
            if self.process.poll() is not None: raise AssertionError('Worker exited: '+'\n'.join(self.lines[-8:]))
            time.sleep(.05)
        raise AssertionError('Worker timeout: '+'\n'.join(self.lines[-12:]))
    def close(self):
        if self.process.poll() is None:
            try: self.send('hangup'); self.send('quit'); self.process.wait(20)
            except (BrokenPipeError, subprocess.TimeoutExpired): self.process.kill(); self.process.wait(5)
    def event(self, type_, test=lambda e:True):
        return any(e.get('type')==type_ and test(e) for e in self.events)

def run(args):
    workers=[]; passed=False
    try:
        a=Worker(args.worker,15140,17760,45000,False,440,args.debugger); workers.append(a)
        b=Worker(args.worker,15142,17762,45200,True,523,args.debugger); workers.append(b)
        for w in workers:
            w.await_(lambda: any(x.startswith('Listening SIP on ') for x in w.lines) and any(x.startswith('Listening H.323 on ') for x in w.lines))
            before=len(w.events); w.send('protocol '+args.protocol); w.send('endpoint '+('pcs-xg77' if args.aac=='lc' else 'sx20'))
            w.send('aac-enabled '+('on' if args.aac!='off' else 'off'))
            w.await_(lambda:any(e.get('type')=='aac-availability' and e.get('protocol')==args.protocol for e in w.events[before:]))
            if args.aac!='off': check(w.event('aac-availability',lambda e:e.get('available') is True),'AAC plugin unavailable')
        target=f'sip:test@127.0.0.1:15142' if args.protocol=='sip' else 'h323:127.0.0.1:17762'
        a.send('call '+target)
        for w in workers:
            w.await_(lambda: any('CONFIRMED' in x or x.startswith('call-established ') for x in w.lines))
            w.await_(lambda: w.event('opal-connection-health',lambda e:e.get('decodedNonzeroSamples',0)>1000 and e.get('deviceSubmittedNonzeroSamples',0)>1000 and e.get('receivedFrames',0)>5),args.media_timeout)
            wanted={'off':'g722','ld':'aac-ld','lc':'aac-lc'}[args.aac]
            check(w.event('audio-codec-state',lambda e:e.get('negotiated')==wanted and e.get('sendActive') and e.get('receiveActive')),'Incorrect negotiated codec')
        a.send('aac-enabled off'); a.await_(lambda:a.event('media-error',lambda e:e.get('code')=='aac-change-rejected'))
        if args.probe:
            result=(args.logs or a.recording_dir)/f'{args.protocol}-{args.aac}-playback.json'; result.parent.mkdir(parents=True,exist_ok=True)
            probe=subprocess.run([str(args.probe),'playback',str(a.process.pid),'8',str(result)],capture_output=True,text=True,timeout=25)
            check(probe.returncode==0,'Included-process Windows playback probe failed: '+probe.stdout+probe.stderr)
            print('Actual Windows playback PASS '+str(result))
        if args.transmission_modes: transmission_modes(args,a,b)
        if args.extended or args.content_only: extended(args,a,b)
        before=len(a.events); a.send('hold on'); a.await_(lambda:after(a,before,'call-hold-changed',lambda e:e.get('held') is True))
        resume_starts=[len(w.events) for w in workers]
        before=len(a.events); a.send('hold off'); a.await_(lambda:after(a,before,'call-hold-changed',lambda e:e.get('held') is False))
        for w,index in zip(workers,resume_starts):
            w.await_(lambda:after(w,index,'opal-connection-health'))
            baseline=next(e for e in reversed(w.events[index:]) if e.get('type')=='opal-connection-health')
            w.await_(lambda:w.event('opal-connection-health',lambda e:e.get('decodedNonzeroSamples',0)>baseline['decodedNonzeroSamples']+2000 and e.get('receivedFrames',0)>baseline['receivedFrames']+5),40)
        a.send('dtmf 123 160'); time.sleep(1)
        check(not a.event('media-error',lambda e:e.get('code') in ('dtmf-invalid','dtmf-unavailable')),'DTMF not sent')
        b.await_(lambda:all(any(e.get('type')=='opal-dtmf-received' and digit in e.get('digits','') for e in b.events) for digit in '123'))
        if args.duration:
            check(10<=args.duration<=120,'Bounded sustained duration required')
            baselines=[next(e for e in reversed(w.events) if e.get('type')=='opal-connection-health') for w in workers]
            end=time.monotonic()+args.duration
            while time.monotonic()<end:
                check(all(w.process.poll() is None for w in workers),'Worker ended during sustained call'); time.sleep(1)
            for w,baseline in zip(workers,baselines):
                latest=next(e for e in reversed(w.events) if e.get('type')=='opal-connection-health')
                samples=latest['decodedAudioSamples']-baseline['decodedAudioSamples']; rate=16000 if args.aac=='off' else 48000
                check(samples>args.duration*rate*.90,'Sustained decoded PCM pacing drift: '+str(samples))
                check(latest['deviceSubmittedNonzeroSamples']>baseline['deviceSubmittedNonzeroSamples']+args.duration*48000*.80,'Sustained Windows playback stopped')
                check(latest['receivedFrames']>baseline['receivedFrames']+args.duration*5,'Sustained decoded video stopped')
                print(f'Sustained {args.duration}s actual {args.protocol}/{args.aac} decodedSamples={samples}')
        a.send('hangup')
        for w in workers:
            w.await_(lambda:any('DISCONNECTED' in x or x.startswith('call-cleared ') for x in w.lines))
            seq=[e['sequence'] for e in w.events]; check(seq==sorted(set(seq)),'Sequence duplicate or disorder')
            check(all(e.get('protocol')==args.protocol for e in w.events if e.get('type') in ('audio-codec-state','opal-connection-health')),'Wrong actual protocol tag')
        if args.extended:
            if hasattr(args,'paused_job_start'):
                a.await_(lambda:after(a,args.paused_job_start,'recording-state',lambda e:e.get('state')=='completed'),40)
                completed=next(e for e in a.events[args.paused_job_start:] if e.get('type')=='recording-state' and e.get('state')=='completed')
                check(completed['elapsedMs']==args.paused_job_elapsed,'Paused hangup changed active recording timeline')
                verify_recording(args,a,completed,'hangup-recording')
            switch_protocol(args,a,b)
        passed=True
    finally:
        for index,w in enumerate(workers):
            w.close()
            if args.logs:
                args.logs.mkdir(parents=True,exist_ok=True)
                (args.logs/f'{args.protocol}-{args.aac}-{index}.log').write_text('\n'.join(w.lines),encoding='utf-8')
            if passed:
                check(w.process.returncode==0,'Worker failed clean shutdown: '+str(w.process.returncode))
                check(not any('DEBUG_EXCEPTION' in x or 'Possible deadlock' in x for x in w.lines),'Worker lifecycle crashed or deadlocked')
    print(f'OPAL real loopback PASS protocol={args.protocol} aac={args.aac} cleanExit=0')

def catalog(exe,camera=False):
    class Item(ctypes.Structure):
        _pack_=4
        _fields_=[(name,ctypes.c_uint32) for name in ('structSize','kind','width','height','idOffset','idLength','nameOffset','nameLength')]
    ctypes.windll.ole32.CoInitializeEx(None,0)
    dll=ctypes.CDLL(str(exe.parent/'EssaStudio.CaptureCatalog.dll'))
    query=dll.EssaCameraCatalogQueryV1 if camera else dll.EssaCaptureCatalogQueryV1
    query.argtypes=[ctypes.c_uint32,ctypes.c_uint64,ctypes.POINTER(Item),ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint8),ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32),ctypes.POINTER(ctypes.c_uint32),ctypes.POINTER(ctypes.c_uint64)]
    for attempt in range(5):
        count=ctypes.c_uint32(); size=ctypes.c_uint32(); snapshot=ctypes.c_uint64()
        status=query(1,0,None,0,None,0,ctypes.byref(count),ctypes.byref(size),ctypes.byref(snapshot))
        check(status in (0,1),'Capture catalog sizing failed '+str(status))
        if not count.value: return []
        items=(Item*count.value)(); data=(ctypes.c_uint8*size.value)()
        status=query(1,snapshot.value,items,count.value,data,size.value,ctypes.byref(count),ctypes.byref(size),ctypes.byref(snapshot))
        # The C ABI explicitly invalidates the snapshot if another owned UI
        # creates/closes a window between sizing and read. Refresh boundedly.
        if status in (1,2) and attempt<4:
            print('Capture catalog changed; bounded fresh snapshot '+str(attempt+1))
            time.sleep(.05)
            continue
        check(status==0,'Capture catalog read failed '+str(status))
        return [(i.kind,bytes(data[i.idOffset:i.idOffset+i.idLength]).decode()) for i in items]

def encoded(value): return base64.urlsafe_b64encode(value.encode()).decode().rstrip('=')
def after(worker,index,type_,test=lambda e:True):
    return any(e.get('type')==type_ and test(e) for e in worker.events[index:])

def transmission_modes(args,a,b):
    items=catalog(args.worker)
    monitor=next((id for kind,id in items if kind==1),None)
    check(monitor is not None,'Owned monitor source unavailable')
    proofs=[]
    for mode in ('main','dual','main','auto','main','dual'):
        start=len(a.events); line_start=len(a.lines)
        a.send('share-mode '+mode)
        a.await_(lambda:any(x=='sharing-mode '+mode for x in a.lines[line_start:]) or after(a,start,'media-error'),10)
        check(any(x=='sharing-mode '+mode for x in a.lines[line_start:]),'Sharing transmission mode command unsupported: '+mode)
        a.send('desktop on '+encoded(monitor))
        a.await_(lambda:after(a,start,'video-state',lambda e:e.get('desktopRequested') and e.get('desktopActive')),40)
        if mode=='main':
            a.await_(lambda:after(a,start,'content-sharing-state',lambda e:e.get('mode')=='main-video' and not e.get('active')))
            time.sleep(1)
            check(not after(a,start,'content-sharing-state',lambda e:e.get('active')),'Main-only mode opened the presentation channel')
        else:
            a.await_(lambda:after(a,start,'content-sharing-state',lambda e:e.get('active')),20)
            old_sent=max((e.get('sentFrames',0) for e in a.events if e.get('type')=='opal-content-health'),default=0)
            old_received=max((e.get('receivedFrames',0) for e in b.events if e.get('type')=='opal-content-health'),default=0)
            a.await_(lambda:a.event('opal-content-health',lambda e:e.get('sentFrames',0)>old_sent+5),20)
            b.await_(lambda:b.event('opal-content-health',lambda e:e.get('receivedFrames',0)>old_received+5),20)
        proofs.append({'mode':mode,'desktopActive':True,'presentationExpected':mode!='main'})
    for iteration in range(3):
        start=len(a.events); line_start=len(a.lines)
        old_sent=max((e.get('sentFrames',0) for e in a.events if e.get('type')=='opal-content-health'),default=0)
        old_received=max((e.get('receivedFrames',0) for e in b.events if e.get('type')=='opal-content-health'),default=0)
        for mode in ('main','dual'):
            a.send('share-mode '+mode); a.send('desktop on '+encoded(monitor))
        a.await_(lambda:after(a,start,'content-sharing-state',lambda e:e.get('active')),20)
        a.await_(lambda:'sharing-mode dual' in a.lines[line_start:],10)
        time.sleep(2)
        states=[e for e in a.events[start:] if e.get('type')=='content-sharing-state']
        check(states and states[-1].get('active'),'Final rapid mode did not retain an active presentation')
        a.await_(lambda:a.event('opal-content-health',lambda e:e.get('sentFrames',0)>old_sent+30),20)
        b.await_(lambda:b.event('opal-content-health',lambda e:e.get('receivedFrames',0)>old_received+30),20)
        proofs.append({'rapidSwitch':iteration+1,'presentationRestored':True,'sentAndReceivedFramesAdvanced':True})
    a.send('desktop off')
    if args.logs:
        args.logs.mkdir(parents=True,exist_ok=True)
        (args.logs/'sharing-transmission-modes.json').write_text(json.dumps({'result':'PASS','protocol':args.protocol,'cycles':proofs},indent=2))
    print('Sharing transmission modes and repeated live switching PASS '+args.protocol)

def extended(args,a,b):
    fixture=None
    try:
        if args.share_fixture:
            fixture=subprocess.Popen([str(args.share_fixture),'150'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',creationflags=0x08000000)
            handles=json.loads(fixture.stdout.readline())
            args.share_source=next((id for kind,id in catalog(args.worker) if kind==2 and id.startswith('esw1.') and int.from_bytes(base64.urlsafe_b64decode(id[5:]+'='*((-len(id[5:]))%4))[4:12],'little')==handles['selectedHandle']),None)
            check(args.share_source is not None,'Owned selected window absent from CaptureCatalog')
            print('Explicit owned selected window '+args.share_source)
        extended_share(args,a,b)
    finally:
        if fixture:
            try: fixture.stdin.write('quit\n');fixture.stdin.flush();fixture.wait(5)
            except (BrokenPipeError,subprocess.TimeoutExpired): fixture.kill();fixture.wait(5)
            check(fixture.returncode==0,'Owned share fixture did not exit cleanly')

def extended_share(args,a,b):
    if args.protocol=='sip':
        check(a.event('opal-sdp-offer',lambda e:e.get('bfcp') and e.get('presentation')),'Initial offer omitted BFCP or presentation video before share')
    items=catalog(args.worker); monitor=args.share_source or next((id for kind,id in items if kind==1),None)
    check(monitor is not None and any(id==monitor for kind,id in items),'Explicit share source absent from capture catalog')
    index=len(a.events); a.send('record start '+encoded(monitor)+' mic-on room-on 1920 1080')
    a.await_(lambda:after(a,index,'recording-error',lambda e:e.get('code')=='recording-source-not-shared'))
    index=len(a.events); a.send('desktop on '+encoded(monitor))
    a.await_(lambda:after(a,index,'video-state',lambda e:e.get('desktopRequested') and e.get('desktopActive') and e.get('appliedWidth',0)>0),40)
    if args.require_content:
        check(after(a,index,'content-sharing-state',lambda e:e.get('active') is True and e.get('mode')==('h239' if args.protocol=='h323' else 'bfcp')),'Dedicated presentation floor/token not granted')
        b.await_(lambda:b.event('opal-content-health',lambda e:e.get('receivedFrames',0)>5),20)
        a.await_(lambda:a.event('opal-content-health',lambda e:e.get('sentFrames',0)>5),20)
    if args.content_only:
        check(args.require_content,'Content smoke requires actual independent presentation proof')
        index=len(a.events);a.send('desktop off')
        a.await_(lambda:after(a,index,'video-state',lambda e:not e.get('desktopRequested') and not e.get('desktopActive')))
        a.await_(lambda:after(a,index,'content-sharing-state',lambda e:e.get('active') is False))
        print('Independent '+('BFCP' if args.protocol=='sip' else 'H239')+' presentation decoded and source stopped PASS')
        return
    index=len(a.events); a.send('record start '+encoded('unshared-source-for-regression')+' mic-on room-on 1920 1080')
    a.await_(lambda:after(a,index,'recording-error',lambda e:e.get('code')=='recording-source-not-shared'))
    index=len(a.events); a.send('record start '+encoded(monitor)+' mic-on room-on 1920 1080')
    a.await_(lambda:after(a,index,'recording-state',lambda e:e.get('state')=='recording'),40)
    time.sleep(2); index=len(a.events); a.send('record pause')
    a.await_(lambda:after(a,index,'recording-state',lambda e:e.get('state')=='recording' and e.get('paused') is True))
    paused=next(e for e in a.events[index:] if e.get('type')=='recording-state' and e.get('paused'))
    time.sleep(1); index=len(a.events); a.send('record resume')
    a.await_(lambda:after(a,index,'recording-state',lambda e:e.get('state')=='recording' and e.get('paused') is False))
    time.sleep(2); index=len(a.events); a.send('desktop off')
    a.await_(lambda:after(a,index,'recording-state',lambda e:e.get('state')=='recording' and e.get('paused') is True))
    paused_elapsed=next(e['elapsedMs'] for e in reversed(a.events[index:]) if e.get('type')=='recording-state' and e.get('paused'))
    time.sleep(1); check(not after(a,index,'recording-state',lambda e:e.get('state') in ('finalizing','completed')),'Share stop finalized instead of pausing')
    index=len(a.events);a.send('record resume');a.await_(lambda:after(a,index,'recording-error',lambda e:e.get('code')=='recording-resume-rejected'))
    index=len(a.events);a.send('desktop on '+encoded(monitor));a.await_(lambda:after(a,index,'video-state',lambda e:e.get('desktopActive')),40)
    check(next(e for e in reversed(a.events) if e.get('type')=='recording-state')['paused'] is True,'Sharing resumed recording automatically')
    index=len(a.events);a.send('record resume');a.await_(lambda:after(a,index,'recording-state',lambda e:e.get('state')=='recording' and e.get('paused') is False))
    time.sleep(2); index=len(a.events); a.send('record stop')
    a.await_(lambda:after(a,index,'recording-state',lambda e:e.get('state')=='completed'),40)
    completed=next(e for e in a.events[index:] if e.get('type')=='recording-state' and e.get('state')=='completed')
    check(2500<completed['elapsedMs']<11000,'Recording pause did not preserve a plausible active timeline')
    verify_recording(args,a,completed,'recording')
    # Capture requests are judged by delivered video state after actual encoder acknowledgment.
    index=len(a.events); a.send('desktop on '+encoded(monitor))
    a.await_(lambda:after(a,index,'video-state',lambda e:e.get('desktopRequested') and e.get('desktopActive') and e.get('appliedWidth',0)>0),40)
    index=len(a.events); a.send('desktop off')
    a.await_(lambda:after(a,index,'video-state',lambda e:e.get('desktopRequested') is False and e.get('desktopActive') is False))
    if args.camera_id:
        index=len(a.events); a.send('camera on '+encoded(args.camera_id))
        a.await_(lambda:after(a,index,'video-state',lambda e:e.get('cameraRequested') and e.get('cameraActive')),40)
        index=len(a.events); a.send('camera off')
        a.await_(lambda:after(a,index,'video-state',lambda e:e.get('cameraRequested') is False and e.get('cameraActive') is False))
    else: print('Camera stays off; explicit --camera-id fixture is required for its toggle test')

    # Keep a second actual MP4 open and paused until call termination.
    index=len(a.events);a.send('desktop on '+encoded(monitor));a.await_(lambda:after(a,index,'video-state',lambda e:e.get('desktopActive')),40)
    args.paused_job_start=len(a.events);a.send('record start '+encoded(monitor)+' mic-on room-on 1920 1080')
    a.await_(lambda:after(a,args.paused_job_start,'recording-state',lambda e:e.get('state')=='recording' and not e.get('paused')),40)
    time.sleep(2);index=len(a.events);a.send('desktop off')
    a.await_(lambda:after(a,index,'recording-state',lambda e:e.get('state')=='recording' and e.get('paused')))
    args.paused_job_elapsed=next(e['elapsedMs'] for e in reversed(a.events[index:]) if e.get('type')=='recording-state' and e.get('paused'))
    time.sleep(.3);check(not after(a,index,'recording-state',lambda e:e.get('state') in ('finalizing','completed')),'Paused job finalized before hangup')

def verify_recording(args,a,completed,suffix):
    output=a.recording_dir/completed['fileName'];check(output.is_file() and output.stat().st_size>10000,'Recording MP4 absent or empty')
    if args.probe:
        result=(args.logs or a.recording_dir)/f'{args.protocol}-{args.aac}-{suffix}.json';result.parent.mkdir(parents=True,exist_ok=True)
        probe=subprocess.run([str(args.probe),'recording-pattern' if args.share_fixture else 'recording',str(output),str(result)],capture_output=True,text=True,timeout=30)
        check(probe.returncode==0,'Actual MP4 audio/video decode failed: '+probe.stdout+probe.stderr)
        metadata=json.loads(result.read_text());check(metadata.get('videoWidth')==1920 and metadata.get('videoHeight')==1080,'Actual decoded recording is not1920x1080')
        print('Playable MP4 decoded PASS '+str(output)+' evidence='+str(result))
    else:print('Recording media probe required: '+str(output))

def switch_protocol(args,a,b):
    second='h323' if args.protocol=='sip' else 'sip'
    indices=[len(w.events) for w in (a,b)]; counts=[len(w.lines) for w in (a,b)]
    a.send('protocol '+second); a.send('aac-enabled off'); a.send('audio-profile g722 1 64 100 300 100 40 0')
    # Receiver keeps its prior idle selection: events must use the actual incoming leg.
    target='h323:127.0.0.1:17762' if second=='h323' else 'sip:test@127.0.0.1:15142'
    a.send('call '+target)
    for w,index,count in zip((a,b),indices,counts):
        w.await_(lambda:any('CONFIRMED' in x or x.startswith('call-established ') for x in w.lines[count:]))
        w.await_(lambda:after(w,index,'opal-connection-health',lambda e:e.get('protocol')==second and e.get('decodedNonzeroSamples',0)>1000 and e.get('deviceSubmittedNonzeroSamples',0)>1000 and e.get('receivedFrames',0)>5),40)
        w.await_(lambda:after(w,index,'recording-state',lambda e:e.get('protocol')==second and e.get('state')=='idle' and not e.get('paused')))
        check(all(e.get('protocol')==second for e in w.events[index:] if e.get('type') in ('audio-codec-state','opal-connection-health')),'Incoming protocol inherited idle selection')
    a.send('hangup')
    for w,count in zip((a,b),counts):
        w.await_(lambda:any('DISCONNECTED' in x or x.startswith('call-cleared ') for x in w.lines[count:]))
    print('Shared worker second protocol PASS '+second)

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--worker',type=Path,required=True); parser.add_argument('--protocol',choices=['sip','h323'],required=True); parser.add_argument('--aac',choices=['off','ld','lc'],default='off'); parser.add_argument('--logs',type=Path); parser.add_argument('--probe',type=Path); parser.add_argument('--extended',action='store_true'); parser.add_argument('--content-only',action='store_true'); parser.add_argument('--require-content',action='store_true'); parser.add_argument('--camera-id'); parser.add_argument('--share-source'); parser.add_argument('--share-fixture',type=Path); parser.add_argument('--debugger',type=Path); parser.add_argument('--duration',type=int,default=0); parser.add_argument('--media-timeout',type=int,default=40)
    parser.add_argument('--transmission-modes',action='store_true',help='Verify repeated main/dual/auto sharing transitions in one owned call')
    run(parser.parse_args())
