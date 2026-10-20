#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify actual light/dark appearance pixels, icons, APIs and normal cleanup."""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pw_caption_buttons  # noqa: E402


def caption_strip_verdict(image, log, name):
    """The minimize, maximize and close buttons as captured, pixel for pixel against Wine's own composite of the
    prototype-measured bitmaps over the bar the probe reports (tools/pw_caption_buttons.py), with the hot button the
    probe hovered, and the window's own top frame row above it. This probe runs on the single-surface route (no
    WINE_PS5_WINDOW_SURFACES), where no compositor draws borders or corners, so that row is Wine's frame in the bar
    colour; the default border over it (#66757575, the desktop's window-surfaces route) was measured in
    window-effects. Only the pointer sprite and the 8 x 8 top right corner are left out."""
    found = re.findall(r'appearance: caption-strip (light|dark) left=(-?\d+) top=(-?\d+) bar=(\d+),(\d+),(\d+) '
                       r'hot=(\w+) pointer=(-?\d+),(-?\d+) active=(\d)', log)
    found = [item for item in found if item[0] == name]
    if not found: return {'passed': False, 'error': 'no caption-strip line'}
    _, left, top, r, g, b, hot, px, py, active = found[-1]
    left, top, px, py, bar = int(left), int(top), int(px), int(py), (int(r), int(g), int(b))
    expected = {}
    for index, kind in enumerate(('min', 'max', 'close')):
        state = (1 if kind == hot else 0) + (0 if active == '1' else 4)
        pixels = pw_caption_buttons.render(name, kind, pw_caption_buttons.button_states(name, kind)[state], bar)
        for y in range(32):
            for x in range(46): expected[(left + index * 46 + x, top + y)] = pixels[y][x]
    for x in range(left, left + 130): expected[(x, top - 1)] = bar
    wrong, worst, compared = 0, 0, 0
    for (x, y), want in expected.items():
        if px - 1 <= x < px + 31 and py - 1 <= y < py + 31: continue  # the pointer
        if x >= left + 130 and y < top + 8: continue  # the rounded corner
        if not (0 <= x < image.width and 0 <= y < image.height): return {'passed': False, 'error': 'strip off screen'}
        got = image.getpixel((x, y)); compared += 1
        error = max(abs(a - e) for a, e in zip(got, want))
        wrong += error > 0; worst = max(worst, error)
    return {'strip': [left, top, 138, 32], 'bar': bar, 'hot': hot, 'compared': compared, 'wrong': wrong,
            'worst': worst, 'passed': compared > 4000 and not wrong}


def display_verdict(captures, log):
    positions = {name: (int(x), int(y)) for name,x,y in re.findall(
        r'appearance: (light|dark) client-origin=(\d+),(\d+)',log)}
    palette = json.loads((ROOT/'themes/windows11/appearance.json').read_text())
    results = []
    for index, name in enumerate(('light','dark')):
        item = {'variant':name,'passed':False}
        if index >= len(captures) or name not in positions:
            item['error']='missing capture or measured client origin';results.append(item);continue
        origin = positions[name]
        with Image.open(ROOT/captures[index]) as source:
            image = source.convert('RGB')
            pane = tuple(bytes.fromhex(palette['classic'][name]['window'][1:]))  # the probe paints COLOR_WINDOW
            item['capture']=captures[index]
            item['pane']=image.getpixel((origin[0]+20,origin[1]+195))
            item['expected_pane']=pane
            regions=[]
            for category,icon,offset,size in [('original','folder',(62,238),48),(name,'search',(40,470),24)]:
                with Image.open(ROOT/'.deps/mihawk-icons'/category/f'{icon}-{size}.png') as glyph:
                    expected=Image.new('RGBA',(size,size),pane+(255,));expected.alpha_composite(glyph.convert('RGBA'))
                    x,y=origin[0]+offset[0],origin[1]+offset[1]
                    actual=image.crop((x,y,x+size,y+size))
                    error=sum(ImageStat.Stat(ImageChops.difference(actual,expected.convert('RGB'))).mean)/3
                    regions.append({'icon':icon,'size':size,'mean_rgb_error':round(error,3),
                                    'tolerance':2.0,'passed':error<=2.0})
            item['icon_regions']=regions
            samples=[];seen=set()
            for variant,part,x,y,r,g,b in re.findall(
                    r'appearance: caption (light|dark) part=(\d+) sample=(\d+),(\d+) expected=(\d+),(\d+),(\d+)',log):
                if variant!=name: continue
                point=(int(x),int(y));expected=(int(r),int(g),int(b))
                key=(int(part),point,expected)
                if key in seen: continue
                seen.add(key)
                actual=image.getpixel(point) if 0<=point[0]<image.width and 0<=point[1]<image.height else None
                error=max(abs(a-e) for a,e in zip(actual,expected)) if actual else None
                samples.append({'part':int(part),'point':point,'actual':actual,'expected':expected,
                                'max_rgb_error':error,'tolerance':2,'passed':error is not None and error<=2})
            item['caption_samples']=samples
            complete=all(sum(s['part']==part for s in samples)>=8 for part in (15,17,18))
            item['caption_reference_complete']=complete
            item['caption_strip']=caption_strip_verdict(image,log,name)
            item['passed']=image.size==(1920,1080) and item['pane']==pane and all(x['passed'] for x in regions) and \
                           complete and all(s['passed'] for s in samples) and item['caption_strip']['passed']
        results.append(item)
    return results,len(captures)==2 and all(item['passed'] for item in results)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name',required=True);parser.add_argument('--arch',choices=('32','64'),required=True)
    args=parser.parse_args()
    result=subprocess.run([sys.executable,str(ROOT/'tools/run-console.py'),'--name',args.name,
        '--exe',f'appearance{args.arch}-probe.exe',r'--args=--probe --capture=Z:\data\ps5proton\logs\game-frame.ppm','--marker','PS5_PROTON_APPEARANCE_PROBE',
        '--pad-script',str(ROOT/'tests/pad-scripts/appearance.txt')],cwd=ROOT)
    path=ROOT/'evidence'/(args.name+'.json')
    if not path.exists():return result.returncode or 1
    record=json.loads(path.read_text())
    log='\n'.join(path.read_text(errors='replace') for path in (ROOT/'klog').glob(args.name+'*.log'))
    pixels,passed=display_verdict(record.get('captures',[]),log)
    record['api_passed']=record['passed'];record['appearance_pixels']=pixels
    record['passed']=record['passed'] and passed
    path.write_text(json.dumps(record,indent=1)+'\n')
    print(json.dumps({'appearance_pixels':pixels,'passed':record['passed']},indent=1))
    return 0 if record['passed'] else 1


if __name__=='__main__':sys.exit(main())
