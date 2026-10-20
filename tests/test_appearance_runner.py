# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('appearance',ROOT/'tools/run-appearance.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)


class CapturedAppearance(unittest.TestCase):
    def fixture(self, backgrounds, icons=True, captions=True, wrong_caption=False, repeated=False, alpha_layers=1,
                strip=True, strip_hot=True):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'themes/windows11').mkdir(parents=True)
            (root/'themes/windows11/appearance.json').write_bytes((ROOT/'themes/windows11/appearance.json').read_bytes())
            import shutil
            shutil.copytree(ROOT/'.deps/mihawk-icons',root/'.deps/mihawk-icons')
            paths=[];log=[]
            for index,background in enumerate(backgrounds):
                name=('light','dark')[index];image=Image.new('RGBA',(1920,1080),background)
                if icons:
                    for category,icon,x,y,size in [('original','folder',162,338,48),(name,'search',140,570,24)]:
                        with Image.open(root/'.deps/mihawk-icons'/category/f'{icon}-{size}.png') as glyph:
                            for _ in range(alpha_layers):
                                image.alpha_composite(glyph.convert('RGBA'),(x,y))
                log.append(f'appearance: {name} client-origin=100,100')
                if captions:
                    for part in (15,17,18):
                        for sample in range(8):
                            point=0 if repeated else sample
                            x,y=300+part*10+point,50
                            image.putpixel((x,y),(0,0,0,255) if wrong_caption else (40,50,60,255))
                            log.append(f'appearance: caption {name} part={part} sample={x},{y} expected=40,50,60')
                if strip:
                    # the caption buttons as Wine composes them, with the hovered button hot, under the frame row
                    bar,left,top=((238,242,248),700,400) if name=='light' else ((34,38,45),700,400)
                    hot='max' if name=='light' else 'close'
                    for i,kind in enumerate(('min','max','close')):
                        state=1 if strip_hot and kind==hot else 0
                        pixels=runner.pw_caption_buttons.render(name,kind,runner.pw_caption_buttons.button_states(name,kind)[state],bar)
                        for y in range(32):
                            for x in range(46): image.putpixel((left+i*46+x,top+y),pixels[y][x]+(255,))
                    for x in range(left,left+138): image.putpixel((x,top-1),bar+(255,))
                    log.append(f'appearance: caption-strip {name} left={left} top={top} bar={bar[0]},{bar[1]},{bar[2]} hot={hot} '
                               f'pointer={left+(92 if hot=="close" else 46)+42},{top+28} active=1')
                path=f'{index}.png';image.convert('RGB').save(root/path);paths.append(path)
            with patch.object(runner,'ROOT',root):
                return runner.display_verdict(paths,'\n'.join(log))[1]

    def test_correct_panes_and_alpha_icons(self):
        self.assertTrue(self.fixture(['#ffffff','#191919']))

    def test_repeated_alpha_paint_cannot_pass(self):
        self.assertFalse(self.fixture(['#ffffff','#191919'],alpha_layers=2))

    def test_successful_apis_with_missing_icons_cannot_pass(self):
        self.assertFalse(self.fixture(['#ffffff','#191919'],False))

    def test_duplicate_light_capture_cannot_pass_dark(self):
        self.assertFalse(self.fixture(['#ffffff','#ffffff']))

    def test_caption_reference_is_required(self):
        self.assertFalse(self.fixture(['#ffffff','#191919'],captions=False))

    def test_classic_caption_cannot_pass_themed_reference(self):
        self.assertFalse(self.fixture(['#ffffff','#191919'],wrong_caption=True))

    def test_repeated_log_lines_do_not_replace_unique_caption_samples(self):
        self.assertFalse(self.fixture(['#ffffff','#191919'],repeated=True))

    def test_caption_strip_is_required(self):
        self.assertFalse(self.fixture(['#ffffff','#191919'],strip=False))

    def test_caption_button_without_its_hover_cannot_pass(self):
        self.assertFalse(self.fixture(['#ffffff','#191919'],strip_hot=False))

    def test_missing_second_variant_cannot_pass(self):
        self.assertFalse(self.fixture(['#ffffff']))


if __name__=='__main__':unittest.main()
