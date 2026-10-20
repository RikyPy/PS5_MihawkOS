# SPDX-License-Identifier: GPL-3.0-or-later
"""The effects verdict needs every phase, rectangle and sample; one wrong pixel fails it."""
import importlib.util
import math
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('effects', ROOT / 'tools/run-window-effects.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

RECTS = {'back': (60, 60, 1860, 1020), 'round': (150, 150, 450, 400), 'small': (550, 150, 800, 300),
         'framed': (150, 500, 550, 750), 'dropped': (900, 600, 1000, 700), 'acrylic': (1100, 150, 1400, 400),
         'mica': (1100, 550, 1400, 800), 'destination': (1500, 150, 1800, 400), 'source': (600, 800, 1000, 1000)}


def log_lines(rects=RECTS, phases=runner.PHASES):
    lines = []
    for name, (left, top, right, bottom) in rects.items():
        client = (left + 8, top + 31, right - 8, bottom - 8) if name == 'framed' else (left, top, right, bottom)
        lines.append(f'window-effects rect name={name} window={left},{top},{right},{bottom} '
                     f'client={",".join(map(str, client))}')
    lines.append('window-effects frame name=framed colour=240,180,30')  # the render scene's framed window is solid yellow
    lines.append('window-effects active name=framed')  # and the active one, so round shows the inactive shadow
    lines += [f'window-effects capture={n} phase={phase}' for n, phase in enumerate(phases, 1)]
    return '\n'.join(lines)


class EffectsVerdict(unittest.TestCase):
    def test_reference_geometry(self):
        # Analytic checks of the independent model itself.
        self.assertEqual(runner.coverage(8, 0, 0), 0)
        self.assertEqual(runner.coverage(8, 7, 7), 1)
        cut = sum(1 - runner.coverage(8, i, j) for i in range(8) for j in range(8))
        self.assertAlmostEqual(cut, 64 * (1 - math.pi / 4), delta=0.2)
        self.assertEqual(runner.shadow_alpha((0, 0, 10, 10), 0, 8, 41, 2, 5, 5), 41)
        self.assertEqual(runner.shadow_alpha((0, 0, 10, 10), 0, 8, 41, 2, 30, 5), 0)
        # Skia's three-box kernels: blur 48 is three boxes of 45, blur 8 two of 8 and one of 9.
        border, weights, _ = runner.shadow_kernel(48)
        self.assertEqual((border, sum(weights)), (66, 45 ** 3))
        border, weights, _ = runner.shadow_kernel(8)
        self.assertEqual((border, sum(weights), weights, weights[::-1] == weights), (11, 576, weights, True))
        # The prototype's own darkening beside File Explorer's right side (the reference capture, light, active):
        # its 48 px layer's alpha 27, 22, 18, 10, 5, 1, 0 at 0, 5, 10, 20, 30, 47, 48 px out.
        layer = runner.tokens()['shadow_window']['active']['light'][0]
        rect = (410, 166, 1510, 866)
        alphas = [(56 * (runner.shadow_mask(layer, rect, 8, 1510 + k, 500) + 1)) >> 8 for k in (0, 5, 10, 20, 30, 47, 48)]
        self.assertEqual(alphas, [27, 22, 18, 10, 5, 1, 0])

    def test_synthetic_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            old = runner.ROOT
            runner.ROOT = Path(folder)
            try:
                log = log_lines()
                rects, _ = runner.parse(log)
                root = Image.new('RGB', (1920, 1080), (70, 80, 90))
                root.save(runner.ROOT / '1.png')
                samples = runner.expectations(rects, runner.tokens(), runner.mica_reference(root, runner.tokens()))
                captures = ['1.png']
                for number, phase in enumerate(runner.PHASES[1:], 2):
                    image = Image.new('RGB', (1920, 1080), (0, 0, 0))
                    for _, x, y, expected, _ in samples[phase]:
                        image.putpixel((x, y), tuple(int(math.floor(c + 0.5)) for c in expected))
                    image.save(runner.ROOT / f'{number}.png')
                    captures.append(f'{number}.png')
                self.assertTrue(runner.display_verdict(captures, log)[1])
                # A root retaken under the next number: the last root is judged, the later phases follow it.
                for number in range(len(captures), 0, -1):
                    (runner.ROOT / f'{number}.png').replace(runner.ROOT / f'{number + 1}.png')
                Image.new('RGB', (1920, 1080), (255, 255, 255)).save(runner.ROOT / '1.png')
                retaken = '\n'.join(l for l in log.splitlines() if 'capture=' not in l) + '\nwindow-effects capture=1 phase=root\n' + \
                    '\n'.join(f'window-effects capture={n} phase={phase}' for n, phase in enumerate(runner.PHASES, 2))
                shifted = [f'{n}.png' for n in range(1, len(captures) + 2)]
                self.assertTrue(runner.display_verdict(shifted, retaken)[1])
                self.assertFalse(runner.display_verdict(shifted[1:], log)[1])
                for number in range(1, len(captures) + 1):
                    (runner.ROOT / f'{number + 1}.png').replace(runner.ROOT / f'{number}.png')
                self.assertFalse(runner.display_verdict(captures[:-1], log)[1])
                self.assertFalse(runner.display_verdict(captures, log.replace('phase=thumbnail-live', 'phase=other'))[1])
                self.assertFalse(runner.display_verdict(captures, '\n'.join(l for l in log.splitlines() if 'name=mica' not in l))[1])
                # A square corner where rounding was required fails.
                image = Image.open(runner.ROOT / '2.png').convert('RGB')
                image.putpixel((150, 150), runner.RED)
                image.save(runner.ROOT / '2.png')
                self.assertFalse(runner.display_verdict(captures, log)[1])
            finally:
                runner.ROOT = old


    def test_production_compositor_matches_reference(self):
        """Differential oracle: the C compositor's own rendering of the probe
        scene must satisfy the independent float reference."""
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'render'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(ROOT / 'tests/window-effects-render.c'),
                            str(ROOT / 'src/pw_window_compose.c'), str(ROOT / 'src/pw_window_effects.c'),
                            '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary), folder], check=True, capture_output=True)
            old = runner.ROOT
            runner.ROOT = Path(folder)
            try:
                captures = []
                for number in range(1, len(runner.PHASES) + 1):
                    with Image.open(Path(folder) / f'{number}.ppm') as frame:
                        frame.save(Path(folder) / f'{number}.png')
                    captures.append(f'{number}.png')
                results, passed = runner.display_verdict(captures, log_lines())
                failed = [p for r in results for p in r.get('pixels', []) if not p['passed']]
                self.assertTrue(passed, failed[:12])
                self.assertEqual(sum(len(r['pixels']) for r in results), 59)
            finally:
                runner.ROOT = old


    def test_acrylic_reference_matches_compositor(self):
        """Acrylic over a gradient and checkerboard near the bottom edge: the
        float model from the frame beneath agrees with the compositor."""
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'render'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(ROOT / 'tests/window-effects-render.c'),
                            str(ROOT / 'src/pw_window_compose.c'), str(ROOT / 'src/pw_window_effects.c'),
                            '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary), folder, 'acrylic'], check=True, capture_output=True)
            materials = runner.tokens()
            rect = (700, 500, 1250, 1000)
            with Image.open(Path(folder) / 'a1.ppm') as beneath, Image.open(Path(folder) / 'a2.ppm') as frame:
                sample = runner.acrylic_reference(beneath, rect, materials)
                tint = runner.hex_rgb(materials['acrylic']['light'])
                worst = 0
                for x, y in [(710, 510), (975, 750), (1240, 990), (1200, 520), (720, 980), (1000, 999)]:
                    expected = runner.tint(sample(x, y), tint, materials['acrylic']['tint'])
                    actual = frame.getpixel((x, y))
                    worst = max(worst, max(abs(a - e) for a, e in zip(actual, expected)))
                self.assertLessEqual(worst, 2)
                # The rounded corner reveals the beneath frame darkened by the (inactive, light) shadow.
                corner = runner.window_shadow(beneath.getpixel((700, 500)), rect, 8, 700, 500,
                                              materials['shadow_window']['inactive']['light'])
                self.assertEqual(frame.getpixel((700, 500)), corner)


if __name__ == '__main__':
    unittest.main()
