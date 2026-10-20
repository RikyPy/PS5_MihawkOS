// SPDX-License-Identifier: GPL-3.0-or-later
// MihawkBrowser as the approved prototype draws it (prototypes/windows11-shell/js/apps/browser.js, approved 2026-10-16):
// every named part's box in client coordinates (the window body's top-left, under the 32 px title bar and inside the
// 1 px border), its type and colours, and the composited pixels the native window (src/pw_browser.c) must show, for
// each state in light and dark at 1920 x 1080. Uses a headless Chromium with --remote-debugging-port
// (tools/pw_prototype_capture.mjs) and the prototype served at PROTOTYPE_BASE (the repository root).
//
//   node tools/measure-browser-prototype.mjs <out.json> [png dir]
//     writes the reference (docs/windows11-reference/mihawk-browser.json) and, with a png dir, each state's capture
import { writeFileSync } from 'node:fs';
import { connect, decode, page, sleep } from './pw_prototype_capture.mjs';

const [out, pngs] = process.argv.slice(2);
const base = process.env.PROTOTYPE_BASE ?? 'http://127.0.0.1:8778/prototypes/windows11-shell/index.html';
const STATES = { start: 'browser', page: 'browser-page', loading: 'browser-loading', error: 'browser-error', downloads: 'browser-downloads' };

// In the page: {frame, client, parts: {name: {x, y, w, h, text?, lines?, style}}} with boxes relative to the client.
const MEASURE = `(() => {
  const root = document.querySelector('.br'), c = root.getBoundingClientRect(), frame = root.closest('.window-frame').getBoundingClientRect();
  const box = (r) => ({ x: +(r.x - c.x).toFixed(2), y: +(r.y - c.y).toFixed(2), w: +r.width.toFixed(2), h: +r.height.toFixed(2) });
  const keep = ['backgroundColor', 'color', 'borderTopColor', 'borderBottomColor', 'borderTopWidth', 'borderBottomWidth',
                'borderRadius', 'fontSize', 'fontWeight', 'lineHeight'];
  const parts = {};
  const add = (name, el) => {
    if (!el || !el.getClientRects().length) return;
    const s = getComputedStyle(el), style = {};
    for (const k of keep) style[k] = s[k];
    const part = { ...box(el.getBoundingClientRect()), style };
    const text = [...el.childNodes].filter((n) => n.nodeType === 3 && n.textContent.trim());
    if (text.length) {  // each line's ink box: where the words sit, which the element's box does not say
      const range = document.createRange();
      range.selectNodeContents(el);
      part.text = el.textContent;
      part.lines = [...range.getClientRects()].filter((r) => r.width > 0).map(box);
    }
    if (el.placeholder) part.placeholder = el.placeholder;
    if (el.value) part.value = el.value;
    if (el.disabled) part.disabled = true;
    if (el.getAttribute('aria-label')) part.label = el.getAttribute('aria-label');
    parts[name] = part;
  };
  const q = (s) => root.querySelector(s), qa = (s) => [...root.querySelectorAll(s)];
  add('commands', q('.br-commands'));
  ['back', 'forward', 'reload'].forEach((n, i) => { add(n, q('.br-commands > button:nth-of-type(' + (i + 1) + ')')); add(n + '-glyph', q('.br-commands > button:nth-of-type(' + (i + 1) + ') .icon')); });
  add('address', q('.br-address'));
  add('lock', q('.br-lock:not([hidden]) .icon'));
  add('downloads', q('.br-commands > button:last-of-type'));
  add('downloads-glyph', q('.br-commands > button:last-of-type .icon'));
  add('downloads-progress', q('.br-dl-bar:not([hidden])'));
  add('view', q('.br-view'));
  add('start-text', q('.br-start:not([hidden]) .br-start-text'));
  qa('.br-start:not([hidden]) .br-launcher').forEach((el, i) => add('launcher-' + i, el));
  add('page', q('.br-page:not([hidden])'));
  add('error-title', q('.br-error:not([hidden]) .br-error-title'));
  add('error-detail', q('.br-error:not([hidden]) .br-error-detail'));
  add('flyout', q('.br-downloads:not([hidden])'));
  add('flyout-head', q('.br-downloads:not([hidden]) .br-dl-head'));
  qa('.br-downloads:not([hidden]) .br-dl-row').forEach((row, i) => {
    add('row-' + i, row);
    add('row-' + i + '-icon', row.querySelector('.br-dl-icon'));
    add('row-' + i + '-name', row.querySelector('.br-dl-name'));
    add('row-' + i + '-progress', row.querySelector('.br-dl-progress'));
    add('row-' + i + '-status', row.querySelector('.br-dl-status'));
    [...row.querySelectorAll('.br-dl-actions .btn')].forEach((b, k) => add('row-' + i + '-action-' + k, b));
  });
  return { frame: { x: frame.x, y: frame.y, w: frame.width, h: frame.height }, client: { x: c.x, y: c.y, w: c.width, h: c.height }, parts };
})()`;

// Composited pixels worth comparing, as client points: a surface's fill away from its text, its top and bottom strokes.
function samples(parts) {
  const at = {};
  const point = (name, x, y) => { at[name] = [Math.round(x), Math.round(y)]; };
  if (parts.view) point('surface', parts.view.x + 8, parts.view.y + parts.view.h - 8);
  point('commands', 2, 20);
  for (const [name, p] of Object.entries(parts)) {
    if (!/^(address|launcher-\d|row-\d-action-\d|flyout|row-\d-progress|downloads-progress)$/.test(name)) continue;
    if (/progress/.test(name)) { point(name + '.track', p.x + p.w - 2, p.y + 1); point(name + '.fill', p.x + 2, p.y + 1); continue; }
    point(name + '.fill', p.x + (name === 'address' ? p.w - 8 : 4), p.y + p.h / 2);
    point(name + '.top', p.x + p.w / 2, p.y);
    point(name + '.bottom', p.x + p.w / 2, p.y + p.h - 1);
  }
  if (parts['row-1']) point('row-1.divider', parts['row-1'].x + 40, parts['row-1'].y);
  return at;
}

const session = await connect(process.env.CDP_PORT ?? '9333');
const reference = {
  about: 'MihawkBrowser in the approved prototype (owner approval 2026-10-16: surface, Downloads flyout, taskbar pin after ' +
         'Terminal, icon B3). Boxes are CSS pixels at 96 DPI in client coordinates: x, y from the window body\'s top-left ' +
         '(below the 32 px title bar, inside the 1 px border). pixels are composited RGB at client points [x, y]; hover and ' +
         'press are the pointer over the part. Regenerate with tools/measure-browser-prototype.mjs.',
  chromium: session.browser, viewport: [1920, 1080], font: null, states: {},
};
for (const theme of ['light', 'dark']) {
  for (const [state, open] of Object.entries(STATES)) {
    const view = await page(session, `${base}?fresh&${theme}&open=${open}`);
    await sleep(state === 'downloads' ? 700 : 200);  // the finished download lands on the first 250 ms tick
    const m = await view.evaluate(MEASURE);
    reference.font ??= await view.evaluate("getComputedStyle(document.querySelector('.br')).fontFamily");
    const raw = await view.png(), shot = decode(raw);
    if (pngs) writeFileSync(`${pngs}/browser-${state}-${theme}.png`, raw);
    const pixel = ([x, y]) => shot.at(Math.round(m.client.x + x), Math.round(m.client.y + y));
    const pixels = Object.fromEntries(Object.entries(samples(m.parts)).map(([k, p]) => [k, { at: p, rgb: pixel(p) }]));
    for (const name of state === 'start' ? ['reload', 'launcher-0'] : []) {  // hover and press on a command and a launcher
      const p = m.parts[name], cx = m.client.x + p.x + p.w / 2, cy = m.client.y + p.y + p.h / 2, fill = [p.x + 3, p.y + p.h / 2];
      await view.mouse('mouseMoved', cx, cy);
      pixels[name + '.hover'] = { at: fill.map(Math.round), rgb: decode(await view.png()).at(Math.round(m.client.x + fill[0]), Math.round(m.client.y + fill[1])) };
      await view.mouse('mousePressed', cx, cy);
      pixels[name + '.press'] = { at: fill.map(Math.round), rgb: decode(await view.png()).at(Math.round(m.client.x + fill[0]), Math.round(m.client.y + fill[1])) };
      await view.mouse('mouseMoved', 5, 5);  // released away from it, so the launcher starts no download
      await view.mouse('mouseReleased', 5, 5);
    }
    reference.states[`${state}-${theme}`] = { open: `?fresh&${theme}&open=${open}`, frame: m.frame, client: m.client, parts: m.parts, pixels };
    await view.close();
    console.log(`${state}-${theme}: ${Object.keys(m.parts).length} parts, ${Object.keys(pixels).length} pixels`);
  }
}
session.close();
// One line per part and per pixel, so a state reads top to bottom.
const j = JSON.stringify, block = (o, pad) => `{\n${Object.entries(o).map(([k, v]) => `${pad} ${j(k)}: ${j(v)}`).join(',\n')}\n${pad}}`;
const states = Object.entries(reference.states).map(([name, st]) => `  ${j(name)}: {\n   "open": ${j(st.open)}, "frame": ${j(st.frame)}, "client": ${j(st.client)},\n` +
  `   "parts": ${block(st.parts, '   ')},\n   "pixels": ${block(st.pixels, '   ')}\n  }`);
writeFileSync(out, `{\n "about": ${j(reference.about)},\n "chromium": ${j(reference.chromium)}, "viewport": ${j(reference.viewport)}, "font": ${j(reference.font)},\n` +
  ` "states": {\n${states.join(',\n')}\n }\n}\n`);
