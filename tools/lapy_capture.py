# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the complete owned-root diagnostic stream, not just its final line."""
import re


def validate(text, build_id, requests=1, service=False):
    errors = []
    if not text.endswith('\n'):
        errors.append('Diagnostic stream ended mid-record')
    records = []
    for line in text.splitlines():
        match = re.fullmatch(r'(\d+)\t(\d+)\t(MARK|ERR)\t(.+)', line)
        if not match:
            errors.append('Malformed or incomplete diagnostic record')
            continue
        sequence, _, level, message = match.groups()
        words = message.split()
        fields = dict(word.split('=', 1) for word in words[1:] if '=' in word)
        if int(sequence) != len(records) + 1:
            errors.append('Missing, duplicate or reordered diagnostic record')
        if fields.get('build') != build_id:
            errors.append('Unexpected daemon build identity')
        if level == 'ERR' or words[0] == 'daemon_held':
            errors.append('Daemon reported an error or held ownership')
        records.append((words[0], fields))

    def select(event):
        return [fields for name, fields in records if name == event]

    def require(event, expected, count=None):
        found = select(event)
        if not found or (count is not None and len(found) != count):
            errors.append('Missing or unexpected count: ' + event)
        if any(any(item.get(key) != value for key, value in expected.items()) for item in found):
            errors.append('Unsuccessful record: ' + event)
        return found

    require('daemon_start', {'service': str(int(service)), 'require_client_result': '1'}, 1)
    require('root_layout', {'valid': '1', 'error': '0'}, 1)
    require('donor_reaped', {'status': '9', 'expected_signal': '9'})
    if len(select('donor_reaped')) < 1 + 2 * requests:
        errors.append('Missing donor teardown records')
    require('donor_balance', {'expected_two': '1'}, requests)
    require('request_result', {'stage': 'complete', 'error': '0', 'roots_moved': '1',
                              'cred_changed': '1', 'detached': '1', 'donors_reaped': '1'}, requests)
    require('client_result', {'data_rw': '1', 'open_errno': '0'}, requests)
    require('daemon_result', {'stage': 'service_complete' if service else 'complete',
                              'error': '0', 'root_balanced': '1'}, 1)
    if not records or records[0][0] != 'daemon_start':
        errors.append('Missing initial daemon identity')
    if not records or records[-1][0] != 'daemon_result':
        errors.append('Missing final daemon result')
    if service:
        completed = require('service_request', {'error': '0', 'acknowledged': '1'}, requests)
        if [item.get('completed') for item in completed] != [str(i) for i in range(requests)]:
            errors.append('Service request sequence is incomplete')
    samples = select('root_sample')
    baseline_phase = 'service_baseline' if service else 'baseline'
    final_phase = 'service_exited' if service else 'title_exited'
    baseline = [s for s in samples if s.get('phase') == baseline_phase]
    final = [s for s in samples if s.get('phase') == final_phase]
    balanced = (len(baseline) == len(final) == 1 and
                all(baseline[0].get(k) == final[0].get(k) and baseline[0].get(k, '').isdigit()
                    for k in ('hold', 'use')))
    if not balanced:
        errors.append('Root counters missing or unbalanced')
    return {'passed': not errors, 'errors': sorted(set(errors)), 'records': len(records),
            'requests': len(select('request_result')), 'donors_reaped': len(select('donor_reaped')),
            'baseline': baseline, 'final': final}
