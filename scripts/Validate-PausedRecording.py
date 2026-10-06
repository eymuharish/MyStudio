import array,json,math,pathlib,re,subprocess,sys
import imageio_ffmpeg
root=pathlib.Path(sys.argv[1]); proof=json.loads((root/'sidebar-recording-proof.json').read_text(encoding='utf-8-sig')); path=proof['recordingPath']; exe=imageio_ffmpeg.get_ffmpeg_exe()
def run(*args):return subprocess.run([exe,'-v','error','-i',path,*args],capture_output=True,timeout=45)
a=run('-map','0:a:0','-vn','-ac','1','-ar','48000','-f','s16le','-'); v=run('-map','0:v:0','-an','-vf','scale=128:72','-pix_fmt','gray','-f','rawvideo','-')
samples=array.array('h'); samples.frombytes(a.stdout)
def amp(data,f):return 2*abs(sum(x*complex(math.cos(2*math.pi*f*n/48000),math.sin(2*math.pi*f*n/48000)) for n,x in enumerate(data)))/len(data)/32768
blocks=[samples[i:i+24000] for i in range(0,len(samples)-24000+1,24000)]; tones=[amp(b,440) for b in blocks]; neighbors=[max(amp(b,370),amp(b,510)) for b in blocks]
rms=math.sqrt(sum((x/32768)**2 for x in samples)/len(samples)); frames=[v.stdout[i:i+9216] for i in range(0,len(v.stdout)-9216+1,9216)]; diffs=[sum(abs(x-y) for x,y in zip(a,b))/9216 for a,b in zip(frames,frames[1:])]
ts=subprocess.run([exe,'-v','info','-i',path,'-map','0:v:0','-an','-vf','showinfo','-f','null','-'],capture_output=True,timeout=45); pts=[float(x) for x in re.findall(r'pts_time:([0-9.]+)',ts.stderr.decode(errors='replace'))]; gaps=[b-a for a,b in zip(pts,pts[1:])]
passed=a.returncode==v.returncode==ts.returncode==0 and rms>.001 and len(frames)>20 and sum(d>.05 for d in diffs)>5 and len(samples)/48000<6 and max(gaps,default=999)<.5 and min(tones[:2])>.005 and min(tones[-2:])>.005 and max(tones)>max(neighbors)*3
report=dict(Pass=passed,recording=path,audioDecodeExit=a.returncode,videoDecodeExit=v.returncode,audioSeconds=len(samples)/48000,audioRms=rms,tone440HzHalfSecondBlocks=tones,neighborAmplitudeHalfSecondBlocks=neighbors,videoFrames=len(frames),movingFrameTransitions=sum(d>.05 for d in diffs),maximumFrameDifference=max(diffs,default=0),videoFirstPts=min(pts,default=0),videoLastPts=max(pts,default=0),maximumVideoPtsGap=max(gaps,default=0),omittedWallPauseSeconds=proof['excludedPauseWallMs']/1000)
(root/'paused-recording-decode.json').write_text(json.dumps(report,indent=2),encoding='utf-8');(root/'paused-recording-decode.log').write_bytes(a.stderr+v.stderr+ts.stderr);print(json.dumps(report,indent=2));sys.exit(0 if passed else 1)
