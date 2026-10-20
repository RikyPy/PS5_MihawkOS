#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Known-good lifecycle and corruption cases; no console access."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from lapy_capture import validate

build = 'test-build'
events = [
    'daemon_start service=0 require_client_result=1',
    'root_layout valid=1 error=0',
    'root_sample phase=baseline hold=55 use=54',
    'donor_reaped status=9 expected_signal=9',
    'donor_reaped status=9 expected_signal=9',
    'donor_reaped status=9 expected_signal=9',
    'donor_balance expected_two=1',
    'request_result stage=complete error=0 roots_moved=1 cred_changed=1 detached=1 donors_reaped=1',
    'client_result data_rw=1 open_errno=0',
    'root_sample phase=title_exited hold=55 use=54',
    'daemon_result stage=complete error=0 root_balanced=1',
]
text = ''.join(f'{i}\t{i * 100}\tMARK\t{event} build={build}\n' for i, event in enumerate(events, 1))
assert validate(text, build)['passed']
cases = [text.replace('5\t500\t', '4\t500\t'), '\n'.join(text.splitlines()[1:]),
         text + text.splitlines()[-1] + '\n', text.replace('phase=title_exited hold=55', 'phase=title_exited hold=57'),
         text.replace('data_rw=1', 'data_rw=0'), text.replace('status=9', 'status=0', 1),
         text.replace('build=test-build', 'build=stale', 1), text.rsplit('\n', 2)[0],
         text.replace('stage=complete error=0', 'stage=complete error=60', 1), '']
for corrupted in cases:
    assert not validate(corrupted, build)['passed'], corrupted
print(f'Lapy capture: good lifecycle accepted; {len(cases)} incomplete, stale or failed cases rejected')

from io import StringIO
from console_log import LineLog
output = StringIO()
log = LineLog(output)
for chunk in [b'console 192.0.', b'2.1 caf\xc3', b'\xa9\nlast', b' record']:
    log.write(chunk)
log.finish()
assert output.getvalue() == 'console <address> café\nlast record'
print('Chunked UTF-8 and address redaction: PASS')

service_events = [events[0].replace('service=0', 'service=1'), events[1],
                  events[2].replace('phase=baseline', 'phase=service_baseline')]
service_events += [events[3]]
for completed in range(2):
    service_events += [events[4], events[5], events[6], events[7],
                       f'service_request completed={completed} error=0 acknowledged=1', events[8]]
service_events += [events[9].replace('title_exited', 'service_exited'),
                   events[10].replace('stage=complete', 'stage=service_complete')]
service = ''.join(f'{i}\t{i * 100}\tMARK\t{event} build={build}\n'
                  for i, event in enumerate(service_events, 1))
assert validate(service, build, requests=2, service=True)['passed']
assert not validate(service.replace('completed=1', 'completed=0'), build, requests=2, service=True)['passed']
assert not validate(service.replace('service_exited', 'before_request'), build, requests=2, service=True)['passed']
print('Two-request service: accepted; duplicate request and missing final counters rejected')
