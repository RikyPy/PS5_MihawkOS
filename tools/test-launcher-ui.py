#!/usr/bin/env python3
"""Host UI checks after ps5/tools/host-reference.sh; --render also exercises the picker.
Copyright (C) 2026 Mihawk
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ps5-host'
APP = BUILD / 'app'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render', action='store_true')
    args = parser.parse_args()
    subprocess.run(['ninja', '-C', str(BUILD), 'ps5-proton-ui-test', 'ps5-samples-host'], check=True,
                   stdout=subprocess.DEVNULL)
    preference = APP / 'hui/theme.txt'
    script = APP / 'test-run.txt'
    request, marker, notices = APP / 'wine-run.txt', APP / 'session-running', APP / 'session-notices'
    backup = {p: p.read_bytes() if p.exists() else None for p in (preference, script, request, marker, notices)}
    out = ROOT / 'klog/ui-host'
    out.mkdir(parents=True, exist_ok=True)
    try:
        preference.unlink(missing_ok=True)
        result = subprocess.run([str(BUILD / 'ps5-proton-ui-test')], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert result.stderr.count('to=ui-test cycle=7 reason=ui-test') == 1, result.stderr
        assert 'to=changed' not in result.stderr
        subprocess.run([str(BUILD / 'ps5-proton-ui-test'), 'code'], check=True)
        preference.write_text('not-a-theme\n')
        subprocess.run([str(BUILD / 'ps5-proton-ui-test'), 'gloss'], check=True)
        print(result.stdout.strip())
        print('Fresh-process reload, corrupt preference fallback, deferred handoff: PASS')
        if args.render:
            # All thirty rows, including scrolling; no game can launch in a frame-budget run.
            presses = [(2, 'square')]
            presses += [(4 + i * 2, 'up') for i in range(4)]  # Gloss is row 4
            for i in range(30):
                presses.append((20 + i * 4, 'cross'))
                if i != 29:
                    presses.append((22 + i * 4, 'down'))
            def render(name, frames, events, files={}):
                script.write_text(f'frames {frames}\nscreenshot\nsamples ps5proton\n' +
                                  ''.join(f'press ps5proton {f} {a}\n' for f, a in events))
                for path, text in files.items():
                    path.write_text(text)
                run = subprocess.run([str(BUILD / 'ps5-samples-host')], capture_output=True, text=True, timeout=120)
                log = run.stdout + run.stderr
                (out / f'{name}.log').write_text(log)
                assert run.returncode == 0 and 'ends: status 0' in log, log[-2000:]
                assert 'sample ps5proton: ok' in log
                from PIL import Image, ImageStat
                image = Image.open(APP / 'screenshots/ps5proton.ppm').convert('RGB')
                assert max(ImageStat.Stat(image).stddev) > 2
                image.save(out / f'{name}.png')
                return log
            log = render('all-themes', 180, presses)
            selected = [line.split('launcher theme: ')[1] for line in log.splitlines() if 'launcher theme: ' in line]
            assert len(selected) == len(set(selected)) == 30, selected
            assert preference.read_text() == 'not-a-theme\n', 'render tests changed preference'
            render('gloss-picker', 100, [(2, 'square')])
            render('preview-cancel', 100, [(2, 'square'), (4, 'down'), (6, 'circle')])
            render('apply-return', 100, [(2, 'square'), (4, 'down'), (6, 'cross'), (8, 'circle')])
            print(f'Picker: all 30 themes selected; preview cancel and apply/return rendered. Pictures: {out}')
            # The launcher's reasons (docs/SELF_INSTALL.md, phase 1). The PC build has no /data: every render shows
            # that reason under Mihawk OS; these add a failed session's reason and a session that never came back.
            log = render('session-ended', 30, [],
                         {request: 'cycle=1\nended=Wine: requested profile gate is not in the library\n'})
            assert 'launcher: Last session ended: Wine: requested profile gate is not in the library' in log
            assert 'launcher: games cannot start: No access to /data:' in log
            assert not request.exists(), 'the request is one-shot'
            log = render('session-crashed', 30, [], {marker: 'Mihawk OS'})
            assert 'launcher: The last session (Mihawk OS) did not end normally' in log
            assert not marker.exists(), 'the launcher consumes the marker'
            # What a session said in its notifications (native/proton_wine.cpp notices), shown again after it.
            log = render('session-notices', 30, [], {notices: 'A program could not start (errno 5). Restart the PS5.\n'
                                                              'A test notice: notices show here.\n'})
            assert ('launcher: During the last session: A program could not start (errno 5). Restart the PS5. '
                    'A test notice: notices show here.') in log
            assert not notices.exists(), 'the launcher consumes the notices'
            print(f'Launcher reasons: a failed session, a crashed one and no /data rendered. Pictures: {out}')
            # Settings > Check for updates (docs/SELF_INSTALL.md, phase 8): row 6. A frame-budget run asks no network.
            log = render('updates-panel', 60, [(2, 'triangle')] + [(6 + i * 2, 'down') for i in range(6)] + [(20, 'cross')])
            assert 'update:' not in log, 'a scripted run must not check for updates'
            print(f'Updates panel rendered. Pictures: {out}')
            # Settings > Logging (src/pw_detailed_logs.h): row 7, off by default; a scripted press previews it, saving nothing.
            log = render('logging-panel', 60, [(2, 'triangle')] + [(6 + i * 2, 'down') for i in range(7)] + [(22, 'cross')])
            assert 'Logging: preview on' in log, log[-2000:]
            assert not (APP / 'logging.txt').exists(), 'a scripted run must not save Logging'
            print(f'Logging panel rendered. Pictures: {out}')
    finally:
        for path, content in backup.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(content)


if __name__ == '__main__':
    main()
