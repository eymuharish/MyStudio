"""Authorized five-minute outbound camera test with read-only peer media status.

Transport counters prove traffic, not audible playback or peer rendering.
"""
import argparse, base64, ctypes, json, os, pathlib, queue, struct, subprocess, threading, time
import tkinter as tk
import urllib.request, urllib.error, xml.etree.ElementTree as ET
from html.parser import HTMLParser
from datetime import datetime, timezone

def b64(data):
    return base64.urlsafe_b64encode(data).decode().rstrip('=')

def window_id(hwnd, title):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    user.GetAncestor.restype = ctypes.c_void_p
    hwnd = user.GetAncestor(hwnd, 2)
    process_id = ctypes.c_ulong()
    user.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(process_id))
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_bool, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    handle = kernel.OpenProcess(0x1000, False, process_id.value)
    ticks = [ctypes.c_ulonglong() for _ in range(4)]
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
    if not kernel.GetProcessTimes(handle, *[ctypes.byref(v) for v in ticks]):
        raise ctypes.WinError(ctypes.get_last_error())
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle(handle)
    value = 1469598103934665603
    for ch in title:
        value = ((value ^ ord(ch)) * 1099511628211) & ((1 << 64) - 1)
    return 'esw1.' + b64(b'ESW\x01' + struct.pack('<QIQQ', hwnd, process_id.value, ticks[0].value, value))

class PageText(HTMLParser):
    def __init__(self):
        super().__init__(); self.parts = []
    def handle_data(self, text):
        if text.strip(): self.parts.append(text.strip())

def endpoint_sample(args):
    user = os.environ.get('MYSTUDIO_DEVICE_USER', '')
    password = os.environ.get('MYSTUDIO_DEVICE_PASSWORD', '')
    if not user: return {'error': 'Device credentials are not supplied'}
    route = '/status.xml' if args.model == 'SX20' else '/cgi-bin/qcs_webcgi.cgi?cgipage=cgi_term&kind=0'
    manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    manager.add_password(None, 'http://' + args.target, user, password)
    opener = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(manager), urllib.request.HTTPBasicAuthHandler(manager))
    request = urllib.request.Request('http://' + args.target + route)
    try:
        with opener.open(request, timeout=5) as response: raw = response.read()
        if args.model == 'SX20':
            tree = ET.fromstring(raw)
            nodes = tree.findall('./{*}MediaChannels') + tree.findall('./{*}Call')
            return {'timeUtc': datetime.now(timezone.utc).isoformat(), 'mediaXml': [ET.tostring(n, encoding='unicode') for n in nodes]}
        text = PageText(); text.feed(raw.decode('utf-8', 'replace'))
        return {'timeUtc': datetime.now(timezone.utc).isoformat(), 'communicationStatus': ' '.join(text.parts)}
    except Exception as error: return {'error': type(error).__name__ + ': ' + str(error)}

def run_call(args, capture_id, finished):
    evidence = pathlib.Path(args.evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    rows = queue.Queue()
    started = time.monotonic()
    rejected_commands = []
    process = None
    connected = None
    cutoff_text = os.environ.get("ESSASTUDIO_DEVICE_TEST_CUTOFF_UTC", "2026-10-04T03:55:00Z")
    cutoff = None
    if cutoff_text:
        parsed = datetime.fromisoformat(cutoff_text.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            raise ValueError("Device cutoff must include UTC timezone")
        cutoff = parsed.timestamp() - 30 # Hang up before the user hard cutoff.
    result = {'device': args.model, 'address': args.target, 'network': 'VPN',
              'profile': args.profile, 'requiredSeconds': args.seconds, 'connectedSeconds': 0, 'content': [], 'endpointSamples': [], 'videoStates': [],
              'audibilityVerified': False, 'peerMovingVideoVerified': False}
    with (evidence / f'outbound-{args.model}-vpn.log').open('w', encoding='utf-8') as log:
        lock = threading.Lock()
        def record(kind, text):
            with lock:
                log.write(f'{time.monotonic()-started:.3f} {kind} {text}\n')
                log.flush()
        def reader(pipe, kind):
            for line in pipe:
                line = line.strip()
                record(kind, line)
                if 'command rejected' in line.lower(): rejected_commands.append(line)
                rows.put(line)
        try:
            if cutoff is not None and time.time() + args.seconds + 48 >= cutoff:
                raise RuntimeError('Insufficient time before authorized device-call cutoff')
            env = dict(os.environ, ESSASTUDIO_H323_TRACE=str(args.trace))
            env.pop('MYSTUDIO_DEVICE_USER', None); env.pop('MYSTUDIO_DEVICE_PASSWORD', None)
            worker = pathlib.Path(args.exe).resolve(strict=True)
            env['PTLIBPLUGINDIR'] = str(worker.parent)
            process = subprocess.Popen([str(worker), '--listen', '--interface', args.interface,
                '--port', str(args.port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace', env=env, cwd=worker.parent)
            threads = [threading.Thread(target=reader, args=(pipe, name), daemon=True)
                for pipe, name in [(process.stdout, 'OUT'), (process.stderr, 'ERR')]]
            for thread in threads: thread.start()
            def send(command):
                record('CMD', command)
                process.stdin.write(command + '\n'); process.stdin.flush()
            time.sleep(2)
            if process.poll() is not None:
                raise RuntimeError(f'Worker startup failed exit={process.returncode}')
            send(f'endpoint {args.profile}')
            send('audio-profile g722 1 64 100 300 100 40 0')
            send('network-mode external')
            time.sleep(1)
            if rejected_commands: raise RuntimeError('Native preflight command rejected: ' + rejected_commands[0])
            send(f'call {args.target}')
            connected = None
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if cutoff is not None and time.time() >= cutoff:
                    raise RuntimeError('Authorized device-call cutoff reached')
                try: line = rows.get(timeout=0.5)
                except queue.Empty:
                    if process.poll() is not None: break
                    continue
                if line.startswith('call-established '):
                    connected = time.monotonic(); send('camera on'); break
                if line.startswith('call-cleared '): break
            if connected is None:
                raise RuntimeError('Call did not reach connected state within 45 seconds')
            print(f'{args.model} VPN H323 connected; validating {args.seconds} seconds', flush=True)
            next_toggle = connected + 5
            next_sample = connected + 10
            toggles = 0
            sharing = False
            while time.monotonic() - connected < args.seconds:
                if cutoff is not None and time.time() >= cutoff:
                    send('hangup')
                    raise RuntimeError('Authorized device-call cutoff reached')
                if process.poll() is not None: raise RuntimeError('Worker exited during call')
                now = time.monotonic()
                if now >= next_sample:
                    result['endpointSamples'].append(endpoint_sample(args)); next_sample = now + 15
                if False and toggles < 20 and now >= next_toggle:
                    sharing = not sharing
                    send('desktop on ' + b64(capture_id.encode()) if sharing else 'desktop off')
                    toggles += 1; next_toggle = now + 10
                try: line = rows.get(timeout=0.25)
                except queue.Empty: continue
                if line.startswith('{'):
                    try: event = json.loads(line)
                    except json.JSONDecodeError: continue
                    if event.get('type') == 'video-state': result['videoStates'].append(event)
                    if event.get('type') == 'content-sharing-state': result['content'].append(event)
                    if event.get('type') == 'transport-stats': result['lastTransportStats'] = event
                if line.startswith('call-cleared '):
                    raise RuntimeError('Call ended before five-minute acceptance duration')
            result['connectedSeconds'] = round(time.monotonic() - connected, 2)
            result['sharingToggleCommands'] = toggles
            result['durationPass'] = result['connectedSeconds'] >= args.seconds
            send('camera off'); send('desktop off'); send('hangup'); time.sleep(3); send('quit')
        except Exception as error:
            if connected is not None:
                result['connectedSeconds'] = round(time.monotonic() - connected, 2)
            result['error'] = str(error)
            record('TEST-ERROR', str(error))
        finally:
            if process and process.poll() is None:
                try:
                    process.stdin.write('hangup\nquit\n'); process.stdin.flush()
                    process.wait(timeout=10)
                except Exception: process.kill(); process.wait(timeout=5)
            result['workerExit'] = process.returncode if process else None
            (evidence / f'outbound-{args.model}-vpn.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
            print(json.dumps({k:v for k,v in result.items() if k not in ['endpointSamples','videoStates','content']}), flush=True)
            finished.set()

def main():
    parser = argparse.ArgumentParser()
    for name in ['exe', 'target', 'model', 'profile', 'interface', 'evidence']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--port', type=int, default=1722)
    parser.add_argument('--trace', type=int, choices=[0,3,4], default=3)
    args = parser.parse_args()
    if args.seconds < 300: parser.error('Five-minute device validation required')
    if args.profile not in ['pcs-xg77','pcs-xg80','sx20']: parser.error('Use the native endpoint profile token')
    card = tk.Tk(); title = 'MyStudio - authorized outbound video test'
    card.title(title); card.geometry('640x360+40+40'); card.configure(bg='black')
    canvas = tk.Canvas(card, bg='#102030', highlightthickness=0); canvas.pack(fill='both', expand=True)
    counter = 0; done = threading.Event()
    def animate():
        nonlocal counter
        if done.is_set(): card.destroy(); return
        counter += 1; canvas.delete('all')
        canvas.create_text(320, 80, text='MYSTUDIO OUTBOUND VIDEO TEST', fill='white', font=('Arial', 24))
        canvas.create_text(320, 130, text=f'{args.model} - VPN - frame {counter}', fill='#40e0ff', font=('Arial', 18))
        x = (counter * 8) % 570
        canvas.create_rectangle(x, 200, x+60, 270, fill='#ffbb33')
        card.after(100, animate)
    card.update()
    capture_id = window_id(card.winfo_id(), title)
    worker = threading.Thread(target=run_call, args=(args, capture_id, done), daemon=True)
    worker.start(); animate(); card.mainloop(); worker.join(timeout=15)

if __name__ == '__main__': main()
