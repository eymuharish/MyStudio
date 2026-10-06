"""Actual owned SIP/H323 lifecycle markers stay on serialized stdout under PTrace."""
import argparse, json, os
from pathlib import Path
from LoopbackWorkerTests import Worker, check

def run(args):
    os.environ['ESSASTUDIO_DIAGNOSTIC_LOG']='1'
    workers=[]; passed=False; result=[]
    try:
        a=Worker(args.worker,15140,17760,45000,False,440);workers.append(a)
        b=Worker(args.worker,15142,17762,45200,True,523);workers.append(b)
        for w in workers:
            w.await_(lambda:any(x.startswith('Listening SIP on ') for x in w.lines) and any(x.startswith('Listening H.323 on ') for x in w.lines))
        for protocol in ('h323','sip'):
            begins=[len(w.lines) for w in workers];outputs=[len(w.stdout_lines) for w in workers]
            a.send('protocol '+protocol);a.send('aac-enabled off')
            a.send('call '+('h323:127.0.0.1:17762' if protocol=='h323' else 'sip:test@127.0.0.1:15142'))
            established='call-established remote=' if protocol=='h323' else 'call 1: CONFIRMED ('
            cleared='call-cleared remote=' if protocol=='h323' else 'call 1: DISCONNECTED ('
            for w,begin in zip(workers,begins):
                w.await_(lambda:any(established in x for x in w.lines[begin:]))
                w.await_(lambda:w.event('opal-connection-health',lambda e:e.get('protocol')==protocol and e.get('decodedNonzeroSamples',0)>1000))
            a.send('hangup')
            for w,begin,output in zip(workers,begins,outputs):
                w.await_(lambda:any(cleared in x for x in w.lines[begin:]))
                markers=[x for x in w.stdout_lines[output:] if x.startswith(established) or x.startswith(cleared)]
                check(len(markers)==2,'Diagnostic lifecycle markers must be standalone stdout lines: '+str(markers))
                check(any('PTrace' in x or '\t' in x for x in w.stderr_lines),'PTrace diagnostic traffic was not exercised')
                result.append({'protocol':protocol,'markers':markers,'diagnosticsOnStderr':True})
        passed=True
    finally:
        for index,w in enumerate(workers):
            w.close();args.logs.mkdir(parents=True,exist_ok=True)
            (args.logs/f'marker-{index}.stdout.log').write_text('\n'.join(w.stdout_lines),encoding='utf-8')
            (args.logs/f'marker-{index}.stderr.log').write_text('\n'.join(w.stderr_lines),encoding='utf-8')
            if passed:check(w.process.returncode==0,'Diagnostic marker worker did not close cleanly')
    (args.logs/'summary.json').write_text(json.dumps({'result':'PASS','calls':result,'bothWorkerExit':0},indent=2))
    print('Diagnostic SIP/H323 markers serialized on stdout; actual decoded calls and clean exits PASS')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--worker',type=Path,required=True);p.add_argument('--logs',type=Path,required=True);run(p.parse_args())
