"""Prove native readiness and actual codec construction at the relocated path."""
import argparse, json, os, subprocess
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--worker',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--sip-port',type=int,default=25142)
parser.add_argument('--h323-port',type=int,default=21762)
parser.add_argument('--rtp-port',type=int,default=53004)
args=parser.parse_args()
environment=os.environ.copy(); environment['PATH']=str(Path(os.environ['SystemRoot'])/'System32')
environment['ESSASTUDIO_SIP_TRANSPORT']='udp'; environment['ESSASTUDIO_SIP_SRTP']='disabled'
environment['HTTP_PROXY']='http://127.0.0.1:1'; environment['HTTPS_PROXY']='http://127.0.0.1:1'
for name in ('ESSASTUDIO_SIP_REGISTRAR','ESSASTUDIO_SIP_IDENTITY','ESSASTUDIO_SIP_USERNAME','ESSASTUDIO_SIP_PASSWORD','ESSASTUDIO_SIP_OUTBOUND_PROXY'):
    environment.pop(name,None)
run=subprocess.run([str(args.worker.resolve()),'--sip-port',str(args.sip_port),'--h323-port',str(args.h323_port),'--rtp-port',str(args.rtp_port)],
    cwd=args.worker.resolve().parent,env=environment,input='quit\n',text=True,encoding='utf-8',capture_output=True,timeout=20)
events=[json.loads(line) for line in run.stdout.splitlines() if line.startswith('{')]
ready=next((event for event in events if event.get('type')=='engine-ready'),{})
aac=[event for event in events if event.get('type')=='aac-availability']
passed=run.returncode==0 and ready.get('codePage')==65001 and len(aac)==2 and all(event.get('available') for event in aac)
report={'result':'PASS' if passed else 'FAIL','exitCode':run.returncode,'codePage':ready.get('codePage'),
    'g722EncoderConstructed':bool(ready),'aacEncoderDecoderProfilesConstructed':len(aac)==2 and all(event.get('available') for event in aac),
    'worker':str(args.worker.resolve()),'events':events,'stderr':run.stderr}
args.output.write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
print('PORTABLE_NATIVE_CODECS='+report['result'])
if not passed: raise SystemExit('Native codec or UTF-8 code-page proof failed; see evidence')
