/* Start: the Windows 11 (23H2) Start menu that rises above the taskbar.
   Owns: Shell.start (open, close, toggle, isOpen, pins, pin, unpin, isPinned), the bare Win key
   and Ctrl+Escape, the Start flyout (search box that hands typing to Search, Pinned grid with
   pages, wheel paging and drag-to-reorder, Recommended, All apps with the letter grid, the
   account and power menus). Pins persist under `start-pins`, hidden recommendations under
   `start-rec-hidden`. js/search.js (same module) reuses the helpers on Shell.start._kit.
   Debug captures: ?open=start, start-allapps, start-recommended, start-power. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const rootStyle = () => getComputedStyle(document.documentElement);
  const cssPx = (name) => parseFloat(rootStyle().getPropertyValue(name)) || 0;
  const dur = (name) => cssPx(`--dur-${name}`); // normal 250, fast 167, faster 83
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  /* ---------- small DOM and geometry helpers (shared with search.js) ---------- */
  function h(tag, cls, props) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    for (const [k, v] of Object.entries(props || {})) {
      if (v == null || v === false) continue;
      if (k === 'text') el.textContent = v;
      else el.setAttribute(k, v === true ? '' : String(v));
    }
    return el;
  }

  /* Start and Search share one footprint: 600 x 566 at 1080p, centred above the taskbar
     (left-aligned with the taskbar's left alignment), clamped to the viewport. */
  function frame() {
    const vw = innerWidth, vh = innerHeight;
    const margin = vw < 640 ? 8 : 12;
    const w = Math.min(cssPx('--start-width') || 600, vw - 2 * margin);
    const ht = Math.min(cssPx('--start-height') || 566, vh - cssPx('--taskbar-height') - cssPx('--start-bottom-gap') - margin);
    const left = Shell.settings.taskbarAlignment === 'left' ? margin : Math.round((vw - w) / 2);
    return { w, h: ht, left };
  }
  function place(panel) {
    const f = frame();
    Object.assign(panel.style, { left: `${f.left}px`, width: `${f.w}px`, height: `${f.h}px` });
    panel.classList.toggle('is-narrow', f.w < 480);
    return f;
  }

  /* The host clips at the taskbar's top edge, so the flyout rises out of the taskbar. */
  function mountHost(panel) {
    const host = h('div', 'start-host');
    host.append(panel);
    Shell.layer('flyouts').append(host);
    return host;
  }

  /* Windows 11 Start motion: rises 40 px and fades in (250 ms, fast-out-slow-in); closes
     faster, sinking with slow-out-fast-in. */
  function rise(panel) {
    Shell.animate(panel, [{ opacity: 0 }, { opacity: 1 }], { duration: dur('fast'), easing: 'linear' });
    return Shell.animate(panel, [{ transform: 'translateY(40px)' }, { transform: 'none' }], { duration: dur('normal'), easing: Shell.ease.in });
  }
  function sink(panel) {
    panel.style.pointerEvents = 'none';
    Shell.animate(panel, [{ opacity: 1 }, { opacity: 0 }], { duration: dur('fast'), easing: 'linear', fill: 'forwards' });
    return Shell.animate(panel, [{ transform: 'none' }, { transform: 'translateY(40px)' }], { duration: dur('fast'), easing: Shell.ease.out, fill: 'forwards' });
  }

  /* Light-dismiss exemptions: the menus layer (our context menus) and a taskbar button, so
     clicking the Start or Search button toggles instead of closing then reopening. */
  const inMenus = (e) => !!Shell.layer('menus')?.contains(e.target);
  const onTaskbarButton = (name) => (e) => {
    const r = Shell.taskbar?.buttonRect?.(name);
    return !!r && r.width > 0 && e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom;
  };

  /* Arrow keys move focus to the nearest [data-nav] element in that direction (within a
     cone, so Right never jumps a row); with nothing focused yet, to `first`. */
  function spatial(e, scopes, first) {
    const step = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[e.key];
    if (!step) return false;
    const items = scopes.filter(Boolean).flatMap((s) => [...s.querySelectorAll('[data-nav]')])
      .filter((el) => !el.disabled && el.getClientRects().length);
    const cur = document.activeElement;
    e.preventDefault();
    if (!items.includes(cur)) { (first || items[0])?.focus({ preventScroll: true }); return true; }
    const a = cur.getBoundingClientRect();
    let best = null, bestScore = Infinity;
    for (const el of items) {
      if (el === cur) continue;
      const b = el.getBoundingClientRect();
      const dx = b.left + b.width / 2 - (a.left + a.width / 2), dy = b.top + b.height / 2 - (a.top + a.height / 2);
      const along = dx * step[0] + dy * step[1];
      const across = Math.abs(dx * step[1]) + Math.abs(dy * step[0]);
      if (along <= 1 || across > along * 1.5) continue;
      const score = along + 2 * across;
      if (score < bestScore) { bestScore = score; best = el; }
    }
    if (best) { best.focus({ preventScroll: true }); reveal(best); }
    return true;
  }

  /* Focus stays inside an open flyout until it is dismissed: Tab and Shift+Tab walk `stops`
     (in order) and wrap at either end. */
  const tabStops = (root) => [...root.querySelectorAll('button, input, [tabindex]')]
    .filter((el) => el.tabIndex >= 0 && !el.disabled && el.getClientRects().length);
  function cycleTab(e, stops) {
    if (e.key !== 'Tab' || e.ctrlKey || e.altKey || e.metaKey || !stops.length) return false;
    e.preventDefault();
    const i = stops.indexOf(document.activeElement), n = stops.length;
    const next = stops[i < 0 ? (e.shiftKey ? n - 1 : 0) : (i + (e.shiftKey ? n - 1 : 1)) % n];
    next.focus({ preventScroll: true });
    reveal(next);
    return true;
  }

  /* A group is one Tab stop (roving tabindex): the arrow keys move inside it, and Tab comes
     back to the item focused last. */
  function rove(group, sel, first = 0) {
    group.querySelectorAll(sel).forEach((el, i) => { el.tabIndex = i === first ? 0 : -1; });
    group.addEventListener('focusin', (e) => {
      const t = e.target.closest(sel);
      if (t) group.querySelectorAll(sel).forEach((el) => { el.tabIndex = el === t ? 0 : -1; });
    });
  }

  /* Scroll only the list that holds el (scrollIntoView would also scroll the clip host). */
  function reveal(el) {
    const box = el.closest('.st-list, .sr-list, .sr-preview, .sr-home');
    if (!box) return;
    const a = el.getBoundingClientRect(), b = box.getBoundingClientRect();
    if (a.top < b.top) box.scrollTop -= b.top - a.top;
    else if (a.bottom > b.bottom) box.scrollTop += a.bottom - b.bottom;
  }

  /* Context menus through the ui module; a keyboard-opened menu anchors to the element. */
  function showMenu(e, items) {
    if (!Shell.menu?.show) return;
    e.preventDefault();
    if (e.clientX || e.clientY) { Shell.menu.show({ x: e.clientX, y: e.clientY, items }); return; }
    const r = e.currentTarget.getBoundingClientRect();
    Shell.menu.show({ x: r.left, y: r.bottom, anchorRect: r, placement: 'bottom-start', items });
  }

  /* ---------- relative time and the file system walk ---------- */
  function ago(t) {
    const now = Shell.now(), d = now - t;
    if (d < 60e3) return 'Just now';
    if (d < 3600e3) return `${Math.floor(d / 60e3)}m ago`;
    const midnight = new Date(now).setHours(0, 0, 0, 0);
    if (t >= midnight) return `${Math.floor(d / 3600e3)}h ago`;
    const time = new Date(t).toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' });
    if (t >= midnight - 864e5) return `Yesterday at ${time}`;
    const opts = { month: 'short', day: 'numeric' };
    if (new Date(t).getFullYear() !== new Date(now).getFullYear()) opts.year = 'numeric';
    return new Date(t).toLocaleDateString('en-US', opts);
  }

  /* Everything reachable from This PC (the Recycle Bin is not). */
  function walk() {
    const out = [], queue = ['root'];
    while (queue.length) {
      for (const c of Shell.fs.children(queue.shift())) {
        out.push(c);
        if (c.kind === 'folder' || c.kind === 'drive') queue.push(c.id);
      }
    }
    return out;
  }
  let hiddenRec = new Set(Shell.store.get('start-rec-hidden', []));
  const recentFiles = () => walk().filter((i) => i.kind === 'file' && !hiddenRec.has(i.id)).sort((a, b) => b.modified - a.modified);

  /* ---------- pins ---------- */
  /* A pin ref is an app id or `fs:<item id>` (a game shortcut, a folder). Until the user
     changes them, pins are resolved at open time from the registry, so apps registered by
     modules that load later still appear: Explorer, Settings, the start-pinned apps, then the
     desktop's game shortcuts. */
  const isGame = (it) => it.kind === 'shortcut' && it.target === 'game';
  function defaultPins() {
    const first = ['explorer', 'settings'].filter((id) => Shell.apps.get(id));
    const rest = Shell.apps.list().filter((a) => a.startPinned && !first.includes(a.id)).map((a) => a.id);
    const games = Shell.fs.children('desktop').filter(isGame).map((it) => `fs:${it.id}`);
    return [...first, ...rest, ...games];
  }
  let customPins = Shell.store.get('start-pins', null);
  const pinRefs = () => (customPins ? [...customPins] : defaultPins());
  function savePins(refs) {
    customPins = refs;
    Shell.store.set('start-pins', refs);
    Shell.emit('start-pins', refs);
  }
  /* A desktop shortcut to a registered app pins the app itself, as Windows does. */
  function refOf(id) {
    if (Shell.apps.get(id)) return id;
    const it = Shell.fs.get(id);
    if (!it) return null;
    if (it.kind === 'shortcut' && it.target !== 'game' && Shell.apps.get(it.target)) return it.target;
    return `fs:${id}`;
  }
  function resolve(ref) {
    if (ref.startsWith('fs:')) {
      const it = Shell.fs.get(ref.slice(3));
      if (!it || Shell.fs.path(it.id)[0]?.id === 'recycle') return null;
      return { ref, name: it.name, icon: it.icon, item: it, open: () => Shell.fs.open(it.id) };
    }
    const app = Shell.apps.get(ref);
    return app ? { ref, name: app.name, icon: app.icon || 'object/app-generic', app, open: () => Shell.apps.launch(app.id) } : null;
  }
  const pins = () => pinRefs().map(resolve).filter(Boolean);

  /* All apps: every registered app (the bare `game` launcher is not an app) plus the games. */
  function allApps() {
    const list = Shell.apps.list().filter((a) => a.id !== 'game').map((a) => resolve(a.id));
    const seen = new Set(list.map((e) => e.name.toLowerCase()));
    for (const it of Shell.fs.children('desktop')) {
      if (isGame(it) && !seen.has(it.name.toLowerCase())) { seen.add(it.name.toLowerCase()); list.push(resolve(`fs:${it.id}`)); }
    }
    return list.sort((a, b) => a.name.localeCompare(b.name, 'en', { sensitivity: 'base' }));
  }
  const letterOf = (name) => { const c = name.charAt(0).toUpperCase(); return /[A-Z]/.test(c) ? c : /\d/.test(c) ? '#' : '&'; };

  /* The taskbar owns its pins; without a pin API, flip the registry flag it renders from. */
  function taskbarPinItem(app) {
    const pinned = !!app.pinned;
    return {
      label: pinned ? 'Unpin from taskbar' : 'Pin to taskbar',
      icon: pinned ? 'fluent/pin-off' : 'fluent/pin',
      action() {
        const fn = pinned ? Shell.taskbar?.unpin : Shell.taskbar?.pin;
        if (fn) { fn(app.id); return; }
        app.pinned = !pinned;
        Shell.emit('apps', Shell.apps.list());
      },
    };
  }
  const openLocation = (it) => Shell.apps.launch('explorer', { folder: it.parent, select: it.id });
  const settingsPage = (title, fallback) => (Shell.settingsIndex || []).find((s) => s.title === title)?.page || fallback;

  /* ---------- Start ---------- */
  let st = null;        // the open Start: {host, panel, search, main, sub, foot, view, page, cols, rows, cleanup}
  let handoff = false;  // closing because Search takes over the same footprint: no motion, no focus restore

  function open() {
    if (Shell.transient.isOpen('start')) return;
    const panel = h('section', 'st-panel acrylic-base surface-flyout', { role: 'dialog', 'aria-label': 'Start', tabindex: '-1' });
    const s = st = { panel, host: mountHost(panel), view: 'main', page: 0, cleanup: [], prev: document.activeElement };

    s.search = h('button', 'st-search', { 'data-nav': true, 'aria-label': 'Search' });
    s.search.append(Shell.icon('fluent/search', { size: 16 }), h('span', 'st-search-text', { text: 'Search for apps, settings, and documents' }));
    s.search.addEventListener('click', () => toSearch(''));

    const stage = h('div', 'st-stage');
    s.main = h('div', 'st-view st-main');
    s.sub = h('div', 'st-view st-sub', { hidden: true });
    stage.append(s.main, s.sub);
    s.foot = footer(s);
    panel.append(s.search, stage, s.foot);

    layout(s);
    renderMain(s);

    const onKey = (e) => {
      if (e.defaultPrevented || Shell.layer('menus')?.childElementCount) return; // a menu owns the keyboard
      if (e.key === 'Escape') { e.preventDefault(); close(); return; }
      if (e.key.length === 1 && e.key !== ' ' && !e.ctrlKey && !e.altKey && !e.metaKey) { e.preventDefault(); toSearch(e.key); return; }
      if (cycleTab(e, startStops(s))) return;
      const view = s.view === 'main' ? s.main : s.sub;
      spatial(e, [view, s.search, s.foot], view.querySelector('.st-pin, .st-app, .st-rec-item, .st-letter-cell:not(:disabled)'));
    };
    const onResize = () => { layout(s); if (s.view === 'main') renderMain(s); };
    window.addEventListener('keydown', onKey);
    window.addEventListener('resize', onResize);
    s.cleanup.push(() => window.removeEventListener('keydown', onKey), () => window.removeEventListener('resize', onResize));

    Shell.transient.open('start', { close: teardown, elements: [panel, inMenus, onTaskbarButton('start')], group: 'flyout' });
    panel.focus({ preventScroll: true });
    rise(panel);
  }

  function close() { Shell.transient.close('start'); }

  function teardown() {
    const s = st;
    st = null;
    if (!s) return;
    s.cleanup.forEach((fn) => fn());
    const hadFocus = s.panel.contains(document.activeElement);
    if (handoff) { handoff = false; s.host.remove(); return; }
    sink(s.panel).then(() => s.host.remove());
    if (hadFocus && s.prev && s.prev !== document.body && s.prev.isConnected) s.prev.focus({ preventScroll: true });
  }

  /* Windows visits a group before its header button: search, Pinned, All apps, Recommended,
     More, user, power. */
  function startStops(s) {
    const list = tabStops(s.panel);
    for (const btn of s.main.querySelectorAll('.st-head .st-btn')) {
      const group = btn.parentElement.nextElementSibling;
      const last = list.filter((el) => group?.contains(el)).at(-1);
      if (!last || !list.includes(btn)) continue;
      list.splice(list.indexOf(btn), 1);
      list.splice(list.indexOf(last) + 1, 0, btn);
    }
    return list;
  }

  /* Typing in Start, or clicking its box, turns Start into Search in the same place. */
  function toSearch(query) {
    if (!Shell.search?.open) return;
    handoff = true;
    close();
    Shell.search.open({ query, from: 'start' });
  }

  function launch(entry) {
    close();
    entry.open();
  }

  /* Columns and rows follow the panel: 6 x 3 at full size, fewer columns on a phone-width
     viewport, two rows when the viewport is too short for three. */
  function layout(s) {
    const f = place(s.panel);
    const pinW = cssPx('--start-pin-width') || 91;
    const maxCols = cssPx('--start-columns') || 6;
    const pad = f.w >= maxCols * pinW + 40 ? Math.round((f.w - maxCols * pinW) / 2) : 12;
    s.cols = clamp(Math.floor((f.w - 2 * pad) / 78), 3, maxCols);
    s.rows = f.h >= 520 ? 3 : 2;
    s.panel.style.setProperty('--cols', s.cols);
    s.panel.style.setProperty('--rows', s.rows);
    s.panel.style.setProperty('--grid-pad', `${pad}px`);
  }

  function headRow(title, button) {
    const row = h('div', 'st-head');
    row.append(h('h2', 'st-head-title', { text: title }), button);
    return row;
  }
  function smallButton(label, glyph, before, onClick) {
    const b = h('button', 'st-btn', { 'data-nav': true });
    const icon = Shell.icon(`fluent/${glyph}`, { size: 12 });
    const text = h('span', '', { text: label });
    if (before) b.append(icon, text); else b.append(text, icon);
    b.addEventListener('click', onClick);
    return b;
  }

  function renderMain(s) {
    const { main } = s;
    main.textContent = '';
    const list = pins();
    const per = s.cols * s.rows;
    const pages = Math.max(1, Math.ceil(list.length / per));
    s.page = clamp(s.page, 0, pages - 1);

    const pinned = h('div', 'st-pinned');
    const viewport = h('div', 'st-pages');
    const track = h('div', 'st-track');
    for (let p = 0; p < pages; p++) {
      const grid = h('div', 'st-grid', { 'aria-label': `Pinned apps, page ${p + 1} of ${pages}` });
      list.slice(p * per, (p + 1) * per).forEach((e) => grid.append(pinButton(s, e)));
      wirePinDrag(s, grid, p);
      track.append(grid);
    }
    track.style.transform = `translateY(${-s.page * 100 / pages}%)`;
    viewport.append(track);
    pinned.append(viewport);
    rove(track, '.st-pin', s.page * per);

    if (pages > 1) {
      const pips = h('div', 'st-pips', { 'aria-label': 'Pages' });
      for (let p = 0; p < pages; p++) {
        const pip = h('button', 'st-pip', { 'aria-label': `Page ${p + 1}`, 'aria-current': p === s.page ? 'true' : 'false' });
        pip.addEventListener('click', () => goPage(s, track, pips, p, pages));
        pips.append(pip);
      }
      pinned.append(pips);
      let lastWheel = 0;
      viewport.addEventListener('wheel', (e) => {
        e.preventDefault();
        if (performance.now() - lastWheel < 400 || !e.deltaY) return;
        lastWheel = performance.now();
        goPage(s, track, pips, s.page + Math.sign(e.deltaY), pages);
      }, { passive: false });
    }

    main.append(headRow('Pinned', smallButton('All apps', 'chevron-right', false, (e) => showSub(s, 'all', e.detail === 0))), pinned);

    const files = recentFiles().slice(0, 6);
    if (files.length) {
      const rec = h('div', 'st-rec');
      files.forEach((it) => rec.append(recItem(s, it)));
      rove(rec, '.st-rec-item');
      main.append(headRow('Recommended', smallButton('More', 'chevron-right', false, (e) => showSub(s, 'recommended', e.detail === 0))), rec);
      fitRecommended(s, rec);
    }
  }

  /* Show only the recommended rows that fit; with no room, drop the section. */
  function fitRecommended(s, rec) {
    const cols = s.panel.classList.contains('is-narrow') ? 1 : 2;
    rec.style.setProperty('--rec-cols', cols);
    const rowH = 52;
    const rows = Math.min(3, Math.floor(rec.clientHeight / rowH));
    [...rec.children].forEach((el, i) => { el.hidden = i >= rows * cols; });
    if (rows < 1) { rec.hidden = true; rec.previousElementSibling.hidden = true; }
  }

  function goPage(s, track, pips, page, pages) {
    page = clamp(page, 0, pages - 1);
    if (page === s.page) return;
    const from = track.style.transform;
    s.page = page;
    track.style.transform = `translateY(${-page * 100 / pages}%)`;
    Shell.animate(track, [{ transform: from }, { transform: track.style.transform }], { duration: dur('normal'), easing: Shell.ease.in });
    [...pips.children].forEach((pip, i) => pip.setAttribute('aria-current', i === page ? 'true' : 'false'));
  }

  function pinButton(s, e) {
    const b = h('button', 'st-pin st-plate', { 'data-nav': true, 'data-ref': e.ref, 'aria-label': e.name });
    b.append(Shell.icon(e.icon, { size: 32, className: 'st-pin-icon' }), h('span', 'st-pin-label', { text: e.name }));
    b.addEventListener('click', () => { if (!s.dragged) launch(e); });
    b.addEventListener('contextmenu', (ev) => showMenu(ev, pinMenu(s, e)));
    return b;
  }

  function pinMenu(s, e) {
    const refs = pinRefs();
    const items = [
      { label: 'Move to front', icon: 'fluent/arrow-up', disabled: refs.indexOf(e.ref) === 0,
        action: () => { savePins([e.ref, ...refs.filter((r) => r !== e.ref)]); s.page = 0; if (st === s) renderMain(s); } },
      { label: 'Unpin from Start', icon: 'fluent/pin-off',
        action: () => { savePins(refs.filter((r) => r !== e.ref)); if (st === s) renderMain(s); } },
    ];
    if (e.app) items.push(taskbarPinItem(e.app));
    if (e.item) items.push({ label: 'Open file location', icon: 'fluent/folder-open', action: () => { close(); openLocation(e.item); } });
    return items;
  }

  function recItem(s, it, wide) {
    const b = h('button', `st-rec-item st-plate${wide ? ' is-wide' : ''}`, { 'data-nav': true });
    const text = h('span', 'st-rec-text');
    text.append(h('span', 'st-rec-name', { text: it.name }), h('span', 'st-rec-when', { text: ago(it.modified) }));
    b.append(Shell.icon(it.icon, { size: 32 }), text);
    b.addEventListener('click', () => { close(); Shell.fs.open(it.id); });
    b.addEventListener('contextmenu', (ev) => showMenu(ev, [
      { label: 'Open file location', icon: 'fluent/folder-open', action: () => { close(); openLocation(it); } },
      { label: 'Remove from list', icon: 'fluent/dismiss', action: () => {
        hiddenRec.add(it.id);
        Shell.store.set('start-rec-hidden', [...hiddenRec]);
        if (st === s) { if (s.view === 'main') renderMain(s); else renderSub(s, s.subKind); }
      } },
    ]));
    return b;
  }

  /* ---------- drag to rearrange pins ---------- */
  /* Windows lets pins be dragged to a new place: the item follows the pointer, the others
     slide out of its way, and it settles into the gap on release. */
  function flip(container, mutate) {
    const kids = [...container.children];
    const before = new Map(kids.map((k) => [k, k.getBoundingClientRect()]));
    mutate();
    for (const k of kids) {
      const a = before.get(k), b = k.getBoundingClientRect();
      const dx = a.left - b.left, dy = a.top - b.top;
      if (dx || dy) Shell.animate(k, [{ transform: `translate(${dx}px, ${dy}px)` }, { transform: 'none' }], { duration: dur('normal'), easing: Shell.ease.in });
    }
  }

  function wirePinDrag(s, grid, page) {
    grid.addEventListener('pointerdown', (e) => {
      const pin = e.target.closest('.st-pin');
      if (!pin || e.button !== 0) return;
      const sx = e.clientX, sy = e.clientY;
      let drag = null;
      const move = (ev) => {
        if (!drag) {
          if (Math.hypot(ev.clientX - sx, ev.clientY - sy) < 4) return; // the system drag threshold
          drag = beginDrag(s, grid, pin, sx, sy);
        }
        drag.move(ev.clientX, ev.clientY);
      };
      const off = () => {
        window.removeEventListener('pointermove', move);
        window.removeEventListener('pointerup', up);
        window.removeEventListener('pointercancel', up);
      };
      const up = () => {
        off();
        if (!drag) return;
        drag.end(page);
        s.dragged = true; // swallow the click that follows the release
        setTimeout(() => { s.dragged = false; }, 0);
      };
      window.addEventListener('pointermove', move);
      window.addEventListener('pointerup', up);
      window.addEventListener('pointercancel', up);
      s.cleanup.push(off);
    });
  }

  function beginDrag(s, grid, pin, sx, sy) {
    const r = pin.getBoundingClientRect();
    const ghost = pin.cloneNode(true);
    ghost.className = 'st-pin st-plate is-ghost';
    ghost.removeAttribute('data-nav');
    Object.assign(ghost.style, { left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` });
    s.host.append(ghost);
    pin.classList.add('is-placeholder');
    let idx = [...grid.children].indexOf(pin);
    let at = 'translate(0px, 0px)';
    return {
      move(x, y) {
        at = `translate(${x - sx}px, ${y - sy}px)`;
        ghost.style.transform = at;
        const g = grid.getBoundingClientRect();
        if (x < g.left - 24 || x > g.right + 24 || y < g.top - 24 || y > g.bottom + 24) return; // outside: keep the gap
        const col = clamp(Math.floor((x - g.left) / (g.width / s.cols)), 0, s.cols - 1);
        const row = clamp(Math.floor((y - g.top) / (g.height / s.rows)), 0, s.rows - 1);
        const to = Math.min(row * s.cols + col, grid.children.length - 1);
        if (to === idx) return;
        flip(grid, () => {
          const others = [...grid.children].filter((c) => c !== pin);
          grid.insertBefore(pin, others[to] || null);
        });
        idx = to;
      },
      end(page) {
        const per = s.cols * s.rows;
        const refs = pinRefs();
        refs.splice(page * per, grid.children.length, ...[...grid.children].map((c) => c.dataset.ref));
        savePins(refs);
        const r2 = pin.getBoundingClientRect();
        Shell.animate(ghost, [{ transform: at }, { transform: `translate(${r2.left - r.left}px, ${r2.top - r.top}px)` }],
          { duration: dur('fast'), easing: Shell.ease.in, fill: 'forwards' })
          .then(() => { ghost.remove(); pin.classList.remove('is-placeholder'); });
      },
    };
  }

  /* ---------- All apps and the Recommended list ---------- */
  /* A keyboard activation (click with detail 0) carries focus into the list. */
  function showSub(s, kind, byKeyboard) {
    renderSub(s, kind);
    slide(s, true);
    if (byKeyboard) s.sub.querySelector('[data-nav]:not(.st-btn)')?.focus({ preventScroll: true });
    else s.panel.focus({ preventScroll: true });
  }

  /* The two views sit side by side and slide horizontally, as Start's All apps does. */
  function slide(s, toSub) {
    const from = toSub ? s.main : s.sub, to = toSub ? s.sub : s.main, dir = toSub ? 1 : -1;
    s.view = toSub ? 'sub' : 'main';
    to.hidden = false;
    if (!toSub) renderMain(s);
    const o = { duration: dur('normal'), easing: Shell.ease.in };
    Shell.animate(from, [{ transform: 'none' }, { transform: `translateX(${-dir * 100}%)` }], o);
    Shell.animate(to, [{ transform: `translateX(${dir * 100}%)` }, { transform: 'none' }], o)
      .then(() => { if (s.view === (toSub ? 'sub' : 'main')) from.hidden = true; });
  }

  function renderSub(s, kind) {
    s.subKind = kind;
    s.sub.textContent = '';
    const back = smallButton('Back', 'chevron-left', true, () => { slide(s, false); s.panel.focus({ preventScroll: true }); });
    const list = h('div', 'st-list', { role: 'list' });
    if (kind === 'all') {
      let letter = null;
      for (const e of allApps()) {
        const l = letterOf(e.name);
        if (l !== letter) {
          letter = l;
          const lb = h('button', 'st-letter st-plate', { 'data-nav': true, 'data-letter': l, text: l, 'aria-label': `${l}, jump to a letter` });
          lb.addEventListener('click', () => showLetters(s, list));
          list.append(lb);
        }
        list.append(appRow(s, e));
      }
    } else {
      recentFiles().forEach((it) => list.append(recItem(s, it, true)));
    }
    rove(list, '[data-nav]');
    s.sub.append(headRow(kind === 'all' ? 'All apps' : 'Recommended', back), list);
  }

  function appRow(s, e) {
    const b = h('button', 'st-app st-plate', { 'data-nav': true, role: 'listitem' });
    b.append(Shell.icon(e.icon, { size: 24 }), h('span', 'st-app-name', { text: e.name }));
    b.addEventListener('click', () => launch(e));
    b.addEventListener('contextmenu', (ev) => {
      const refs = pinRefs(), pinned = refs.includes(e.ref);
      const more = [];
      if (e.app) more.push(taskbarPinItem(e.app));
      if (e.item) more.push({ label: 'Open file location', icon: 'fluent/folder-open', action: () => { close(); openLocation(e.item); } });
      const items = [{
        label: pinned ? 'Unpin from Start' : 'Pin to Start', icon: pinned ? 'fluent/pin-off' : 'fluent/pin',
        action: () => savePins(pinned ? refs.filter((r) => r !== e.ref) : [...refs, e.ref]),
      }];
      if (more.length) items.push({ label: 'More', submenu: more });
      showMenu(ev, items);
    });
    return b;
  }

  /* Clicking a letter header shows the alphabet; letters without apps are disabled. */
  function showLetters(s, list) {
    const present = new Set([...list.querySelectorAll('.st-letter')].map((b) => b.dataset.letter));
    const grid = h('div', 'st-letters', { role: 'group', 'aria-label': 'Jump to a letter' });
    for (const l of ['&', '#', ...'ABCDEFGHIJKLMNOPQRSTUVWXYZ']) {
      const b = h('button', 'st-letter-cell st-plate', { 'data-nav': true, text: l });
      b.disabled = !present.has(l);
      b.addEventListener('click', () => {
        grid.remove();
        list.hidden = false;
        const target = list.querySelector(`[data-letter="${CSS.escape(l)}"]`);
        list.scrollTop = target ? target.offsetTop - 4 : 0;
        Shell.animate(list, [{ opacity: 0, transform: 'scale(0.96)' }, { opacity: 1, transform: 'none' }], { duration: dur('fast'), easing: Shell.ease.in });
        target?.focus({ preventScroll: true });
      });
      grid.append(b);
    }
    rove(grid, '.st-letter-cell:not(:disabled)');
    list.hidden = true;
    list.after(grid);
    Shell.animate(grid, [{ opacity: 0, transform: 'scale(1.08)' }, { opacity: 1, transform: 'none' }], { duration: dur('fast'), easing: Shell.ease.in });
    grid.querySelector('button:not(:disabled)')?.focus({ preventScroll: true });
  }

  /* ---------- account and power ---------- */
  function footer(s) {
    const foot = h('div', 'st-foot');
    const user = h('button', 'st-user st-plate', { 'data-nav': true, 'aria-haspopup': 'menu', 'data-tooltip': 'Mihawk' });
    user.append(Shell.icon('object/user-avatar', { size: 32, className: 'st-avatar' }), h('span', 'st-user-name', { text: 'Mihawk' }));
    user.addEventListener('click', () => anchored(user, 'top-start', [
      { label: 'Change account settings', action: () => { close(); Shell.apps.launch('settings', { page: settingsPage('Your info', 'accounts') }); } },
      { label: 'Lock', action: () => { close(); Shell.session?.lock?.(); } },
      { label: 'Sign out', action: () => { close(); Shell.session?.lock?.(); } },
    ]));
    const power = h('button', 'st-power st-plate', { 'data-nav': true, 'aria-label': 'Power', 'aria-haspopup': 'menu', 'data-tooltip': 'Power' });
    power.append(Shell.icon('fluent/power', { size: 16 }));
    power.addEventListener('click', () => anchored(power, 'top', [
      { label: 'Sign-in options', icon: 'fluent/settings', action: () => { close(); Shell.apps.launch('settings', { page: settingsPage('Sign-in options', 'accounts') }); } },
      { separator: true },
      { label: 'Sleep', icon: 'fluent/weather-moon', action: () => { close(); Shell.session?.sleep?.(); } },
      { label: 'Shut down', icon: 'fluent/power', action: () => { close(); Shell.session?.shutdown?.(); } },
      { label: 'Restart', icon: 'fluent/arrow-clockwise', action: () => { close(); Shell.session?.restart?.(); } },
    ]));
    foot.append(user, power);
    return foot;
  }
  function anchored(btn, placement, items) {
    const r = btn.getBoundingClientRect();
    Shell.menu?.show?.({ x: r.left, y: r.top, anchorRect: r, placement, items });
  }

  /* ---------- public API ---------- */
  Shell.start = {
    open,
    close,
    toggle: () => (Shell.transient.isOpen('start') ? close() : open()),
    isOpen: () => Shell.transient.isOpen('start'),
    /* Resolved pins in order: [{ref, name, icon, app?, item?, open()}]. */
    pins,
    isPinned: (id) => pinRefs().includes(refOf(id)),
    pin(id) { const r = refOf(id); if (r && !pinRefs().includes(r)) savePins([...pinRefs(), r]); },
    unpin(id) { const r = refOf(id); savePins(pinRefs().filter((x) => x !== r)); },
    /* Internal to the start module (js/search.js). */
    _kit: { h, dur, place, mountHost, rise, sink, inMenus, onTaskbarButton, spatial, reveal, tabStops, cycleTab, showMenu, ago, walk, recentFiles, resolve, refOf, taskbarPinItem, openLocation },
  };

  Shell.keys.on('Ctrl+Escape', () => Shell.start.toggle());

  Shell.on('boot', () => {
    Shell.on('win-key', () => Shell.start.toggle());
    Shell.on('setting', ({ key }) => { if (st && key === 'taskbarAlignment') place(st.panel); });
    Shell.on('apps', () => { if (st && st.view === 'main') renderMain(st); });
    /* After every boot handler has run, so apps registered at boot are pinned. */
    setTimeout(() => {
      const want = Shell.debugOpen.filter((n) => n === 'start' || n.startsWith('start-'));
      if (!want.length) return;
      open();
      if (want.includes('start-allapps')) showSub(st, 'all');
      if (want.includes('start-recommended')) showSub(st, 'recommended');
      if (want.includes('start-power')) st.foot.querySelector('.st-power').click();
    }, 0);
  });
})();
