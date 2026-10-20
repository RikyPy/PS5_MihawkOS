// SPDX-License-Identifier: GPL-3.0-or-later
// Measure the approved Windows 11 prototype's classic check boxes, radio buttons and push buttons (its Windows
// controls gallery, js/apps/controls.js and css/apps/controls.css), the reference the managed theme's
// Button.Checkbox, Button.Radiobutton and Button.Pushbutton bitmaps are built from (tools/build-wine-theme.py,
// themes/windows11/controls/controls.json).
//
//   node tools/measure-prototype-controls.mjs <prototype url> <out.json> [chrome debugging port, default 9333]
//
// The gallery's state matrices (unchecked, checked and mixed check boxes, unchecked and checked radio buttons;
// normal, hot, pressed and disabled) are moved, as they are, to whole-pixel positions, and six empty push buttons
// in the gallery's classes are added at the theme bitmap's 22 x 23 (normal, hot, pressed, disabled, default twice;
// light, and dark as the dark Explorer theme draws them),
// over black and then over white: each pixel's straight colour and alpha follow from the two
// (alpha = 1 - (white - black) / 255).
import { writeFileSync } from 'node:fs';
import { connect, page } from './pw_prototype_capture.mjs';

const [base, out, port = '9333'] = process.argv.slice(2);
if (!base || !out) { console.error('usage: measure-prototype-controls.mjs <prototype url> <out.json> [port]'); process.exit(2); }
const session = await connect(port);
const tab = await page(session, `${base}?fresh&light&open=controls-buttons`);
const specimens = await tab.evaluate(`(async () => {
  Shell.set('animations', false); Shell.set('brightness', 100); Shell.set('nightLight', false);
  await new Promise((r) => setTimeout(r, 1500));
  const plate = document.createElement('div');
  plate.className = 'w32';
  plate.id = 'pw-measure';
  plate.style.cssText = 'position:fixed;left:100px;top:100px;width:400px;height:300px;z-index:2147483647;background:#000';
  document.body.append(plate);
  const tables = [...document.querySelectorAll('table.w32-matrix')];
  const out = [];
  tables.forEach((table, t) => [...table.querySelectorAll('td')].forEach((cell, i) => {
    const check = cell.firstElementChild;
    check.style.cssText = 'position:absolute;left:' + (20 + (i % 4) * 40) + 'px;top:' + (20 + t * 140 + Math.floor(i / 4) * 40) + 'px;transition:none';
    plate.append(check);
  }));
  const dark = document.createElement('div');
  dark.className = 'w32-dark';
  plate.append(dark);
  for (const [row, parent] of [[0, plate], [1, dark]])
    ['', 'is-hot', 'is-pressed', 'is-disabled', 'default', 'default'].forEach((cls, i) => {
      const button = document.createElement('button');
      button.className = 'w32-btn ' + cls;
      button.disabled = cls === 'is-disabled';
      button.style.cssText = 'position:absolute;left:' + (220 + i * 30) + 'px;top:' + (20 + row * 40) + 'px;width:22px;min-width:0;padding:0;transition:none';
      parent.append(button);
    });
  await new Promise((r) => setTimeout(r, 500));
  const rect = (e) => { const r = e.getBoundingClientRect(); return [r.x, r.y, r.width, r.height]; };
  const byPosition = (a, b) => a[1] - b[1] || a[0] - b[0];  // light row first, then dark, each left to right
  return { boxes: [...plate.querySelectorAll('.w32-box')].map(rect).sort(byPosition), buttons: [...plate.querySelectorAll('button.w32-btn')].map(rect).sort(byPosition) };
})()`);
const { boxes, buttons } = specimens;
if (boxes.length !== 20 || boxes.some(([x, y, w, h]) => x % 1 || y % 1 || w !== 13 || h !== 13)) throw new Error('boxes not at whole pixels: ' + JSON.stringify(boxes));
if (buttons.length !== 12 || buttons.some(([x, y, w, h]) => x % 1 || y % 1 || w !== 22 || h !== 23)) throw new Error('buttons not 22 x 23 at whole pixels: ' + JSON.stringify(buttons));
const black = await tab.shot();
await tab.evaluate(`document.getElementById('pw-measure').style.background = '#fff'`);
const white = await tab.shot();
await tab.close();
const NAMES = ['unchecked', 'checked', 'mixed', 'radio-unchecked', 'radio-checked'], STATES = ['normal', 'hot', 'pushed', 'disabled'];
const pixels = (bx, by, width, height) => Array.from({ length: height }, (_, y) => Array.from({ length: width }, (_, x) => {
  const k = black.at(bx + x, by + y), w = white.at(bx + x, by + y);
  const alpha = 255 - Math.round((w[0] - k[0] + w[1] - k[1] + w[2] - k[2]) / 3);
  return alpha ? [...k.map((v) => Math.min(255, Math.round(v * 255 / alpha))), alpha] : [0, 0, 0, 0];
}));
const images = {};
boxes.forEach(([bx, by], i) => { images[`${NAMES[Math.floor(i / 4)]}-${STATES[i % 4]}`] = pixels(bx, by, 13, 13); });
['normal', 'hot', 'pushed', 'disabled', 'defaulted', 'defaulted-animating'].forEach((state, i) => {
  images[`pushbutton-${state}`] = pixels(buttons[i][0], buttons[i][1], 22, 23);
  images[`pushbutton-dark-${state}`] = pixels(buttons[6 + i][0], buttons[6 + i][1], 22, 23);  // DarkMode_Explorer
});
writeFileSync(out, JSON.stringify({
  about: 'The Windows 11 prototype\'s classic check boxes and radio buttons (13 x 13) and push buttons (22 x 23) at 96 DPI, straight RGBA per pixel, per state.',
  source: 'PS5_Proton-design prototypes/windows11-shell (js/apps/controls.js matrices, css/apps/controls.css), measured by tools/measure-prototype-controls.mjs',
  renderer: session.browser, measured: new Date().toISOString().slice(0, 10), images,
}) + '\n');
console.log(JSON.stringify({ renderer: session.browser, images: Object.keys(images).length }));
session.close();
