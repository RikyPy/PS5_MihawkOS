# SPDX-License-Identifier: GPL-3.0-or-later
"""MihawkBrowser's own "browser:" lines (src/pw_browser.c) read into a run's record and verdict, the same way for the
host oracle (tools/run-browser-host.py) and the console (tools/run-browser.py). The lines carry no query, path, user
or address (pw_browser_log_url); the record keeps them as they are."""
import re

LOAD = re.compile(r'^browser: load (scheme=\S+ host=\S+ path=\S+) status=(-?\d+) ms=(\d+) at=(\d+)', re.M)
LOAD_ERROR = re.compile(r'^browser: load-error code=(-?\d+) (scheme=\S+ host=\S+)', re.M)
DONE = re.compile(r'^browser: download done (scheme=\S+ host=\S+) path=\S+ bytes=(\d+) ms=(\d+) sha256=(\w+)', re.M)
HANDOFF = re.compile(r'^browser: handoff child=\d+ window_ms=(\d+) (\w+)', re.M)
PROCESS = re.compile(r'^browser: process type=(\S+?)(?: service=(\S+))? pid=\d+', re.M)


def first(pattern, text):
    match = re.search(pattern, text, re.M)
    return int(match.group(1)) if match else None


def summary(lines, script, expect_sha256):
    """The record of one run from its browser lines, and whether it passed: every load 200, no load error or step
    timeout, the download done with the expected SHA-256 when the script downloads, and the hand-off when it opens."""
    text = '\n'.join(lines)
    loads = [{'where': w, 'status': int(s), 'ms': int(ms), 'at_ms': int(at)} for w, s, ms, at in LOAD.findall(text)]
    errors = [{'code': int(c), 'where': w} for c, w in LOAD_ERROR.findall(text)]
    done = DONE.search(text)
    handoff = HANDOFF.search(text)
    timeouts = len(re.findall(r'^browser: step timeout', text, re.M))
    record = {
        'startup': {'window_ms': first(r'^browser: window ms=(\d+)', text),
                    'browser_created_ms': first(r'^browser: created ms=(\d+)', text),
                    'first_load_at_ms': loads[0]['at_ms'] if loads else None},
        'loads': loads, 'load_errors': errors, 'step_timeouts': timeouts,
        'download': done and {'where': done.group(1), 'bytes': int(done.group(2)), 'ms': int(done.group(3)),
                              'sha256': done.group(4), 'sha256_expected': done.group(4) == expect_sha256},
        'handoff': handoff and {'window_ms': int(handoff.group(1)), 'action': handoff.group(2)},
        'process_types': [t + (f' {s}' if s else '') for t, s in PROCESS.findall(text)],
        'exited': re.search(r'^browser: exit ms=\d+', text, re.M) is not None,
    }
    downloads = re.search(r'(^|;)\s*download\b', script) is not None
    opens = re.search(r'(^|;)\s*open\b', script) is not None
    record['passed'] = (timeouts == 0 and not errors and bool(loads) and all(row['status'] == 200 for row in loads)
                        and (not downloads or (done is not None and done.group(4) == expect_sha256))
                        and (not opens or (handoff is not None and handoff.group(2) == 'exiting')))
    return record
