#!/usr/bin/env python3
# SPDX-License-Identifier: OFL-1.1
"""Build Proton UI from pinned OFL Selawik; no proprietary font is an input.

Keep the screen-oriented outlines, advances and mark positioning. Add conservative
Latin kerning (in thousandths of an em) to GPOS and legacy GDI's kern table. These
are our optical spacing choices, not values extracted from Segoe. The output
family has its own name, and lighter weights retain separate legacy GDI families.
"""
import hashlib
import json
from pathlib import Path

from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables import otTables
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
from fontTools.otlLib.builder import buildPairPosGlyphs, buildValue

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'themes/windows11/fonts/upstream'
OUTPUT = ROOT / '.deps/windows11-theme/fonts'
STYLES = {'selawk.ttf': ('Regular', 400), 'selawkb.ttf': ('Bold', 700),
          'selawkl.ttf': ('Light', 300), 'selawksl.ttf': ('Semilight', 350),
          'selawksb.ttf': ('Semibold', 600)}
# Diagonal/open pairs; accented forms inherit their base pair below.
PAIRS = {'AV': -35, 'AW': -25, 'AY': -40, 'AT': -20,
         'VA': -35, 'WA': -25, 'YA': -40, 'TA': -20,
         'To': -35, 'Ta': -30, 'Te': -30, 'Tu': -20,
         'Vo': -20, 'Va': -20, 'Ve': -20, 'Wo': -15,
         'Wa': -15, 'We': -15, 'Yo': -40, 'Ya': -35, 'Ye': -35,
         'Fo': -15, 'Fa': -15, 'LT': -15, 'LV': -20, 'LY': -25,
         'P.': -25, 'T.': -35, 'V.': -30, 'W.': -25, 'Y.': -40}


def build(source, style, weight):
    font = TTFont(source, recalcTimestamp=False)
    original = TTFont(source, recalcTimestamp=False)
    import unicodedata
    cmap = font.getBestCmap()
    groups = {}
    for code, glyph in cmap.items():
        decomposed = unicodedata.normalize('NFD', chr(code))
        if len(decomposed) == 1 or all(unicodedata.combining(c) for c in decomposed[1:]):
            groups.setdefault(decomposed[0], set()).add(glyph)
    pairs = {}
    for (left, right), value in PAIRS.items():
        for a in groups.get(left, ()):
            for b in groups.get(right, ()):
                pairs[a, b] = round(value * font['head'].unitsPerEm / 1000)
    kern = newTable('kern'); kern.version = 0
    sub = KernTable_format_0(); sub.version = 0; sub.coverage = 1; sub.kernTable = pairs
    kern.kernTables = [sub]; font['kern'] = kern
    # Append to the existing GPOS so combining-mark positioning remains intact.
    gpos = font['GPOS'].table
    lookup = otTables.Lookup(); lookup.LookupType = 2; lookup.LookupFlag = 0
    lookup.SubTable = buildPairPosGlyphs({pair: (buildValue({'XAdvance': value}), None)
                                        for pair, value in pairs.items()}, font.getReverseGlyphMap())
    lookup.SubTableCount = len(lookup.SubTable)
    index = len(gpos.LookupList.Lookup)
    gpos.LookupList.Lookup.append(lookup); gpos.LookupList.LookupCount += 1
    feature = otTables.FeatureRecord(); feature.FeatureTag = 'kern'
    feature.Feature = otTables.Feature(); feature.Feature.FeatureParams = None
    feature.Feature.LookupListIndex = [index]; feature.Feature.LookupCount = 1
    # 'kern' sorts before upstream's 'mark' and 'mkmk'. Update every old index.
    assert all(record.FeatureTag > 'kern' for record in gpos.FeatureList.FeatureRecord)
    gpos.FeatureList.FeatureRecord.insert(0, feature); gpos.FeatureList.FeatureCount += 1
    for record in gpos.ScriptList.ScriptRecord:
        languages = [r.LangSys for r in record.Script.LangSysRecord]
        if record.Script.DefaultLangSys is not None:
            languages.append(record.Script.DefaultLangSys)
        for language in languages:
            language.FeatureIndex = [0] + [i + 1 for i in language.FeatureIndex]
            language.FeatureCount = len(language.FeatureIndex)
            if language.ReqFeatureIndex != 0xffff: language.ReqFeatureIndex += 1
    legacy_family = 'Proton UI' if style in ('Regular', 'Bold') else 'Proton UI ' + style
    full_name = 'Proton UI' + ('' if style == 'Regular' else ' ' + style)
    names = {1: legacy_family, 2: style if style == 'Bold' else 'Regular',
             3: 'ProtonUI-1.001-' + style, 4: full_name, 5: 'Version 1.001',
             6: 'ProtonUI-' + style, 7: 'Proton UI is a derivative of OFL Selawik; no endorsement implied.',
             8: 'PS5_Proton contributors', 16: 'Proton UI', 17: style,
             19: 'Proton UI specimen: Settings, Library, Play, AV To Yo.'}
    font['name'].names = [n for n in font['name'].names if n.nameID not in names]
    for name_id, value in names.items():
        font['name'].setName(value, name_id, 3, 1, 0x409)
        font['name'].setName(value, name_id, 1, 0, 0)
    font['OS/2'].usWeightClass = weight
    font['head'].fontRevision = 1.001
    if 'DSIG' in font: del font['DSIG']  # The modified binary carries no upstream signature.
    target = OUTPUT / ('ProtonUI-' + style + '.ttf')
    font.save(target)
    checked = TTFont(target, checkChecksums=2, recalcTimestamp=False)
    assert checked.getBestCmap() == original.getBestCmap()
    assert checked['glyf'].compile(checked) == original['glyf'].compile(original)
    assert checked['hmtx'].metrics == original['hmtx'].metrics
    assert checked['name'].getDebugName(16) == 'Proton UI'
    assert checked['OS/2'].fsType == 0 and checked['OS/2'].usWeightClass == weight
    assert checked['kern'].kernTables[0].kernTable == pairs
    assert all(c in checked.getBestCmap() for c in range(32, 127))
    return {'file': target.name, 'weight': weight, 'characters': len(cmap),
            'kerning_pairs': len(pairs), 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}


def main():
    pin = json.loads((SOURCE.parent / 'upstream.json').read_text())
    for name, digest in pin['sha256'].items():
        if hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Font input checksum mismatch: ' + name)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    fonts = [build(SOURCE / name, *data) for name, data in STYLES.items()]
    (OUTPUT / 'LICENSE.txt').write_bytes((SOURCE / 'LICENSE.txt').read_bytes())
    record = {'family': 'Proton UI', 'license': 'OFL-1.1', 'upstream': pin,
              'fonts': fonts, 'passed': True, 'console_qualified': False,
              'segoe_binary_used': False, 'pixel_identical_claimed': False}
    (OUTPUT / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__': main()
