"""Pending setup failure, codec-unavailable and listener conflict regressions.

Only explicit loopback calls. Keep logs and isolated codec stage for diagnosis.
"""
import argparse, base64, json, shutil, subprocess, tempfile, time
from pathlib import Path
from LoopbackWorkerTests import Worker, check, encoded, after, catalog

def ready(w):
    w.await_(lambda:any(x.startswith('Listening SIP on ') for x in w.lines) and any(x.startswith('Listening H.323 on ') for x in w.lines))

def run(args):
    workers=[]; passed=False; expected_failures=set()
    try:
        a=Worker(args.worker,15140,17760,45000,False,440); workers.append(a)
        b=Worker(args.worker,15142,17762,45200,True,523); workers.append(b)
        ready(a); ready(b)
        # Failed media Start must return to idle and permit another protocol.
        a.send('audio-devices default '+encoded('missing-speaker-for-regression'))
        a.send('call sip:test@127.0.0.1:15142')
        a.await_(lambda:any(x.startswith('startup error: call-setup-failed') for x in a.lines))
        index=len(a.events); a.send('protocol h323'); a.send('audio-devices default default')
        a.send('call h323:127.0.0.1:17762')
        a.await_(lambda:after(a,index,'opal-connection-health',lambda e:e.get('protocol')=='h323' and e.get('deviceSubmittedNonzeroSamples',0)>1000),40)
        a.send('hangup')
        a.await_(lambda:any(x.startswith('call-cleared ') for x in a.lines))
        b.await_(lambda:any(x.startswith('call-cleared ') for x in b.lines))
        # Incoming device failure must announce the lease and terminal failure.
        b.send('audio-devices default '+encoded('missing-incoming-speaker'))
        line_count=len(b.lines); a.send('protocol sip'); a.send('call sip:test@127.0.0.1:15142')
        b.await_(lambda:any(x.startswith('startup error: incoming-media-failed') for x in b.lines[line_count:]))
        incoming=next(i for i,x in enumerate(b.lines[line_count:]) if x.startswith('incoming call from '))
        failure=next(i for i,x in enumerate(b.lines[line_count:]) if x.startswith('startup error: incoming-media-failed'))
        check(incoming<failure,'Incoming failure did not establish the managed lease')
        b.send('audio-devices default default')
        if args.share_fixture:
            a.await_(lambda:any('DISCONNECTED' in x for x in a.lines))
            before=len(a.events);a.send('call sip:test@127.0.0.1:15142')
            a.await_(lambda:after(a,before,'opal-connection-health',lambda e:e.get('deviceSubmittedNonzeroSamples',0)>1000),40)
            fixture=subprocess.Popen([str(args.share_fixture),'60'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,creationflags=0x08000000)
            try:
                handles=json.loads(fixture.stdout.readline())
                source=next((id for kind,id in catalog(args.worker) if kind==2 and id.startswith('esw1.') and int.from_bytes(base64.urlsafe_b64decode(id[5:]+'='*((-len(id[5:]))%4))[4:12],'little')==handles['selectedHandle']),None)
                check(source is not None,'Owned fast-stop source absent')
                before=len(a.events)
                a.send('desktop on '+encoded(source));a.send('desktop off');a.send('desktop on '+encoded(source));a.send('desktop off')
                a.await_(lambda:after(a,before,'content-sharing-state',lambda e:e.get('detail','').startswith('Sharing stopped')))
                time.sleep(2)
                stopped=max(i for i,e in enumerate(a.events) if i>=before and e.get('type')=='content-sharing-state' and e.get('detail','').startswith('Sharing stopped'))
                check(not after(a,stopped+1,'content-sharing-state',lambda e:e.get('active')),'Late grant restarted stopped sharing')
                check(not after(a,stopped+1,'video-state',lambda e:e.get('desktopActive')),'Late grant started selected capture after stop')
                check(a.process.poll() is None,'Rapid pre-grant stop/repeated request crashed')
                cleared=len(a.lines);a.send('hangup');a.await_(lambda:any('DISCONNECTED' in x for x in a.lines[cleared:]))
            finally:
                fixture.stdin.write('quit\n');fixture.stdin.flush();fixture.wait(5)
                check(fixture.returncode==0,'Owned fast-stop fixture did not exit cleanly')
        high=Worker(args.worker,15150,17970,65136,False,600);workers.append(high);ready(high);high.close()
        for rtp in (65137,65138):
            invalid=Worker(args.worker,15150,17970,rtp,False,600);workers.append(invalid);expected_failures.add(invalid)
            invalid.process.wait(25)
            check(invalid.process.returncode!=0 and any('RTP base leaves no valid TCP control port range' in x for x in invalid.lines),'Unsafe high RTP/control range accepted')
            check(not any(x.startswith('Listening ') for x in invalid.lines),'Invalid range advertised listener readiness')
        # Startup must reject partial listener binding (one failed TCP listener).
        conflict=Worker(args.worker,15140,17960,45600,False,600); workers.append(conflict);expected_failures.add(conflict)
        conflict.process.wait(25)
        check(conflict.process.returncode!=0,'Conflicting SIP port reported ready')
        check(not any(x.startswith('Listening SIP on ') for x in conflict.lines),'Partial SIP listener bind claimed readiness')
        # Isolated plugin stage proves AAC disabled availability and rejection.
        stage=Path(tempfile.mkdtemp(prefix='MyStudio-opal-without-aac-'))
        shutil.copytree(args.worker.parent,stage,dirs_exist_ok=True,ignore=lambda path,names:['mystudio_opal_aac.dll'] if 'mystudio_opal_aac.dll' in names else [])
        missing=Worker(stage/args.worker.name,15150,17970,45800,False,660); workers.append(missing)
        ready(missing)
        check(missing.event('aac-availability',lambda e:e.get('available') is False),'AAC missing DLL advertised available')
        missing.send('aac-enabled on')
        missing.await_(lambda:missing.event('media-error',lambda e:e.get('code')=='aac-change-rejected'))
        passed=True
    finally:
        for index,w in enumerate(workers):
            w.close()
            if args.logs:
                args.logs.mkdir(parents=True,exist_ok=True)
                (args.logs/f'worker-failure-{index}.log').write_text('\n'.join(w.lines),encoding='utf-8')
        if passed:
            for w in workers:
                if w not in expected_failures:
                    check(w.process.returncode==0,'Failure regression worker did not exit cleanly: '+str(w.process.returncode))
            print('OPAL worker failures PASS with clean exits; isolated stage '+str(stage))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--worker',type=Path,required=True);p.add_argument('--logs',type=Path);p.add_argument('--share-fixture',type=Path)
    run(p.parse_args())
