#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""MihawkBrowser's chrome against the approved prototype (docs/MIHAWK_BROWSER.md §4.1).

Compares the bitmaps a run's "snapshot NAME" steps wrote (tools/run-browser-host.py --snapshots), each NAME a state of
docs/windows11-reference/mihawk-browser.json (start, page, error, downloads), with that state's named pixels, in light
or dark (--dark). The flyout's pixels are read from NAME-flyout.bmp, whose top left lies at the client's
(width - 364, 40). Skipped, as a snapshot cannot hold them: hover and press (tests/test_pw_browser_chrome.c checks
those colours), the compositor's flyout stroke, the page (CEF draws it), and the address box while the flyout is open
(the native flyout takes the focus). Prints every pixel that differs and a verdict; exits 1 when one does.

With --prototype DIR (the captures `node tools/measure-browser-prototype.mjs <json> DIR` writes), each fixed text's ink
box (the start view's lines and launchers, the flyout's head and button labels, the error panel) is compared with the
prototype's own render too, within 2 px: GDI and Skia draw glyphs differently, so pixels are not compared; a GDI run
starts on a whole pixel where Chromium's starts between two, and hinting can move a stem one more.
Download names and sizes are the run's own and are not compared.

    tools/check-browser-reference.py klog/<run>-snapshots [--dark] [--tolerance 2] [--prototype DIR]
    tools/check-browser-reference.py --capture start=klog/<run>-capture-01.png ... [--dark] [--prototype DIR]
"""
import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / 'docs/windows11-reference/mihawk-browser.json'
SKIP = ('.hover', '.press', 'flyout.top', 'flyout.bottom')
# Per state: the page's own surface (CEF), and the address box with the flyout open: the native flyout takes the focus
# a click on Downloads moves, where the prototype's capture kept the input's.
SKIP_IN = {'page': ('surface',), 'downloads': ('address.',)}
TEXTS = ('start-text', 'launcher-', 'flyout-head', 'row-0-action-', 'row-1-action-', 'error-title', 'error-detail')


def ink(image, box):
    """The bounding box of what differs from the box's top-left colour: a text's ink."""
    crop = image.crop(box)
    mask = ImageChops.difference(crop, Image.new('RGB', crop.size, crop.getpixel((0, 0)))).convert('L')
    found = mask.point(lambda v: 255 if v > 50 else 0).getbbox()
    return found and (box[0] + found[0], box[1] + found[1], box[0] + found[2], box[1] + found[3])


def subjects(args, reference, theme):
    """(state name, the client's image, the flyout's image or None) for each snapshot or capture."""
    for main_path in sorted(args.snapshots.glob('*-main.bmp')) if args.snapshots else []:
        name = main_path.name[:-len('-main.bmp')]
        flyout = args.snapshots / f'{name}-flyout.bmp'
        yield name, Image.open(main_path).convert('RGB'), Image.open(flyout).convert('RGB') if flyout.exists() else None
    for item in args.capture:
        name, _, path = item.partition('=')
        state = reference['states'].get(f'{name}-{theme}')
        if state:  # the screen's picture: the client where the reference has it, the flyout drawn into it
            c = state['client']
            yield name, Image.open(path).convert('RGB').crop((c['x'], c['y'], c['x'] + c['w'], c['y'] + c['h'])), None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('snapshots', type=Path, nargs='?')
    parser.add_argument('--capture', action='append', default=[], metavar='STATE=PNG',
                        help="a console run's screen picture of a state, the window placed as the reference's "
                             "(MIHAWK_BROWSER_CLIENT=321,149): the compositor's flyout stroke is checked too")
    parser.add_argument('--dark', action='store_true')
    parser.add_argument('--tolerance', type=int, default=2)
    parser.add_argument('--prototype', type=Path, help="measure-browser-prototype.mjs's captures")
    args = parser.parse_args()
    reference = json.loads(REFERENCE.read_text())
    theme = 'dark' if args.dark else 'light'
    checked = differing = 0
    for name, main, flyout in subjects(args, reference, theme):
        state = reference['states'].get(f'{name}-{theme}')
        if not state:
            print(f'{name}: no reference state {name}-{theme}')
            differing += 1
            continue
        if main.size != (state['client']['w'], state['client']['h']):
            print(f'{name}: client {main.size}, the reference is {state["client"]["w"]}x{state["client"]["h"]}')
            differing += 1
            continue
        captured = flyout is None and bool(args.capture)
        left = main.size[0] - 364
        for key, pixel in state['pixels'].items():
            skip = SKIP if not captured else ('.hover', '.press')
            if any(part in key for part in skip) or key.startswith(SKIP_IN.get(name, ())): continue
            x, y = pixel['at']
            image = main
            if key.startswith(('flyout.', 'row-')) and not captured:
                if flyout is None:
                    print(f'{name} {key}: no flyout snapshot')
                    differing += 1
                    continue
                image, x, y = flyout, x - left, y - 40
            got = image.getpixel((x, y))
            checked += 1
            if max(abs(a - b) for a, b in zip(got, pixel['rgb'])) > args.tolerance:
                differing += 1
                print(f'{name} {key} at {pixel["at"]}: {list(got)}, the prototype {pixel["rgb"]}')
        capture = args.prototype and args.prototype / f'browser-{name}-{theme}.png'
        if capture and capture.exists():
            c = state['client']
            prototype = Image.open(capture).convert('RGB').crop((c['x'], c['y'], c['x'] + c['w'], c['y'] + c['h']))
            ours = main.copy()
            if flyout: ours.paste(flyout, (left, 40))
            for part, value in state['parts'].items():
                if not part.startswith(TEXTS): continue
                for line in value.get('lines', []):
                    box = (int(line['x']) - 3, int(line['y']) - 3, int(line['x'] + line['w']) + 4, int(line['y'] + line['h']) + 4)
                    want, got = ink(prototype, box), ink(ours, box)
                    checked += 1
                    if not want or not got or max(abs(a - b) for a, b in zip(want, got)) > 2:
                        differing += 1
                        print(f'{name} {part} text: ink {got}, the prototype {want}')
    verdict = 'PASS' if checked and not differing else 'FAIL'
    print(f'MIHAWK_BROWSER_REFERENCE {verdict}: {checked} pixels checked, {differing} differ ({theme})')
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
