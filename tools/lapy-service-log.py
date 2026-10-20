#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the resident Lapy daemon's log on the console and judge the grants it made.

The resident service (tools/start-lapy-service.py) writes numbered records to
/data/ps5proton/logs/lapy-service.log. This reads the records of its latest start, or those after a
record number, and checks each request: completed with error 0 and acknowledged, the client's /data
proof (data_rw=1), and the root counters at the service baseline before it (so the title before it gave
its references back). Addresses are redacted; nothing here names the console.

  tools/lapy-service-log.py                      every request since the service started
  tools/lapy-service-log.py --after 166          only records after 166 (a run's own requests)
  tools/lapy-service-log.py --evidence NAME      also write evidence/NAME.json
"""
import argparse
import io
import json
import sys
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
sys.path.insert(0, str(ROOT / 'tools'))
import ps5_console  # noqa: E402
from console_log import redact  # noqa: E402

LOG = '/data/ps5proton/logs/lapy-service.log'


def fields(message):
    words = message.split()
    return words[0], dict(word.split('=', 1) for word in words[1:] if '=' in word)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--after', type=int, default=0, help='only records with a higher number')
    parser.add_argument('--evidence', help='write evidence/<name>.json')
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    data = io.BytesIO()
    with FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')
        ftp.retrbinary('RETR ' + LOG, data.write)
    lines = redact(data.getvalue().decode('utf-8', 'replace')).splitlines()
    # The latest start: records restart at 1 with daemon_start.
    start = max((i for i, line in enumerate(lines) if '\tdaemon_start ' in line), default=0)
    records = []
    for line in lines[start:]:
        parts = line.split('\t')
        if len(parts) >= 4 and parts[0].isdigit():
            records.append((int(parts[0]), parts[2], parts[3]))
    baseline = next((f for n, level, m in records for k, f in [fields(m)]
                     if k == 'root_sample' and f.get('phase') == 'service_baseline'), None)
    if baseline is None:
        raise SystemExit('no service baseline in the latest start')
    chosen = [r for r in records if r[0] > args.after]
    requests, errors = [], [m for n, level, m in chosen if level == 'ERR']
    before = None
    for number, level, message in chosen:
        kind, f = fields(message)
        if kind == 'root_sample' and f.get('phase') == 'before_request':
            before = (f['hold'], f['use'])
        elif kind == 'service_request':
            requests.append({'record': number, 'pid': f.get('pid'), 'error': f.get('error'),
                             'acknowledged': f.get('acknowledged'), 'before': before})
        elif kind == 'client_result' and requests:
            requests[-1]['data_rw'] = f.get('data_rw')
    expected = (baseline['hold'], baseline['use'])
    passed = bool(requests) and not errors and all(
        r['error'] == '0' and r['acknowledged'] == '1' and r.get('data_rw') == '1' and r['before'] == expected
        for r in requests)
    build = next((f.get('build') for n, level, m in records for k, f in [fields(m)] if k == 'daemon_start'), None)
    summary = {'daemon_build_id': build, 'baseline': {'hold': expected[0], 'use': expected[1]},
               'records': len(chosen), 'last_record': records[-1][0] if records else None,
               'requests': len(requests), 'error_records': errors,
               'all_at_baseline_before': all(r['before'] == expected for r in requests),
               'all_client_proofs': all(r.get('data_rw') == '1' for r in requests), 'passed': passed}
    print(json.dumps(summary, indent=1))
    if args.evidence:
        summary['request_records'] = requests
        (ROOT / 'evidence' / f'{args.evidence}.json').write_text(json.dumps(summary, indent=1) + '\n')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
