"""Owned localhost SIP flow: genuine EOF and a partial first-byte timeout."""
import argparse
import json
import os
import socket
import subprocess
import threading
import time
from pathlib import Path


def run_case(args, case):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(20)
    closed = socket.socket()
    closed.bind(('127.0.0.1', 0))  # Bound but never listening: invalid Contact.
    media = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    media.bind(('127.0.0.1', 0))
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control.bind(('127.0.0.1', 0))
    ack, inject, finished = threading.Event(), threading.Event(), threading.Event()
    records, peer_errors, lines = [], [], []
    evidence = {'case': case, 'acceptedTcpFlows': [], 'closedContactPort': closed.getsockname()[1]}

    def peer():
        try:
            conn, address = listener.accept()
            evidence['acceptedTcpFlows'].append({'local': conn.getsockname(), 'peer': address})
            with conn:
                conn.settimeout(.1)
                buffer = b''
                while not inject.is_set():
                    try:
                        data = conn.recv(65536)
                    except socket.timeout:
                        continue
                    if not data:
                        raise EOFError('Owned flow closed before negative injection')
                    buffer += data
                    while b'\r\n\r\n' in buffer:
                        header, body = buffer.split(b'\r\n\r\n', 1)
                        parts = header.decode().split('\r\n')
                        fields = {}
                        for line in parts[1:]:
                            if ':' in line:
                                key, value = line.split(':', 1)
                                fields.setdefault(key.lower(), []).append(value.strip())
                        count = int(fields.get('content-length', ['0'])[0])
                        if len(body) < count:
                            break
                        buffer = body[count:]
                        method = parts[0].split()[0]
                        records.append({'method': method, 'cseq': fields.get('cseq', [''])[0],
                                        'receivedMonotonic': time.monotonic(), 'tcpFlow': 1})
                        if method == 'ACK':
                            ack.set()
                            continue
                        if method not in ('INVITE', 'OPTIONS', 'INFO', 'BYE'):
                            raise AssertionError('Unexpected owned-peer method ' + method)
                        to = fields['to'][0]
                        if ';tag=' not in to:
                            to += ';tag=owned-read-failure'
                        answer = ''
                        if method == 'INVITE':
                            answer = ('v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Owned read regression\r\n'
                                      'c=IN IP4 127.0.0.1\r\nt=0 0\r\n'
                                      f'm=audio {media.getsockname()[1]} RTP/AVP 9\r\n'
                                      f'a=rtcp:{control.getsockname()[1]} IN IP4 127.0.0.1\r\n'
                                      'a=sendrecv\r\na=rtpmap:9 G722/8000\r\n')
                        response = ('SIP/2.0 200 OK\r\nAllow: INVITE,ACK,INFO,BYE,OPTIONS\r\n'
                                    + ''.join('Via: ' + value + '\r\n' for value in fields['via'])
                                    + 'From: ' + fields['from'][0] + '\r\nTo: ' + to
                                    + '\r\nCall-ID: ' + fields['call-id'][0]
                                    + '\r\nCSeq: ' + fields['cseq'][0]
                                    + f'\r\nContact: <sip:owned@127.0.0.1:{closed.getsockname()[1]};transport=tcp>\r\n'
                                    + ('Content-Type: application/sdp\r\n' if answer else '')
                                    + f'Content-Length: {len(answer)}\r\n\r\n' + answer)
                        conn.sendall(response.encode())
                        records[-1]['response'] = 200
                evidence['injectedMonotonic'] = time.monotonic()
                if case == 'eof':
                    conn.shutdown(socket.SHUT_WR)  # Real read EOF; retain read side to observe SDK close.
                else:
                    conn.sendall(b'S')  # First PDU byte, incomplete start line/header, no CRLF.
                conn.settimeout(20)
                while conn.recv(65536):
                    pass
                evidence['peerSawCloseMonotonic'] = time.monotonic()
                evidence['closeElapsedSeconds'] = evidence['peerSawCloseMonotonic'] - evidence['injectedMonotonic']
        except Exception as error:
            peer_errors.append(str(error))
        finally:
            finished.set()

    peer_thread = threading.Thread(target=peer, daemon=True)
    peer_thread.start()
    env = os.environ.copy()
    env.update(ESSASTUDIO_TEST_MODE='1', ESSASTUDIO_TEST_TONE_HZ='440',
               ESSASTUDIO_SIP_TRANSPORT='tcp', ESSASTUDIO_DIAGNOSTIC_LOG='1',
               ESSASTUDIO_SIP_SRTP='disabled', ESSASTUDIO_MICROPHONE_ID='', ESSASTUDIO_SPEAKER_ID='')
    process = subprocess.Popen([str(args.worker), '--sip-port', str(args.sip_port), '--h323-port', str(args.h323_port),
                                '--rtp-port', str(args.rtp_port), '--interface', '127.0.0.1'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=env, creationflags=0x08000000)

    def drain(stream):
        for line in stream:
            lines.append(line.rstrip())
    for stream in (process.stdout, process.stderr):
        threading.Thread(target=drain, args=(stream,), daemon=True).start()

    def command(value):
        process.stdin.write(value + '\n')
        process.stdin.flush()

    def wait_for(predicate, seconds, detail):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if predicate():
                return
            if peer_errors:
                raise AssertionError(peer_errors[0])
            if process.poll() is not None:
                raise AssertionError('Worker exited before ' + detail)
            time.sleep(.025)
        raise AssertionError(detail)

    failure = None
    try:
        wait_for(lambda: any(x.startswith('Listening H.323 on ') for x in lines), 20, 'Worker startup missing')
        command('aac-enabled off')
        command('call sip:owned@127.0.0.1:' + str(listener.getsockname()[1]))
        wait_for(lambda: ack.is_set() and any('CONFIRMED' in x for x in lines), 20, 'Valid established dialog/ACK missing')
        wait_for(lambda: records and time.monotonic() - records[-1]['receivedMonotonic'] >= 1, 8, 'Dialog did not settle before injection')
        inject.set()
        wait_for(finished.is_set, 24, 'SDK did not close the failed owned TCP flow')
        if peer_errors:
            raise AssertionError(peer_errors[0])
        elapsed = evidence['closeElapsedSeconds']
        if case == 'eof' and elapsed > 5:
            raise AssertionError('Real EOF failed to close promptly: ' + str(elapsed))
        if case == 'partial' and not 8 <= elapsed <= 16:
            raise AssertionError('Partial PDU did not retain the approximately 10-second timeout: ' + str(elapsed))
        wait_for(lambda: any('Reliable transport lost' in x for x in lines)
                 and any('Transport read thread finished' in x for x in lines), 3, 'Read-thread termination evidence missing')
        command('quit')
        process.wait(20)
        if process.returncode:
            raise AssertionError('Worker unclean exit ' + str(process.returncode))
        evidence['result'] = 'PASS'
        print(f'SIP {case} owned TCP read failure PASS closeElapsedSeconds={elapsed:.3f} exit=0')
    except Exception as error:
        failure = error
        evidence.update(result='FAIL', failure=str(error))
    finally:
        if process.poll() is None:
            try:
                command('quit')
                process.wait(20)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.kill()
                process.wait(5)
        for owned in (listener, closed, media, control):
            owned.close()
        peer_thread.join(2)
        evidence.update(records=records, peerErrors=peer_errors, workerExit=process.returncode)
        if args.logs:
            args.logs.mkdir(parents=True, exist_ok=True)
            (args.logs / f'sip-read-{case}.log').write_text('\n'.join(lines))
            (args.logs / f'sip-read-{case}.json').write_text(json.dumps(evidence, indent=2))
    if failure:
        raise failure


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--logs', type=Path)
    parser.add_argument('--case', choices=('eof', 'partial', 'all'), default='all')
    parser.add_argument('--sip-port', type=int, default=15250)
    parser.add_argument('--h323-port', type=int, default=17975)
    parser.add_argument('--rtp-port', type=int, default=49000)
    args = parser.parse_args()
    for case in ('eof', 'partial') if args.case == 'all' else (args.case,):
        run_case(args, case)
