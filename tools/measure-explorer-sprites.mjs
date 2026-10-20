// SPDX-License-Identifier: GPL-3.0-or-later
// The approved prototype's File Explorer chrome as its own pixels, for the native File Explorer (Wine patch 1150,
// wine/ps5/pw_shell_explorer.h): every element below is rendered alone (everything else hidden, its ancestors' and its
// own plate cleared unless asked) over black and over white, as tools/measure-prototype-sprites.mjs does, but in the
// state its group sets up (a location, a selection, the pointer over it, the window inactive) and over the box that
// covers its sub-pixel position (floor to ceil), so a sprite drawn at whole pixels reproduces Chromium's phase.
//
//   node tools/measure-explorer-sprites.mjs <out.json.gz> light|dark   (themes/windows11/explorer/explorer-<theme>.json.gz)
//     writes {theme, origin, sprites: {name: {x, y, w, h, box: [x, y, w, h], black, white}}}: x, y, w, h is the
//     captured region and box the element's exact box, both relative to the window's client origin (inside its 1 px
//     border, 411,167 on the 1920 x 1080 desktop); black and white are base64 RGB rows of the region.
import { readFileSync, writeFileSync } from 'node:fs';
import { gunzipSync, gzipSync } from 'node:zlib';
import { connect, decode, page, sleep } from './pw_prototype_capture.mjs';

const [out, theme = 'light'] = process.argv.slice(2);
const base = process.env.PROTOTYPE_BASE ?? 'http://127.0.0.1:8778/prototypes/windows11-shell/index.html';
const CMDS = ['new', 'cut', 'copy', 'paste', 'rename', 'share', 'delete', 'sort', 'view', 'more', 'details'];
const NAV = ['home', 'gallery', 'desktop', 'downloads', 'documents', 'pictures', 'music', 'videos', 'root', 'network'];
const LOCATIONS = ['home', 'gallery', 'desktop', 'downloads', 'documents', 'pictures', 'music', 'videos', 'root', 'network',
                   'recycle', 'f-shots', 'c'];
// [name, selector, options]: n = which match, hide = descendants to hide, own = keep the element's own plate,
// pointer = hover|press, margin = [left, top, right, bottom] around the cover box.
// The details pane (Alt+Shift+P) and a click on Documents' n-th item.
const PANE = ['js', `document.querySelector('.ex').dispatchEvent(new KeyboardEvent('keydown', { key: 'P', code: 'KeyP', altKey: true,
  shiftKey: true, bubbles: true }))`];
const pick = (n, ctrl = false) => ['js', `(() => { const r = document.querySelectorAll('.ex-row')[${n}]; const o = { bubbles: true, button: 0, ctrlKey: ${ctrl} };
  for (const k of ['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click']) r.dispatchEvent(k.startsWith('pointer') ? new PointerEvent(k, o) : new MouseEvent(k, o)); })()`];
// Documents' list emptied and the prototype's note in its place (details view's grid, or alone as the icon views show it).
const note = (text, icons) => ['js', `(() => { const dv = document.querySelector('.ex-dv'), n = document.createElement('div');
  n.className = 'ex-note'; n.textContent = ${JSON.stringify(text)};
  if (${icons}) dv.replaceWith(n); else { dv.querySelectorAll('.ex-row').forEach((r) => r.remove()); dv.append(n); } })()`];
const groups = [
  { name: 'home', open: 'explorer', sprites: [
    ['tab-home', '.ex-tab.selected', { own: true, margin: [9, 0, 9, 0] }],
    ['tab', '.ex-tab.selected', { own: true, margin: [9, 0, 9, 0], hide: '.ex-tab-icon, .ex-tab-title' }],
    ['tab-title-home', '.ex-tab-title', { margin: [2, 2, 2, 2] }],
    ['tab-add', '.ex-tab-add', {}], ['tab-add-hover', '.ex-tab-add', { own: true, pointer: 'hover' }],
    ['tab-add-press', '.ex-tab-add', { own: true, pointer: 'press' }],
    ['tab-close-hover', '.ex-tab-close', { own: true, pointer: 'hover' }],
    ['tab-close-press', '.ex-tab-close', { own: true, pointer: 'press' }],
    ...['min', 'max', 'close'].flatMap((k) => [[`cap-${k}`, `.window-caption-btn.${k}`, { own: true }],
      [`cap-${k}-hover`, `.window-caption-btn.${k}`, { own: true, pointer: 'hover' }],
      [`cap-${k}-press`, `.window-caption-btn.${k}`, { own: true, pointer: 'press' }]]),
    ['back-disabled', '.ex-iconbtn', { n: 0 }], ['forward-disabled', '.ex-iconbtn', { n: 1 }],
    ['up-disabled', '.ex-iconbtn', { n: 2 }], ['refresh', '.ex-iconbtn', { n: 3 }],
    ['iconbtn-hover', '.ex-iconbtn', { n: 3, own: true, pointer: 'hover', hide: '*' }],
    ['iconbtn-press', '.ex-iconbtn', { n: 3, own: true, pointer: 'press', hide: '*' }],
    ['address', '.ex-address', { own: true, hide: '*' }], ['address-hover', '.ex-address', { own: true, hide: '*', pointer: 'hover' }],
    ['crumb-chev', '.ex-crumb-chev', {}], ['crumb-home', '.ex-crumb', { margin: [0, 0, 1, 0] }],
    ['crumb-hover', '.ex-crumb', { own: true, pointer: 'hover', hide: '*' }],
    ['search', '.ex-search', { own: true, hide: '*' }], ['search-hover', '.ex-search', { own: true, hide: '*', pointer: 'hover' }],
    ['search-text-home', '.ex-search-input', {}], ['search-glyph', '.ex-search-glyph', {}],
    ...['new', 'cut', 'copy', 'paste', 'rename', 'share', 'delete'].map((k) => [`cmd-${k}-disabled`, `.ex-cmd-${k}`, {}]),
    ['cmd-sort', '.ex-cmd-sort', {}], ['cmd-view-details', '.ex-cmd-view', {}], ['cmd-more', '.ex-cmd-more', {}],
    ['cmd-details', '.ex-cmd-details', {}],
    ...CMDS.flatMap((k) => [[`cmd-${k}-hover`, `.ex-cmd-${k}`, { own: true, pointer: 'hover', hide: '*', enable: true }],
      [`cmd-${k}-press`, `.ex-cmd-${k}`, { own: true, pointer: 'press', hide: '*', enable: true }]]),
    ['sep-1', '.ex-cmd-sep', { n: 0, own: true }], ['sep-2', '.ex-cmd-sep', { n: 1, own: true }],
    ['sep-3', '.ex-cmd-sep.ex-sep-more', { own: true }],
    ...NAV.map((k) => [`nav-${k}`, `.ex-nav-item[data-loc="${k}"]`, {}]),
    ['nav-selected', '.ex-nav-item.selected', { own: true, hide: '*' }],
    ['nav-hover', '.ex-nav-item[data-loc="gallery"]', { own: true, hide: '*', pointer: 'hover' }],
    ['nav-press', '.ex-nav-item[data-loc="gallery"]', { own: true, hide: '*', pointer: 'press' }],
    ['nav-selected-hover', '.ex-nav-item.selected', { own: true, hide: '*', pointer: 'hover' }],
    ['nav-sep', '.ex-nav-sep', { own: true }],
    ['head-qa', '.ex-section-head', { n: 0 }], ['head-fav', '.ex-section-head', { n: 1 }],
    ['head-recent', '.ex-section-head', { n: 2 }],
    ['head-hover', '.ex-section-head', { n: 1, own: true, pointer: 'hover', hide: '*' }],
    ...[0, 1, 2, 3, 4, 5].map((i) => [`qa-${['desktop', 'downloads', 'documents', 'pictures', 'music', 'videos'][i]}`, '.ex-qa', { n: i }]),
    ['qa-hover', '.ex-qa', { n: 0, own: true, pointer: 'hover', hide: '*' }],
    ['fav-note', '.ex-note', { n: 0 }],
    ['recent-head', '.ex-dv-head', {}],
    ['status-details-disabled', '.ex-status-btn', { n: 0, own: true }], ['status-large-disabled', '.ex-status-btn', { n: 1, own: true }],
  ] },
  // Documents with its first item selected and a file on the clipboard: the commands enabled.
  { name: 'selection', open: 'explorer:documents', actions: [['click', '.ex-row'], ['js', "Shell.clipboard.set(['f-doc1'])"]],
    sprites: [...['new', 'cut', 'copy', 'paste', 'rename', 'delete'].map((k) => [`cmd-${k}`, `.ex-cmd-${k}`, {}]),
      ['status-details', '.ex-status-btn', { n: 0, own: true }], ['status-large', '.ex-status-btn', { n: 1, own: true }]] },
  // Documents, then This PC: back enabled; then back to Documents: forward and up enabled (back is disabled again there).
  { name: 'history', open: 'explorer:documents', actions: [['click', '.ex-nav-item[data-loc="root"]']],
    sprites: [['back', '.ex-iconbtn', { n: 0 }]] },
  { name: 'history-forward', open: 'explorer:documents', actions: [['click', '.ex-nav-item[data-loc="root"]'], ['click', '.ex-iconbtn']],
    sprites: [['forward', '.ex-iconbtn', { n: 1 }], ['up', '.ex-iconbtn', { n: 2 }]] },
  // Three tabs (Documents, then two new ones on Home, the last selected): an unselected tab with its separator (its
  // close button shows only under the pointer), and under the pointer.
  { name: 'tabs', open: 'explorer:documents', actions: [['js', "document.querySelector('.ex-tab-add').click()"],
      ['js', "document.querySelector('.ex-tab-add').click()"]],
    sprites: [['tab-unselected', '.ex-tab', { n: 0, own: true, margin: [0, 0, 1, 0], hide: '.ex-tab-icon, .ex-tab-title' }],
      ['tab-unselected-hover', '.ex-tab', { n: 0, own: true, margin: [0, 0, 1, 0], pointer: 'hover', hide: '.ex-tab-icon, .ex-tab-title' }]] },
  { name: 'inactive', open: 'explorer', actions: [['click-at', 200, 600]],
    sprites: [...['min', 'max', 'close'].map((k) => [`cap-${k}-inactive`, `.window-caption-btn.${k}`, { own: true }]),
      ['tab-add-inactive', '.ex-tab-add', {}]] },
  { name: 'maximized', open: 'explorer', actions: [['dblclick', '.window-titlebar-fill']],
    sprites: [['cap-restore', '.window-caption-btn.max', { own: true }],
      ['cap-restore-hover', '.window-caption-btn.max', { own: true, pointer: 'hover' }],
      ['cap-restore-press', '.window-caption-btn.max', { own: true, pointer: 'press' }]] },
  ...LOCATIONS.map((loc) => ({ name: `at-${loc}`, open: `explorer:${loc}`, actions: [PANE], sprites: [
    [`tab-icon-${loc}`, '.ex-tab-icon', {}], [`crumb-icon-${loc}`, '.ex-crumb-icon', {}], [`dp-loc-${loc}`, '.ex-dp-loc', {}]] })),
  // The details pane on Documents: nothing selected (its icon, the hint, Details checked), a folder, a file, two items.
  { name: 'dpane-none', open: 'explorer:documents', actions: [PANE], sprites: [['dp-hint', '.ex-dp-hint', {}],
    ['cmd-details-checked', '.ex-cmd-details', { own: true }], ['dp-divider', '.ex-dpane', { own: true, hide: '*', margin: [0, 0, -279, 0] }]] },
  { name: 'dpane-folder', open: 'explorer:documents', actions: [PANE, pick(0)], sprites: [['dp-icon-folder', '.ex-dp-head .ex-ico', {}],
    ['dp-title', '.ex-dp-title', {}], ['dp-key-contains', '.ex-dp-props dt', { n: 0 }], ['dp-key-modified', '.ex-dp-props dt', { n: 1 }],
    ['dp-key-path', '.ex-dp-props dt', { n: 2 }]] },
  { name: 'dpane-file', open: 'explorer:documents', actions: [PANE, pick(1)], sprites: [['dp-icon-document', '.ex-dp-head .ex-ico', {}],
    ['dp-key-size', '.ex-dp-props dt', { n: 0 }]] },
  { name: 'dpane-two', open: 'explorer:documents', actions: [PANE, pick(1), pick(2, true)], sprites: [['dp-key-total', '.ex-dp-props dt', { n: 0 }]] },
  // Filter: checked (a filter on), and the notes of an empty list.
  { name: 'filter-on', open: 'explorer:documents', actions: [['js', "document.querySelector('.ex-cmd-filter').classList.add('checked')"]],
    sprites: [['cmd-filter-checked', '.ex-cmd-filter', { own: true }]] },
  ...[['empty', 'This folder is empty.'], ['filtered', 'No items match your filter.']].flatMap(([key, text]) => [
    { name: `note-${key}`, open: 'explorer:documents', actions: [note(text, false)], sprites: [[`note-${key}`, '.ex-note', {}]] },
    { name: `note-${key}-icons`, open: 'explorer:documents', actions: [note(text, true)], sprites: [[`note-${key}-icons`, '.ex-note', {}]] }]),
  // Home with no file to list: the Recent section's note (the prototype's own markup for an empty list).
  { name: 'home-empty', open: 'explorer', actions: [['js', `(() => { const dv = document.querySelector('.ex-home .ex-dv');
      dv.querySelectorAll('.ex-row').forEach((r) => r.remove()); const n = document.createElement('div'); n.className = 'ex-note';
      n.textContent = "After you've opened some files, we'll show the most recent ones here."; dv.append(n); })()`]],
    sprites: [['recent-empty', '.ex-home .ex-dv .ex-note', {}]] },
  // A folder in details view, sorted by name: the column header, its cells, the sort glyph, Filter and See more.
  { name: 'folder', open: 'explorer:documents', sprites: [
    ['dv-head', '.ex-dv-head', {}], ['dv-name', '.ex-dv-hcell', { n: 0 }],
    ['dv-name-plain', '.ex-dv-hcell', { n: 0, hide: '.ex-dv-sort' }], ['dv-modified', '.ex-dv-hcell', { n: 1 }],
    ['dv-type', '.ex-dv-hcell', { n: 2 }], ['dv-size', '.ex-dv-hcell', { n: 3 }], ['dv-sort-up', '.ex-dv-sort', {}],
    ['dv-hover', '.ex-dv-hcell', { n: 1, own: true, pointer: 'hover', hide: '*' }],
    ['dv-press', '.ex-dv-hcell', { n: 1, own: true, pointer: 'press', hide: '*' }],
    ['cmd-filter', '.ex-cmd-filter', {}], ['cmd-filter-hover', '.ex-cmd-filter', { own: true, pointer: 'hover', hide: '*' }],
    ['cmd-filter-press', '.ex-cmd-filter', { own: true, pointer: 'press', hide: '*' }],
    ['cmd-more-folder', '.ex-cmd-more', {}], ['cmd-more-folder-hover', '.ex-cmd-more', { own: true, pointer: 'hover', hide: '*' }],
    ['cmd-more-folder-press', '.ex-cmd-more', { own: true, pointer: 'press', hide: '*' }],
    ['sep-3-folder', '.ex-cmd-sep.ex-sep-more', { own: true }]] },
  { name: 'folder-disabled-filter', open: 'explorer:documents', actions: [['js', "document.querySelector('.ex-cmd-filter').disabled = true"]],
    sprites: [['cmd-filter-disabled', '.ex-cmd-filter', {}]] },
  { name: 'folder-descending', open: 'explorer:documents', actions: [['click', '.ex-dv-hcell']],
    sprites: [['dv-sort-down', '.ex-dv-sort', {}]] },
  ...[['2', 'large'], ['4', 'small'], ['5', 'list']].map(([digit, view]) => ({ name: `view-${view}`, open: 'explorer:documents',
    actions: [['js', `document.querySelector('.ex').dispatchEvent(new KeyboardEvent('keydown', { key: '${digit}', code: 'Digit${digit}',
      ctrlKey: true, shiftKey: true, bubbles: true }))`]],
    sprites: [[`cmd-view-${view}`, '.ex-cmd-view', {}],
      ...(view === 'large' ? [['status-details-plain', '.ex-status-btn', { n: 0, own: true }],
                              ['status-large-checked', '.ex-status-btn', { n: 1, own: true }]] : []),
      // The items' icons (Documents lists a folder, then files): 96 px in large icons, 16 px in the others.
      ...(view !== 'list' ? [[`icon-folder-${view === 'large' ? 96 : 16}`, '.ex-tile .ex-ico', { n: 0 }],
                             [`icon-document-${view === 'large' ? 96 : 16}`, '.ex-tile .ex-ico', { n: 1 }]] : [])] })),
];

const session = await connect(process.env.CDP_PORT ?? '9333');
// GROUPS=a,b measures those groups again and keeps every other sprite already in <out>.
const only = process.env.GROUPS?.split(',');
const result = only ? JSON.parse(gunzipSync(readFileSync(out))) : { theme, origin: null, sprites: {} };
const still = `(() => { const s = document.createElement('style'); s.id = 'pw-still';
  s.textContent = '*{transition:none!important;animation:none!important;caret-color:transparent!important}';
  document.head.appendChild(s); })()`;
for (const group of groups.filter((g) => !only || only.includes(g.name))) {
  const view = await page(session, `${base}?fresh&${theme}&open=${group.open}`);
  await view.evaluate(still);
  const rect = (selector, n = 0) => view.evaluate(`(() => { const e = document.querySelectorAll(${JSON.stringify(selector)})[${n}];
    if (!e) return null; const r = e.getBoundingClientRect(); return [r.x, r.y, r.width, r.height]; })()`);
  const press = async (x, y, count = 1) => {
    await view.mouse('mouseMoved', x, y);
    for (let c = 1; c <= count; ++c) {
      await view.mouse('mousePressed', x, y); await view.mouse('mouseReleased', x, y);
    }
    await sleep(400);
  };
  for (const [kind, a, b] of group.actions ?? []) {
    if (kind === 'js') await view.evaluate(a);
    else if (kind === 'click-at') await press(a, b);
    else { const r = await rect(a); if (!r) throw new Error(`${group.name}: no ${a}`); await press(r[0] + r[2] / 2, r[1] + r[3] / 2, kind === 'dblclick' ? 2 : 1); }
    if (kind === 'dblclick') await view.evaluate(`document.querySelector('.window-titlebar-fill').dispatchEvent(new MouseEvent('dblclick', {bubbles: true}))`);
    await sleep(300);
  }
  for (const [name, selector, o] of group.sprites) {
    // The window's client origin, read for every sprite: a window may still be settling after its group's actions.
    const frame = await rect('.window');
    const origin = [frame[0] + 1, frame[1] + 1];
    if (group.name === 'home') result.origin = origin;
    const n = o.n ?? 0, m = o.margin ?? [0, 0, 0, 0];
    const r = await rect(selector, n);
    if (!r) throw new Error(`${name}: nothing matches ${selector}`);
    const x0 = Math.floor(r[0]) - m[0], y0 = Math.floor(r[1]) - m[1];
    const w = Math.ceil(r[0] + r[2]) + m[2] - x0, h = Math.ceil(r[1] + r[3]) + m[3] - y0;
    if (o.enable) await view.evaluate(`document.querySelectorAll(${JSON.stringify(selector)})[${n}].disabled = false`);
    if (o.pointer) {
      await view.mouse('mouseMoved', r[0] + r[2] / 2, r[1] + r[3] / 2);
      if (o.pointer === 'press') await view.mouse('mousePressed', r[0] + r[2] / 2, r[1] + r[3] / 2);
      await sleep(200);
    }
    const shots = [];
    for (const ground of ['#000', '#fff']) {
      // Only the element's subtree paints; the inline styles changed here are restored after both captures.
      await view.evaluate(`(() => {
        const el = document.querySelectorAll(${JSON.stringify(selector)})[${n}];
        const keep = new Set(); for (let e = el.parentElement; e; e = e.parentElement) keep.add(e);
        const touched = window.__pwTouched = window.__pwTouched || new Map();
        const set = (e, k, v) => { if (!touched.has(e)) touched.set(e, e.getAttribute('style')); e.style.setProperty(k, v, 'important'); };
        for (const e of document.querySelectorAll('body *')) {
          if (keep.has(e) || (e === el && ${!o.own})) for (const [k, v] of [['background', 'transparent'], ['box-shadow', 'none'],
              ['border-color', 'transparent'], ['backdrop-filter', 'none'], ['-webkit-backdrop-filter', 'none']]) set(e, k, v);
          else if (e !== el && !el.contains(e)) set(e, 'visibility', 'hidden');
        }
        ${o.hide ? `for (const e of el.querySelectorAll(${JSON.stringify(o.hide)})) set(e, 'visibility', 'hidden');` : ''}
        for (const e of [document.documentElement, document.body]) set(e, 'background', '${ground}');
      })()`);
      shots.push(decode(await view.png({ x: x0, y: y0, width: w, height: h })));
    }
    await view.evaluate(`(() => { for (const [e, s] of window.__pwTouched) s == null ? e.removeAttribute('style') : e.setAttribute('style', s);
      window.__pwTouched = new Map(); })()`);
    // A press is released where it was, with every click swallowed: a caption button would close the window, and
    // moving away while pressed would drag it.
    if (o.pointer === 'press') {
      await view.evaluate(`window.__pwSwallow = (e) => { e.stopImmediatePropagation(); e.preventDefault(); };
        for (const k of ['click', 'pointerup', 'mouseup', 'dblclick']) window.addEventListener(k, window.__pwSwallow, true)`);
      await view.mouse('mouseReleased', r[0] + r[2] / 2, r[1] + r[3] / 2); await sleep(100);
      await view.evaluate(`for (const k of ['click', 'pointerup', 'mouseup', 'dblclick']) window.removeEventListener(k, window.__pwSwallow, true)`);
    }
    if (o.pointer) { await view.mouse('mouseMoved', 1, 1); await sleep(200); }
    const moved = await rect('.window');
    if (Math.abs(moved[0] - frame[0]) > 0.5 || Math.abs(moved[1] - frame[1]) > 0.5) throw new Error(`${name}: the window moved`);
    const black = Buffer.alloc(w * h * 3), white = Buffer.alloc(w * h * 3);
    for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
      const pb = shots[0].at(x, y), pw = shots[1].at(x, y);
      for (let k = 0; k < 3; ++k) { black[(y * w + x) * 3 + k] = pb[k]; white[(y * w + x) * 3 + k] = pw[k]; }
    }
    result.sprites[name] = { x: x0 - origin[0], y: y0 - origin[1], w, h,
      box: [r[0] - origin[0], r[1] - origin[1], r[2], r[3]].map((v) => Math.round(v * 100) / 100),
      black: black.toString('base64'), white: white.toString('base64') };
    console.log(`${name}: ${w}x${h} at ${x0 - origin[0]},${y0 - origin[1]}`);
  }
  await view.close();
}
writeFileSync(out, out.endsWith('.gz') ? gzipSync(JSON.stringify(result), { level: 9 }) : JSON.stringify(result));
session.close();
