"""Owned localhost TCP SIP peer with a closed ephemeral Contact port.
Proves hold/resume INVITE, INFO and BYE reuse the established dialog; no room calls.
Optional system-audio mode uses the real Windows process-loopback capture backend.
"""
import argparse,json,os,socket,struct,subprocess,threading,time
from pathlib import Path

def run(args):
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(20)
    closed=socket.socket();closed.bind(('127.0.0.1',0));closed_port=closed.getsockname()[1]
    records=[]; errors=[]; flow={}; idle_wait_elapsed=0.0; system_audio_evidence=[]; bye=threading.Event();ack=threading.Event()
    codec_pending=threading.Event();codec_response=threading.Event();hold_pending=threading.Event();hold_response=threading.Event()
    media_stop=threading.Event();media_streams={};media_threads=[]
    def open_media(index,kind,remote_port,remote_rtcp):
        if index in media_streams:
            stream=media_streams[index];stream['remoteRtcp']=('127.0.0.1',remote_rtcp);return stream
        rtp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);rtp.bind(('127.0.0.1',0));rtp.settimeout(.2)
        rtcp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);rtcp.bind(('127.0.0.1',0));rtcp.settimeout(.2)
        stream={'kind':kind,'rtp':rtp,'rtcp':rtcp,'rtpPort':rtp.getsockname()[1],'rtcpPort':rtcp.getsockname()[1],
                'remoteRtcp':('127.0.0.1',remote_rtcp),'ssrc':0x51510000+index,'sourceSsrc':0,'highestSequence':0,
                'sendAllowed':True,'receivedRtpPackets':0,'receivedG722Packets':0,'echoedRtpPackets':0,'receivedRtcpPackets':0,'sentReceiverReports':0}
        media_streams[index]=stream
        def rtp_loop():
            try:
                while not media_stop.is_set():
                    try:packet,address=rtp.recvfrom(65536)
                    except socket.timeout:continue
                    if address[0]!='127.0.0.1':raise AssertionError('RTP peer is not owned localhost')
                    if len(packet)<12 or packet[0]>>6!=2:raise AssertionError('Invalid outgoing RTP frame')
                    stream['receivedRtpPackets']+=1;stream['sourceSsrc']=struct.unpack_from('!I',packet,8)[0]
                    payload=packet[1]&0x7f
                    if payload not in stream.setdefault('payloadTypes',[]):stream['payloadTypes'].append(payload)
                    if kind=='audio' and packet[1]&0x7f==9:stream['receivedG722Packets']+=1
                    stream['ssrc']=(stream['sourceSsrc']^0xA5A5A5A5) or 1;stream['highestSequence']=struct.unpack_from('!H',packet,2)[0]
                    if stream['sendAllowed']:
                        reflected=bytearray(packet);struct.pack_into('!I',reflected,8,stream['ssrc'])
                        rtp.sendto(reflected,address);stream['echoedRtpPackets']+=1
            except OSError as error:
                if not media_stop.is_set():errors.append('Owned RTP: '+str(error))
            except Exception as error:errors.append('Owned RTP: '+str(error))
        def rtcp_loop():
            last_report=0.0
            try:
                while not media_stop.is_set():
                    try:
                        packet,address=rtcp.recvfrom(65536)
                        if address[0]!='127.0.0.1':raise AssertionError('RTCP peer is not owned localhost')
                        if len(packet)<4 or packet[0]>>6!=2:raise AssertionError('Invalid outgoing RTCP frame')
                        stream['receivedRtcpPackets']+=1;stream['remoteRtcp']=address
                    except socket.timeout:pass
                    if time.monotonic()-last_report<1 or not stream['sourceSsrc']:continue
                    # A real compound RR + SDES, reporting the echoed codec stream.
                    report=struct.pack('!BBHIIIIIII',0x81,201,7,stream['ssrc'],stream['sourceSsrc'],0,stream['highestSequence'],0,0,0)
                    cname=b'owned-sip-idle';chunk=struct.pack('!I',stream['ssrc'])+bytes((1,len(cname)))+cname+b'\0'
                    chunk+=b'\0'*((-len(chunk))%4)
                    report+=struct.pack('!BBH',0x81,202,len(chunk)//4)+chunk
                    rtcp.sendto(report,stream['remoteRtcp']);stream['sentReceiverReports']+=1;last_report=time.monotonic()
            except OSError as error:
                if not media_stop.is_set():errors.append('Owned RTCP: '+str(error))
            except Exception as error:errors.append('Owned RTCP: '+str(error))
        for target in (rtp_loop,rtcp_loop):
            worker=threading.Thread(target=target,daemon=True);worker.start();media_threads.append(worker)
        return stream
    def media_answer(body):
        sections=[]
        for line in body.decode().splitlines():
            if line.startswith('m='):sections.append([line])
            elif sections:sections[-1].append(line)
        answer=['v=0','o=- 1 1 IN IP4 127.0.0.1','s=Owned flow regression','c=IN IP4 127.0.0.1','t=0 0']
        for index,section in enumerate(sections,1):
            kind,remote_port,transport,*payloads=section[0][2:].split();remote_port=int(remote_port)
            mapping={line.split(':',1)[1].split(' ',1)[0]:line for line in section if line.startswith('a=rtpmap:')}
            initial_aac=(args.early_reinvite or args.explicit_mono or args.queued_hold) and kind=='audio'
            first_early=initial_aac and sum(r['method']=='INVITE' for r in records)==1
            payload=next((p for p in payloads if 'MP4A-LATM/90000/2' in mapping.get(p,'')),None) if initial_aac and (first_early or not args.explicit_mono) else next((p for p in payloads if (kind=='audio' and p=='9') or (kind=='video' and 'H264/90000' in mapping.get(p,''))),None)
            if first_early and not payload:raise AssertionError('Early TX failure fixture requires offered stereo AAC')
            split_video=getattr(args,'split_video_formats',False)
            if split_video and kind=='video':
                slides='a=content:slides' in section
                payload=next((p for p in payloads if 'H264/90000' in mapping.get(p,'') and
                    any(line.startswith('a=fmtp:'+p+' ') and 'packetization-mode=1' in line for line in section)==slides),None)
                if payload is None:raise AssertionError('Split-video fixture needs both H264 packetization modes in the offer')
            if kind not in ('audio','video') or ('a=content:slides' in section and not split_video) or not payload or not remote_port:
                answer.extend([f'm={kind} 0 {transport} '+ ' '.join(payloads),'a=inactive']);continue
            direction='sendonly' if 'a=recvonly' in section else 'recvonly' if 'a=sendonly' in section else 'inactive' if 'a=inactive' in section else 'sendrecv'
            remote_rtcp=next((int(line[7:].split()[0]) for line in section if line.startswith('a=rtcp:')),remote_port+1)
            stream=open_media(index,kind,remote_port,remote_rtcp);stream['sendAllowed']=direction in ('sendrecv','sendonly')
            stream['expectedPayload']=int(payload)
            answer.extend([f'm={kind} {stream["rtpPort"]} {transport} {payload}'+(' 9' if first_early else ''),f'a={direction}',f'a=rtcp:{stream["rtcpPort"]} IN IP4 127.0.0.1',mapping.get(payload,'a=rtpmap:9 G722/8000').replace('/90000/2','/90000/1') if first_early and args.explicit_mono else mapping.get(payload,'a=rtpmap:9 G722/8000')])
            if first_early:answer.append('a=rtpmap:9 G722/8000')
            answer.extend(line for line in section if line.startswith('a=fmtp:'+payload+' ') or (kind=='video' and line in ('a=content:main','a=content:slides')))
        return '\r\n'.join(answer)+'\r\n'
    def peer():
        try:
            conn,address=listener.accept();conn.settimeout(max(60 if args.session_timer else 30,args.idle_seconds+30));buffer=b''
            flow.update(id=1,local=conn.getsockname(),peer=address,acceptedMonotonic=time.monotonic())
            with conn:
                while not bye.is_set():
                    while b'\r\n\r\n' not in buffer:
                        data=conn.recv(65536)
                        if not data:raise EOFError('Owned dialog closed before BYE')
                        buffer+=data
                    headers,buffer=buffer.split(b'\r\n\r\n',1);lines=headers.decode().split('\r\n');fields={}
                    for line in lines[1:]:
                        if ':' in line:
                            k,v=line.split(':',1);fields.setdefault(k.lower(),[]).append(v.strip())
                    count=int(fields.get('content-length',['0'])[0])
                    while len(buffer)<count:
                        data=conn.recv(65536)
                        if not data:raise EOFError('Owned dialog closed during body')
                        buffer+=data
                    body,buffer=buffer[:count],buffer[count:];method=lines[0].split()[0];records.append({'method':method,'request':lines[0],'cseq':fields.get('cseq',[''])[0],'callId':fields.get('call-id',[''])[0],'sessionExpires':fields.get('session-expires',[''])[0],'minSe':fields.get('min-se',[''])[0],'tcpFlow':1,'receivedMonotonic':time.monotonic(),'sdp':body.decode()})
                    if method=='ACK':
                        if not args.min_se_retry or any(r.get('response')==200 and r['method']=='INVITE' for r in records):ack.set()
                        continue
                    if method not in ('INVITE','BYE','OPTIONS','INFO'):continue
                    to=fields['to'][0]
                    if ';tag=' not in to:to+=';tag=owned-flow'
                    if args.min_se_retry and method=='INVITE' and sum(r['method']=='INVITE' for r in records)==1:
                        response='SIP/2.0 422 Session Interval Too Small\r\nMin-SE: 3600\r\n'+''.join('Via: '+v+'\r\n' for v in fields['via'])+'From: '+fields['from'][0]+'\r\nTo: '+to+'\r\nCall-ID: '+fields['call-id'][0]+'\r\nCSeq: '+fields['cseq'][0]+'\r\nContent-Length: 0\r\n\r\n'
                        conn.sendall(response.encode());records[-1].update(response=422,responseSentMonotonic=time.monotonic());continue
                    answer=''
                    if method=='INVITE':answer=media_answer(body)
                    if args.queued_hold and method=='INVITE':
                        count=sum(r['method']=='INVITE' for r in records)
                        if count==2:
                            codec_pending.set()
                            if not codec_response.wait(12):raise AssertionError('Owned delayed codec response was not released')
                        elif count==3:
                            hold_pending.set()
                            if not hold_response.wait(12):raise AssertionError('Owned delayed hold response was not released')
                    response_code=200
                    if args.queued_hold and method=='INVITE':
                        count=sum(r['method']=='INVITE' for r in records)
                        if count==2:response_code=int(args.queued_prior_result)
                        elif count==3:response_code=int(args.queued_hold_result)
                    if response_code!=200:answer=''
                    parameter='REFRESHER = UAC' if args.refresher_token=='spaced-uac' else 'refresher=uas;extension=uac' if args.refresher_token=='uas-note-uac' else 'refresher=uac'
                    timer=('Session-Expires: 90;'+parameter+'\r\nSupported: timer\r\n'+('Require: timer\r\n' if args.session_timer=='required' else '')) if args.session_timer and method=='INVITE' else ''
                    reason={200:'OK',488:'Not Acceptable Here',422:'Session Interval Too Small',491:'Request Pending'}[response_code]
                    response=f'SIP/2.0 {response_code} {reason}\r\nAllow: INVITE,ACK,INFO,BYE,OPTIONS\r\n'+('Min-SE: 3600\r\n' if response_code==422 else '')+timer+''.join('Via: '+v+'\r\n' for v in fields['via'])+'From: '+fields['from'][0]+'\r\nTo: '+to+'\r\nCall-ID: '+fields['call-id'][0]+'\r\nCSeq: '+fields['cseq'][0]+f'\r\nContact: <sip:owned@127.0.0.1:{closed_port};transport=tcp>\r\n'+('Content-Type: application/sdp\r\n' if answer else '')+f'Content-Length: {len(answer)}\r\n\r\n'+answer
                    conn.sendall(response.encode());records[-1].update(response=response_code,responseSentMonotonic=time.monotonic())
                    if method=='BYE':records[-1]['response']=200;bye.set()
        except Exception as error:errors.append(str(error))
    thread=threading.Thread(target=peer,daemon=True);thread.start()
    env=os.environ.copy();env.update(ESSASTUDIO_TEST_MODE='1',ESSASTUDIO_TEST_TONE_HZ='440',ESSASTUDIO_TEST_DTMF_INFO='1',ESSASTUDIO_SIP_TRANSPORT='tcp',ESSASTUDIO_DIAGNOSTIC_LOG='1',ESSASTUDIO_SIP_SRTP='disabled',ESSASTUDIO_MICROPHONE_ID='',ESSASTUDIO_SPEAKER_ID='',PTLIB_ASSERT_ACTION='A')
    process=subprocess.Popen([str(args.worker),'--sip-port',str(args.sip_port),'--h323-port',str(args.h323_port),'--rtp-port',str(args.rtp_port),'--interface','127.0.0.1'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env,creationflags=0x08000000)
    lines=[]
    def drain(stream):
        for line in stream:lines.append(line.rstrip())
    for stream in (process.stdout,process.stderr):threading.Thread(target=drain,args=(stream,),daemon=True).start()
    def command(value):process.stdin.write(value+'\n');process.stdin.flush()
    failure=None
    try:
        end=time.monotonic()+20
        while (not any(x.startswith('Listening H.323 on ') for x in lines) or sum(x.startswith('{') and json.loads(x).get('type')=='aac-availability' for x in lines)<2) and time.monotonic()<end:
            if process.poll() is not None:raise AssertionError('Worker failed startup: '+str(process.returncode))
            time.sleep(.05)
        if args.early_reinvite or args.explicit_mono or args.queued_hold:
            command('endpoint sx20');command('aac-enabled on');command('audio-profile aac-ld 2 128 100 600 200 100 0')
        else:command('aac-enabled off')
        command('call sip:owned@127.0.0.1:'+str(listener.getsockname()[1]))
        if not ack.wait(20):raise AssertionError('Owned TCP INVITE dialog did not receive ACK')
        def wait_for(predicate, detail):
            end=time.monotonic()+8
            while time.monotonic()<end:
                if predicate():return
                time.sleep(.025)
            raise AssertionError(detail)
        wait_for(lambda:any('CONFIRMED' in x for x in lines),'Worker did not establish owned dialog')
        if args.early_reinvite:
            wait_for(lambda:sum(r['method']=='INVITE' for r in records)>=2 and sum(r['method']=='ACK' for r in records)>=2,'Early TX failure did not complete owned re-INVITE200/ACK')
            queued=next((i for i,line in enumerate(lines) if 'Queueing re-INVITE to resolve multiple codecs' in line),None)
            confirmed=next((i for i,line in enumerate(lines) if 'CONFIRMED' in line),None)
            if queued is None or confirmed is None or queued>=confirmed:raise AssertionError('Fixture did not trigger actual multiple-codec re-INVITE before connection confirmation')
            def media_slots(sdp):return [tuple(line[2:].split()[:2]) for line in sdp.splitlines() if line.startswith('m=')]
            initial=media_slots(next(r['sdp'] for r in records if r['method']=='INVITE'))
            if [slot[0] for slot in initial]!=['audio','video','video','application']:raise AssertionError('Initial early-failure offer did not contain all four configured sessions')
            for offer in (r for r in records if r['method']=='INVITE'):
                if media_slots(offer['sdp'])!=initial:raise AssertionError('Early TX failure changed/appended on-wire SDP sessions: '+str(media_slots(offer['sdp'])))
            wait_for(lambda:any(s['kind']=='audio' and s['receivedRtpPackets']>=10 for s in media_streams.values()),'Early multiple-codec negotiation did not retain outgoing RTP')
        if args.explicit_mono:
            wait_for(lambda:any(s['kind']=='audio' and s['receivedG722Packets']>=10 for s in media_streams.values()),'Explicit incompatible mono AAC did not recover outgoing G722 RTP')
        if args.min_se_retry:
            invites=[r for r in records if r['method']=='INVITE']
            if len(invites)<2:raise AssertionError('422 did not retry the initial INVITE')
            if invites[1]['sessionExpires'].split(';')[0]!='3600' or invites[1]['minSe']!='3600':raise AssertionError('422 retry failed to honor Min-SE3600')
            if invites[0]['callId']!=invites[1]['callId'] or int(invites[1]['cseq'].split()[0])!=int(invites[0]['cseq'].split()[0])+1:raise AssertionError('422 retry changed dialog or failed to increment CSeq')
        if args.queued_hold:
            if not codec_pending.wait(8):raise AssertionError('Initial codec-reduction INVITE was not pending')
            hold_start=len(lines);command('hold on')
            wait_for(lambda:any('Queueing re-INVITE to put connection on hold' in line for line in lines[hold_start:]),'Hold was not queued behind the earlier transaction')
            codec_response.set()
            if not hold_pending.wait(8):raise AssertionError('Queued hold INVITE did not follow earlier200')
            time.sleep(.1)
            if any(line.startswith('{') and json.loads(line).get('type')=='call-hold-changed' for line in lines[hold_start:]):raise AssertionError('Older codec-reduction response falsely changed hold before held INVITE200')
            command('hold on');time.sleep(.1)
            if sum(r['method']=='INVITE' for r in records)!=3:raise AssertionError('Duplicate pending hold created another INVITE')
            hold_response.set()
            if args.queued_hold_result=='488':
                wait_for(lambda:any(line.startswith('{') and json.loads(line).get('type')=='call-hold-changed' and json.loads(line).get('held') is False for line in lines[hold_start:]),'Rejected held INVITE did not restore previous unheld state')
                if any(line.startswith('{') and json.loads(line).get('type')=='call-hold-changed' and json.loads(line).get('held') is True for line in lines[hold_start:]):raise AssertionError('Rejected held INVITE falsely confirmed hold')
                command('hold on')
            wait_for(lambda:any(line.startswith('{') and json.loads(line).get('type')=='call-hold-changed' and json.loads(line).get('held') is True for line in lines[hold_start:]),'Successful held INVITE200 did not confirm hold')
            success=next((r for r in records if r['method']=='INVITE' and int(r['cseq'].split()[0])>=3 and r.get('response')==200),None)
            if not success:raise AssertionError('Hold confirmation lacked successful matching INVITE')
            if args.queued_hold_result in ('422','491') and int(success['cseq'].split()[0])<=3:raise AssertionError('Retry did not advance hold INVITE CSeq')
        for held, count in (() if getattr(args,'split_video_formats',False) or args.early_reinvite or args.explicit_mono else (('off',4 if args.queued_hold_result=='200' else 5),) if args.queued_hold else (('on',2),('off',3))):
            count+=1 if args.min_se_retry else 0
            hold_start=len(lines);command('hold '+held)
            wait_for(lambda:sum(r['method']=='INVITE' for r in records)>=count and sum(r['method']=='ACK' for r in records)>=count,'Hold/resume re-INVITE200/ACK missing on established flow')
            wait_for(lambda:any(json.loads(line).get('type')=='call-hold-changed' and json.loads(line).get('held')==(held=='on') for line in lines[hold_start:] if line.startswith('{')),'Worker did not confirm actual hold/resume state')
            time.sleep(.15)
        if not args.early_reinvite and not args.explicit_mono and not getattr(args,'split_video_formats',False):
            command('dtmf 1')
            wait_for(lambda:any(r['method']=='INFO' and r.get('response')==200 for r in records),'DTMF INFO200 missing on established flow')
        if args.system_audio:
            # Observe actual worker JSON, excluding the establishment snapshot.
            # Test mode synthesizes microphone input, but process loopback remains real.
            first_audio_line=len(lines)
            def audio_events(start):
                return [json.loads(line) for line in lines[start:] if line.startswith('{')]
            for value,enabled in (('on',True),('on',True),('off',False)):
                start=len(lines);requested=time.monotonic();command('system-audio '+value)
                wait_for(lambda:any(event.get('type')=='system-audio-changed' and event.get('enabled')==enabled for event in audio_events(start)),
                         'Real process-loopback did not confirm system-audio '+value)
                events=audio_events(start)
                acknowledgements=[event for event in events if event.get('type')=='system-audio-changed']
                if any(event.get('enabled')!=enabled for event in acknowledgements):
                    raise AssertionError('System audio confirmed the wrong state while '+value+' was pending')
                system_audio_evidence.append({'command':value,'confirmedEnabled':enabled,
                    'confirmationElapsedSeconds':time.monotonic()-requested,'acknowledgements':acknowledgements})
            if any(event.get('type')=='media-error' and event.get('code','').startswith('system-audio-') for event in audio_events(first_audio_line)):
                raise AssertionError('Real worker reported system-audio failure')
        if args.session_timer and args.negotiation_only:
            states=[json.loads(line) for line in lines if line.startswith('{') and '"type":"sip-session-timer"' in line]
            expected=args.refresher_token!='uas-note-uac'
            if not states or states[-1]['sessionIntervalSeconds']!=90 or states[-1]['localRefresher']!=expected or states[-1]['refreshAfterSeconds']!=(45 if expected else 0):raise AssertionError('Session refresher parameter was not parsed exactly and case-insensitively')
        if args.session_timer and not args.negotiation_only:
            # RFC 4028's real minimum is 90 seconds; do not accelerate the clock
            # or inject invalid short intervals into production for this test.
            last_invite=next(r for r in reversed(records) if r['method']=='INVITE')
            end=last_invite['responseSentMonotonic']+49
            before=sum(s['receivedRtpPackets'] for s in media_streams.values())
            while time.monotonic()<end and sum(r['method']=='INVITE' for r in records)<4:
                if errors:raise AssertionError('Owned timer dialog failed: '+errors[0])
                if process.poll() is not None:raise AssertionError('Worker exited before session refresh')
                time.sleep(.05)
            refreshes=[r for r in records if r['method']=='INVITE'][3:]
            if not refreshes:raise AssertionError('No session refresh near half of negotiated 90-second interval')
            refresh=refreshes[0];elapsed=refresh['receivedMonotonic']-last_invite['responseSentMonotonic']
            if not 44<=elapsed<=49:raise AssertionError('Session refresh occurred at '+str(elapsed)+' seconds')
            if refresh['sessionExpires'].split(';')[0]!='90':raise AssertionError('Refresh did not retain negotiated interval: '+refresh['sessionExpires'])
            wait_for(lambda:refresh.get('response')==200,'Session refresh did not complete')
            if sum(s['receivedRtpPackets'] for s in media_streams.values())<=before+100:raise AssertionError('RTP did not remain active before session refresh')
            after=sum(s['receivedRtpPackets'] for s in media_streams.values())
            wait_for(lambda:sum(s['receivedRtpPackets'] for s in media_streams.values())>after+10,'RTP did not continue after session refresh')
        if args.idle_seconds:
            idle_start=time.monotonic();idle_end=idle_start+args.idle_seconds;quiet_count=len(records)
            while time.monotonic()<idle_end:
                idle_wait_elapsed=time.monotonic()-idle_start
                if errors:raise AssertionError('Owned TCP dialog failed during idle: '+errors[0])
                if process.poll() is not None:raise AssertionError('Worker exited during idle: '+str(process.returncode))
                if len(records)!=quiet_count:raise AssertionError('Unexpected SIP request during the required quiet interval')
                time.sleep(min(.25,max(0,idle_end-time.monotonic())))
            idle_wait_elapsed=time.monotonic()-idle_start
        if getattr(args,'split_video_formats',False):
            main=next(s for s in media_streams.values() if s['kind']=='video')
            wait_for(lambda:main['receivedRtpPackets']>=10,'Split-video peer did not receive main RTP')
            if set(main['payloadTypes'])!={main['expectedPayload']}:
                raise AssertionError('Main video used another SDP section codec/payload: expected '+str(main['expectedPayload'])+', received '+str(sorted(main['payloadTypes'])))
            if errors:raise AssertionError('Split-video peer failed: '+errors[0])
        # Media has been verified. Stop probes before BYE closes the remote
        # sockets, so expected Windows UDP reset notifications are not failures.
        media_stop.set()
        command('hangup')
        if not bye.wait(8):raise AssertionError('BYE missing on established TCP dialog; Contact is intentionally closed')
        bye_record=next(r for r in records if r['method']=='BYE')
        last_before_bye=max(r.get('responseSentMonotonic',r['receivedMonotonic']) for r in records if r['method']!='BYE')
        quiet_gap=bye_record['receivedMonotonic']-last_before_bye
        if quiet_gap+0.05<args.idle_seconds:raise AssertionError('Actual quiet interval before BYE was only '+str(quiet_gap))
        if any(r['tcpFlow']!=flow['id'] for r in records):raise AssertionError('Dialog controls changed the accepted TCP flow')
        if args.idle_seconds and not any(s['kind']=='audio' and s['echoedRtpPackets']>100 and s['sentReceiverReports']>0 for s in media_streams.values()):raise AssertionError('Owned media did not sustain actual G722 RTP/RTCP during idle')
        # Allow the same bounded OPAL shutdown budget as the production owner.
        # Assertions abort this owned test process instead of consuming stdin.
        command('quit');process.wait(25)
        if process.returncode:raise AssertionError('Worker unclean exit '+str(process.returncode))
        flow_label='INVITE/RTP-per-section/BYE200' if args.split_video_formats else 'INVITE/INFO/BYE200'
        print('SIP established TCP dialog '+flow_label+' PASS contact='+str(closed_port)+' quietGapSeconds='+str(round(quiet_gap,3)))
    except Exception as error:failure=error
    finally:
        codec_response.set();hold_response.set()
        if process.poll() is None:
            try:command('quit');process.wait(25)
            except (BrokenPipeError,subprocess.TimeoutExpired):process.kill();process.wait(5)
        listener.close();closed.close();media_stop.set()
        for stream in media_streams.values():stream['rtp'].close();stream['rtcp'].close()
        for worker in media_threads:worker.join(1)
        if args.logs:
            bye_records=[r for r in records if r['method']=='BYE']
            quiet_gap=bye_records[0]['receivedMonotonic']-max(r.get('responseSentMonotonic',r['receivedMonotonic']) for r in records if r['method']!='BYE') if bye_records else None
            media_evidence=[{k:v for k,v in s.items() if k not in ('rtp','rtcp')} for s in media_streams.values()]
            args.logs.mkdir(parents=True,exist_ok=True);(args.logs/'sip-dialog-flow.log').write_text('\n'.join(lines));(args.logs/'sip-dialog-flow.json').write_text(json.dumps({'closedContactPort':closed_port,'acceptedTcpFlows':[flow] if flow else [],'requestedIdleSeconds':args.idle_seconds,'idleWaitElapsedSeconds':idle_wait_elapsed,'quietGapBeforeByeSeconds':quiet_gap,'systemAudio':system_audio_evidence,'ownedMedia':media_evidence,'records':records,'peerErrors':errors,'workerExit':process.returncode,'result':'FAIL' if failure else 'PASS'},indent=2))
    if failure:raise failure
if __name__=='__main__':
    def bounded_idle_seconds(value):
        seconds=float(value)
        if not 0<=seconds<=120:raise argparse.ArgumentTypeError('idle seconds must be between 0 and 120')
        return seconds
    parser=argparse.ArgumentParser();parser.add_argument('--worker',type=Path,required=True);parser.add_argument('--logs',type=Path);parser.add_argument('--sip-port',type=int,default=15150);parser.add_argument('--h323-port',type=int,default=17970);parser.add_argument('--rtp-port',type=int,default=45600);parser.add_argument('--early-reinvite',action='store_true',help='Verify multiple-codec answer queues an early re-INVITE while preserving negotiated SDP sessions');parser.add_argument('--queued-hold',action='store_true',help='Delay earlier re-INVITE200 and verify hold confirms only its own successful transaction');parser.add_argument('--queued-prior-result',choices=('200','488'),default='200');parser.add_argument('--queued-hold-result',choices=('200','488','422','491'),default='200');parser.add_argument('--explicit-mono',action='store_true',help='Reject incompatible stereo AAC explicit mono and verify actual G722 fallback');parser.add_argument('--system-audio',action='store_true',help='Verify real process-loopback worker ON/duplicate ON/OFF acknowledgements');parser.add_argument('--negotiation-only',action='store_true');parser.add_argument('--refresher-token',choices=('canonical','spaced-uac','uas-note-uac'),default='canonical');parser.add_argument('--min-se-retry',action='store_true',help='Reject initial INVITE with422 and verify retry honors Min-SE3600');parser.add_argument('--session-timer',choices=('required','supported'),help='Negotiate a real 90-second session and verify refresh at half interval');parser.add_argument('--idle-seconds',type=bounded_idle_seconds,default=0,help='Quiet interval before BYE, in seconds (0..120; default 0)');parser.add_argument('--split-video-formats',action='store_true',help='Select different H264 packetization modes per main and slides SDP sections and verify actual RTP payloads');run(parser.parse_args())
