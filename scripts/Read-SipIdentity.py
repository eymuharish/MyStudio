"""Read-only SIP OPTIONS identity probe; never sends INVITE or changes settings."""
import argparse
import datetime
import json
import socket
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('addresses', nargs='+')
args = parser.parse_args()
if datetime.datetime.now(datetime.timezone.utc) >= datetime.datetime(2026, 10, 4, 3, 55, tzinfo=datetime.timezone.utc):
    parser.error('The user studio contact cutoff has passed')
results = []
for ip in args.addresses:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.connect((ip, 5060))
        local, port = connection.getsockname()
        connection.settimeout(3)
        tag = uuid.uuid4().hex
        message = (f'OPTIONS sip:{ip} SIP/2.0\r\n'
                   f'Via: SIP/2.0/UDP {local}:{port};branch=z9hG4bK{tag};rport\r\n'
                   f'From: <sip:mystudio-test@{local}>;tag={tag}\r\n'
                   f'To: <sip:{ip}>\r\nCall-ID: {tag}@{local}\r\n'
                   'CSeq: 1 OPTIONS\r\nMax-Forwards: 70\r\n'
                   'User-Agent: MyStudio-readonly-identity\r\nContent-Length: 0\r\n\r\n')
        connection.send(message.encode())
        result = dict(ip=ip, request='OPTIONS only')
        try:
            response = connection.recv(8192).decode(errors='replace')
            result['status'] = response.splitlines()[0]
            result['productHeaders'] = [line for line in response.splitlines()
                if line.lower().startswith(('server:', 'user-agent:', 'organization:'))]
        except socket.timeout:
            result['status'] = 'timeout'
        results.append(result)
print(json.dumps(results, indent=2))
