# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from PIL import Image, ImageDraw

spec = importlib.util.spec_from_file_location('desktop_session', Path(__file__).resolve().parents[1] /
                                             'tools/run-desktop-session.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def paint_explorer(image, shift=0, at=(410, 166), active=1, what='explorer'):
    """File Explorer on Home (or on Documents, what='folder') as Wine patch 1150 draws it, where the prototype's is: the
    expected sprites in its client area over a white window; shift moves the command bar's sprites right."""
    expected, _ = runner.pw_explorer_expected.home('light', {'explorer': 'home', 'folder': 'documents'}.get(what, what))
    if shift: expected[96:143, 8 + shift:] = expected[96:143, 8:expected.shape[1] - shift].copy()
    l, t = at
    image.paste(Image.new('RGB', (1100, 700), (255, 255, 255)), (l, t))
    image.paste(Image.fromarray(expected), (l + 1, t + 1))
    return (f'desktop-session {what} frame={l},{t},{l + 1100},{t + 700} client={l + 1},{t + 1},{l + 1099},{t + 699} '
            f'active={active} bar=238,242,248 pointer=0,0')


VIEWS = ['large', 'small', 'list', 'filter', 'pane', 'tabs', 'parent']


class DisplayChecks(unittest.TestCase):
    VIEWS_LOG = 'pw-explorer: view created held=0 visible=0\n' + 2 * 'pw-explorer: view created held=1 visible=0\n'

    def check_pictures(self, colors, logged=True, theme='light', views=VIEWS_LOG, **explorer):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            names, log = [], views
            for index, color in enumerate(colors):
                image = Image.new('RGB', (1920, 1080), (42, 166, 254))
                if color in ('explorer', 'folder', *VIEWS):
                    line = paint_explorer(image, what=color, **explorer)
                    log += line + '\n' if logged else ''
                elif color is not None:
                    ImageDraw.Draw(image).rectangle((300, 220, 900, 650), fill=color)
                name = f'{index}.png'
                image.save(root / name)
                names.append(name)
            with patch.object(runner, 'ROOT', root):
                return runner.display_verdict(names, log, theme)[1]

    def test_both_architectures_and_browser_must_be_displayed(self):
        self.assertTrue(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', *VIEWS]))

    def test_explorer_must_match_the_prototype(self):
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', *VIEWS], shift=1))
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', *VIEWS], at=(200, 100)))
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', *VIEWS], active=0))
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', *VIEWS], logged=False))
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), (255,255,255)]))

    def test_every_icon_view_and_the_tabs_must_be_captured(self):
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', 'large', 'small', 'list']))
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', 'large', 'small']))
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', 'large', 'large', 'list']))

    def test_the_theme_asked_for_must_be_shown(self):
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), 'explorer', 'folder', *VIEWS], theme='dark'))

    def test_a_view_painted_before_it_was_styled_cannot_pass(self):
        """A navigation's view shown when created (Wine's classic list on screen first) fails, as does no record."""
        everything = [(30,110,210), (30,180,70), 'explorer', 'folder', *VIEWS]
        self.assertFalse(self.check_pictures(everything, views=self.VIEWS_LOG + 'pw-explorer: view created held=0 visible=1\n'))
        self.assertFalse(self.check_pictures(everything, views=''))

    def test_background_shell_cannot_pass_a_successful_lifecycle(self):
        self.assertFalse(self.check_pictures([None, None, None]))

    def test_two_captures_of_the_same_architecture_cannot_pass(self):
        self.assertFalse(self.check_pictures([(30,110,210), (30,110,210), (255,255,255)]))

    def test_missing_browser_or_missing_capture_cannot_pass(self):
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70), None]))
        self.assertFalse(self.check_pictures([(30,110,210), (30,180,70)]))


if __name__ == '__main__':
    unittest.main()
