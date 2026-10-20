/* Taskbar: the 48 px acrylic bar along the bottom edge, as Windows 11 23H2 draws it.
   Owns: the Start, Search and Task View buttons; pinned and running app buttons (running pill,
   hover thumbnails, jump list, drag to reorder, overflow flyout); the notification area
   (hidden icons, network/volume/battery, clock with notification badge, show desktop sliver);
   the Win+X menu on Start; the taskbar context menu; Win+T, Win+B, Win+X, Win+1..9; attention
   flashing, progress bars and badges on app buttons; auto-hide; Peek at desktop; the volume wheel.
   API: Shell.taskbar.buttonRect(appId), .flash(appId), .setProgress(appId, value, state),
   .setBadge(appId, text), .pin(appId), .unpin(appId), .isPinned(appId).
   Settings owned: taskbarSearch ('box' | 'icon' | 'hidden'), taskbarAutoHide, taskbarPeek (both
   default false), taskbarFlashing, taskbarBadges (both default true).
   Store: taskbar-order = {pins: [pinned ids in taskbar order], known: [default pins already adopted]}. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const PITCH = 44;        // 40 px button + 2 px margin each side (appearance.json: item 40, gap 4)
  const GUTTER = 10;       // 12 px taskbar padding minus the slot margin
  const SEARCH_BOX = 224;  // 220 px search pill + its margins
  const THUMB_DELAY = 400; // hover time before thumbnails appear
  const HIDE_DELAY = 300;  // grace period to travel from a button to its thumbnails
  const THUMB_W = 200, THUMB_H = 140;
  const PEEK_DELAY = 500;  // resting on a thumbnail before Peek shows that window alone
  const DRAG_RAISE = 1000; // a dragged file resting on a running app's button brings its window forward
  const DRAG_START = 4;    // system drag threshold (SM_CXDRAG)
  const FLASH_MS = 7 * 1060; // ForegroundFlashCount 7 pulses at twice the 530 ms caret blink (css: tb-flash)
  const AH_EDGE = 2;       // bottom rows that reveal a hidden taskbar
  const AH_REVEAL = 300;   // resting on that edge before the taskbar slides up
  const AH_HIDE = 500;     // after the pointer leaves, before it slides away again
  const DESK_PEEK_DELAY = 500; // resting on the Show desktop sliver before Peek

  const el = (tag, className, attrs = {}) => {
    const n = document.createElement(tag);
    if (className) n.className = className;
    for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
    return n;
  };
  const text = (tag, className, value) => { const n = el(tag, className); n.textContent = value; return n; };

  const DEFAULTS = { taskbarSearch: 'box', taskbarAutoHide: false, taskbarPeek: false, taskbarFlashing: true, taskbarBadges: true };
  for (const [k, v] of Object.entries(DEFAULTS)) if (!(k in Shell.settings)) Shell.settings[k] = v;

  /* Per-app button state set through the API; painted on every button of that app. */
  const flashing = new Set(); // apps asking for attention until they are focused
  const progress = new Map(); // appId -> {value 0-1, state}
  const badges = new Map();   // appId -> text ('' draws the dot)

  /* ---------- pins and order ---------- */
  let pins = [];           // pinned app ids, in taskbar order
  const known = new Set(); // default pins already adopted, so an unpin survives a reload
  const saved = Shell.store.get('taskbar-order', null);
  if (saved && Array.isArray(saved.pins)) { pins = saved.pins.slice(); (saved.known || []).forEach((id) => known.add(id)); }
  const savePins = () => Shell.store.set('taskbar-order', { pins, known: [...known] });

  function adoptDefaultPins() {
    let changed = false;
    Shell.apps.list().forEach((a) => {
      if (!a.pinned || known.has(a.id)) return;
      known.add(a.id);
      if (!pins.includes(a.id)) pins.push(a.id);
      changed = true;
    });
    if (changed) savePins();
  }
  /* Pins follow the on-screen order; pins of apps not shown keep their place at the end. */
  const reorderPins = () => { pins = [...order.filter((x) => pins.includes(x)), ...pins.filter((x) => !order.includes(x))]; };
  function setPinned(id, on) {
    const i = pins.indexOf(id);
    if (on && i < 0) pins.push(id);
    if (!on && i >= 0) pins.splice(i, 1);
    known.add(id);
    reorderPins();
    savePins();
    sync(true);
  }

  /* ---------- windows ---------- */
  const winList = () => { try { return Shell.wm?.list?.() || []; } catch { return []; } };
  const winsOf = (id) => winList().filter((w) => w.appId === id);
  const runningIds = () => [...new Set(winList().map((w) => w.appId).filter(Boolean))];
  const focusedApp = () => { const f = Shell.wm?.focused?.(); return f && !f.isMinimized?.() ? f.appId : null; };
  const info = (id) => {
    const app = Shell.apps.get(id), w = winsOf(id)[0];
    return { name: app?.name || w?.title || id, icon: app?.icon || w?.icon || 'object/app-generic' };
  };
  const focusWin = (w) => { if (w.isMinimized?.()) w.restore?.(); w.focus?.(); };
  /* The window manager may blur the active window on the same pointerdown that clicks its
     taskbar button; remember the last blur so that click still minimizes. */
  let lastBlur = { win: null, t: 0 };

  /* ---------- DOM ---------- */
  let bar, group, appsBox, moreBtn, tray, startBtn, searchBox, searchIcon, taskViewBtn;
  let chevronBtn, statusBtn, clockBtn, peekBtn, timeEl, dateEl, badgeEl;
  const statusIcons = {};
  const slots = new Map(); // appId -> button in the bar
  let order = [];          // ids in the app area (pinned and running), in taskbar order
  let overflowIds = [];
  let booted = false;

  function startGlyph() {
    const g = el('span', 'tb-glyph tb-logo');
    /* The project's Mihawk mark (themes/windows11/start.svg), 22 px. */
    g.innerHTML = '<img src="../../themes/windows11/start.svg" width="22" height="22" alt="">';
    return g;
  }
  function glyph(name, size) { const g = el('span', 'tb-glyph'); g.append(Shell.icon(name, { size })); return g; }
  function button(className, label, tooltip) {
    const b = el('button', `tb-btn ${className}`, { type: 'button', 'aria-label': label });
    if (tooltip) b.dataset.tooltip = tooltip;
    return b;
  }

  /* ---------- surfaces owned by other modules ---------- */
  const SURFACES = {
    start: { ids: ['start'], toggle: () => Shell.start?.toggle?.(), btns: [] },
    search: { ids: ['search'], toggle: () => Shell.search?.toggle?.(), btns: [] },
    taskview: { ids: ['taskview', 'taskView', 'task-view'], toggle: () => Shell.taskView?.toggle?.(), btns: [] },
    status: { ids: ['quicksettings', 'quickSettings', 'quick-settings'], toggle: () => Shell.quickSettings?.toggle?.(), btns: [] },
    clock: { ids: ['notifications', 'notification-center', 'calendar'], toggle: () => Shell.notifications?.toggle?.(), btns: [] },
  };
  /* A flyout light-dismisses on the pointerdown that lands on its own taskbar button; the click
     that follows must not reopen it. Ids closed during the current event are kept one task. */
  const dismissedNow = new Set();
  function wireSurface(btn, key) {
    const s = SURFACES[key];
    let skip = false;
    btn.setAttribute('aria-haspopup', 'dialog');
    btn.setAttribute('aria-expanded', 'false');
    btn.addEventListener('pointerdown', () => { skip = s.ids.some((id) => dismissedNow.has(id)); });
    btn.addEventListener('click', () => { if (skip) { skip = false; return; } hideThumbs(); clickMotion(btn); s.toggle(); });
    s.btns.push(btn);
  }
  function paintSurfaces() {
    for (const s of Object.values(SURFACES)) {
      const open = s.ids.some((id) => Shell.transient.isOpen(id));
      s.btns.forEach((b) => { b.classList.toggle('is-open', open); b.setAttribute('aria-expanded', String(open)); });
    }
  }

  /* ---------- glyph motion ---------- */
  /* Windows animates the glyph itself: a launch or restore hops it up and settles, a minimize
     squashes it down and back; Search and Task View answer a click. Start keeps its CSS squish. */
  const MOTIONS = {
    up: [{ transform: 'none' }, { transform: 'translateY(-5px)', offset: 0.35 }, { transform: 'translateY(1px)', offset: 0.7 }, { transform: 'none' }],
    down: [{ transform: 'none' }, { transform: 'translateY(3px) scale(1, 0.86)', offset: 0.4 }, { transform: 'none' }],
    tilt: [{ transform: 'none' }, { transform: 'rotate(-14deg) scale(0.92)', offset: 0.4 }, { transform: 'none' }],
    squeeze: [{ transform: 'none' }, { transform: 'scale(0.8, 0.92)', offset: 0.4 }, { transform: 'none' }],
  };
  function glyphMotion(b, kind) {
    const g = b && !b.hidden && b.isConnected && b.querySelector('.tb-glyph, .icon');
    if (g) Shell.animate(g, MOTIONS[kind], { duration: 300, easing: 'ease-out' });
  }
  function clickMotion(b) {
    if (b === taskViewBtn) glyphMotion(b, 'squeeze');
    else if (b === searchBox || b === searchIcon) glyphMotion(b, 'tilt');
  }
  const minimizedWins = new WeakSet(); // a 'restore' of one of these is a restore from the taskbar
  function windowMotion(type, win) {
    if (!win?.appId) return;
    const b = slots.get(win.appId);
    if (type === 'minimize') { minimizedWins.add(win); glyphMotion(b, 'down'); }
    else if (type === 'open') glyphMotion(b, 'up');
    else if (type === 'restore' && minimizedWins.has(win)) { minimizedWins.delete(win); glyphMotion(b, 'up'); }
    else if (type === 'close') minimizedWins.delete(win);
  }

  /* ---------- app buttons ---------- */
  function makeAppButton(id) {
    const b = button('tb-slot tb-app', id);
    b.dataset.app = id;
    b.append(el('span', 'tb-glyph'), el('span', 'tb-pill'), el('span', 'tb-progress'), el('span', 'badge tb-appbadge', { hidden: '' }));
    b.addEventListener('click', (e) => {
      if (suppressClick) return;
      activate(id, e, b);
      if (b.closest('.tb-overflow')) Shell.transient.close('taskbar-overflow');
    });
    b.addEventListener('auxclick', (e) => { if (e.button === 1 && Shell.apps.get(id)) Shell.apps.launch(id); });
    b.addEventListener('contextmenu', (e) => { e.preventDefault(); e.stopPropagation(); jumpList(id, b); });
    b.addEventListener('pointerenter', () => onAppEnter(id, b));
    b.addEventListener('pointerleave', onAppLeave);
    b.addEventListener('pointerdown', (e) => { if (b.parentNode === appsBox) onAppPointerDown(e, id, b); });
    return b;
  }
  function paint(b, id) {
    const wins = winsOf(id), inf = info(id), active = focusedApp() === id;
    b.classList.toggle('is-running', wins.length > 0);
    b.classList.toggle('is-active', wins.length > 0 && active);
    if (active) flashing.delete(id);
    b.classList.toggle('is-flashing', flashing.has(id) && Shell.settings.taskbarFlashing !== false);
    const p = progress.get(id), pbar = b.querySelector('.tb-progress');
    b.classList.toggle('has-progress', !!p);
    if (p) { pbar.dataset.state = p.state; pbar.style.setProperty('--value', p.value); }
    const badge = Shell.settings.taskbarBadges !== false ? badges.get(id) : undefined, bd = b.querySelector('.tb-appbadge');
    bd.hidden = badge === undefined;
    if (bd.textContent !== (badge ?? '')) bd.textContent = badge ?? '';
    const extra = (badge ? `, ${badge}` : '') + (p && p.state !== 'indeterminate' ? `, ${Math.round(p.value * 100)}% complete` : '');
    b.setAttribute('aria-label', (wins.length > 1 ? `${inf.name} - ${wins.length} running windows` : wins.length ? `${inf.name} - 1 running window` : inf.name) + extra);
    if (wins.length) b.removeAttribute('data-tooltip'); else b.dataset.tooltip = inf.name; // running apps show thumbnails instead
    if (b._icon !== inf.icon) { b._icon = inf.icon; b.firstChild.replaceChildren(Shell.icon(inf.icon, { size: 24 })); }
  }

  function activate(id, e = {}, anchor) {
    const wins = winsOf(id);
    if (!wins.length || e.shiftKey) { hideThumbs(); if (Shell.apps.get(id)) Shell.apps.launch(id); return; }
    if (wins.length > 1) { if (!thumbs || thumbs.id !== id) showThumbs(id, anchor || slotFor(id)); return; }
    hideThumbs();
    const w = wins[0];
    const wasFocused = w.isFocused?.() || (lastBlur.win === w && performance.now() - lastBlur.t < 400);
    if (w.isMinimized?.()) focusWin(w);
    else if (wasFocused) w.minimize?.();
    else w.focus?.();
  }
  const slotFor = (id) => { const b = slots.get(id); return b && !b.hidden ? b : moreBtn; };

  /* ---------- sync and layout ---------- */
  function sync(animate = true) {
    if (!booted) return;
    const running = runningIds();
    // flashing and progress belong to windows: they end with the app's last window on any desktop
    const alive = new Set((Shell.wm?.all?.() || winList()).map((w) => w.appId));
    for (const id of [...flashing, ...progress.keys()]) if (!alive.has(id)) { flashing.delete(id); progress.delete(id); }
    const want = pins.filter((id) => Shell.apps.get(id) || running.includes(id));
    running.forEach((id) => { if (!want.includes(id)) want.push(id); });
    order = order.filter((id) => want.includes(id));
    want.forEach((id) => { if (!order.includes(id)) order.push(id); }); // new buttons join at the end
    const before = animate ? rects() : null;
    for (const [id, b] of slots) if (!order.includes(id)) { slots.delete(id); b._undrop?.(); b.remove(); }
    order.forEach((id, i) => {
      let b = slots.get(id);
      const fresh = !b;
      if (fresh) { b = makeAppButton(id); b._undrop = dropHover(id, b); slots.set(id, b); }
      if (appsBox.children[i] !== b) appsBox.insertBefore(b, appsBox.children[i] || null);
      if (fresh) b.getBoundingClientRect(); // so the running pill grows from zero
      paint(b, id);
    });
    document.querySelectorAll('.tb-overflow .tb-app').forEach((b) => paint(b, b.dataset.app));
    layout(before);
  }

  function rects() {
    const m = new Map();
    group.querySelectorAll('.tb-slot').forEach((n) => { if (!n.hidden) m.set(n, n.getBoundingClientRect().left); });
    return m;
  }
  /* FLIP: slots that moved slide from their old place; slots that appeared grow in. */
  function flip(before) {
    if (!before || Shell.reducedMotion()) return;
    group.querySelectorAll('.tb-slot').forEach((n) => {
      if (n.hidden) return;
      if (!before.has(n)) {
        Shell.animate(n, [{ opacity: 0, transform: 'scale(0.5)' }, { opacity: 1, transform: 'none' }], { duration: 250 });
        return;
      }
      const dx = before.get(n) - n.getBoundingClientRect().left;
      if (Math.abs(dx) > 0.5) Shell.animate(n, [{ transform: `translateX(${dx}px)` }, { transform: 'none' }], { duration: 250 });
    });
  }

  /* Places the centre group and decides what fits: the search box falls back to its icon,
     then apps overflow into the "See more" flyout, then Task View and Search give way. */
  function layout(before) {
    if (!booted) return;
    const W = window.innerWidth;
    bar.classList.toggle('is-narrow', W < 700);
    bar.classList.toggle('is-tiny', W < 400);
    const avail = W - tray.offsetWidth - GUTTER * 2;
    const n = order.length;
    let search = Shell.settings.taskbarSearch || 'box', showTV = true;
    const fixed = () => PITCH + (search === 'box' ? SEARCH_BOX : search === 'icon' ? PITCH : 0) + (showTV ? PITCH : 0);
    if (search === 'box' && avail - fixed() < Math.min(n, 4) * PITCH) search = 'icon';
    let cap = Math.floor((avail - fixed()) / PITCH);
    // need room for one app or for "See more"; Task View gives way first, then Search
    if (n && cap < 1) { showTV = false; cap = Math.floor((avail - fixed()) / PITCH); }
    if (n && cap < 1 && search !== 'hidden') { search = 'hidden'; cap = Math.floor((avail - fixed()) / PITCH); }
    const visible = n <= cap ? n : Math.max(0, cap - 1);
    overflowIds = order.slice(visible);
    order.forEach((id, i) => { slots.get(id).hidden = i >= visible; });
    moreBtn.hidden = overflowIds.length === 0;
    moreBtn.classList.toggle('is-flashing', Shell.settings.taskbarFlashing !== false && overflowIds.some((id) => flashing.has(id)));
    searchBox.hidden = search !== 'box';
    searchIcon.hidden = search !== 'icon';
    taskViewBtn.hidden = !showTV;
    const gw = group.offsetWidth;
    let left = Shell.settings.taskbarAlignment === 'left' ? GUTTER : Math.round((W - gw) / 2);
    left = Math.max(GUTTER, Math.min(left, W - tray.offsetWidth - GUTTER - gw));
    group.style.left = `${left}px`;
    flip(before);
    if (!overflowIds.length) Shell.transient.close('taskbar-overflow');
    else if (Shell.transient.isOpen('taskbar-overflow')) fillOverflow();
  }

  /* ---------- drag to reorder ---------- */
  let drag = null, suppressClick = false;
  function onAppPointerDown(e, id, b) {
    if (e.button !== 0) return;
    const sx = e.clientX, sy = e.clientY, pid = e.pointerId;
    const move = (ev) => {
      if (ev.pointerId !== pid) return;
      if (!drag) {
        if (Math.hypot(ev.clientX - sx, ev.clientY - sy) < DRAG_START) return;
        drag = startDrag(id, b, pid);
        if (!drag) { end(); return; }
      }
      moveDrag(ev.clientX - sx);
    };
    const end = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', up);
    };
    const up = (ev) => {
      if (ev.pointerId !== pid) return;
      end();
      if (!drag) return;
      finishDrag();
      suppressClick = true;
      setTimeout(() => { suppressClick = false; });
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', up);
  }
  function startDrag(id, b, pid) {
    const list = order.map((x) => slots.get(x)).filter((n) => !n.hidden);
    const index = list.indexOf(b);
    if (index < 0 || list.length < 2) return null;
    hideThumbs();
    Shell.tooltip?.hide?.();
    try { b.setPointerCapture(pid); } catch { /* pointer already released */ }
    appsBox.classList.add('is-dragging');
    b.classList.add('is-dragged');
    return { id, b, list, index, target: index };
  }
  function moveDrag(dx) {
    const { b, list, index } = drag;
    dx = Math.max(-index * PITCH, Math.min((list.length - 1 - index) * PITCH, dx));
    b.style.transform = `translateX(${dx}px)`;
    const t = Math.max(0, Math.min(list.length - 1, index + Math.round(dx / PITCH)));
    drag.target = t;
    list.forEach((n, j) => {
      if (n === b) return;
      const shift = index < t && j > index && j <= t ? -PITCH : index > t && j >= t && j < index ? PITCH : 0;
      n.style.transform = shift ? `translateX(${shift}px)` : '';
    });
  }
  function finishDrag() {
    const { id, b, list, index, target } = drag;
    drag = null;
    const before = rects();
    list.forEach((n) => { n.style.transform = ''; });
    appsBox.classList.remove('is-dragging');
    b.classList.remove('is-dragged');
    if (target !== index) {
      order.splice(order.indexOf(id), 1);
      order.splice(target, 0, id); // visible buttons are the head of `order`
      reorderPins();
      savePins();
      order.forEach((x, i) => { const n = slots.get(x); if (appsBox.children[i] !== n) appsBox.insertBefore(n, appsBox.children[i] || null); });
    }
    flip(before);
  }

  /* A file dragged from the desktop or Explorer and resting on a running app's button brings
     that app's top window forward (22H2), so the file can be dropped into it. The button
     itself takes nothing: a drop on it leaves the files where they were. */
  let dragRaiseTimer = 0;
  function dropHover(id, b) {
    if (!Shell.drag?.registerTarget) return null;
    return Shell.drag.registerTarget(b, {
      accepts: () => !b.hidden && winsOf(id).length > 0,
      onEnter: () => {
        clearTimeout(dragRaiseTimer);
        b.classList.add('is-hover');
        dragRaiseTimer = setTimeout(() => { const ws = winsOf(id); if (ws.length) focusWin(ws[ws.length - 1]); }, DRAG_RAISE);
      },
      onLeave: () => { clearTimeout(dragRaiseTimer); b.classList.remove('is-hover'); },
      onDrop: () => {},
    });
  }

  /* ---------- thumbnails ---------- */
  let thumbs = null, hoverTimer = 0, hideTimer = 0, jumpOpen = false;
  let peekTimer = 0, peekWin = null, peekAnimTimer = 0;
  function peek(w) {
    const layer = Shell.layer('windows');
    if (!layer || !w.el) return;
    clearTimeout(peekAnimTimer);
    peekWin?.el?.classList.remove('tb-peek-target');
    peekWin = w;
    w.el.classList.add('tb-peek-target');
    layer.classList.add('tb-peek-anim', 'tb-peeking');
  }
  function unpeek() {
    clearTimeout(peekTimer);
    if (!peekWin) return;
    const layer = Shell.layer('windows');
    peekWin.el?.classList.remove('tb-peek-target');
    peekWin = null;
    layer?.classList.remove('tb-peeking');
    peekAnimTimer = setTimeout(() => layer?.classList.remove('tb-peek-anim'), 200); // keep the fade-back transition
  }
  function onAppEnter(id, b) {
    clearTimeout(hideTimer);
    if (drag || jumpOpen) return;
    if (!winsOf(id).length) { if (thumbs) hideThumbs(); return; }
    if (thumbs) { if (thumbs.id !== id) showThumbs(id, b); return; }
    clearTimeout(hoverTimer);
    hoverTimer = setTimeout(() => showThumbs(id, b), THUMB_DELAY);
  }
  function onAppLeave() { clearTimeout(hoverTimer); scheduleHide(); }
  function scheduleHide() { clearTimeout(hideTimer); hideTimer = setTimeout(hideThumbs, HIDE_DELAY); }
  function hideThumbs() { clearTimeout(hoverTimer); clearTimeout(hideTimer); Shell.transient.close('taskbar-thumbs'); }

  function winSize(w) {
    const r = w.el?.getBoundingClientRect?.();
    if (r && r.width > 1 && r.height > 1) return { w: r.width, h: r.height };
    const sw = parseFloat(w.el?.style?.width), sh = parseFloat(w.el?.style?.height);
    if (sw > 1 && sh > 1) return { w: sw, h: sh };
    return { w: 16, h: 10 }; // a minimized window without geometry: Windows' usual aspect
  }

  function showThumbs(id, anchor) {
    clearTimeout(hoverTimer);
    clearTimeout(hideTimer);
    const wins = winsOf(id);
    if (!wins.length || !anchor || !anchor.isConnected) { hideThumbs(); return; }
    const inf = info(id);
    const box = el('div', 'tb-thumbs acrylic surface-flyout', { role: 'list', 'aria-label': inf.name });
    const maxW = Math.min(THUMB_W, (window.innerWidth - 16 - 8 - (wins.length - 1) * 4) / wins.length - 16);
    wins.forEach((w) => {
      const r = winSize(w);
      const s = Math.min(maxW / r.w, THUMB_H / r.h);
      const card = el('div', 'tb-thumb', { role: 'listitem', tabindex: '-1' });
      const close = el('button', 'tb-thumb-close', { type: 'button', 'aria-label': 'Close', 'data-tooltip': 'Close' });
      close.append(Shell.icon('fluent/dismiss', { size: 12 }));
      const head = el('div', 'tb-thumb-head');
      head.append(Shell.icon(w.icon || inf.icon, { size: 16 }), text('span', 'tb-thumb-title', w.title || inf.name), close);
      const view = el('div', 'tb-thumb-view');
      view.style.width = `${Math.round(r.w * s)}px`;
      view.style.height = `${Math.round(r.h * s)}px`;
      let snap = null;
      try { snap = w.snapshot?.(s); } catch { snap = null; }
      if (snap instanceof Node) view.append(snap);
      else { view.classList.add('is-empty'); view.append(Shell.icon(inf.icon, { size: 32 })); }
      card.append(head, view);
      card.addEventListener('click', () => { unpeek(); focusWin(w); hideThumbs(); });
      close.addEventListener('click', (e) => { e.stopPropagation(); unpeek(); w.close?.(); });
      card.addEventListener('pointerenter', () => {
        clearTimeout(peekTimer);
        if (peekWin) peek(w); else peekTimer = setTimeout(() => peek(w), PEEK_DELAY); // once peeking, others peek at once
      });
      card.addEventListener('pointerleave', unpeek);
      box.append(card);
    });
    box.addEventListener('pointerenter', () => clearTimeout(hideTimer));
    box.addEventListener('pointerleave', scheduleHide);
    box.addEventListener('contextmenu', (e) => e.preventDefault());
    const old = thumbs;
    unpeek();
    thumbs = { id, el: box, anchor };
    Shell.layer('flyouts').appendChild(box);
    placeAbove(box, anchor, 8);
    if (old) old.el.remove();
    else Shell.animate(box, [{ opacity: 0, transform: 'translateY(8px)' }, { opacity: 1, transform: 'none' }], { duration: 167 });
    Shell.transient.open('taskbar-thumbs', { group: 'thumbs', elements: [box, anchor], close: removeThumbs });
  }
  function removeThumbs() {
    const t = thumbs;
    thumbs = null;
    unpeek();
    if (!t) return;
    Shell.animate(t.el, [{ opacity: 1 }, { opacity: 0 }], { duration: 83, easing: Shell.ease.out }).then(() => t.el.remove());
  }

  /* Bottom-anchored flyouts sit `gap` px above the taskbar (or above an anchor that is not in
     it), centred on the anchor and clamped 8 px inside the screen. */
  function placeAbove(box, anchor, gap) {
    const r = anchor.getBoundingClientRect();
    const top = bar.contains(anchor) ? bar.getBoundingClientRect().top : r.top;
    box.style.bottom = `${Math.round(window.innerHeight - top + gap)}px`;
    const w = box.offsetWidth;
    box.style.left = `${Math.round(Math.max(8, Math.min(window.innerWidth - w - 8, r.left + r.width / 2 - w / 2)))}px`;
  }
  const rise = (box) => Shell.animate(box, [{ opacity: 0, transform: 'translateY(12px)' }, { opacity: 1, transform: 'none' }], { duration: 250 });
  function sink(box) {
    return Shell.animate(box, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateY(12px)' }], { duration: 167, easing: Shell.ease.out })
      .then(() => box.remove());
  }

  /* ---------- menus ---------- */
  function jumpList(id, b) {
    hideThumbs();
    const app = Shell.apps.get(id), wins = winsOf(id), inf = info(id);
    const items = [];
    if (id === 'explorer') {
      items.push({ header: 'Pinned' });
      ['desktop', 'downloads', 'documents', 'pictures'].forEach((fid) => {
        const it = Shell.fs.get(fid);
        if (it) items.push({ label: it.name, icon: it.icon, action: () => Shell.apps.launch('explorer', { folder: fid }) });
      });
      if (items.length > 1) items.push({ separator: true }); else items.length = 0;
    }
    if (app) {
      items.push({ label: inf.name, icon: inf.icon, action: () => Shell.apps.launch(id) });
      items.push(pins.includes(id)
        ? { label: 'Unpin from taskbar', icon: 'fluent/pin-off', action: () => setPinned(id, false) }
        : { label: 'Pin to taskbar', icon: 'fluent/pin', action: () => setPinned(id, true) });
    }
    if (wins.length) {
      items.push({ label: wins.length > 1 ? 'Close all windows' : 'Close window', icon: 'fluent/dismiss', action: () => wins.forEach((w) => w.close?.()) });
    }
    if (!items.length || !Shell.menu?.show) return;
    const r = menuAnchor(b);
    jumpOpen = true;
    b.classList.add('is-open');
    showBarMenu({
      x: r.left + r.width / 2, y: r.top, anchorRect: r, placement: 'top', minWidth: 256, items,
      onClose: () => { jumpOpen = false; b.classList.remove('is-open'); },
    });
  }

  /* A menu opened from the taskbar keeps an auto-hidden taskbar up while it is open. */
  let barMenu = false;
  function showBarMenu(opts) {
    if (!Shell.menu?.show) return;
    barMenu = true;
    ahCheck();
    Shell.menu.show({ ...opts, onClose: () => { barMenu = false; opts.onClose?.(); ahCheck(); } });
  }

  /* Menus from taskbar buttons float 12 px above the bar, like its flyouts: the anchor reaches
     8 px above the bar (plus the menu's 4 px gap) and still covers the button for re-clicks.
     Measured where the bar rests, so an auto-hidden bar still below the screen (it slides up
     as the menu opens) does not drag the menu down over itself. */
  function menuAnchor(b) {
    const r = b.getBoundingClientRect();
    if (!bar.contains(b)) return r;
    const rest = Shell.layer('taskbar').getBoundingClientRect().top, dy = bar.getBoundingClientRect().top - rest;
    const top = Math.min(r.top - dy, rest - 8);
    return new DOMRect(r.left, top, r.width, r.bottom - dy - top);
  }

  function winXMenu() {
    if (!Shell.menu?.show || !startBtn) return;
    const settings = (page) => () => Shell.apps.launch('settings', { page });
    const r = menuAnchor(startBtn);
    const has = (id) => !!Shell.apps.get(id);
    const session = (fn) => () => Shell.session?.[fn]?.();
    /* Terminal (Admin) asks for consent on the secure desktop first, when the dialogs module is there. */
    const admin = async () => {
      if (Shell.dialogs?.uac && !(await Shell.dialogs.uac({ app: 'Terminal', publisher: 'Mihawk OS', verified: true }))) return;
      Shell.apps.launch('terminal');
    };
    showBarMenu({
      x: r.left, y: r.top, anchorRect: r, placement: 'top-start', minWidth: 220,
      items: [
        /* Access keys are Windows 11's (Win+X then A opens Terminal (Admin)). Win+X never greys an entry. The MMC tools have no window here, so each opens its
           nearest stand-in: Quick Settings for Mobility Center's brightness/volume/battery tiles,
           Settings pages for the rest (Storage carries Windows 11's own Disks & volumes). */
        { label: 'Installed apps', accessKey: 'F', action: settings('apps/installed'), disabled: !has('settings') },
        { label: 'Mobility Center', accessKey: 'B', action: () => (Shell.quickSettings?.open ? Shell.quickSettings.open() : settings('system/power')()) },
        { label: 'Power Options', accessKey: 'O', action: settings('system/power'), disabled: !has('settings') },
        { label: 'Event Viewer', accessKey: 'V', action: settings('system/troubleshoot'), disabled: !has('settings') },
        { label: 'System', accessKey: 'Y', action: settings('system'), disabled: !has('settings') },
        { label: 'Device Manager', accessKey: 'M', action: settings('bluetooth'), disabled: !has('settings') },
        { label: 'Network Connections', accessKey: 'W', action: settings('network'), disabled: !has('settings') },
        { label: 'Disk Management', accessKey: 'K', action: settings('system/storage'), disabled: !has('settings') },
        { label: 'Computer Management', accessKey: 'G', action: settings('system/about'), disabled: !has('settings') },
        { label: 'Terminal', accessKey: 'I', action: () => Shell.apps.launch('terminal'), disabled: !has('terminal') },
        { label: 'Terminal (Admin)', accessKey: 'A', action: admin, disabled: !has('terminal') },
        { separator: true },
        { label: 'Task Manager', accessKey: 'T', action: () => Shell.apps.launch('taskmgr'), disabled: !has('taskmgr') },
        { label: 'Settings', accessKey: 'N', action: settings(), disabled: !has('settings') },
        { label: 'File Explorer', accessKey: 'E', action: () => Shell.apps.launch('explorer'), disabled: !has('explorer') },
        { label: 'Search', accessKey: 'S', action: () => Shell.search?.open?.() },
        { label: 'Run', accessKey: 'R', action: () => Shell.dialogs?.run?.(), disabled: !Shell.dialogs?.run },
        { separator: true },
        { label: 'Shut down or sign out', accessKey: 'U', disabled: !Shell.session, submenu: [
          { label: 'Sign out', accessKey: 'I', action: Shell.session?.signOut ? session('signOut') : session('lock') },
          { label: 'Sleep', accessKey: 'S', action: session('sleep') },
          { label: 'Shut down', accessKey: 'U', action: session('shutdown') },
          { label: 'Restart', accessKey: 'R', action: session('restart') },
        ] },
        { label: 'Desktop', accessKey: 'D', action: () => Shell.wm?.showDesktop?.() },
      ],
    });
  }

  function taskbarMenu(e) {
    showBarMenu({
      x: e.clientX, y: e.clientY, placement: 'top-start',
      items: [{ label: 'Taskbar settings', icon: 'fluent/settings', action: () => Shell.apps.launch('settings', { page: 'taskbar' }) }],
    });
  }

  /* ---------- overflow ("See more") and hidden icons flyouts ---------- */
  /* Esc closes the flyout wherever focus is (a menu open over it closes first). */
  function openFlyout(id, trigger, box, onClose) {
    trigger.classList.add('is-open');
    trigger.setAttribute('aria-expanded', 'true');
    Shell.layer('flyouts').appendChild(box);
    const onKey = (e) => {
      if (e.key !== 'Escape' || Shell.menu?.isOpen?.()) return;
      e.preventDefault();
      e.stopPropagation();
      Shell.transient.close(id);
      trigger.focus();
    };
    document.addEventListener('keydown', onKey, true);
    Shell.transient.open(id, {
      group: 'flyout',
      elements: [box, trigger, (e) => !!thumbs && thumbs.el.contains(e.target)],
      close: () => {
        document.removeEventListener('keydown', onKey, true);
        trigger.classList.remove('is-open');
        trigger.setAttribute('aria-expanded', 'false');
        if (onClose) onClose();
        sink(box);
      },
    });
    rise(box);
  }

  let overflowBox = null;
  function fillOverflow() {
    if (!overflowBox) return;
    const cols = Math.max(1, Math.min(overflowIds.length, 6, Math.floor((window.innerWidth - 24) / PITCH)));
    overflowBox.style.setProperty('--cols', cols);
    overflowBox.replaceChildren(...overflowIds.map((id) => { const b = makeAppButton(id); paint(b, id); return b; }));
    placeAbove(overflowBox, moreBtn, 12);
  }
  const focusFirst = (box) => box.querySelector('button')?.focus();
  /* `fromKeys`: opened with Enter or Space, so focus moves to the first icon. */
  function toggleOverflow(fromKeys) {
    if (Shell.transient.isOpen('taskbar-overflow')) { Shell.transient.close('taskbar-overflow'); return; }
    hideThumbs();
    overflowBox = el('div', 'tb-overflow acrylic surface-flyout', { role: 'toolbar', 'aria-label': 'See more' });
    openFlyout('taskbar-overflow', moreBtn, overflowBox, () => { overflowBox = null; hideThumbs(); });
    fillOverflow();
    if (fromKeys) focusFirst(overflowBox);
  }

  const HIDDEN_ICONS = [
    { icon: 'fluent/shield', label: 'Windows Security - No actions needed.', action: () => Shell.apps.launch('settings', { page: 'privacy' }) },
    { icon: 'fluent/bluetooth', label: 'Bluetooth Devices', when: () => Shell.settings.bluetooth, action: () => Shell.apps.launch('settings', { page: 'bluetooth' }) },
    { icon: 'fluent/hard-drive', label: 'Safely Remove Hardware and Eject Media' },
  ];
  function toggleTray(fromKeys) {
    if (Shell.transient.isOpen('taskbar-tray')) { Shell.transient.close('taskbar-tray'); return; }
    hideThumbs();
    const items = HIDDEN_ICONS.filter((i) => !i.when || i.when());
    const box = el('div', 'tb-trayflyout acrylic surface-flyout', { role: 'toolbar', 'aria-label': 'Hidden icons' });
    box.style.setProperty('--cols', Math.min(items.length, 3));
    items.forEach((i) => {
      const b = button('tb-trayicon', i.label, i.label);
      b.append(glyph(i.icon, 16));
      b.addEventListener('click', () => { Shell.transient.close('taskbar-tray'); if (i.action && Shell.apps.get('settings')) i.action(); });
      box.append(b);
    });
    openFlyout('taskbar-tray', chevronBtn, box);
    placeAbove(box, chevronBtn, 12);
    if (fromKeys) focusFirst(box);
  }

  /* ---------- notification area ---------- */
  function setStatusIcon(key, name, tip) {
    const slot = statusIcons[key];
    slot.dataset.tooltip = tip;
    if (slot._icon === name) return;
    slot._icon = name;
    slot.replaceChildren(Shell.icon(name, { size: 16 }));
  }
  function paintStatus() {
    const s = Shell.settings;
    const v = Math.round(s.volume ?? 62);
    setStatusIcon('net', s.airplane ? 'fluent/airplane' : s.wifi ? 'fluent/wifi' : 'fluent/wifi-off',
      s.airplane ? 'Airplane mode' : s.wifi ? 'Home Wi-Fi\nInternet access' : 'Not connected\nNo connections available');
    setStatusIcon('vol', s.muted || v === 0 ? 'fluent/speaker-mute' : v < 34 ? 'fluent/speaker-0' : v < 67 ? 'fluent/speaker-1' : 'fluent/speaker-2',
      s.muted ? 'Speakers: muted' : `Speakers: ${v}%`);
    setStatusIcon('bat', s.energySaver ? 'fluent/battery-saver' : 'fluent/battery-charge', 'Battery status: 100% available (plugged in)');
    statusBtn.setAttribute('aria-label', `${s.airplane ? 'Airplane mode' : s.wifi ? 'Home Wi-Fi, Internet access' : 'Not connected'}, Speakers ${v}%, Battery 100%`);
  }

  let notifCount = 0;
  function paintClock() {
    const d = Shell.now();
    const t = d.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' });
    const ds = d.toLocaleDateString('en-US');
    if (timeEl.textContent === t && dateEl.textContent === ds) return false;
    timeEl.textContent = t;
    dateEl.textContent = ds;
    clockBtn.dataset.tooltip = d.toLocaleDateString('en-US', { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' });
    paintBadge();
    return true;
  }
  function paintBadge() {
    const dnd = !!Shell.settings.doNotDisturb;
    badgeEl.replaceChildren();
    badgeEl.className = 'tb-badge';
    if (dnd) { badgeEl.classList.add('is-bell'); badgeEl.append(Shell.icon('fluent/alert-off', { size: 16 })); }
    else if (notifCount > 0) badgeEl.textContent = notifCount > 99 ? '99+' : String(notifCount);
    badgeEl.hidden = !dnd && notifCount <= 0;
    const extra = dnd ? ', Do not disturb is on' : notifCount ? `, ${notifCount} new notification${notifCount > 1 ? 's' : ''}` : '';
    clockBtn.setAttribute('aria-label', `Clock ${timeEl.textContent} ${dateEl.textContent}${extra}`);
  }

  /* The wheel over the speaker icon steps the volume by 2 per notch (23H2); its tooltip follows. */
  let wheelAcc = 0, volTip = false;
  function onVolumeWheel(e) {
    e.preventDefault();
    wheelAcc += e.deltaMode ? e.deltaY * 100 : e.deltaY; // line or page mode: one event is one notch
    if (Math.abs(wheelAcc) < 50) return;                  // touchpads: half a notch of travel per step
    const v = Math.max(0, Math.min(100, Math.round(Shell.settings.volume ?? 62) - Math.sign(wheelAcc) * 2));
    wheelAcc = 0;
    if (v !== Shell.settings.volume) Shell.set('volume', v);
    volTip = true;
    Shell.tooltip?.show?.(statusIcons.vol, statusIcons.vol.dataset.tooltip);
  }
  function endVolTip() { if (volTip) { volTip = false; Shell.tooltip?.hide?.(); } }

  /* ---------- Peek at desktop ---------- */
  /* Resting on the Show desktop sliver turns every window into a glass outline; leaving brings
     them back. A click shows the desktop for real; Peek then waits for the pointer to come back. */
  let deskPeekTimer = 0, deskPeeking = false, deskPeekAnimTimer = 0;
  function deskPeek(on) {
    clearTimeout(deskPeekTimer);
    const layer = Shell.layer('windows');
    if (!layer || on === deskPeeking) return;
    deskPeeking = on;
    clearTimeout(deskPeekAnimTimer);
    layer.classList.add('tb-desk-peek-anim');
    layer.classList.toggle('tb-desk-peek', on);
    if (!on) deskPeekAnimTimer = setTimeout(() => layer.classList.remove('tb-desk-peek-anim'), 250); // keep the fade back
  }
  function onPeekEnter() {
    clearTimeout(deskPeekTimer);
    if (Shell.settings.taskbarPeek) deskPeekTimer = setTimeout(() => deskPeek(true), DESK_PEEK_DELAY);
  }
  function onPeekClick() {
    hideThumbs();
    clearTimeout(deskPeekTimer);
    Shell.wm?.showDesktop?.();
    if (deskPeeking) setTimeout(() => deskPeek(false), 300); // the outlines minimize away, then the class goes
  }
  function peekMenu(e) {
    e.preventDefault();
    e.stopPropagation();
    deskPeek(false);
    const r = menuAnchor(peekBtn);
    showBarMenu({ x: r.right, y: r.top, anchorRect: r, placement: 'top', items: [
      { label: 'Show desktop', action: () => Shell.wm?.showDesktop?.() },
      { label: 'Peek at desktop', checked: !!Shell.settings.taskbarPeek, action: () => Shell.set('taskbarPeek', !Shell.settings.taskbarPeek) },
    ] });
  }

  /* ---------- auto-hide ---------- */
  /* Hidden, the taskbar waits below the screen and the work area takes the whole height
     (--taskbar-reserve: 0px). It slides up when the pointer rests on the bottom edge, while a
     surface it owns or opened is open (Start, Search, Quick Settings, notifications, a menu,
     thumbnails), while it holds keyboard focus (Win+T) and while an app flashes; it slides away
     AH_HIDE ms after none of that holds. A revealed taskbar covers windows, as on Windows. */
  let ahHidden = false, ahHover = false, ahTimer = 0, ahRevealTimer = 0, ahHoldUntil = 0;
  const AH_SURFACES = ['taskbar-thumbs', 'taskbar-overflow', 'taskbar-tray',
    ...['start', 'search', 'status', 'clock'].flatMap((k) => SURFACES[k].ids)];
  const ahHeld = () => ahHover || barMenu || bar.contains(document.activeElement) || performance.now() < ahHoldUntil
    || AH_SURFACES.some((id) => Shell.transient.isOpen(id));
  function setHidden(h) {
    if (h === ahHidden) return;
    ahHidden = h;
    bar.classList.toggle('is-autohidden', h);
    Shell.layer('taskbar').classList.toggle('tb-host-hidden', h); // the empty strip must not catch the pointer
  }
  function ahCheck() {
    if (!booted) return;
    clearTimeout(ahTimer);
    if (!Shell.settings.taskbarAutoHide || ahHeld()) { setHidden(false); return; }
    if (!ahHidden) ahTimer = setTimeout(() => { if (Shell.settings.taskbarAutoHide && !ahHeld()) setHidden(true); }, AH_HIDE);
  }
  const ahReveal = () => { ahRevealTimer = 0; ahHover = true; ahCheck(); };
  function ahPointer(e) {
    if (e.pointerType === 'touch') return;
    const H = window.innerHeight;
    if (ahHidden) {
      if (e.clientY < H - AH_EDGE) { clearTimeout(ahRevealTimer); ahRevealTimer = 0; }
      else if (!ahRevealTimer) ahRevealTimer = setTimeout(ahReveal, AH_REVEAL);
      return;
    }
    const over = e.clientY >= H - bar.offsetHeight;
    if (over !== ahHover) { ahHover = over; ahCheck(); }
  }
  /* In a browser window the pointer can leave through the bottom edge instead of resting on it. */
  function ahLeave(e) {
    if (e.relatedTarget || !ahHidden || ahRevealTimer || e.clientY < window.innerHeight - 8) return;
    ahRevealTimer = setTimeout(ahReveal, AH_REVEAL);
  }
  function applyAutoHide() {
    const on = !!Shell.settings.taskbarAutoHide, root = document.documentElement;
    if (on === (root.style.getPropertyValue('--taskbar-reserve') === '0px')) { ahCheck(); return; }
    if (on) root.style.setProperty('--taskbar-reserve', '0px'); else root.style.removeProperty('--taskbar-reserve');
    const listen = on ? 'addEventListener' : 'removeEventListener';
    window[listen]('pointermove', ahPointer, true);
    document[listen]('pointerout', ahLeave, true);
    clearTimeout(ahRevealTimer);
    ahRevealTimer = 0;
    ahHover = false;
    ahCheck();
    window.dispatchEvent(new Event('resize')); // the work area changed: maximized and snapped windows refit
  }

  /* Clicking empty taskbar space makes the taskbar the foreground: the active window turns
     inactive. Shell.wm has no blur call yet, so do what a click on the wallpaper does (only
     the window manager listens for pointerdown on that layer). */
  function deactivateWindows() {
    if (!Shell.wm?.focused?.()) return;
    if (Shell.wm.blur) Shell.wm.blur();
    else Shell.layer('wallpaper')?.dispatchEvent(new PointerEvent('pointerdown'));
    lastBlur = { win: null, t: 0 }; // this blur must not turn the next button click into a minimize
  }

  /* ---------- keyboard ---------- */
  function focusables() { return [...bar.querySelectorAll('button')].filter((b) => !b.hidden && b.offsetParent); }
  function onBarKey(e) {
    const list = focusables(), i = list.indexOf(document.activeElement);
    if (i < 0) return;
    let next = null;
    if (e.key === 'ArrowRight') next = list[(i + 1) % list.length];
    else if (e.key === 'ArrowLeft') next = list[(i - 1 + list.length) % list.length];
    else if (e.key === 'Home') next = list[0];
    else if (e.key === 'End') next = list[list.length - 1];
    else if (e.key === 'ContextMenu' || (e.key === 'F10' && e.shiftKey)) {
      const b = document.activeElement;
      e.preventDefault();
      if (b.dataset.app) jumpList(b.dataset.app, b); else if (b === startBtn) winXMenu();
      return;
    }
    if (next) { e.preventDefault(); next.focus(); }
  }

  /* ---------- mount ---------- */
  function mount() {
    const host = Shell.layer('taskbar');
    if (!host) return;
    bar = el('div', 'tb acrylic', { role: 'toolbar', 'aria-label': 'Taskbar' });
    group = el('div', 'tb-group');

    startBtn = button('tb-slot tb-start', 'Start', 'Start');
    startBtn.append(startGlyph());
    wireSurface(startBtn, 'start');
    startBtn.addEventListener('contextmenu', (e) => { e.preventDefault(); e.stopPropagation(); winXMenu(); });

    searchBox = button('tb-slot tb-searchbox', 'Search');
    searchBox.append(Shell.icon('fluent/search', { size: 16 }), text('span', 'tb-search-text', 'Search'));
    wireSurface(searchBox, 'search');
    searchIcon = button('tb-slot tb-search', 'Search', 'Search');
    searchIcon.append(glyph('fluent/search', 24));
    wireSurface(searchIcon, 'search');
    taskViewBtn = button('tb-slot tb-taskview', 'Task View', 'Task View');
    taskViewBtn.append(glyph('fluent/task-view', 24));
    wireSurface(taskViewBtn, 'taskview');

    appsBox = el('div', 'tb-apps');
    moreBtn = button('tb-slot tb-more', 'See more', 'See more');
    moreBtn.setAttribute('aria-haspopup', 'true');
    moreBtn.append(glyph('fluent/more-horizontal', 20));
    moreBtn.addEventListener('click', (e) => toggleOverflow(e.detail === 0));
    group.append(startBtn, searchBox, searchIcon, taskViewBtn, appsBox, moreBtn);

    tray = el('div', 'tb-tray');
    chevronBtn = button('tb-chevron', 'Show hidden icons', 'Show hidden icons');
    chevronBtn.setAttribute('aria-haspopup', 'true');
    chevronBtn.append(glyph('fluent/chevron-up', 16));
    chevronBtn.addEventListener('click', (e) => toggleTray(e.detail === 0));

    statusBtn = button('tb-status', 'Quick settings');
    ['net', 'vol', 'bat'].forEach((k) => { statusIcons[k] = el('span', 'tb-status-icon'); statusBtn.append(statusIcons[k]); });
    wireSurface(statusBtn, 'status');
    statusIcons.vol.addEventListener('wheel', onVolumeWheel, { passive: false });
    statusBtn.addEventListener('pointerleave', endVolTip);
    statusBtn.addEventListener('pointerdown', endVolTip);

    clockBtn = button('tb-clock', 'Clock');
    const lines = el('span', 'tb-clock-text');
    timeEl = el('span', 'tb-time');
    dateEl = el('span', 'tb-date');
    lines.append(timeEl, dateEl);
    badgeEl = el('span', 'tb-badge');
    clockBtn.append(lines, badgeEl);
    wireSurface(clockBtn, 'clock');

    peekBtn = el('button', 'tb-peek', { type: 'button', 'aria-label': 'Show desktop', 'data-tooltip': 'Show desktop' }); // a sliver, no plate
    peekBtn.addEventListener('click', onPeekClick);
    peekBtn.addEventListener('pointerenter', onPeekEnter);
    peekBtn.addEventListener('pointerleave', () => deskPeek(false));
    peekBtn.addEventListener('contextmenu', peekMenu);
    tray.append(chevronBtn, statusBtn, clockBtn, peekBtn);

    bar.append(group, tray);
    bar.addEventListener('contextmenu', (e) => { e.preventDefault(); taskbarMenu(e); });
    bar.addEventListener('keydown', onBarKey);
    bar.addEventListener('focusin', ahCheck);
    bar.addEventListener('focusout', () => setTimeout(ahCheck));
    bar.addEventListener('pointerdown', (e) => { if (!e.target.closest('button')) deactivateWindows(); });
    host.replaceChildren(bar);

    booted = true;
    adoptDefaultPins();
    paintStatus();
    paintClock();
    sync(false);
    paintSurfaces();
    applyAutoHide();
    if (document.fonts?.ready) document.fonts.ready.then(() => layout(null));
  }

  /* ---------- wiring ---------- */
  Shell.on('boot', () => {
    mount();
    if (!booted) return;
    Shell.on('apps', () => { adoptDefaultPins(); sync(true); });
    Shell.on('window', ({ type, win } = {}) => {
      if (type === 'blur' && win) lastBlur = { win, t: performance.now() };
      sync(true);
      windowMotion(type, win);
      if (thumbs && (!win || win.appId === thumbs.id)) {
        if (thumbs.anchor.isConnected && !thumbs.anchor.hidden) showThumbs(thumbs.id, thumbs.anchor); else hideThumbs();
      }
    });
    Shell.on('setting', ({ key } = {}) => {
      if (['wifi', 'airplane', 'volume', 'muted', 'energySaver'].includes(key)) paintStatus();
      if (key === 'doNotDisturb') paintBadge();
      if (key === 'taskbarAlignment' || key === 'taskbarSearch') layout(rects());
      if (key === 'taskbarFlashing' || key === 'taskbarBadges') sync(false);
      if (key === 'taskbarAutoHide') applyAutoHide();
      if (key === 'taskbarPeek' && !Shell.settings.taskbarPeek) deskPeek(false);
    });
    Shell.on('notifications', ({ count } = {}) => { notifCount = count || 0; paintBadge(); layout(null); });
    Shell.on('tick', () => { if (paintClock()) layout(null); });
    Shell.on('transient', ({ id, open } = {}) => {
      if (!open) { dismissedNow.add(id); setTimeout(() => dismissedNow.delete(id)); }
      else if (id !== 'taskbar-thumbs') hideThumbs();
      paintSurfaces();
      ahCheck();
    });
    window.addEventListener('resize', () => {
      hideThumbs();
      Shell.transient.close('taskbar-tray');
      layout(null);
    });

    Shell.keys.on('Win+X', winXMenu);
    Shell.keys.on('Win+T', () => { const first = order.map((x) => slots.get(x)).find((b) => !b.hidden) || startBtn; first.focus(); });
    Shell.keys.on('Win+B', () => (chevronBtn.offsetParent ? chevronBtn : statusBtn).focus());
    for (let i = 1; i <= 9; i++) Shell.keys.on(`Win+${i}`, () => { const id = order[i - 1]; if (id) activate(id, {}, slotFor(id)); });

    setTimeout(debugOpen);
  });

  /* ?open=taskbar-demo (launch Explorer and Notepad), taskbar-hover, taskbar-thumbs,
     taskbar-jumplist, taskbar-tray, taskbar-overflow, taskbar-left, taskbar-winx,
     taskbar-flash (Explorer asks for attention behind Notepad), taskbar-progress (normal, paused,
     error and indeterminate bars plus a badge), taskbar-autohide, taskbar-peek. */
  function debugOpen() {
    const want = (n) => Shell.debugOpen.includes(n);
    if (want('taskbar-left')) Shell.set('taskbarAlignment', 'left');
    if (want('taskbar-autohide')) Shell.set('taskbarAutoHide', true);
    const apps = want('taskbar-progress') ? ['explorer', 'notepad', 'terminal', 'calculator']
      : ['taskbar-demo', 'taskbar-thumbs', 'taskbar-flash', 'taskbar-peek'].some(want) ? ['explorer', 'notepad'] : [];
    const launch = apps.length > 0 && !runningIds().length;
    if (launch) apps.forEach((id) => { if (Shell.apps.get(id)) Shell.apps.launch(id); });
    /* Open captures once the UI font has landed and the bar has settled, as a person's click would. */
    const settle = (fn, ms) => (document.fonts?.ready || Promise.resolve()).then(() => setTimeout(fn, ms));
    settle(() => {
      if (want('taskbar-flash')) Shell.taskbar.flash('explorer');
      if (want('taskbar-progress')) {
        Shell.taskbar.setProgress('explorer', 0.62);
        Shell.taskbar.setProgress('notepad', 0.35, 'paused');
        Shell.taskbar.setProgress('terminal', 0.8, 'error');
        Shell.taskbar.setProgress('calculator', null, 'indeterminate');
        Shell.taskbar.setBadge('settings', '3');
      }
      if (want('taskbar-peek')) deskPeek(true);
      const first = runningIds()[0] || order[0];
      if (want('taskbar-hover') && order[0]) slots.get(order[0]).classList.add('is-hover');
      if (want('taskbar-thumbs') && first) showThumbs(first, slotFor(first));
      if (want('taskbar-jumplist') && order[0]) jumpList(order[0], slotFor(order[0]));
      if (want('taskbar-tray')) toggleTray();
      if (want('taskbar-overflow') && overflowIds.length) toggleOverflow();
      if (want('taskbar-winx')) winXMenu();
    }, launch ? 900 : 500);
  }

  /* ---------- public API ---------- */
  const NAMED = { start: () => startBtn, search: () => (searchBox && !searchBox.hidden ? searchBox : searchIcon),
    taskview: () => taskViewBtn, tray: () => chevronBtn, quicksettings: () => statusBtn, clock: () => clockBtn, notifications: () => clockBtn };
  Shell.taskbar = {
    /* DOMRect of the app's button; the "See more" button when it overflowed; Start otherwise. */
    buttonRect(appId) {
      const named = NAMED[appId]?.();
      const b = named && !named.hidden ? named : slots.get(appId) ? slotFor(appId) : startBtn;
      if (b && b.isConnected) return b.getBoundingClientRect();
      return new DOMRect(window.innerWidth / 2 - 20, window.innerHeight - 44, 40, 40);
    },
    /* FlashWindowEx: the button pulses orange FLASH_MS, then keeps its orange plate and pill until
       the app is focused (or its last window closes). An auto-hidden taskbar comes up meanwhile. */
    flash(appId) {
      if (!booted || !slots.has(appId) || !winsOf(appId).length || focusedApp() === appId || Shell.settings.taskbarFlashing === false) return;
      flashing.add(appId);
      const sel = `.tb-app[data-app="${CSS.escape(appId)}"]`;
      [moreBtn, ...document.querySelectorAll(`.tb ${sel}, .tb-overflow ${sel}`)].forEach((n) => n.classList.remove('is-flashing'));
      void bar.offsetWidth; // so the pulses start over
      sync(false);
      ahHoldUntil = performance.now() + FLASH_MS;
      ahCheck();
      setTimeout(ahCheck, FLASH_MS + 50);
    },
    /* ITaskbarList3: a bar across the bottom of the button in place of the running pill.
       value 0-1, or null to remove it (an indeterminate bar needs no value). */
    setProgress(appId, value, state = 'normal') {
      if (!['normal', 'paused', 'error', 'indeterminate'].includes(state)) state = 'normal';
      if (value == null && state !== 'indeterminate') progress.delete(appId);
      else progress.set(appId, { value: Math.max(0, Math.min(1, Number(value) || 0)), state });
      sync(false);
    },
    /* Badge on the icon's top-right corner: a count or short text; '' draws a dot; null removes it. */
    setBadge(appId, value) {
      if (value == null) badges.delete(appId); else badges.set(appId, String(value));
      sync(false);
    },
    pin: (appId) => setPinned(appId, true),
    unpin: (appId) => setPinned(appId, false),
    isPinned: (appId) => pins.includes(appId),
  };
})();
