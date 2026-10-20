#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How fast data moves through the runtime: tests/net-throughput-probe.c on the console against a sender on this machine.

  tools/run-net-throughput.py --name RUN [--bits 32|64] [--port 1749]

Serves zeros as fast as the line takes them on --port (1749 is one of the two the host firewall lets through, as the
TLS fixture's are) and runs the probe through tools/run-console.py with this machine's address on the route to the
console. The probe's line (MB/s per step) is copied to evidence/RUN.json under "throughput". A measurement: the run
passes when every step ran.
"""
import argparse
import json
import re
import socket
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402


def serve(listener, stop):
    chunk = bytes(1 << 20)

    def send(connection):
        try:
            while not stop.is_set():
                connection.sendall(chunk)
        except OSError:
            pass
        finally:
            connection.close()

    while not stop.is_set():
        try:
            connection, _ = listener.accept()
        except OSError:
            break
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=send, args=(connection,), daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--bits', choices=('32', '64'), default='64')
    parser.add_argument('--port', type=int, default=1749)
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((settings['host'], 9))
    here = probe.getsockname()[0]
    probe.close()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('0.0.0.0', args.port))
    listener.listen(16)
    stop = threading.Event()
    threading.Thread(target=serve, args=(listener, stop), daemon=True).start()
    try:
        result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--exe',
                                 f'net-throughput{args.bits}-probe.exe', '--args', f'{here} {args.port}', '--seconds', '90',
                                 '--name', args.name], cwd=ROOT)
    finally:
        stop.set()
        listener.close()
    log = ROOT / 'klog' / (args.name + '.log')
    text = log.read_text(errors='replace') if log.exists() else ''
    line = re.findall(r'PS5_PROTON_NET_THROUGHPUT (?:PASS|FAIL)[^"\\\n]*', text)
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if line and evidence.exists():
        record = json.loads(evidence.read_text())
        record['throughput'] = line[-1]
        evidence.write_text(json.dumps(record, indent=1) + '\n')
    print(line[-1] if line else 'no throughput line in the klog')
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
