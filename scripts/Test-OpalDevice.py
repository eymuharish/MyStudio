"""Bounded, explicitly authorized device calls. No device configuration changes."""
import argparse, base64, ctypes, hashlib, importlib.util, json, os, queue, re, subprocess, threading, time
from pathlib import Path

TARGETS={'10.136.51.25':'sx20','10.136.51.11':'pcs-xg77','10.136.203.16':'pcs-xg80',
         '10.136.101.18':'ex90','10.136.101.12':'ex90','10.136.61.31':'pcs-xg80'}

def call_connected_line(line):
    return bool(re.match(r'^call \d+: CONFIRMED \(connected\)$',line)) or line.startswith('call-established remote=')

def call_ended_line(line):
    return bool(re.match(r'^call \d+: DISCONNECTED \(',line)) or line.startswith('call-cleared remote=')

def verify_native_errors(events,allow_call_busy=False):
    for event in events:
        if event.get('type') in ('media-error','recording-error'):
            if allow_call_busy and event.get('type')=='media-error' and event.get('code')=='call-busy':continue
            raise RuntimeError('Native '+str(event.get('type'))+': '+str(event.get('code')))

def process_resources(pid):
    from ctypes import wintypes
    class Counters(ctypes.Structure):
        _fields_=[('cb',wintypes.DWORD),('pageFaults',wintypes.DWORD)]+[(name,ctypes.c_size_t) for name in
            ('peakWorkingSet','workingSet','peakPagedPool','pagedPool','peakNonpagedPool','nonpagedPool','pagefile','peakPagefile','private')]
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);psapi=ctypes.WinDLL('psapi',use_last_error=True)
    kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];kernel.OpenProcess.restype=wintypes.HANDLE
    kernel.CloseHandle.argtypes=[wintypes.HANDLE];kernel.CloseHandle.restype=wintypes.BOOL
    kernel.GetProcessHandleCount.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)];kernel.GetProcessHandleCount.restype=wintypes.BOOL
    psapi.GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.POINTER(Counters),wintypes.DWORD];psapi.GetProcessMemoryInfo.restype=wintypes.BOOL
    handle=kernel.OpenProcess(0x400|0x10,False,pid)
    if not handle:raise ctypes.WinError(ctypes.get_last_error())
    try:
        counters=Counters();counters.cb=ctypes.sizeof(counters);handles=wintypes.DWORD()
        if not psapi.GetProcessMemoryInfo(handle,ctypes.byref(counters),counters.cb) or not kernel.GetProcessHandleCount(handle,ctypes.byref(handles)):
            raise ctypes.WinError(ctypes.get_last_error())
        return {'privateBytes':counters.private,'workingSetBytes':counters.workingSet,
                'peakWorkingSetBytes':counters.peakWorkingSet,'handleCount':handles.value}
    finally:kernel.CloseHandle(handle)

def audio_formats(events):
    # RTP payload numbers are dialog-local; compare the actual audio format.
    keys=('codec','sampleRate','channels','bitrateKbps')
    directions={}
    for event in events:
        if event.get('type')=='audio-direction-state':
            directions[event.get('direction')]={key:event.get(key) for key in keys}
    summary=next((event for event in reversed(events) if event.get('type')=='audio-codec-state'),{})
    return {'directions':directions,'profile':{key:summary.get(key) for key in ('negotiated','sampleRate','channels','bitrateKbps')}}

def verify_audio_formats(events,baseline):
    for event in events:
        if event.get('type')=='audio-direction-state':
            actual={key:event.get(key) for key in ('codec','sampleRate','channels','bitrateKbps')}
            if actual!=baseline['directions'].get(event.get('direction')):
                raise RuntimeError('Negotiated audio format changed during the call')
        elif event.get('type')=='audio-codec-state':
            actual={key:event.get(key) for key in ('negotiated','sampleRate','channels','bitrateKbps')}
            if actual!=baseline['profile']:raise RuntimeError('Negotiated audio profile changed during the call')

def sdp_media_sections(sdp):
    sections=[]
    # The worker's JSON quote() replaces each CR and LF with a space. Split
    # flattened diagnostic field boundaries as well as ordinary SDP newlines.
    for line in re.split(r'\r\n|\r|\n| {2,}(?=[a-z]=)',sdp):
        if line.startswith('m='):
            fields=line[2:].split()
            if len(fields)<4:raise RuntimeError('Invalid local SDP media description')
            port=int(fields[1].split('/')[0])
            if not 0<=port<=65535:raise RuntimeError('Invalid local SDP media port')
            protocol=fields[2].upper()
            sections.append({'order':len(sections),'kind':fields[0].lower(),'port':port,
                             'protocol':protocol,'protocolFamily':protocol.replace('AVPF','AVP'),
                             'content':'main' if fields[0].lower()=='video' else None})
        elif line.startswith('a=content:') and sections:
            sections[-1]['content']=line.partition(':')[2].strip().lower()
    return sections

def verify_sdp_sections(sections,baseline=None):
    if ([section['kind'] for section in sections]!=['audio','video','video','application']
        or sections[1]['content'] not in ('main',None) or sections[2]['content']!='slides'
        or 'BFCP' not in sections[3]['protocolFamily']):
        raise RuntimeError('Local SDP lost the four ordered audio/main/slides/BFCP sections')
    # Ports are recorded, not frozen: legitimate media/NAT allocation can change
    # across dialogs. A rejected BFCP section may retain its identity at port0.
    identity=lambda values:[(s['order'],s['kind'],s['protocolFamily'],s['content']) for s in values]
    if baseline is not None and identity(sections)!=identity(baseline):
        raise RuntimeError('Local SDP media order/protocol/content identity changed')

def validate_stability_duration(seconds,long_stability=False,redial_cycles=0):
    if long_stability:
        if redial_cycles or not 1801<=seconds<=2160:
            raise ValueError('Long stability requires one call bounded to1801–2160connected seconds')
    elif not 45<=seconds<=90:
        raise ValueError('A call must be bounded to45–90connected seconds; longer tests require explicit --long-stability')

def run(args):
    if args.target not in TARGETS: raise ValueError('Target is outside the user-authorized matrix')
    validate_stability_duration(args.seconds,getattr(args,'long_stability',False),args.redial_cycles)
    if (args.exercise_recording or args.share_multiple_sources) and not args.share_fixture:
        raise ValueError('Recording/source-switch exercises require the owned --share-fixture')
    if not 0<=args.redial_cycles<=2:raise ValueError('Redial cycles must be0–2')
    if args.redial_cycles and (args.seconds!=90 or not args.share_fixture):
        raise ValueError('Redial exercises require a90-second first call and the owned --share-fixture')
    if args.redial_cycles and args.exercise_recording and not args.playback_probe:
        raise ValueError('Redial recordings require --playback-probe for actual MP4 decode proof')
    if args.exercise_recording and not args.playback_probe:raise ValueError('Recording exercises require actual MP4 decode via --playback-probe')
    if not 1<=args.sharing_cycles<=4:raise ValueError('Sharing cycles must be1–4')
    for path in (args.worker,args.share_fixture,args.playback_probe,args.debugger):
        if path is not None and not path.is_file():raise ValueError('Test executable is absent: '+str(path))
    if args.verify_sdp_shape and args.protocol!='sip':raise ValueError('SDP shape proof requires SIP')
    args.output.mkdir(parents=True,exist_ok=True)
    name=f'{TARGETS[args.target]}-{args.target}-{args.protocol}-aac-{args.aac}'
    lines=[]; events=[]; timed_health=[]; commands=[]; readers=[]; started=time.monotonic(); connected=None; process=None; fixture=None
    result={'device':TARGETS[args.target],'address':args.target,'protocol':args.protocol,'aac':args.aac,
            'connectionEnvironment':args.connection_mode,
            'requiredConnectedSeconds':args.seconds,'result':'FAIL','receiverDecodedVideoVerified':False,
            'humanAudibilityVerified':False,'workerDecodedAndSubmittedAudio':False,
            'windowsProcessPlaybackVerified':False,'syntheticOutgoingMedia':not args.real_microphone,
            'realMicrophoneRequested':args.real_microphone,'peerReceivedSdpVerified':False,
            'memoryLeakAbsenceVerified':False,
            'redialCyclesRequested':args.redial_cycles}
    result['workerSha256']=hashlib.sha256(args.worker.read_bytes()).hexdigest()
    result['ports']={'sip':args.sip_port,'h323':args.h323_port,'rtpStart':args.rtp_port}
    recording_dir=(args.output/(name+'-recordings')).resolve()
    recordings=[]; snapshots=[]; dialogs=[]; format_baseline=None; format_event_start=0
    call_event_start=0;call_line_start=0;call_seconds=args.seconds;dialog_index=0;dialog_active=False
    pending_recording=None
    sdp_baseline=None
    hangup_requested=False;resource_samples=[]
    existing_recordings={path.name for path in recording_dir.glob('*.mp4')}
    def observe_resources(phase):
        value={'dialog':dialog_index,'phase':phase,'seconds':round(time.monotonic()-started,3)}
        try:value.update(process_resources(process.pid))
        except (OSError,AttributeError) as error:value['samplingError']=str(error)
        resource_samples.append(value)
    def read(stream):
        for line in stream:
            line=line.rstrip(); lines.append(line)
            if line.startswith('{'):
                try:
                    event=json.loads(line); events.append(event)
                    if event.get('type')=='opal-connection-health': timed_health.append((time.monotonic(),event))
                except json.JSONDecodeError: pass
    def send(line):
        commands.append({'seconds':round(time.monotonic()-started,3),'command':line})
        process.stdin.write(line+'\n');process.stdin.flush()
    def wait(test,seconds,enforce_call_deadline=True):
        end=time.monotonic()+seconds
        if enforce_call_deadline and connected is not None:end=min(end,connected+call_seconds)
        while time.monotonic()<end:
            if enforce_call_deadline and dialog_active and any(call_ended_line(x) for x in lines[call_line_start:]):
                raise RuntimeError('Call ended before the required stable duration')
            if test(): return
            if process.poll() is not None: raise RuntimeError('Owned worker exited early')
            time.sleep(.05)
        raise TimeoutError('Native confirmation timed out')
    def formats():
        return audio_formats(events[call_event_start:])
    def snapshot(stage):
        value=formats();snapshots.append({'dialog':dialog_index,'stage':stage,'connectedSeconds':round(time.monotonic()-connected,3),'formats':value})
        if format_baseline is not None and value!=format_baseline:
            raise RuntimeError('Negotiated codec/profile/rate/channels/bitrate changed at '+stage)
    def dwell(seconds):
        end=time.monotonic()+seconds
        wait(lambda:time.monotonic()>=end,seconds+.5)
    def video_switch(token,active):
        previous=next((e.get('sentFrames',0) for e in reversed(events[call_event_start:]) if e.get('type')=='opal-connection-health'),0)
        before=len(events);send('desktop on '+token if active else 'desktop off')
        wait(lambda:any(e.get('type')=='video-state' and e.get('desktopRequested') is active
             and e.get('desktopActive') is active and (not active or
             (e.get('appliedWidth',0)>0 and e.get('appliedHeight',0)>0 and not e.get('desktopError'))) for e in events[before:]),20)
        if active:
            wait(lambda:any(e.get('type')=='opal-connection-health' and e.get('sentFrames',0)>previous for e in events[before:]),5)
            dwell(2)
    def probe_playback():
        if not args.playback_probe:return
        proof=args.output/(name+('-windows-playback.json' if not dialog_index else f'-windows-playback-dialog-{dialog_index}.json'))
        proof.unlink(missing_ok=True)
        probe=subprocess.run([str(args.playback_probe),'playback',str(process.pid),'8',str(proof)],
                capture_output=True,text=True,timeout=15,creationflags=0x08000000)
        metadata=json.loads(proof.read_text(encoding='utf-8')) if proof.is_file() else {}
        result['windowsPlayback']=metadata
        verified=(probe.returncode==0 and metadata.get('result')=='PASS' and metadata.get('pid')==process.pid
                  and metadata.get('nonzeroSamples',0)>1000 and metadata.get('peak',0)>100)
        result['windowsProcessPlaybackVerified']=verified
        if not verified:raise RuntimeError('Fresh Windows process playback proof is missing for dialog '+str(dialog_index))
    def share(source,stage):
        token=base64.urlsafe_b64encode(source.encode()).decode().rstrip('=')
        # Confirm the previous source is stopped before observing activation of
        # the next one; an old active notification cannot prove a source switch.
        video_switch(token,False);video_switch(token,True);snapshot(stage)
        return token
    def collect_recording(completed,pause_cycles,call):
        file_name=completed.get('fileName','')
        if not file_name or Path(file_name).name!=file_name:raise RuntimeError('Recording output escaped the owned directory')
        if file_name in existing_recordings:raise RuntimeError('Recording completion reused a pre-existing MP4')
        path=recording_dir/file_name
        if not path.is_file() or path.stat().st_size<=10000 or completed.get('elapsedMs',0)<=0:
            raise RuntimeError('Completed MP4 absent, empty, or missing active timeline metadata')
        if any(item['file']==str(path) for item in recordings):raise RuntimeError('Independent recording file names were reused')
        recordings.append({'file':str(path),'bytes':path.stat().st_size,'elapsedMs':completed['elapsedMs'],
                           'dialog':call,'pauseResumeCycles':pause_cycles,'decodedMediaVerified':False})
    def record(token,index,pause_cycles=3,leave_paused=False):
        nonlocal pending_recording
        before=len(events);send('record start '+token+' mic-on room-on 1920 1080')
        wait(lambda:any(e.get('type')=='recording-state' and e.get('state')=='recording'
             and e.get('paused') is False for e in events[before:]),20)
        dwell(2)
        for cycle in range(pause_cycles):
            before=len(events);send('record pause')
            wait(lambda:any(e.get('type')=='recording-state' and e.get('state')=='recording'
                 and e.get('paused') is True for e in events[before:]),8)
            paused=next(e for e in reversed(events[before:]) if e.get('type')=='recording-state' and e.get('paused'))
            dwell(.3)
            before=len(events);send('record resume')
            wait(lambda:any(e.get('type')=='recording-state' and e.get('state')=='recording'
                 and e.get('paused') is False for e in events[before:]),8)
            resumed=next(e for e in reversed(events[before:]) if e.get('type')=='recording-state' and e.get('paused') is False)
            if resumed.get('elapsedMs',0)<paused.get('elapsedMs',0):raise RuntimeError('Recording timeline moved backwards on resume')
            if resumed.get('elapsedMs',0)>paused.get('elapsedMs',0)+100:raise RuntimeError('Recording timeline advanced while paused')
            before=len(events);dwell(1.5)
            wait(lambda:any(e.get('type')=='recording-state' and e.get('state')=='recording' and e.get('paused') is False
                 and e.get('elapsedMs',0)>=resumed.get('elapsedMs',0)+500 for e in events[before:]),3)
            snapshot(f'recording-{index}-resume-{cycle+1}')
        if leave_paused:
            before=len(events);send('record pause')
            wait(lambda:any(e.get('type')=='recording-state' and e.get('state')=='recording'
                 and e.get('paused') is True for e in events[before:]),8)
            paused=next(e for e in reversed(events[before:]) if e.get('type')=='recording-state' and e.get('paused') is True)
            pending_recording={'dialog':dialog_index,'pauseResumeCycles':pause_cycles,'pausedElapsedMs':paused.get('elapsedMs',0)}
            snapshot(f'recording-{index}-left-paused')
            return
        before=len(events);send('record stop')
        wait(lambda:any(e.get('type')=='recording-state' and e.get('state')=='completed' for e in events[before:]),20)
        completed=next(e for e in events[before:] if e.get('type')=='recording-state' and e.get('state')=='completed')
        collect_recording(completed,pause_cycles,dialog_index)
        snapshot(f'recording-{index}-completed')
    def start_dialog(seconds):
        nonlocal connected,call_seconds,call_event_start,call_line_start,format_event_start,format_baseline,dialog_active,sdp_baseline,hangup_requested
        connected=None;call_seconds=seconds;call_event_start=len(events);call_line_start=len(lines)
        hangup_requested=False;send('call '+target);dialog_active=True
        wait(lambda:any(call_connected_line(x) for x in lines[call_line_start:]),45)
        connected=time.monotonic()
        result['callConnected']=True
        wait(lambda:all(direction in formats()['directions'] for direction in ('send','receive')) and
             bool(formats()['profile']['negotiated']),12)
        current=formats()
        for values in [current['profile'],*current['directions'].values()]:
            if any(type(values.get(key)) is not int or values[key]<=0 for key in ('sampleRate','channels','bitrateKbps')):
                raise RuntimeError('Negotiated audio format lacks actual rate/channels/bitrate evidence')
        if any(not isinstance(values.get('codec'),str) or not values['codec'].strip() for values in current['directions'].values()):
            raise RuntimeError('Negotiated audio direction lacks an actual codec')
        if format_baseline is None:format_baseline=current
        elif current!=format_baseline:raise RuntimeError('Negotiated audio format/profile changed across redials')
        if args.verify_sdp_shape:
            wait(lambda:any(e.get('type')=='opal-sdp-offer' and e.get('protocol')=='sip' for e in events[call_event_start:]),12)
            local_offer=next(e for e in reversed(events[call_event_start:]) if e.get('type')=='opal-sdp-offer' and e.get('protocol')=='sip')
            sections=sdp_media_sections(local_offer.get('sdp',''));verify_sdp_sections(sections,sdp_baseline)
            if sdp_baseline is None:sdp_baseline=sections
        format_event_start=len(events);snapshot('connected')
        observe_resources('connected')
        print('CONNECTED '+name+' dialog='+str(dialog_index)+' PID='+str(process.pid),flush=True)
    def restart_owned_source(token,cycles,stage):
        for active in [value for _ in range(cycles) for value in (True,False)] + [True]:
            video_switch(token,active)
        snapshot(stage)
    def finish_dialog():
        next_resource_sample=time.monotonic()+30
        while time.monotonic()-connected<call_seconds:
            if process.poll() is not None or any(call_ended_line(x) for x in lines[call_line_start:]):
                raise RuntimeError('Call ended before the required stable duration')
            if time.monotonic()>=next_resource_sample:
                observe_resources('connected-periodic')
                next_resource_sample=time.monotonic()+30
            time.sleep(.1)
        if any(call_ended_line(x) for x in lines[call_line_start:]):raise RuntimeError('Call ended before controlled hangup')
        current_events=events[call_event_start:]
        verify_native_errors(current_events,args.verify_second_call_rejection)
        health=[e for e in current_events if e.get('type')=='opal-connection-health']
        audio=[e for e in current_events if e.get('type')=='audio-direction-state']
        decoded=any(e.get('decodedNonzeroSamples',0)>1000 and e.get('deviceSubmittedNonzeroSamples',0)>1000 for e in health)
        sent=any(e.get('sentFrames',0)>20 for e in health)
        captured=any(e.get('capturedSamples',0)>1000 for e in health)
        if args.real_microphone and (not captured or any(e.get('code')=='capture-device-failed' for e in current_events)):
            raise RuntimeError('Actual attached microphone capture proof is missing or the capture device failed')
        end=time.monotonic()
        baseline=[e for t,e in timed_health if connected<=t<=end-12]
        recent=[e for t,e in timed_health if max(connected,end-3)<=t<=end]
        deltas={key:recent[-1].get(key,0)-baseline[-1].get(key,0)
                for key in ('decodedNonzeroSamples','deviceSubmittedNonzeroSamples','sentFrames','receivedFrames')} if baseline and recent else {}
        if deltas.get('decodedNonzeroSamples',0)<1000 or deltas.get('deviceSubmittedNonzeroSamples',0)<1000 or deltas.get('sentFrames',0)<5:
            raise RuntimeError('Media did not continue through the final stable window of dialog '+str(dialog_index))
        if any('G.711' in json.dumps(e) or 'g711' in json.dumps(e) for e in audio):raise RuntimeError('Excluded G.711 negotiated')
        if not decoded or not sent:raise RuntimeError('Actual decoded PCM and outgoing video proof is missing')
        if any('Terminated_TransportError' in line for line in lines[call_line_start:]):
            raise RuntimeError('An in-dialog transaction failed to obtain its transport')
        snapshot('final-stable-window');verify_audio_formats(events[format_event_start:],format_baseline)
        offers=[{'sequence':e.get('sequence'),'protocol':e.get('protocol'),'sections':sdp_media_sections(e.get('sdp',''))}
                for e in current_events if e.get('type')=='opal-sdp-offer']
        if args.verify_sdp_shape:
            if not offers:raise RuntimeError('Local SDP diagnostic trace is unavailable')
            for offer in offers:
                if offer['protocol']!='sip':raise RuntimeError('Local SDP diagnostic protocol mismatch')
                verify_sdp_sections(offer['sections'],sdp_baseline)
        summary={'dialog':dialog_index,'requiredConnectedSeconds':call_seconds,
                 'connectedSeconds':round(end-connected,3),'finalWindowMediaDeltas':deltas,
                 'workerDecodedAndSubmittedAudio':decoded,'workerSentVideo':sent,'negotiationStable':True,
                 'directions':audio[-4:],'formats':formats(),'lastHealth':health[-1] if health else None,
                 'localSdpOffers':offers,'localSdpShapeVerified':args.verify_sdp_shape,
                 'actualMicrophoneCaptureVerified':args.real_microphone and captured,
                 'windowsProcessPlaybackVerified':result['windowsProcessPlaybackVerified'],
                 'sameSourceRestartCycles':3 if dialog_index else args.sharing_cycles if args.share_fixture else 0}
        dialogs.append(summary)
        result.update(workerDecodedAndSubmittedAudio=decoded,workerSentVideo=sent,
                      workerDecodedVideo=any(e.get('receivedFrames',0)>5 for e in health),
                      directions=summary['directions'],lastHealth=summary['lastHealth'],
                      finalWindowMediaDeltas=deltas,negotiationStable=True)
        result['actualMicrophoneCaptureVerified']=args.real_microphone and captured
        result['localSdpShapeVerified']=args.verify_sdp_shape
        result['lastContentHealth']=next((e for e in reversed(current_events) if e.get('type')=='opal-content-health'),None)
        observe_resources('stable-window-completed')
    def hangup_dialog():
        nonlocal connected,dialog_active,pending_recording,hangup_requested
        before=len(events);before_lines=len(lines);connected=None
        hangup_requested=True;send('hangup')
        # Pinned OPAL's non-INVITE completion timeout is16s. Allow its BYE
        # completion and call/media teardown, while keeping a hard25s bound.
        wait(lambda:any(call_ended_line(x) for x in lines[before_lines:]),25,False)
        dialog_active=False
        if pending_recording is not None:
            wait(lambda:any(e.get('type')=='recording-state' and e.get('state')=='completed' for e in events[before:]),20,False)
            completed=next(e for e in events[before:] if e.get('type')=='recording-state' and e.get('state')=='completed')
            if not pending_recording['pausedElapsedMs']<=completed.get('elapsedMs',0)<=pending_recording['pausedElapsedMs']+100:
                raise RuntimeError('Paused recording timeline advanced before hangup completion')
            collect_recording(completed,pending_recording['pauseResumeCycles'],pending_recording['dialog'])
            if dialogs:dialogs[-1]['pausedRecordingCompletedOnHangup']=True
            pending_recording=None
        if dialogs:dialogs[-1]['cleanHangupConfirmed']=True
        observe_resources('hangup-completed')
    try:
        env=os.environ.copy()
        for key in ('MYSTUDIO_DEVICE_USER','MYSTUDIO_DEVICE_PASSWORD','ESSASTUDIO_DEVICE_TEST_CUTOFF_UTC'):
            env.pop(key,None)
        env.pop('ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR',None)
        for key in ('ESSASTUDIO_SIP_REGISTRAR','ESSASTUDIO_SIP_IDENTITY','ESSASTUDIO_SIP_OUTBOUND_PROXY',
                    'ESSASTUDIO_SIP_USERNAME','ESSASTUDIO_SIP_PASSWORD'):
            env.pop(key,None)
        env.update(ESSASTUDIO_SIP_TRANSPORT='udp',ESSASTUDIO_SIP_SRTP='disabled',
                   ESSASTUDIO_TEST_MODE='0' if args.real_microphone else '1',ESSASTUDIO_TEST_TONE_HZ='440',
                   ESSASTUDIO_MICROPHONE_ID='',ESSASTUDIO_SPEAKER_ID='')
        if args.real_microphone or args.verify_sdp_shape:env['ESSASTUDIO_DIAGNOSTIC_LOG']='1'
        if args.exercise_recording:
            recording_dir.mkdir(parents=True,exist_ok=True)
            env['ESSASTUDIO_RECORDING_DIRECTORY']=str(recording_dir)
        launch=([str(args.debugger),str(args.worker)] if args.debugger else [str(args.worker)])
        process=subprocess.Popen(launch+['--sip-port',str(args.sip_port),'--h323-port',str(args.h323_port),
            '--rtp-port',str(args.rtp_port),'--interface',args.interface],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,text=True,encoding='utf-8',errors='replace',env=env,creationflags=0x08000000)
        for stream in (process.stdout,process.stderr):
            reader=threading.Thread(target=read,args=(stream,),daemon=True);reader.start();readers.append(reader)
        wait(lambda:any(x.startswith('Listening SIP on ') for x in lines) and
                    any(x.startswith('Listening H.323 on ') for x in lines),15)
        send('protocol '+args.protocol);send('endpoint '+TARGETS[args.target]);send('aac-enabled '+args.aac)
        send('network-mode '+('external' if args.connection_mode=='vpn' else 'direct'))
        codec='aac-lc' if TARGETS[args.target].startswith('pcs') else 'aac-ld'
        channels,kbps=(2,192) if codec=='aac-lc' else ((1,64) if args.protocol=='sip' and not args.prefer_stereo else (2,128))
        buffers='100 600 200 100' if args.connection_mode=='vpn' else '100 300 100 40'
        send(f'audio-profile {codec if args.aac=="on" else "g722"} {channels if args.aac=="on" else 1} {kbps if args.aac=="on" else 64} {buffers} 0')
        if not TARGETS[args.target].startswith('pcs'):
            send('video-preferences fhd 4000');result['requestedVideo']='1080p/4000kbps'
        else:
            send('video-preferences hd 3000');result['requestedVideo']='720p/3000kbps'
        target=f'sip:{args.target}:5060' if args.protocol=='sip' else f'h323:{args.target}:1720'
        start_dialog(args.seconds)
        probe_playback()
        if args.verify_second_call_rejection:
            wait(lambda:any(e.get('type')=='opal-connection-health' for e in events[call_event_start:]),5)
            baseline=next(e for e in reversed(events[call_event_start:]) if e.get('type')=='opal-connection-health');before=len(events);before_lines=len(lines)
            # The policy must reject before any second network dialog is opened.
            send('call sip:127.0.0.1:65534')
            wait(lambda:any(e.get('type')=='media-error' and e.get('code')=='call-busy' for e in events[before:]),5)
            wait(lambda:any(e.get('type')=='opal-connection-health'
                 and e.get('decodedAudioSamples',0)>baseline.get('decodedAudioSamples',0)
                 and e.get('sentFrames',0)>baseline.get('sentFrames',0) for e in events[before:]),12)
            if any(call_ended_line(line) for line in lines[before_lines:]):
                raise RuntimeError('First call ended after second-call rejection')
            result['secondCallRejected']=True;result['firstCallContinuedAfterRejection']=True
            result['secondCallAttemptTarget']='sip:127.0.0.1:65534';snapshot('second-call-rejected')
        if args.exercise_controls:
            for held in (True,False):
                before=len(events);send('hold '+('on' if held else 'off'))
                # A hold re-INVITE may wait behind OPAL's 32-second INVITE
                # transaction. Each direction remains bounded and requires a
                # fresh confirmation with the exact requested state.
                wait(lambda:any(e.get('type')=='call-hold-changed' and e.get('held') is held for e in events[before:]),45)
                if held:time.sleep(1)
            result['callHoldResumeConfirmed']=True
            snapshot('call-hold-resume')
        if args.share_fixture:
            fixture_seconds=args.seconds+180 if getattr(args,'long_stability',False) else (400 if args.redial_cycles else 150)
            fixture=subprocess.Popen([str(args.share_fixture),str(fixture_seconds)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,text=True,encoding='utf-8',creationflags=0x08000000)
            fixture_lines=queue.Queue()
            threading.Thread(target=lambda:fixture_lines.put(fixture.stdout.readline()),daemon=True).start()
            handles=json.loads(fixture_lines.get(timeout=5))
            helper_path=Path(__file__).resolve().parents[1]/'native/opal/tests/LoopbackWorkerTests.py'
            spec=importlib.util.spec_from_file_location('opal_owned_capture_helper',helper_path)
            helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
            items=helper.catalog(args.worker)
            def window_source(handle):
                return next((source for kind,source in items if kind==2 and source.startswith('esw1.')
                    and int.from_bytes(base64.urlsafe_b64decode(source[5:]+'='*((-len(source[5:]))%4))[4:12],'little')==handle),None)
            source=window_source(handles['selectedHandle'])
            if source is None:raise RuntimeError('Owned selected share window absent from CaptureCatalog')
            token=base64.urlsafe_b64encode(source.encode()).decode().rstrip('=')
            restart_owned_source(token,args.sharing_cycles,'same-source-share-restarts')
            result['sharingPauseResumeConfirmed']=True
            result['sameSourceRestartCycles']=args.sharing_cycles
            result['contentState']=next((e for e in reversed(events) if e.get('type')=='content-sharing-state'),None)
            result['ownedWindowShared']=True
            if args.share_multiple_sources:
                other=window_source(handles['decoyHandle']);monitor=next((value for kind,value in items if kind==1),None)
                if other is None or monitor is None:raise RuntimeError('Owned second window or monitor absent from CaptureCatalog')
                switched=[]
                for label,capture in [('other-owned-window',other),('monitor',monitor),('selected-owned-window',source)]:
                    token=share(capture,'source-switch-'+label);switched.append({'kind':label,'nativeActiveConfirmed':True})
                result['sourceSwitches']=switched;result['multipleSourcesConfirmed']=True
            if args.exercise_recording:
                # Both files record the selected owned cyan pattern; source switches
                # are exercised separately so the pattern probe stays meaningful.
                for index in (1,2):record(token,index)
                if len({item['file'] for item in recordings})!=2:raise RuntimeError('Two independent recording files were not produced')
                result['recordingPauseResumeConfirmed']=True
                result['recordingPauseResumeCycles']=6
        finish_dialog()
        if args.playback_probe and not result['windowsProcessPlaybackVerified']:
            raise RuntimeError('Windows process playback proof is missing')
        for cycle in range(1,args.redial_cycles+1):
            hangup_dialog();dwell(2)
            observe_resources('settled-after-hangup')
            if fixture.poll() is not None:raise RuntimeError('Owned sharing fixture exited before redial')
            dialog_index=cycle
            start_dialog(45)
            probe_playback()
            # Restart the exact window token retained from the first dialog.
            selected_token=base64.urlsafe_b64encode(source.encode()).decode().rstrip('=')
            restart_owned_source(selected_token,3,'redial-same-source-share-restarts')
            if args.exercise_recording:record(selected_token,'redial-'+str(cycle),pause_cycles=1,leave_paused=True)
            finish_dialog()
            result['redialCyclesCompleted']=cycle
        result['redialCyclesRequested']=args.redial_cycles
        result['sameWorkerProcessRedialVerified']=args.redial_cycles>0
        result['result']='LOCAL_MEDIA_PASS_PEER_VIDEO_PENDING'
    except Exception as error:
        result['error']=type(error).__name__+': '+str(error)
    finally:
        result['connectedSeconds']=dialogs[0]['connectedSeconds'] if dialogs else round(time.monotonic()-connected,3) if connected else 0
        result['dialogs']=dialogs
        result['workerPid']=process.pid if process else None
        if process is not None and process.poll() is None:
            try:
                if dialog_active and any(call_ended_line(x) for x in lines[call_line_start:]):
                    # A failed/peer-cleared dialog is already closed. A second
                    # hangup cannot produce a new clear event for this token.
                    result['callAlreadyCleared']=True;dialog_active=False;connected=None
                    observe_resources('call-already-cleared')
                if dialog_active:
                    if hangup_requested:raise TimeoutError('Previous fresh hangup confirmation exceeded its25-second bound')
                    hangup_dialog()
                dwell(2);observe_resources('settled-after-hangup')
                send('quit');process.wait(15)
            except (BrokenPipeError,TimeoutError,RuntimeError,subprocess.TimeoutExpired) as error:
                result['cleanupError']=type(error).__name__+': '+str(error)
                if process.poll() is None:process.kill();result['forcedOwnedCleanup']=True
                process.wait(5)
        for reader in readers:reader.join(2)
        if fixture is not None:
            try:fixture.stdin.write('quit\n');fixture.stdin.flush();fixture.wait(5)
            except (BrokenPipeError,subprocess.TimeoutExpired):fixture.kill();fixture.wait(5);result['forcedFixtureCleanup']=True
            result['fixtureExitCode']=fixture.returncode
        result['workerExitCode']=process.returncode if process else None
        result['dialogTransportFailure']=any('Terminated_TransportError' in line for line in lines)
        result['callFormatSnapshots']=snapshots;result['recordings']=recordings
        result['processResourceSamples']=resource_samples
        measured=[sample for sample in resource_samples if 'privateBytes' in sample]
        result['processResourceSamplingAvailable']=bool(measured)
        if measured:
            result['processResourcePeakScope']='Phase samples; peakWorkingSetBytes is the OS lifetime high-water mark'
            result['processResourcePeaks']={key:max(sample[key] for sample in measured) for key in
                ('privateBytes','workingSetBytes','peakWorkingSetBytes','handleCount')}
        if recordings:result['recordingPauseResumeCycles']=sum(item['pauseResumeCycles'] for item in recordings)
        if result['result'].startswith('LOCAL_MEDIA_PASS') and args.playback_probe:
            try:
                for index,item in enumerate(recordings,1):
                    proof=args.output/(name+f'-recording-{index}-decode.json')
                    proof.unlink(missing_ok=True)
                    probe=subprocess.run([str(args.playback_probe),'recording-pattern',item['file'],str(proof)],
                            capture_output=True,text=True,timeout=30,creationflags=0x08000000)
                    metadata=json.loads(proof.read_text(encoding='utf-8')) if proof.is_file() else {}
                    item['decodeMetadata']=metadata
                    if (probe.returncode!=0 or metadata.get('result')!='PASS' or metadata.get('videoWidth')!=1920 or metadata.get('videoHeight')!=1080
                        or metadata.get('videoSamples',0)<10 or metadata.get('audioSamples',0)<10
                        or metadata.get('audioNonzeroPcmSamples',0)<=1000):
                        raise RuntimeError('Actual recording MP4 decode/pattern/dimensions failed: '+str(proof))
                    item['decodedMediaVerified']=True
                if recordings:result['recordingsDecodedMediaVerified']=True
            except Exception as error:
                result['result']='FAIL';result['error']=type(error).__name__+': '+str(error)
        if result['dialogTransportFailure']:
            result['result']='FAIL';result.setdefault('error','An in-dialog transaction or BYE failed to obtain its transport')
        try:verify_native_errors(events,args.verify_second_call_rejection)
        except RuntimeError as error:
            result['result']='FAIL';result.setdefault('error',str(error));result['nativeError']=str(error)
        if result.get('cleanupError') or result.get('forcedOwnedCleanup') or result['workerExitCode']!=0 or result.get('forcedFixtureCleanup') or result.get('fixtureExitCode',0)!=0:
            result['result']='FAIL';result.setdefault('error',result.get('cleanupError','Clean call teardown was not confirmed'))
        result['totalSeconds']=round(time.monotonic()-started,3)
        (args.output/(name+'.log')).write_text('\n'.join(lines),encoding='utf-8')
        (args.output/(name+'-commands.json')).write_text(json.dumps(commands,indent=2),encoding='utf-8')
        (args.output/(name+'.json')).write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(f"{name}: {result['result']} connected={result['connectedSeconds']}s exit={result['workerExitCode']}"+
              (f" error={result['error']}" if result.get('error') else ''),flush=True)
    return 0 if result['result'].startswith('LOCAL_MEDIA_PASS') else 1

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--worker',type=Path,required=True);p.add_argument('--target',required=True)
    p.add_argument('--protocol',choices=['sip','h323'],required=True);p.add_argument('--aac',choices=['on','off'],default='off')
    p.add_argument('--interface',required=True);p.add_argument('--seconds',type=int,default=50);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--long-stability',action='store_true',help='Explicitly permit a single bounded call longer than30minutes (1801–2160seconds)')
    p.add_argument('--playback-probe',type=Path)
    p.add_argument('--debugger',type=Path)
    p.add_argument('--exercise-controls',action='store_true')
    p.add_argument('--share-fixture',type=Path)
    p.add_argument('--prefer-stereo',action='store_true')
    p.add_argument('--sharing-cycles',type=int,default=1)
    p.add_argument('--share-multiple-sources',action='store_true')
    p.add_argument('--exercise-recording',action='store_true')
    p.add_argument('--verify-second-call-rejection',action='store_true')
    p.add_argument('--real-microphone',action='store_true',help='Use the actual default microphone and require captured PCM evidence')
    p.add_argument('--verify-sdp-shape',action='store_true',help='Require local SIP offer diagnostics with stable ordered audio/main/slides/BFCP sections')
    p.add_argument('--redial-cycles',type=int,choices=range(3),default=0,
                   help='After a90-second first call, run up to two45-second calls in the same worker with the same owned window')
    p.add_argument('--connection-mode',choices=['lan','vpn'],default='vpn')
    p.add_argument('--sip-port',type=int,default=15180); p.add_argument('--h323-port',type=int,default=17820)
    p.add_argument('--rtp-port',type=int,default=45400)
    raise SystemExit(run(p.parse_args()))
