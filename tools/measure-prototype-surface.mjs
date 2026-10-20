// SPDX-License-Identifier: GPL-3.0-or-later
// One surface of the approved prototype (prototypes/windows11-shell) as the native shell must draw it: its pixels and
// the geometry and style of every element under a selector, at 1920 x 1080, from the prototype's default settings
// (?fresh), light or dark. Uses a headless Chromium with --remote-debugging-port (tools/pw_prototype_capture.mjs) and
// the prototype served at BASE (the repository root, so its fonts resolve).
//
//   node tools/measure-prototype-surface.mjs <out dir> <open> <selector> [light|dark]
//     <open>     the prototype's ?open= names, comma separated, or '-' for the bare desktop
//     writes <out>/<open>-<theme>.png and <out>/<open>-<theme>.json: [{path, tag, cls, text, x, y, w, h, style}]
import { writeFileSync } from 'node:fs';
import { connect, decode, page } from './pw_prototype_capture.mjs';

const [out, open, selector, theme = 'light'] = process.argv.slice(2);
const base = process.env.PROTOTYPE_BASE ?? 'http://127.0.0.1:8778/prototypes/windows11-shell/index.html';
const url = `${base}?fresh&${theme}${open === '-' ? '' : `&open=${open}`}`;
const session = await connect(process.env.CDP_PORT ?? '9333');
const view = await page(session, url);
const elements = await view.evaluate(`(() => {
  const keep = ['backgroundColor','color','borderTopColor','borderTopWidth','borderRadius','fontFamily','fontSize','fontWeight',
                'lineHeight','opacity','boxShadow','backdropFilter','backgroundImage'];
  const path = (el) => { const parts = []; for (let e = el; e && e.nodeType === 1 && parts.length < 6; e = e.parentElement)
    parts.unshift(e.tagName.toLowerCase() + (e.id ? '#' + e.id : '') + (e.classList.length ? '.' + [...e.classList].join('.') : ''));
    return parts.join(' > '); };
  const each = ${JSON.stringify(selector)}.split(',').map((s) => s.trim());
  return [...document.querySelectorAll([...each, ...each.map((s) => s + ' *')].join(', '))].map((el) => {
    const r = el.getBoundingClientRect(), s = getComputedStyle(el), style = {};
    for (const k of keep) style[k] = s[k];
    const own = [...el.childNodes].filter((n) => n.nodeType === 3).map((n) => n.textContent.trim()).join(' ').trim();
    return { path: path(el), tag: el.tagName.toLowerCase(), cls: el.className?.baseVal ?? el.className, text: own,
             x: r.x, y: r.y, w: r.width, h: r.height, style };
  }).filter((e) => e.w > 0 && e.h > 0);
})()`);
const raw = await view.png(), shot = decode(raw);
const name = `${out}/${open === '-' ? 'desktop' : open.replace(/,/g, '+')}-${theme}`;
writeFileSync(`${name}.json`, JSON.stringify(elements, null, 1));
writeFileSync(`${name}.png`, raw);
await view.close(); session.close();
console.log(`${name}.png ${shot.width} wide, ${elements.length} elements`);

