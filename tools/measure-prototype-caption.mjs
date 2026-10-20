// SPDX-License-Identifier: GPL-3.0-or-later
// Measure the approved Windows 11 prototype's caption buttons, the reference the native theme's caption bitmaps
// are built from (tools/build-wine-theme.py, themes/windows11/caption/glyphs.json).
//
//   node tools/measure-prototype-caption.mjs <prototype url> <out.json> [chrome debugging port, default 9333]
//
// The prototype (PS5_Proton-design prototypes/windows11-shell) is served read-only, and a headless Chromium runs with
// --remote-debugging-port --window-size=1920,1080 --force-device-scale-factor=1. The page opens Calculator (a standard
// 32 px title bar) with transparency off, so the bar is a known solid colour, at full brightness (the prototype's
// screen filter would scale every pixel), in light and in dark, and maximized for the restore glyph.
//
// Each glyph's coverage is measured where the prototype draws it (10 x 10 at 18,11 in its 46 x 32 button):
// cov = |pixel - bar| / |glyph colour - bar|, once for dark ink (89.4% black on #f3f3f3) and once for light ink
// (white on #202020): Skia's antialiasing differs between the two (a max corner is 0.124 dark, 0.112 light), so a
// glyph uses the mask of its ink's luminance. Chromium's rasterizer is the reference, so the masks are data, not a
// re-render: librsvg differs by up to 66 levels on the close glyph's diagonals (measured 2026-10-16).
import { writeFileSync } from 'node:fs';
import { connect, page } from './pw_prototype_capture.mjs';

const [base, out, port = '9333'] = process.argv.slice(2);
if (!base || !out) { console.error('usage: measure-prototype-caption.mjs <prototype url> <out.json> [port]'); process.exit(2); }
const session = await connect(port);

async function capture(theme, maximize) {
  const tab = await page(session, `${base}?fresh&${theme}&open=calculator`);
  const buttons = await tab.evaluate(`(async () => {
    Shell.set('animations', false); Shell.set('transparency', false); Shell.set('brightness', 100); Shell.set('nightLight', false);
    const w = Shell.wm.list().pop(); if (${maximize}) w.maximize(); await new Promise((r) => setTimeout(r, 2500));
    return [...w.el.querySelectorAll('.window-caption-btn')].map((e) => { const r = e.getBoundingClientRect(); return [e.className.split(' ').pop(), r.x, r.y, r.width, r.height]; }); })()`);
  const { mouse, shot } = tab;
  const shots = {};
  await mouse('mouseMoved', 5, 5); shots.rest = await shot();
  for (const [kind, bx, by, bw, bh] of buttons) {
    const x = bx + bw / 2, y = by + bh / 2;
    await mouse('mouseMoved', x, y); shots[`hot-${kind}`] = await shot();
    await mouse('mousePressed', x, y); shots[`pushed-${kind}`] = await shot();
    await mouse('mouseMoved', 5, 5); await mouse('mouseReleased', 5, 5);  // released away: no click
  }
  await mouse('mousePressed', 960, 1060); await mouse('mouseReleased', 960, 1060);  // the taskbar takes activation
  await mouse('mouseMoved', 5, 5); shots.inactive = await shot();
  for (const [kind, bx, by, bw, bh] of buttons) { await mouse('mouseMoved', bx + bw / 2, by + bh / 2); shots[`inactive-hot-${kind}`] = await shot(); }
  await tab.close();
  return { buttons, shots };
}

// The eight msstyles images (normal, hot, pushed, disabled, then inactive) and the capture each one is: a disabled
// button draws as an inactive one at rest; pressing a button activates its window, so inactive pushed is pushed.
const IMAGES = [['rest', 0], ['hot', 1], ['pushed', 1], ['inactive', 0], ['inactive', 0], ['inactive-hot', 1], ['pushed', 1], ['inactive', 0]];
const BAR = { light: 243, dark: 32 }, INK = { light: 243 * 0.894, dark: 255 - 32 };  // |glyph colour - bar|
const masks = {}, targets = {}, checks = {};
const corner = (kind, maximize, x, y) => kind === 'close' && !maximize && x >= 38 && y < 8;  // the window's rounded corner
for (const theme of ['light', 'dark']) {
  for (const maximize of [false, true]) {
    const { buttons, shots } = await capture(theme, maximize);
    for (const [kind, bx, by, bw, bh] of buttons) {
      if (bw !== 46 || bh !== 32) throw new Error(`${theme} ${kind} button is ${bw}x${bh}, not 46x32`);
      const glyph = kind === 'max' && maximize ? 'restore' : kind;
      if (maximize !== (glyph === 'restore')) continue;
      targets[theme] ||= {};
      targets[theme][glyph] = IMAGES.map(([state, own]) => {
        const image = shots[own ? `${state}-${kind}` : state], plate = image.at(bx + 2, by + 28);
        let stray = 0;
        for (let y = 0; y < 32; y++) for (let x = 0; x < 46; x++)
          if (!(x >= 18 && x < 28 && y >= 11 && y < 21) && !corner(kind, maximize, x, y) && image.at(bx + x, by + y).some((v, k) => v !== plate[k])) stray++;
        if (stray) throw new Error(`${theme} ${glyph} ${state}: ${stray} pixels outside the glyph differ from the plate`);
        return { plate, glyph: Array.from({ length: 10 }, (_, y) => Array.from({ length: 10 }, (_, x) => image.at(bx + 18 + x, by + 11 + y))) };
      });
      const rest = shots.rest, ink = theme === 'light' ? 'dark_ink' : 'light_ink';
      (masks[ink] ||= {})[glyph] = Array.from({ length: 10 }, (_, y) => Array.from({ length: 10 }, (_, x) =>
        Math.min(255, Math.round(Math.abs(rest.at(bx + 18 + x, by + 11 + y)[0] - BAR[theme]) / INK[theme] * 255))));
      checks[`${theme}-${glyph}`] = { button: [bx, by, bw, bh], plates: targets[theme][glyph].map((t) => t.plate) };
    }
  }
}
writeFileSync(out, JSON.stringify({
  about: 'The Windows 11 prototype caption buttons at 96 DPI: 46 x 32, glyph 10 x 10 at 18,11. targets: per theme, glyph and msstyles image (normal, hot, pushed, disabled, then inactive) the plate colour and the glyph box pixels it renders over its reference bar (#f3f3f3 light, #202020 dark). glyphs: coverage 0-255 per ink luminance (dark ink on the light bar, light ink on the dark bar), for sizes the prototype was not measured at.',
  source: 'PS5_Proton-design prototypes/windows11-shell (js/wm.js GLYPH paths, css/window.css states), measured by tools/measure-prototype-caption.mjs',
  renderer: session.browser, measured: new Date().toISOString().slice(0, 10), checks, glyphs: masks, targets,
}) + '\n');
console.log(JSON.stringify({ renderer: session.browser, inks: Object.keys(masks), checks }));
session.close();
