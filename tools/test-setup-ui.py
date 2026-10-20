#!/usr/bin/env python3
"""The guided setup's pictures on the PC, and its checks (docs/GUIDED_SETUP.md).

    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json python3 tools/test-setup-ui.py

After ps5/tools/build.sh and ps5/tools/host-reference.sh. Each step is shown through PROTON_SETUP_TEST
(<step>[:<the fake install's scenario>][@<share held>]) and PROTON_SETUP_LANGUAGE, walked with the test run's presses,
and kept as klog/ui-host/setup-<name>.png. The checks: every run ends well, the log names the step it reached, the
picture is not blank, the QR code reads back as the instructions' address, and choosing a language switches the text.
Copyright (C) 2026 Mihawk
SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ps5-host'
APP = BUILD / 'app'
OUT = ROOT / 'klog/ui-host'
URL = re.search(r'SETUP_INSTRUCTIONS = "([^"]+)"', (ROOT / 'ps5/tools/generate_assets.py').read_text()).group(1)

COVER_LANGUAGES = ('en-US', 'fr-FR', 'es-ES', 'de-DE', 'it-IT', 'ru-RU', 'zh-CN', 'ja-JP', 'pt-BR')
COVER_STAGES = ('settings', 'dotnet', 'finishing', 'ready')  # PwSetupBuild's order

# name, PROTON_SETUP_TEST, language, frames, presses (frame, action), the step the log must end on
RENDERS = [
    ('welcome', '0', 'en-US', 90, [], 0),
    ('language', '1', 'en-US', 60, [], 1),
    ('language-pick-french', '1', 'en-US', 90, [(20, 'right'), (28, 'cross')], 2),
    ('method', '2', 'en-US', 60, [], 2),
    ('usb', '3', 'en-US', 60, [], 3),
    ('usb-no-drive', '3:no-stick', 'en-US', 60, [(10, 'cross')], 3),
    ('usb-partial', '3:partial', 'en-US', 60, [(10, 'cross')], 3),
    ('usb-found', '3', 'en-US', 60, [(10, 'cross')], 3),
    ('installing', '4@0.62', 'en-US', 40, [], 4),
    ('installing-damaged', '4:damaged@0.5', 'en-US', 40, [], 4),
    ('preparing', '5', 'en-US', 60, [], 5),
    ('done', '6', 'en-US', 90, [], 6),
    ('welcome-fr', '0', 'fr-FR', 90, [], 0),
    ('usb-fr-no-drive', '3:no-stick', 'fr-FR', 60, [(10, 'cross')], 3),
    ('installing-fr', '4@0.62', 'fr-FR', 40, [], 4),
    ('preparing-fr', '5', 'fr-FR', 60, [], 5),
    ('method-de', '2', 'de-DE', 60, [], 2),
    ('installing-ru', '4@0.62', 'ru-RU', 40, [], 4),
    ('usb-zh-no-drive', '3:no-stick', 'zh-CN', 60, [(10, 'cross')], 3),
    ('welcome-zh', '0', 'zh-CN', 90, [], 0),
    ('preparing-ja', '5', 'ja-JP', 60, [], 5),
    ('language-ja', '1', 'ja-JP', 60, [], 1),
    ('done-es', '6', 'es-ES', 90, [], 6),
    ('usb-pt-partial', '3:partial', 'pt-BR', 60, [(10, 'cross')], 3),
    ('method-it', '2', 'it-IT', 60, [], 2),
    # The console gate's walk (tools/run-launcher.py --setup): Welcome to Preparing, in Japanese, through the fake install
    # (each frame is at most 0.05 s of the tour's time: 700 frames cover the 8.6 s install at 15 fps or more)
    # The first session's cover (src/pw_setup.h): Start's frame, kept by the launcher, in every language; the title's
    # marks are drawn over it by the same C code (test_pw_setup's picture mode) as setup-cover-<language>-<stage>.png.
    *[('cover-' + tag, '5:cover', tag, 40, [], 5) for tag in COVER_LANGUAGES],
    ('walk-ja', '0', 'en-US', 700, [(30, 'cross'), (50, 'down'), (56, 'right'), (62, 'right'), (70, 'cross'),
                                     (100, 'cross'), (130, 'cross'), (150, 'cross')], 5),
]


def main():
    from PIL import Image, ImageStat
    subprocess.run(['ninja', '-C', str(BUILD), 'ps5-samples-host'], check=True, stdout=subprocess.DEVNULL)
    marks = OUT / 'cover-marks'
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cc', '-std=gnu11', '-O2', str(ROOT / 'tests/test_pw_setup.c'), str(ROOT / 'src/pw_setup.c'), '-lm',
                    '-o', str(marks)], check=True)
    script = APP / 'test-run.txt'
    backup = script.read_bytes() if script.exists() else None
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        for name, test, language, frames, presses, step in RENDERS:
            script.write_text(f'frames {frames}\nscreenshot\nsamples ps5proton\n' +
                              ''.join(f'press ps5proton {f} {a}\n' for f, a in presses))
            env = dict(os.environ, PROTON_SETUP_TEST=test, PROTON_SETUP_LANGUAGE=language)
            run = subprocess.run([str(BUILD / 'ps5-samples-host')], capture_output=True, text=True, timeout=900, env=env)
            log = run.stdout + run.stderr
            (OUT / f'setup-{name}.log').write_text(log)
            assert run.returncode == 0 and 'ends: status 0' in log, f'{name}: {log[-1500:]}'
            steps = re.findall(r'setup: (?:starts at )?step (\d+)', log)
            assert steps and int(steps[-1]) == step, f'{name}: reached step {steps[-1:]} not {step}'
            assert 'setup: the sky' not in log, f'{name}: the sky failed'
            image = Image.open(APP / 'screenshots/ps5proton.ppm').convert('RGB')
            assert max(ImageStat.Stat(image).stddev) > 8, f'{name}: blank picture'
            image.save(OUT / f'setup-{name}.png')
            if test == '5:cover':
                stages = range(len(COVER_STAGES)) if language == 'en-US' else (1,)
                for stage in stages:
                    marked = OUT / 'cover-marked.ppm'
                    subprocess.run([str(marks), str(ROOT), str(APP / 'screenshots/ps5proton.ppm'), str(marked), str(stage), '3'],
                                   check=True, stdout=subprocess.DEVNULL)
                    Image.open(marked).save(OUT / f'setup-{name}-{COVER_STAGES[stage]}.png')
                    marked.unlink()
            print(f'setup-{name}: step {step}, {frames} frames')
        if 'setup: language fr-FR' not in (OUT / 'setup-language-pick-french.log').read_text():
            raise AssertionError('choosing the third tile did not choose French')
        if shutil.which('zbarimg'):
            for name in ('usb', 'method'):
                read = subprocess.run(['zbarimg', '-q', '--raw', str(OUT / f'setup-{name}.png')], capture_output=True, text=True)
                assert read.stdout.strip() == URL, f'setup-{name}: the QR code reads {read.stdout.strip()!r}'
            print(f'QR code: reads {URL}')
        else:
            print('QR code: not read back (no zbarimg)')
        print(f'Setup pictures: {OUT}')
    finally:
        if backup is None:
            script.unlink(missing_ok=True)
        else:
            script.write_bytes(backup)


if __name__ == '__main__':
    main()
