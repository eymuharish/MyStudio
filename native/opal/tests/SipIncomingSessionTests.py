"""Owned localhost SIP UAS session timers; real RFC4028 minimum, no devices."""
import argparse,json,os,socket,struct,subprocess,threading,time,uuid
from pathlib import Path

def run(args):
    probe=socket.socket();probe.bind(('127.0.0.1',0));sip_port=probe.getsockname()[1]
    # The worker listens on TCP and UDP. Keep both namespaces reserved while
    # allocating our peer sockets; a TCP-only probe can collide with peer RTP.
    sip_udp_probe=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    while True:
        try:sip_udp_probe.bind(('127.0.0.1',sip_port));break
        except OSError:
            probe.close();probe=socket.socket();probe.bind(('127.0.0.1',0));sip_port=probe.getsockname()[1]
    h323_probe=socket.socket();h323_probe.bind(('127.0.0.1',0));h323_port=h323_probe.getsockname()[1]
    media=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);media.bind(('127.0.0.1',0));media.settimeout(.2)
    rtcp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);rtcp.bind(('127.0.0.1',0));rtcp.settimeout(.2)
    stop=threading.Event();media_packets=[];records=[];lines=[];errors=[]
    def reflect():
        try:
            while not stop.is_set():
                try:packet,address=media.recvfrom(65536)
                except socket.timeout:continue
                if address[0]!='127.0.0.1':raise AssertionError('RTP was not localhost')
                if len(packet)<12 or packet[0]>>6!=2:raise AssertionError('Invalid RTP')
                reflected=bytearray(packet);ssrc=struct.unpack_from('!I',packet,8)[0]^0xA5A5A5A5;struct.pack_into('!I',reflected,8,ssrc or 1)
                media.sendto(reflected,address);media_packets.append(time.monotonic())
        except Exception as error:
            if not stop.is_set():errors.append(str(error))
    threading.Thread(target=reflect,daemon=True).start()
    env=os.environ.copy();env.update(ESSASTUDIO_TEST_MODE='1',ESSASTUDIO_TEST_TONE_HZ='440',ESSASTUDIO_SIP_TRANSPORT='tcp',ESSASTUDIO_DIAGNOSTIC_LOG='1',ESSASTUDIO_SIP_SRTP='disabled',ESSASTUDIO_MICROPHONE_ID='',ESSASTUDIO_SPEAKER_ID='')
    def drain(stream):
        for line in stream:lines.append(line.rstrip())
    conn=None;failure=None;buffer=b'';start=time.monotonic();call_id=str(uuid.uuid4());to='<sip:studio@127.0.0.1:'+str(sip_port)+'>';contact=to;session_interval=60 if args.short_interval else 90
    body=('v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Owned incoming timer\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n'
          f'm=audio {media.getsockname()[1]} RTP/AVP 9\r\na=rtpmap:9 G722/8000\r\na=sendrecv\r\na=rtcp:{rtcp.getsockname()[1]} IN IP4 127.0.0.1\r\n')
    video_sockets=[]
    if args.offerless_reinvite:
        for index,role in ((2,'main'),(3,'slides')):
            video=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);video.bind(('127.0.0.1',0));video_sockets.append(video)
            body+=f'm=video {video.getsockname()[1]} RTP/AVP 98\r\na=rtpmap:98 H264/90000\r\na=fmtp:98 packetization-mode=1\r\na=sendrecv\r\na=content:{role}\r\na=label:{index}\r\n'
        # A disabled control description still occupies its negotiated m-line.
        body+='m=application 0 TCP/BFCP *\r\na=setup:actpass\r\na=connection:new\r\na=floorctrl:s-only\r\na=confid:1\r\na=userid:2\r\na=floorid:2 mstrm:3\r\n'
    peer_ports={media.getsockname()[1],rtcp.getsockname()[1],*(s.getsockname()[1] for s in video_sockets)}
    if sip_port in peer_ports or sip_port==h323_port:raise AssertionError('Worker and owned peer socket reservations overlap')
    probe.close();sip_udp_probe.close();h323_probe.close()
    process=subprocess.Popen([str(args.worker),'--sip-port',str(sip_port),'--h323-port',str(h323_port),'--rtp-port',str(args.rtp_port),'--interface','127.0.0.1','--test-loopback-autoanswer'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env,creationflags=0x08000000)
    for stream in (process.stdout,process.stderr):threading.Thread(target=drain,args=(stream,),daemon=True).start()
    def request(method,cseq,content=None):
        peer_port=conn.getsockname()[1]
        uri=contact.split('<',1)[1].split('>',1)[0] if '<' in contact else contact
        branch_method='INVITE' if method=='ACK' and session_interval<90 else method
        message=f'{method} {uri} SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1:{peer_port};branch=z9hG4bK-owned-{cseq}-{branch_method}\r\nMax-Forwards: 70\r\nFrom: <sip:owned@127.0.0.1>;tag=owned-incoming\r\nTo: {to}\r\nCall-ID: {call_id}\r\nCSeq: {cseq} {method}\r\nContact: <sip:owned@127.0.0.1:{peer_port};transport=tcp>\r\n'
        if content is None:content=body if method=='INVITE' else ''
        parameter='REFRESHER = '+args.refresher.upper() if args.unusual_refresher else 'refresher='+args.refresher
        if content:
            if method=='INVITE':message+=f'Supported: timer\r\nSession-Expires: {session_interval};{parameter}\r\n'
            message+='Content-Type: application/sdp\r\n'
        conn.sendall((message+f'Content-Length: {len(content)}\r\n\r\n'+content).encode())
    def receive(timeout):
        nonlocal buffer
        conn.settimeout(timeout)
        while b'\r\n\r\n' not in buffer:
            part=conn.recv(65536)
            if not part:raise AssertionError('Incoming TCP dialog closed')
            buffer+=part
        head,buffer=buffer.split(b'\r\n\r\n',1);parts=head.decode().split('\r\n');fields={}
        for line in parts[1:]:
            if ':' in line:
                key,value=line.split(':',1);fields.setdefault(key.lower(),[]).append(value.strip())
        count=int(fields.get('content-length',['0'])[0])
        while len(buffer)<count:buffer+=conn.recv(65536)
        content,buffer=buffer[:count],buffer[count:]
        record={'line':parts[0],'fields':fields,'receivedMonotonic':time.monotonic(),'sdp':content.decode()};records.append(record)
        return record,content
    def answer_invite(record):
        fields=record['fields']
        message='SIP/2.0 200 OK\r\n'+''.join('Via: '+v+'\r\n' for v in fields['via'])+''.join(key+': '+fields[key.lower()][0]+'\r\n' for key in ('From','To','Call-ID','CSeq'))
        message+=f'Contact: <sip:owned@127.0.0.1:{conn.getsockname()[1]};transport=tcp>\r\nSupported: timer\r\nSession-Expires: 90;refresher=uac\r\nContent-Type: application/sdp\r\nContent-Length: {len(body)}\r\n\r\n'+body
        conn.sendall(message.encode())
    def expect_success(cseq):
        nonlocal to,contact
        end=time.monotonic()+12
        while time.monotonic()<end:
            record,_=receive(end-time.monotonic());fields=record['fields']
            if record['line'].startswith('SIP/2.0 200 ') and fields.get('cseq',[''])[0]==str(cseq)+' INVITE':
                if fields.get('session-expires',[''])[0]!=f'90;refresher={args.refresher}':raise AssertionError('Incoming 200 failed to negotiate Session-Expires/refresher: '+str(fields.get('session-expires')))
                to=fields['to'][0];contact=fields['contact'][0];request('ACK',cseq);return record
            if record['line'].startswith('SIP/2.0 4') and fields.get('cseq',[''])[0]==str(cseq)+' INVITE':raise AssertionError('Incoming INVITE rejected: '+record['line'])
        raise AssertionError('Incoming INVITE200 missing')
    try:
        end=time.monotonic()+20
        while (not any(x.startswith('Listening H.323 on ') for x in lines) or sum(x.startswith('{') and json.loads(x).get('type')=='aac-availability' for x in lines)<2) and time.monotonic()<end:
            if process.poll() is not None:raise AssertionError('Worker failed startup: '+str(process.returncode))
            time.sleep(.05)
        configured=len(lines);process.stdin.write('aac-enabled off\n');process.stdin.flush();end=time.monotonic()+8
        while not any(x.startswith('{') and json.loads(x).get('type')=='aac-availability' for x in lines[configured:]):
            if time.monotonic()>=end:raise AssertionError('Idle AAC configuration acknowledgement timed out')
            time.sleep(.025)
        conn=socket.create_connection(('127.0.0.1',sip_port),timeout=10);request('INVITE',1)
        first_cseq=1
        if args.short_interval:
            rejected,_=receive(8)
            while rejected['line'].startswith('SIP/2.0 1'):rejected,_=receive(8)
            if not rejected['line'].startswith('SIP/2.0 422 ') or rejected['fields'].get('min-se',[''])[0]!='90':raise AssertionError('UAS did not reject sub90-second interval with422/Min-SE90')
            original_to=to;to=rejected['fields']['to'][0];request('ACK',1);to=original_to;session_interval=90;first_cseq=2;request('INVITE',first_cseq)
        initial=expect_success(first_cseq);start=initial['receivedMonotonic']
        if args.short_interval and any(r['line'].startswith('SIP/') and r['fields'].get('cseq',[''])[0]=='1 ACK' for r in records):raise AssertionError('UAS sent a forbidden SIP response to non2xx ACK')
        if args.offerless_reinvite:
            def sessions(sdp):
                return [tuple(line[2:].split()[:2]) for line in sdp.splitlines() if line.startswith('m=')]
            negotiated=sessions(initial['sdp'])
            if [s[0] for s in negotiated]!=['audio','video','video','application'] or negotiated[-1][1]!='0':raise AssertionError('Initial offerless fixture lost the main/slides/disabled BFCP descriptions')
            # RFC3261: offer in200, answer inACK. Existing m-line identities/ports
            # must survive an offerless refresh instead of appending new sessions.
            for cseq in range(first_cseq+1,first_cseq+4):
                before=len(media_packets);request('INVITE',cseq,'');end=time.monotonic()+12
                while True:
                    record,offer=receive(max(.1,end-time.monotonic()))
                    if record['line'].startswith('SIP/2.0 200 ') and record['fields'].get('cseq',[''])[0]==str(cseq)+' INVITE':break
                    if record['line'].startswith('SIP/2.0 4') or time.monotonic()>=end:raise AssertionError('Offerless re-INVITE was rejected: '+record['line'])
                if sessions(offer.decode())!=negotiated:raise AssertionError('Offerless re-INVITE changed/appended negotiated sessions: '+str(sessions(offer.decode())))
                # Accept the offered RTP profile while retaining our owned ports.
                transports=[line.split()[2] for line in offer.decode().splitlines() if line.startswith('m=')]
                answer_lines=body.splitlines();index=0
                for position,line in enumerate(answer_lines):
                    if line.startswith('m='):
                        fields=line.split();fields[2]=transports[index];answer_lines[position]=' '.join(fields);index+=1
                request('ACK',cseq,'\r\n'.join(answer_lines)+'\r\n')
                end=time.monotonic()+2
                while len(media_packets)<before+10 and time.monotonic()<end:time.sleep(.05)
                if len(media_packets)<before+10:raise AssertionError('RTP stopped after offerless re-INVITE answer inACK')
        if args.short_interval or args.handshake_only:
            end=time.monotonic()+3
            while len(media_packets)<10 and time.monotonic()<end:time.sleep(.05)
        elif args.refresher=='uac':
            # The owned caller refreshes twice; media must outlive original expiry.
            for cseq in (2,3):
                while time.monotonic()<start+45*(cseq-1):
                    if process.poll() is not None:raise AssertionError('Worker exited before remote refresh')
                    time.sleep(.05)
                before=len(media_packets);request('INVITE',cseq);expect_success(cseq)
                end=time.monotonic()+2
                while len(media_packets)<before+10 and time.monotonic()<end:time.sleep(.05)
                if len(media_packets)<before+10:raise AssertionError('RTP stopped after remote refresh')
        else:
            for index in (1,2):
                end=start+49*index
                while time.monotonic()<end:
                    record,_=receive(end-time.monotonic())
                    if record['line'].startswith('BYE '):raise AssertionError('Worker expired an active incoming session')
                    if record['line'].startswith('INVITE '):
                        elapsed=record['receivedMonotonic']-start
                        if not 44*index<=elapsed<=49*index:raise AssertionError('Incoming local refresh timing: '+str(elapsed))
                        if record['fields'].get('session-expires',[''])[0]!='90;refresher=uac':raise AssertionError('Local refresh did not preserve interval/refresher role')
                        answer_invite(record);break
                else:raise AssertionError('Incoming local refresh missing')
        if errors:raise AssertionError(errors[0])
        if len(media_packets)<(10 if args.short_interval or args.handshake_only else 200) or time.monotonic()-media_packets[-1]>1:raise AssertionError('RTP did not sustain incoming session')
        process.stdin.write('hangup\n');process.stdin.flush();record,_=receive(8)
        while not record['line'].startswith('BYE '):record,_=receive(8)
        fields=record['fields'];response='SIP/2.0 200 OK\r\n'+''.join('Via: '+v+'\r\n' for v in fields['via'])+''.join(key+': '+fields[key.lower()][0]+'\r\n' for key in ('From','To','Call-ID','CSeq'))+'Content-Length: 0\r\n\r\n';conn.sendall(response.encode())
        print('SIP incoming session timer '+args.refresher+' repeated refresh/RTP PASS')
    except Exception as error:failure=error
    finally:
        if process.poll() is None:
            try:process.stdin.write('quit\n');process.stdin.flush();process.wait(12)
            except (BrokenPipeError,subprocess.TimeoutExpired):process.kill();process.wait(5)
        stop.set();media.close();rtcp.close()
        for video in video_sockets:video.close()
        if conn:conn.close()
        if process.returncode!=0 and failure is None:failure=AssertionError('Worker unclean exit '+str(process.returncode))
        if args.logs:
            args.logs.mkdir(parents=True,exist_ok=True);(args.logs/'sip-incoming-timer.log').write_text('\n'.join(lines));(args.logs/'sip-incoming-timer.json').write_text(json.dumps({'refresher':args.refresher,'records':records,'rtpPackets':len(media_packets),'elapsedSeconds':time.monotonic()-start,'workerExit':process.returncode,'result':'FAIL' if failure else 'PASS','failure':str(failure) if failure else None},indent=2))
    if failure:raise failure

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--worker',type=Path,required=True);parser.add_argument('--logs',type=Path);parser.add_argument('--offerless-reinvite',action='store_true',help='Check three actual offerless re-INVITE200/SDP-answer-inACK cycles retain negotiated sessions');parser.add_argument('--handshake-only',action='store_true');parser.add_argument('--unusual-refresher',action='store_true',help='Use case-insensitive refresher parameter with valid whitespace');parser.add_argument('--short-interval',action='store_true',help='Check422/Min-SE90 rejection before a valid retry');parser.add_argument('--refresher',choices=('uac','uas'),required=True);parser.add_argument('--rtp-port',type=int,default=47000);run(parser.parse_args())
