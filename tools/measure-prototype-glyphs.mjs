// SPDX-License-Identifier: GPL-3.0-or-later
// The approved prototype's own glyph pixels for text the native shell draws (src/pw_text_glyphs.h): every character in
// the font and colour of a prototype element, alone, at each of the four quarter-pixel pen phases Skia draws at, over
// black and over white (premultiplied colour = black, alpha = 255 - (white - black), as for the sprites), with its
// advance; and whole strings, laid out by Chromium right-aligned at a whole pixel as the taskbar clock is, to check the
// native composition against.
//
// Why pixels and not a rasteriser: Chromium 154 draws web fonts with Fontations (skrifa's autohinter, slight = light
// vertical-only hinting) and Skia's analytic path coverage, then a gamma/contrast preblend chosen by the paint's
// luminance (gamma 1.1875, contrast 0.2). FreeType 2.14's light autohinter on the same Selawik file matches only 48% of
// the ink pixels; Chromium's own glyphs placed on Skia's grid (below) match whole strings exactly.
//
//   node tools/measure-prototype-glyphs.mjs <out.json> <theme> <selector> <chars> [<string>...]
//     the font, size, line height and colour of the first element matching <selector> (in ?fresh&<theme>)
//     writes {url, theme, selector, font: {css, color, size, line, baseline}, glyphs: {char: {advance, phases: [4 x
//       {x, y, w, h, black, white}]}}, strings: [{text, x, baseline, x0, y0, w, h, black, white}]}; glyph x/y are the
//       box's offset from the pen's whole pixel and the baseline, black/white base64 RGB
import { writeFileSync } from 'node:fs';
import { connect, decode, page } from './pw_prototype_capture.mjs';

const [out, theme, selector, chars, ...strings] = process.argv.slice(2);
const base = process.env.PROTOTYPE_BASE ?? 'http://127.0.0.1:8778/prototypes/windows11-shell/index.html';
const url = `${base}?fresh&${theme}`;
const session = await connect(process.env.CDP_PORT ?? '9333');
const view = await page(session, url);
// Glyphs on a 32 px grid, four phases side by side; strings below, each right-aligned at a whole pixel.
const cell = 32, row = 40, perRow = Math.floor(1800 / (4 * cell)), list = [...new Set(chars)];
const layout = await view.evaluate(`(async () => {
  const source = document.querySelector(${JSON.stringify(selector)}); if (!source) return null;
  await document.fonts.ready;
  const s = getComputedStyle(source);
  const style = ['font-family', 'font-size', 'font-weight', 'font-style', 'font-stretch', 'line-height', 'color',
                 'font-variant-numeric', 'font-feature-settings', 'font-kerning', 'letter-spacing', 'text-rendering', 'text-shadow']
    .map((k) => k + ':' + s.getPropertyValue(k)).join(';');
  for (const e of document.querySelectorAll('body *')) e.style.setProperty('visibility', 'hidden', 'important');
  const host = document.createElement('div');
  host.style.cssText = 'position:fixed;inset:0;visibility:visible;z-index:2147483647';
  const span = (text, css) => { const e = document.createElement('span'); e.style.cssText = style + ';white-space:pre;' + css;
    e.textContent = text; const probe = document.createElement('i');
    probe.style.cssText = 'display:inline-block;width:0;height:0;vertical-align:baseline'; e.append(probe); return e; };
  const glyphs = ${JSON.stringify(list)}.map((ch, i) => [0, 1, 2, 3].map((p) => {
    const e = span(ch, 'position:absolute;left:' + (20 + (i % ${perRow}) * ${4 * cell} + p * ${cell} + p / 4) + 'px;top:' +
                       (10 + Math.floor(i / ${perRow}) * ${row}) + 'px');
    host.append(e); return e; }));
  const top = 10 + Math.ceil(${list.length} / ${perRow}) * ${row};
  const lines = ${JSON.stringify(strings)}.map((text, i) => {
    const box = document.createElement('div');
    box.style.cssText = 'position:absolute;left:0;width:' + (300 + 400 * (i % 4)) + 'px;top:' + (top + Math.floor(i / 4) * ${row}) +
                        'px;display:flex;flex-direction:column;align-items:flex-end';
    const e = span(text, ''); box.append(e); host.append(box); return e; });
  document.body.append(host);
  await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
  const canvas = document.createElement('canvas').getContext('2d');
  canvas.font = s.getPropertyValue('font-style') + ' ' + s.getPropertyValue('font-weight') + ' ' + s.getPropertyValue('font-size') + ' ' + s.getPropertyValue('font-family');
  const at = (e) => { const r = e.getBoundingClientRect(); return {x: r.x, y: r.y, w: r.width, h: r.height, baseline: e.querySelector('i').getBoundingClientRect().y}; };
  return {font: {css: canvas.font, color: s.color, size: parseFloat(s.fontSize), line: parseFloat(s.lineHeight)},
          glyphs: glyphs.map((phases, i) => ({advance: canvas.measureText(${JSON.stringify(list)}[i]).width, phases: phases.map(at)})),
          strings: lines.map(at)};
})()`);
if (!layout) throw new Error(`nothing matches ${selector}`);
const shots = [];
for (const ground of ['#000', '#fff']) {
  await view.evaluate(`for (const e of [document.documentElement, document.body]) e.style.setProperty('background', '${ground}', 'important')`);
  shots.push(decode(await view.png()));
}
const grab = (x0, y0, w, h) => {
  const black = Buffer.alloc(w * h * 3), white = Buffer.alloc(w * h * 3);
  for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
    const b = shots[0].at(x0 + x, y0 + y), wh = shots[1].at(x0 + x, y0 + y);
    for (let k = 0; k < 3; ++k) { black[(y * w + x) * 3 + k] = b[k]; white[(y * w + x) * 3 + k] = wh[k]; }
  }
  return { black, white };
};
// Skia's pen grid: a glyph is drawn at floor(x + 1/8) with the quarter phase of x + 1/8, on the rounded baseline.
const pen = (x) => { const q = Math.floor(x * 65536) + 0x2000; return { whole: q >> 16, phase: (q >> 14) & 3 }; };
const result = { url, theme, selector, font: null, glyphs: {}, strings: [] };
const ink = ({ black, white }, w, h) => { // the box of pixels that differ from transparent
  let x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x)
    if ([0, 1, 2].some((k) => black[(y * w + x) * 3 + k] !== 0 || white[(y * w + x) * 3 + k] !== 255))
      { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
  return x1 < 0 ? null : { x0, y0, x1, y1 };
};
const crop = ({ black, white }, w, b) => {
  const cw = b.x1 - b.x0 + 1, ch = b.y1 - b.y0 + 1, out = { black: Buffer.alloc(cw * ch * 3), white: Buffer.alloc(cw * ch * 3) };
  for (let y = 0; y < ch; ++y) for (let x = 0; x < cw * 3; ++x) {
    out.black[y * cw * 3 + x] = black[((b.y0 + y) * w + b.x0) * 3 + x]; out.white[y * cw * 3 + x] = white[((b.y0 + y) * w + b.x0) * 3 + x];
  }
  return out;
};
let baseline = null;
list.forEach((ch, i) => {
  const glyph = layout.glyphs[i], phases = [];
  glyph.phases.forEach((box, p) => {
    const { whole, phase } = pen(box.x), line = Math.round(box.baseline);
    if (phase !== p) throw new Error(`${ch}: phase ${p} drawn at ${box.x} lands on phase ${phase}`);
    if (Math.ceil(glyph.advance * 64) / 64 !== box.w) throw new Error(`${ch}: advance ${glyph.advance}, box ${box.w}`);
    baseline ??= line - Math.round(box.y);
    // The glyph's own cell (the next phase is a cell to the right); ink at the window's edge stops the capture.
    const x0 = whole - cell / 4, y0 = Math.round(box.y) - 4, w = cell, h = row - 2, pixels = grab(x0, y0, w, h);
    const b = ink(pixels, w, h);
    if (!b) { phases.push({ x: 0, y: 0, w: 0, h: 0, black: '', white: '' }); return; }
    if (b.x0 === 0 || b.x1 === w - 1 || b.y0 === 0 || b.y1 === h - 1) throw new Error(`${ch}: ink reaches the cell's edge`);
    const c = crop(pixels, w, b);
    phases.push({ x: x0 + b.x0 - whole, y: y0 + b.y0 - line, w: b.x1 - b.x0 + 1, h: b.y1 - b.y0 + 1,
                  black: c.black.toString('base64'), white: c.white.toString('base64') });
  });
  result.glyphs[ch] = { advance: glyph.advance, phases };
});
result.font = { ...layout.font, baseline };
strings.forEach((text, i) => {
  const box = layout.strings[i], x0 = Math.floor(box.x) - 4, y0 = Math.round(box.y) - 4, w = Math.ceil(box.w) + 8;
  const h = Math.ceil(layout.font.line) + 8;
  const { black, white } = grab(x0, y0, w, h);
  result.strings.push({ text, x: box.x, baseline: Math.round(box.baseline), x0, y0, w, h,
                        black: black.toString('base64'), white: white.toString('base64') });
});
writeFileSync(out, JSON.stringify(result));
console.log(`${list.length} glyphs, ${strings.length} strings, baseline ${baseline} of a ${layout.font.line} px line, ${layout.font.css} ${layout.font.color}`);
await view.close(); session.close();
