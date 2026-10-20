/* Window manager: every application window and the snap system around it.
   Owns: window chrome (32/48 px title bar, caption buttons, Mica frame, 8 px resize borders),
   focus and z-order, active/inactive look, maximize/minimize/restore with motion, snapping
   (edges, corners, top, the top snap bar, the Snap Layouts flyout, Snap Assist, resizing a snap
   group together), the system menu with keyboard Move and Size, Win+Arrow/D/M/Z/Home and
   Alt+F4, show desktop, virtual desktop bookkeeping and static snapshots for previews.
   Contract: CONTRACT.md "wm". Geometry is in viewport pixels: #windows starts at 0,0. */
(() => {
  'use strict';

  const Shell = window.Shell;

  const EDGE = 4;             // px from a screen edge that arms an edge snap while dragging
  const DRAG_SLOP = 3;        // px a title bar press travels before it becomes a move
  const LAYOUTS_DELAY = 500;  // hover on Maximize before Snap Layouts opens
  const LAYOUTS_GRACE = 300;  // leaving the button and the flyout for this long closes it
  const BAR_REACH = 160;      // a dragged window's pointer this close to the top wakes the snap bar
  const BAR_SCALE = 0.42;     // the snap bar's collapsed size (CSS uses the same number)
  const ZONE_INSET = 8;       // the snap preview and Snap Assist sit this far inside their zone
  const CASCADE = 32;         // each new window opens this far down-right of the last
  const KEY_STEP = 8;         // px per arrow press in keyboard Move and Size
  const HUNG_AFTER = 5000;    // Windows declares a window hung after 5 s without pumping messages (IsHungAppWindow)

  /* Segoe Fluent Icons E921 / E922 / E923 / E8BB redrawn as crisp 10 x 10 paths (1 px strokes on
     half pixels; maximize and restore keep Windows 11's rounded corners). */
  const GLYPH = {
    min: '<svg viewBox="0 0 10 10" aria-hidden="true"><path d="M0 5.5h10"/></svg>',
    max: '<svg viewBox="0 0 10 10" aria-hidden="true"><rect x=".5" y=".5" width="9" height="9" rx="1.5"/></svg>',
    restore: '<svg viewBox="0 0 10 10" aria-hidden="true"><rect x=".5" y="2.5" width="7" height="7" rx="1.5"/><path d="M2.5 2.5V2A1.5 1.5 0 0 1 4 .5h4A1.5 1.5 0 0 1 9.5 2v4A1.5 1.5 0 0 1 8 7.5h-.5"/></svg>',
    close: '<svg viewBox="0 0 10 10" aria-hidden="true"><path d="M.5.5l9 9m0-9l-9 9"/></svg>',
  };
  /* Title bar presses on these do their own thing instead of moving the window. */
  const NO_DRAG = 'button, input, select, textarea, a, [role="tab"], [role="button"], [contenteditable=""], [contenteditable="true"], [data-no-drag], .window-titlebar-icon';

  const windows = [];         // bottom-to-top, every desktop
  const S = new Map();        // handle -> private state
  const desktops = [{ name: 'Desktop 1' }];
  let current = 0;
  let focusedWin = null;
  let shownDesktop = null;    // windows Show desktop minimized, brought back by the next toggle
  let layouts = null;         // the open Snap Layouts flyout
  let assist = null;          // the running Snap Assist session
  let nextId = 1;

  const h = (tag, className, attrs = {}) => {
    const el = document.createElement(tag);
    if (className) el.className = className;
    for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
    return el;
  };
  const emit = (type, win) => Shell.emit('window', { type, win });

  /* ---------- geometry ---------- */
  function workArea() {
    const layer = Shell.layer('windows');
    const width = layer ? layer.clientWidth : window.innerWidth;
    const height = layer ? layer.clientHeight : window.innerHeight - 48;
    return { x: 0, y: 0, left: 0, top: 0, width, height, right: width, bottom: height };
  }
  const Z = (x, y, w, hh) => ({ x, y, w, h: hh });
  const NAMED = {
    left: Z(0, 0, 0.5, 1), right: Z(0.5, 0, 0.5, 1),
    'top-left': Z(0, 0, 0.5, 0.5), 'top-right': Z(0.5, 0, 0.5, 0.5),
    'bottom-left': Z(0, 0.5, 0.5, 0.5), 'bottom-right': Z(0.5, 0.5, 0.5, 0.5),
    top: Z(0, 0, 1, 0.5), bottom: Z(0, 0.5, 1, 0.5),
  };
  const QUARTERS = ['top-left', 'top-right', 'bottom-left', 'bottom-right'];
  const sameZone = (a, b) => !!a && !!b && ['x', 'y', 'w', 'h'].every((k) => Math.abs(a[k] - b[k]) < 0.002);
  const nameOf = (z) => Object.keys(NAMED).find((n) => sameZone(NAMED[n], z)) || null;
  /* Zones are fractions of the work area so they follow its size; neighbours share edges exactly. */
  function zoneRect(z, wa = workArea()) {
    const x = Math.round(z.x * wa.width), y = Math.round(z.y * wa.height);
    return { x, y, w: Math.round((z.x + z.w) * wa.width) - x, h: Math.round((z.y + z.h) * wa.height) - y };
  }
  const toZone = (r, wa = workArea()) => Z(r.x / wa.width, r.y / wa.height, r.w / wa.width, r.h / wa.height);
  const shrink = (r, d) => ({ x: r.x + d, y: r.y + d, w: Math.max(0, r.w - 2 * d), h: Math.max(0, r.h - 2 * d) });
  const geo = (r) => ({ left: `${r.x}px`, top: `${r.y}px`, width: `${r.w}px`, height: `${r.h}px` });
  const within = (r, x, y, pad = 0) => x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y - pad && y <= r.y + r.h + pad;
  const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), Math.max(lo, hi));

  /* The layouts Windows 11 offers for the screen's shape: six on wide screens, four on common
     laptop widths, two when narrow; stacked rows on portrait screens. */
  function layoutsFor(wa = workArea()) {
    const t = 1 / 3;
    const quarters = QUARTERS.map((n) => NAMED[n]);
    if (wa.height > wa.width) return [[NAMED.top, NAMED.bottom], [Z(0, 0, 1, t), Z(0, t, 1, t), Z(0, 2 * t, 1, t)], quarters];
    const halves = [NAMED.left, NAMED.right];
    if (wa.width < 1000) return [halves, quarters];
    const wide = wa.width >= 1920;
    return [
      halves,
      [Z(0, 0, 2 * t, 1), Z(2 * t, 0, t, 1)],
      ...(wide ? [[Z(0, 0, t, 1), Z(t, 0, t, 1), Z(2 * t, 0, t, 1)]] : []),
      [NAMED.left, NAMED['top-right'], NAMED['bottom-right']],
      quarters,
      ...(wide ? [[Z(0, 0, 0.25, 1), Z(0.25, 0, 0.5, 1), Z(0.75, 0, 0.25, 1)]] : []),
    ];
  }
  /* What Snap Assist offers to fill after a drag or Win+Arrow snap. */
  function restOf(zone) {
    const n = nameOf(zone);
    if (n === 'left' || n === 'right') return [NAMED[n === 'left' ? 'right' : 'left']];
    if (n === 'top' || n === 'bottom') return [NAMED[n === 'top' ? 'bottom' : 'top']];
    if (QUARTERS.includes(n)) return QUARTERS.filter((q) => q !== n).map((q) => NAMED[q]);
    return [];
  }

  /* ---------- window state ---------- */
  function visibleRect(win) {
    const st = S.get(win);
    if (st.mode === 'max') { const wa = workArea(); return { x: 0, y: 0, w: wa.width, h: wa.height }; }
    if (st.mode === 'snap') return zoneRect(st.zone);
    return { ...st.rect };
  }
  /* A normal window keeps its minimum size, fits the work area, and keeps its title bar reachable. */
  function fit(win, r) {
    const st = S.get(win), wa = workArea();
    const w = Math.round(Math.min(Math.max(r.w, st.minW), wa.width));
    const hh = Math.round(Math.min(Math.max(r.h, st.minH), wa.height));
    const grip = Math.min(w, 96);
    return {
      x: Math.round(clamp(r.x, grip - w, wa.width - grip)),
      y: Math.round(clamp(r.y, 0, wa.height - Math.max(st.tbh, 32))),
      w, h: hh,
    };
  }
  function render(win, from) {
    const st = S.get(win), r = visibleRect(win), el = win.el;
    el.classList.toggle('maximized', st.mode === 'max');
    el.classList.toggle('snapped', st.mode === 'snap');
    Object.assign(el.style, geo(r));
    if (st.btnMax) {
      const max = st.mode === 'max';
      st.btnMax.innerHTML = max ? GLYPH.restore : GLYPH.max;
      st.btnMax.setAttribute('aria-label', max ? 'Restore Down' : 'Maximize');
    }
    if (from) morph(win, from, r);
  }
  /* Maximize, restore and snap animate the frame's geometry (not a transform, so Mica keeps
     sampling the wallpaper under the window). The final geometry is already set. */
  function morph(win, from, to, duration = 250) {
    const st = S.get(win);
    st.anim?.cancel();
    st.anim = null;
    if (Shell.reducedMotion() || ['x', 'y', 'w', 'h'].every((k) => Math.round(from[k]) === Math.round(to[k]))) return;
    st.anim = win.el.animate([geo(from), geo(to)], { duration, easing: Shell.ease.in });
  }

  /* ---------- focus and z-order ---------- */
  const restack = () => windows.forEach((w, i) => { w.el.style.zIndex = String((i + 1) * 2); });
  function raise(win) {
    const i = windows.indexOf(win);
    if (i < 0 || i === windows.length - 1) return;
    windows.splice(i, 1);
    windows.push(win);
    restack();
  }
  const onDesk = (w) => w.desktop === current;
  const list = () => windows.filter(onDesk);
  const topVisible = (except) => [...windows].reverse().find((w) => w !== except && onDesk(w) && !S.get(w).minimized) || null;

  function focusWin(win, { pointer = false } = {}) {
    const st = S.get(win);
    if (!st) return;
    if (win.desktop !== current) setDesktop(win.desktop);
    if (st.minimized) unminimize(win);
    raise(win);
    shownDesktop = null;
    if (focusedWin !== win) {
      const prev = focusedWin;
      focusedWin = win;
      if (prev) { prev.el.classList.remove('active'); emit('blur', prev); }
      win.el.classList.add('active');
      emit('focus', win);
    }
    if (!pointer && !win.el.contains(document.activeElement)) {
      const last = st.lastFocus;
      (last && last.isConnected && win.el.contains(last) ? last : win.el).focus({ preventScroll: true });
    }
  }
  function blurAll() {
    const prev = focusedWin;
    if (!prev) return;
    focusedWin = null;
    prev.el.classList.remove('active');
    if (prev.el.contains(document.activeElement)) document.activeElement.blur();
    emit('blur', prev);
  }
  function focusNext(except) {
    const next = topVisible(except);
    if (next) focusWin(next); else blurAll();
  }

  /* Tab and Shift+Tab cycle through the window's own controls and never reach the taskbar or
     another window, as in Windows 11 (the taskbar is reached by Win+T, Win+B or Alt+Tab).
     Caption buttons carry tabindex -1 and stay out of the cycle. */
  const TABBABLE = 'a[href], button, input:not([type="hidden"]), select, textarea, iframe, summary, [tabindex], [contenteditable]:not([contenteditable="false"])';
  function tabStops(root) {
    return [...root.querySelectorAll(TABBABLE)].filter((n) => n.tabIndex >= 0 && !n.matches(':disabled')
      && !n.closest('[inert]') && n.checkVisibility({ visibilityProperty: true })
      /* a radio group is one stop: its checked radio */
      && !(n.type === 'radio' && !n.checked && n.name && root.querySelector(`input[type="radio"][name="${CSS.escape(n.name)}"]:checked`)));
  }
  function cycleTab(root, e) {
    if (e.key !== 'Tab' || e.defaultPrevented || e.ctrlKey || e.altKey || e.metaKey) return;
    e.preventDefault();
    const stops = tabStops(root);
    if (!stops.length) return;
    const a = document.activeElement, i = stops.indexOf(a);
    const F = Node.DOCUMENT_POSITION_FOLLOWING, P = Node.DOCUMENT_POSITION_PRECEDING;
    let next;
    if (i >= 0) next = stops[(i + (e.shiftKey ? stops.length - 1 : 1)) % stops.length];
    else if (e.shiftKey) next = stops.findLast((n) => a.compareDocumentPosition(n) & P) || stops[stops.length - 1];
    else next = stops.find((n) => a.compareDocumentPosition(n) & F) || stops[0];
    next.focus();
  }

  /* ---------- minimize, maximize, restore, snap ---------- */
  /* Toward (or from) the app's taskbar button, shrinking and fading as Windows does. */
  function taskbarTransform(win, r) {
    const b = Shell.taskbar?.buttonRect?.(win.appId);
    const tx = b && b.width ? b.left + b.width / 2 : window.innerWidth / 2;
    const ty = b && b.height ? b.top + b.height / 2 : window.innerHeight - 24;
    const s = clamp(160 / Math.max(r.w, 1), 0.08, 0.3);
    return `translate(${Math.round(tx - (r.x + r.w / 2))}px, ${Math.round(ty - (r.y + r.h / 2))}px) scale(${s.toFixed(3)})`;
  }
  function minimize(win, { refocus = true } = {}) {
    const st = S.get(win);
    if (!st || st.minimized) return;
    st.minimized = true;
    clearTimeout(st.layoutsTimer);
    if (layouts?.win === win) closeLayouts();
    st.anim?.cancel();
    st.minAnim?.cancel();
    const el = win.el;
    el.classList.add('minimizing');
    const a = el.animate(
      [{ transform: 'none', opacity: 1 }, { transform: taskbarTransform(win, visibleRect(win)), opacity: 0 }],
      { duration: Shell.reducedMotion() ? 0 : 250, easing: Shell.ease.standard, fill: 'forwards' },
    );
    st.minAnim = a;
    a.finished.then(() => {
      if (st.minimized) el.classList.add('minimized');
      el.classList.remove('minimizing');
      a.cancel();
    }).catch(() => { /* restored mid-flight */ });
    if (focusedWin === win) { if (refocus) focusNext(win); else blurAll(); }
    emit('minimize', win);
  }
  function unminimize(win) {
    const st = S.get(win);
    if (!st || !st.minimized) return;
    st.minimized = false;
    st.minAnim?.cancel();
    st.minAnim = null;
    win.el.classList.remove('minimized', 'minimizing');
    render(win);
    Shell.animate(win.el, [{ transform: taskbarTransform(win, visibleRect(win)), opacity: 0 }, { transform: 'none', opacity: 1 }], { duration: 250 });
    emit('restore', win);
  }
  function setMode(win, mode, zone = null, animate = true) {
    const st = S.get(win);
    const from = st.minimized ? null : visibleRect(win);
    st.mode = mode;
    st.zone = zone;
    if (st.minimized) { unminimize(win); return; }
    render(win, animate ? from : null);
  }
  function maximize(win) {
    const st = S.get(win);
    if (!st || !st.maximizable || (st.mode === 'max' && !st.minimized)) return;
    setMode(win, 'max');
    emit('maximize', win);
  }
  function restore(win) {
    const st = S.get(win);
    if (!st) return;
    if (st.minimized) { unminimize(win); return; }
    if (st.mode === 'normal') return;
    setMode(win, 'normal');
    emit('restore', win);
  }
  function toggleMaximize(win) {
    if (S.get(win)?.mode === 'max') restore(win); else maximize(win);
  }
  /* zone: a name (left, right, top-left, top-right, bottom-left, bottom-right, top, bottom),
     'maximize', 'restore', or fractions of the work area {x, y, w, h}. */
  function snapTo(win, zone, animate = true) {
    const st = S.get(win);
    if (!st) return;
    if (zone === 'maximize' || zone === 'max') { maximize(win); return; }
    if (zone === 'restore' || zone === 'normal') { restore(win); return; }
    const z = typeof zone === 'string' ? NAMED[zone] : zone;
    if (!z || !st.resizable) return;
    setMode(win, 'snap', { ...z }, animate);
    emit('snap', win);
  }
  function stretchVertically(win) {
    const st = S.get(win);
    if (!st || !st.resizable || st.mode === 'max') return;
    const from = visibleRect(win);
    if (st.mode === 'snap') { st.rect = { ...from }; st.mode = 'normal'; st.zone = null; }
    st.rect = { ...st.rect, y: 0, h: workArea().height };
    render(win, from);
  }

  /* ---------- close ---------- */
  /* {force}: End task. A hung window cannot answer WM_CLOSE, so its ghost asks instead; force
     terminates it (onClose still runs for cleanup, its veto is ignored). */
  function close(win, { force = false } = {}) {
    const st = S.get(win);
    if (!st) return false;
    if (st.hung && !force) { if (st.ghost) hangDialog(win); return false; }
    if (st.opts.onClose && st.opts.onClose() === false && !force) return false;
    S.delete(win);
    windows.splice(windows.indexOf(win), 1);
    clearTimeout(st.layoutsTimer);
    clearTimeout(st.hungTimer);
    closeHangDialog(st);
    if (layouts?.win === win) closeLayouts();
    if (assist) {
      if (assist.anchor === win) endAssist();
      else if (assist.candidates.includes(win)) {
        assist.candidates = assist.candidates.filter((w) => w !== win);
        if (assist.candidates.length) renderAssist(); else endAssist();
      }
    }
    if (shownDesktop) shownDesktop = shownDesktop.filter((w) => w !== win);
    if (focusedWin === win) {
      focusedWin = null;
      win.el.classList.remove('active');
      focusNext(win);
    }
    emit('close', win);
    const el = win.el;
    el.style.pointerEvents = 'none';
    st.anim?.cancel();
    if (st.minimized) { el.remove(); return true; }
    Shell.animate(el, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'scale(0.95)' }],
      { duration: 167, easing: Shell.ease.out, fill: 'forwards' }).then(() => el.remove());
    return true;
  }

  /* ---------- not responding (the ghost window) ---------- */
  /* A hung window stops taking input at once (its content and custom title bar go inert). After
     HUNG_AFTER Windows swaps in its ghost: same frame, the title gains " (Not Responding)", the
     client area washes white, the pointer is busy over it, and a click or Close asks Windows
     Error Reporting's question. Windows with their own title bar content get the ghost's plain
     title strip over it. isNotResponding() is the ghost state, as IsHungAppWindow reports it. */
  const HANG_CLOSE = 'Close the program', HANG_WAIT = 'Wait for the program to respond';
  function setNotResponding(win, on, { after = HUNG_AFTER } = {}) {
    const st = S.get(win);
    if (!st || !!st.hung === !!on) return;
    st.hung = !!on;
    clearTimeout(st.hungTimer);
    win.el.classList.toggle('hung', st.hung);
    const content = st.titlebar?.querySelector(':scope > .window-titlebar-content');
    if (st.hung) {
      st.inertBefore = [win.body.inert, content?.inert];
      win.body.inert = true;
      if (content) content.inert = true;
      st.hungTimer = setTimeout(() => setGhost(win, true), Math.max(0, after));
      return;
    }
    win.body.inert = !!st.inertBefore?.[0];
    if (content) content.inert = !!st.inertBefore?.[1];
    setGhost(win, false);
    if (focusedWin === win) focusWin(win); // focus goes back into the content
  }
  function setGhost(win, on) {
    const st = S.get(win);
    if (!st || !!st.ghost === on) return;
    const real = st.realTitle ?? win.title;
    st.ghost = on;
    win.el.classList.toggle('ghost', on);
    if (on && st.opts.titlebarContent && !st.ghostBar) {
      st.ghostBar = h('div', 'wm-ghost-title', { 'aria-hidden': 'true' });
      const icon = h('span', 'window-titlebar-icon');
      icon.append(Shell.icon(win.icon, { size: 16 }));
      st.ghostTitle = h('span', 'window-title');
      st.ghostBar.append(icon, st.ghostTitle);
      st.titlebar.prepend(st.ghostBar);
    }
    win.setTitle(real);
    if (!on) closeHangDialog(st);
    emit(on ? 'hung' : 'responding', win);
  }
  async function hangDialog(win) {
    const st = S.get(win);
    if (!st || st.hangDialog) { st?.hangDialog?.focus?.(); return; }
    const name = Shell.apps?.get?.(win.appId)?.name || st.realTitle || win.title;
    const o = {
      title: name, instruction: `${name} is not responding`, content: 'If you close the program, you might lose information.',
      commandLinks: [{ label: HANG_CLOSE }, { label: HANG_WAIT }], owner: win,
    };
    let answer = null;
    if (Shell.dialogs?.taskDialog) {
      st.hangDialog = {};
      try {
        const known = new Set(windows);
        const done = Shell.dialogs.taskDialog(o);
        // its window, so that responding again closes it (closeHangDialog) and it answers nothing
        st.hangDialog = windows.find((w) => !known.has(w)) || st.hangDialog;
        answer = (await done)?.button;
      } catch { answer = null; }
    } else answer = await hangWindow(win, st, o);
    if (S.get(win) !== st) return; // closed meanwhile
    st.hangDialog = null;
    if (answer === HANG_CLOSE || answer?.label === HANG_CLOSE) close(win, { force: true });
  }
  /* Without the dialogs module: the task dialog as its own small window, centred on the owner. */
  function hangWindow(win, st, o) {
    return new Promise((resolve) => {
      let answer = null;
      const content = h('div', 'wm-hang', { role: 'alertdialog' });
      const head = h('div', 'wm-hang-instruction');
      head.textContent = o.instruction;
      const text = h('p', 'wm-hang-text');
      text.textContent = o.content;
      content.append(head, text);
      for (const { label } of o.commandLinks) {
        const b = h('button', 'wm-hang-link', { type: 'button' });
        const span = h('span');
        span.textContent = label;
        b.append(Shell.icon('fluent/arrow-right', { size: 16 }), span);
        b.addEventListener('click', () => { answer = label; d.close(); });
        content.append(b);
      }
      content.addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.preventDefault(); d.close(); } });
      const r = visibleRect(win), W = 440, H = 232;
      const d = open({
        appId: 'wm-hang', title: o.title, icon: win.icon, width: W, height: H, x: Math.round(r.x + (r.w - W) / 2), y: Math.round(r.y + (r.h - H) / 2),
        resizable: false, maximizable: false, minimizable: false, background: 'solid', content, onClose: () => resolve(answer),
      });
      content.setAttribute('aria-label', o.instruction);
      st.hangDialog = d;
      content.querySelector('.wm-hang-link')?.focus();
    });
  }
  function closeHangDialog(st) {
    const d = st.hangDialog;
    st.hangDialog = null;
    d?.close?.();
  }

  /* ---------- pointer tracking (move, resize) ---------- */
  /* Captures the pointer on `el`; Escape cancels. onEnd(lastEvent, cancelled). */
  function track(e, el, onMove, onEnd) {
    const id = e.pointerId;
    try { el.setPointerCapture(id); } catch { /* pointer already gone */ }
    let last = e, done = false;
    const move = (ev) => { if (ev.pointerId === id) { last = ev; onMove(ev); } };
    const up = (ev) => { if (ev.pointerId === id) { last = ev; finish(false); } };
    const cancel = (ev) => { if (ev.pointerId === id) finish(true); };
    const key = (ev) => { if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); finish(true); } };
    function finish(cancelled) {
      if (done) return;
      done = true;
      el.removeEventListener('pointermove', move);
      el.removeEventListener('pointerup', up);
      el.removeEventListener('pointercancel', cancel);
      window.removeEventListener('keydown', key, true);
      try { el.releasePointerCapture(id); } catch { /* already released */ }
      onEnd(last, cancelled);
    }
    el.addEventListener('pointermove', move);
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', cancel);
    window.addEventListener('keydown', key, true);
  }
  function setCursor(c) {
    const root = document.documentElement;
    if (c) root.style.setProperty('--wm-cursor', c); else root.style.removeProperty('--wm-cursor');
    root.classList.toggle('wm-cursor', !!c);
  }
  /* AppStarting: for about a second after a launch the arrow is the arrow with the working ring
     (css/cursors.css .app-starting), as Windows shows while a new process starts. */
  let appStarting = 0;
  Shell.on('app-launch', () => {
    document.documentElement.classList.add('app-starting');
    clearTimeout(appStarting);
    appStarting = setTimeout(() => document.documentElement.classList.remove('app-starting'), 1000);
  });

  /* ---------- move and snap by dragging the title bar ---------- */
  function beginMove(win, e) {
    const st = S.get(win);
    const sx = e.clientX, sy = e.clientY;
    const before = { mode: st.mode, zone: st.zone, rect: { ...st.rect } };
    let moving = false, off = null, target = null;
    track(e, e.currentTarget, (ev) => {
      if (!moving) {
        if (Math.hypot(ev.clientX - sx, ev.clientY - sy) < DRAG_SLOP) return;
        moving = true;
        st.anim?.cancel();
        endAssist();
        closeLayouts();
        const cur = visibleRect(win);
        if (st.mode !== 'normal') {
          /* A maximized or snapped window comes back to its own size under the press point. */
          const fx = clamp((sx - cur.x) / cur.w, 0, 1);
          st.rect = { ...st.rect, x: Math.round(sx - fx * st.rect.w), y: cur.y };
          st.mode = 'normal';
          st.zone = null;
          render(win);
          emit('restore', win);
        }
        off = { x: st.rect.x - sx, y: st.rect.y - sy }; // the grab point, so the slop's distance is not lost
        win.el.classList.add('moving');
      }
      st.rect.x = ev.clientX + off.x;
      st.rect.y = Math.max(0, ev.clientY + off.y);
      Object.assign(win.el.style, geo(st.rect));
      target = snapTarget(win, ev.clientX, ev.clientY);
      if (target) previewShow(zoneRect(target.max ? Z(0, 0, 1, 1) : target.zone), ev.clientX, ev.clientY);
      else previewHide(ev.clientX, ev.clientY);
    }, (ev, cancelled) => {
      if (!moving) return;
      win.el.classList.remove('moving');
      barHide();
      if (cancelled) {
        previewHide(ev.clientX, ev.clientY);
        const from = { ...st.rect };
        Object.assign(st, { mode: before.mode, zone: before.zone, rect: before.rect });
        render(win, from);
        return;
      }
      const from = { ...st.rect };
      if (target) {
        previewHide();
        st.rect = before.rect; // restoring later returns to where the window was before the drag
        if (target.max) { st.mode = 'max'; st.zone = null; render(win, from); emit('maximize', win); return; }
        st.mode = 'snap';
        st.zone = { ...target.zone };
        render(win, from);
        emit('snap', win);
        startAssist(win, target.rest || restOf(target.zone));
        return;
      }
      previewHide(ev.clientX, ev.clientY);
      st.rect = fit(win, st.rect);
      render(win, from);
    });
  }
  /* Where a drag at (x, y) would snap: the top bar's zones first, then edges and corners. */
  function snapTarget(win, x, y) {
    const st = S.get(win);
    if (!st.resizable) return null;
    const bar = barUpdate(x, y);
    if (bar) return bar.zone ? bar : null;
    const wa = workArea(), W = wa.width;
    const cy = Math.max(48, wa.height * 0.15), cx = Math.max(48, W * 0.08);
    if (x <= EDGE || x >= W - 1 - EDGE) {
      const side = x <= EDGE ? 'left' : 'right';
      const name = y < cy ? `top-${side}` : y > wa.height - cy ? `bottom-${side}` : side;
      return { zone: NAMED[name] };
    }
    if (y <= EDGE) {
      if (x < cx) return { zone: NAMED['top-left'] };
      if (x > W - cx) return { zone: NAMED['top-right'] };
      return st.maximizable ? { max: true } : null;
    }
    return null;
  }

  /* The snap preview: a rounded, blurred, accent-tinted rectangle just under the dragged window,
     grown from the pointer and morphed between zones by CSS transitions. FLIP: it is laid out at its
     zone once and only its transform animates. While it moves it drops its blur (.moving) and the blur
     fades back in once it settles: re-blurring a moving box drops frames (measured in software
     compositing: 33-83 ms frames with the blur, 16.7 ms without), and under an 85-96 % fill it barely shows. */
  const preview = { el: null, on: false, timer: 0, settle: 0, box: null };
  const pointerBox = (x, y) => ({ x: Math.round(x - 20), y: Math.round(y - 20), w: 40, h: 40 });
  const toward = (box, to) => `translate(${to.x - box.x}px, ${to.y - box.y}px) scale(${to.w / Math.max(1, box.w)}, ${to.h / Math.max(1, box.h)})`;
  function previewShow(rect, x, y) {
    let el = preview.el;
    if (!el) {
      el = preview.el = h('div', 'wm-snap-preview acrylic', { 'aria-hidden': 'true' });
      Shell.layer('windows').append(el);
    }
    clearTimeout(preview.timer);
    el.style.zIndex = String(windows.length * 2 - 1);
    const box = shrink(rect, ZONE_INSET), old = preview.box;
    if (preview.on && old.x === box.x && old.y === box.y && old.w === box.w && old.h === box.h) return;
    clearTimeout(preview.settle);
    el.classList.add('moving');
    preview.settle = setTimeout(() => el.classList.remove('moving'), Shell.reducedMotion() ? 0 : 250);
    let from = pointerBox(x, y);
    if (preview.on) { // morph from where it is drawn now, mid-transition included
      const b = el.getBoundingClientRect(), p = el.parentElement.getBoundingClientRect();
      from = { x: b.left - p.left, y: b.top - p.top, w: b.width, h: b.height };
    } else el.style.opacity = '0';
    el.style.transitionProperty = 'opacity'; // jump the transform to its start without stopping a fade
    Object.assign(el.style, geo(box), { display: 'block', transform: toward(box, from) });
    el.getBoundingClientRect(); // commit the starting transform before transitioning out of it
    el.style.transitionProperty = '';
    Object.assign(el.style, { transform: 'none', opacity: '1' });
    preview.on = true;
    preview.box = box;
  }
  function previewHide(x, y) {
    const el = preview.el;
    if (!el || !preview.on) return;
    preview.on = false;
    clearTimeout(preview.settle);
    el.classList.add('moving');
    if (x != null) el.style.transform = toward(preview.box, pointerBox(x, y));
    el.style.opacity = '0';
    preview.timer = setTimeout(() => { if (!preview.on) el.style.display = 'none'; }, 260);
  }

  /* The top snap bar (Windows 11 22H2): a pill at the top centre while a window is dragged near
     the top; reaching it opens the layouts, and dropping on a zone snaps there. */
  const bar = { el: null, state: 'hidden', set: null, rect: null, timer: 0 };
  function barState(state) {
    bar.state = state;
    bar.el.dataset.state = state;
  }
  function barUpdate(x, y) {
    if (y > BAR_REACH && bar.state !== 'expanded') { barHide(); return null; }
    if (!bar.el) {
      const wa = workArea();
      bar.set = layoutsFor(wa);
      bar.el = buildLayouts(bar.set, wa);
      bar.el.classList.add('wm-snapbar', 'acrylic', 'surface-flyout');
      bar.el.setAttribute('aria-hidden', 'true');
      bar.el.dataset.state = 'hidden';
      Shell.layer('flyouts').append(bar.el);
      const w = bar.el.offsetWidth, hh = bar.el.offsetHeight;
      bar.rect = { x: Math.round((window.innerWidth - w) / 2), y: 8, w, h: hh };
      bar.el.style.left = `${bar.rect.x}px`;
      bar.el.getBoundingClientRect();
    }
    clearTimeout(bar.timer);
    const r = bar.rect;
    const small = { x: r.x + (r.w * (1 - BAR_SCALE)) / 2, y: r.y, w: r.w * BAR_SCALE, h: r.h * BAR_SCALE };
    if (bar.state === 'hidden') barState('collapsed');
    if (bar.state === 'collapsed' && within(small, x, y)) barState('expanded'); // the edge above it still maximizes
    else if (bar.state === 'expanded' && !within(r, x, y, 24)) barState(y > BAR_REACH ? 'hidden' : 'collapsed');
    let hot = null;
    if (bar.state === 'expanded') {
      hot = [...bar.el.querySelectorAll('.wm-zone')].find((z) => {
        const b = z.getBoundingClientRect();
        return within({ x: b.left, y: b.top, w: b.width, h: b.height }, x, y);
      }) || null;
    }
    bar.el.querySelectorAll('.wm-zone.hot').forEach((z) => { if (z !== hot) z.classList.remove('hot'); });
    if (bar.state !== 'expanded') return null;
    if (!hot) return { over: true };
    hot.classList.add('hot');
    const li = +hot.dataset.layout, zi = +hot.dataset.zone;
    return { zone: bar.set[li][zi], rest: bar.set[li].filter((_, i) => i !== zi) };
  }
  function barHide() {
    const el = bar.el;
    if (!el) return;
    bar.el = null;
    bar.state = 'hidden';
    el.dataset.state = 'hidden';
    setTimeout(() => el.remove(), 200);
  }

  /* ---------- resize from the invisible borders ---------- */
  function resized(win, r, edges, dx, dy) {
    const st = S.get(win);
    let { x, y, w, h: hh } = r;
    if (edges.includes('e')) w = Math.max(st.minW, w + dx);
    if (edges.includes('s')) hh = Math.max(st.minH, hh + dy);
    if (edges.includes('w')) { const right = x + w; x = Math.min(x + dx, right - st.minW); w = right - x; }
    if (edges.includes('n')) { const bottom = y + hh; y = Math.min(Math.max(0, y + dy), bottom - st.minH); hh = bottom - y; }
    return { x: Math.round(x), y: Math.round(y), w: Math.round(w), h: Math.round(hh) };
  }
  const CURSOR = { n: 'var(--cursor-ns)', s: 'var(--cursor-ns)', e: 'var(--cursor-ew)', w: 'var(--cursor-ew)', nw: 'var(--cursor-nwse)', se: 'var(--cursor-nwse)', ne: 'var(--cursor-nesw)', sw: 'var(--cursor-nesw)' }; // css/cursors.css
  function beginResize(win, e, edges) {
    if (e.button !== 0) return;
    e.preventDefault();
    const st = S.get(win);
    const start = visibleRect(win);
    const snapped = st.mode === 'snap';
    /* Snapped neighbours sharing the dragged edge follow it, as a Windows 11 snap group does. */
    const group = snapped ? list().filter((w) => w !== win && S.get(w).mode === 'snap' && !S.get(w).minimized)
      .map((w) => ({ w, r: visibleRect(w), zone: S.get(w).zone })) : [];
    const zone0 = st.zone;
    st.anim?.cancel();
    endAssist();
    closeLayouts();
    setCursor(CURSOR[edges]);
    /* Capture on the border itself, so a double-click on the top or bottom border still targets it. */
    track(e, e.target.closest('.wm-resize') || e.currentTarget, (ev) => {
      let r = resized(win, start, edges, ev.clientX - e.clientX, ev.clientY - e.clientY);
      if (snapped) { r = followEdges(r, start, edges, group); st.zone = toZone(r); } else st.rect = r;
      Object.assign(win.el.style, geo(r));
    }, (ev, cancelled) => {
      setCursor('');
      if (cancelled) {
        if (snapped) { st.zone = zone0; group.forEach((g) => { S.get(g.w).zone = g.zone; render(g.w); }); } else st.rect = start;
        render(win);
        return;
      }
      if (snapped) { render(win); group.forEach((g) => render(g.w)); return; }
      const from = { ...st.rect };
      st.rect = fit(win, st.rect);
      render(win, from);
    });
  }
  function followEdges(r, start, edges, group) {
    const touches = [];
    for (const g of group) {
      const n = g.r;
      const v = n.y < start.y + start.h && n.y + n.h > start.y;
      const hz = n.x < start.x + start.w && n.x + n.w > start.x;
      if (edges.includes('e') && v && Math.abs(n.x - (start.x + start.w)) <= 1) touches.push([g, 'w']);
      if (edges.includes('w') && v && Math.abs(n.x + n.w - start.x) <= 1) touches.push([g, 'e']);
      if (edges.includes('s') && hz && Math.abs(n.y - (start.y + start.h)) <= 1) touches.push([g, 'n']);
      if (edges.includes('n') && hz && Math.abs(n.y + n.h - start.y) <= 1) touches.push([g, 's']);
    }
    /* The shared edge stops where a neighbour reaches its minimum size. */
    const out = { ...r };
    for (const [g, side] of touches) {
      const m = S.get(g.w);
      if (side === 'w') out.w = Math.min(out.w, g.r.x + g.r.w - m.minW - out.x);
      if (side === 'e') { const right = out.x + out.w; out.x = Math.max(out.x, g.r.x + m.minW); out.w = right - out.x; }
      if (side === 'n') out.h = Math.min(out.h, g.r.y + g.r.h - m.minH - out.y);
      if (side === 's') { const bottom = out.y + out.h; out.y = Math.max(out.y, g.r.y + m.minH); out.h = bottom - out.y; }
    }
    for (const [g, side] of touches) {
      const n = { ...g.r };
      if (side === 'w') { const right = n.x + n.w; n.x = out.x + out.w; n.w = right - n.x; }
      if (side === 'e') n.w = out.x - n.x;
      if (side === 'n') { const bottom = n.y + n.h; n.y = out.y + out.h; n.h = bottom - n.y; }
      if (side === 's') n.h = out.y - n.y;
      S.get(g.w).zone = toZone(n);
      Object.assign(g.w.el.style, geo(n));
    }
    return out;
  }

  /* ---------- system menu, with keyboard Move and Size ---------- */
  function systemMenu(win, x, y) {
    const st = S.get(win);
    if (!st || !Shell.menu?.show) return;
    const max = st.mode === 'max';
    Shell.menu.show({
      x, y, minWidth: 200,
      items: [
        { label: 'Restore', icon: 'fluent/restore', disabled: st.mode === 'normal', action: () => restore(win) },
        { label: 'Move', disabled: max, action: () => keyboardMode(win, 'move') },
        { label: 'Size', disabled: max || !st.resizable, action: () => keyboardMode(win, 'size') },
        { label: 'Minimize', icon: 'fluent/subtract', disabled: !st.minimizable, action: () => minimize(win) },
        { label: 'Maximize', icon: 'fluent/maximize', disabled: max || !st.maximizable, action: () => maximize(win) },
        { separator: true },
        { label: 'Close', icon: 'fluent/dismiss', shortcut: 'Alt+F4', action: () => close(win) },
      ],
    });
  }
  /* Move: arrows (Ctrl for 1 px) or the mouse move the window; Enter or a click keeps it, Escape
     puts it back. Size: the first arrow picks an edge, a perpendicular arrow adds a corner. */
  function keyboardMode(win, kind) {
    const st = S.get(win);
    if (!st || st.mode === 'max') return;
    if (st.mode === 'snap') { st.rect = visibleRect(win); st.mode = 'normal'; st.zone = null; render(win); }
    const start = { ...st.rect };
    let edges = '', px = null, py = null, finished = false;
    setCursor('var(--cursor-move)'); // Move and Size both start on the four-way arrow; Size switches once an arrow key picks an edge
    const apply = (dx, dy) => {
      if (kind === 'move') st.rect = { ...st.rect, x: st.rect.x + dx, y: Math.max(0, st.rect.y + dy) };
      else if (edges) st.rect = resized(win, st.rect, edges, edges.match(/[ew]/) ? dx : 0, edges.match(/[ns]/) ? dy : 0);
      Object.assign(win.el.style, geo(st.rect));
    };
    const key = (e) => {
      e.preventDefault();
      e.stopPropagation();
      if (e.key === 'Escape' || e.key === 'Enter') { done(e.key === 'Escape'); return; }
      const d = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[e.key];
      if (!d) return;
      if (kind === 'size') {
        const edge = { ArrowLeft: 'w', ArrowRight: 'e', ArrowUp: 'n', ArrowDown: 's' }[e.key];
        const vertical = edge === 'n' || edge === 's';
        if (!edges) { edges = edge; setCursor(CURSOR[edges]); return; }
        if (vertical && !/[ns]/.test(edges)) { edges = edge + edges; setCursor(CURSOR[edges]); return; }
        if (!vertical && !/[ew]/.test(edges)) { edges += edge; setCursor(CURSOR[edges]); return; }
      }
      const step = e.ctrlKey ? 1 : KEY_STEP;
      apply(d[0] * step, d[1] * step);
    };
    const mouse = (e) => { if (px !== null) apply(e.clientX - px, e.clientY - py); px = e.clientX; py = e.clientY; };
    const click = (e) => { e.preventDefault(); e.stopPropagation(); done(false); };
    function done(cancel) {
      finished = true;
      window.removeEventListener('keydown', key, true);
      window.removeEventListener('pointermove', mouse, true);
      window.removeEventListener('pointerdown', click, true);
      setCursor('');
      if (cancel) st.rect = start;
      const from = { ...st.rect };
      st.rect = fit(win, st.rect);
      render(win, from);
    }
    window.addEventListener('keydown', key, true);
    window.addEventListener('pointermove', mouse, true);
    setTimeout(() => { if (!finished) window.addEventListener('pointerdown', click, true); }); // not the menu's own click
  }

  /* ---------- Snap Layouts ---------- */
  function buildLayouts(set, wa) {
    const aspect = wa.width / Math.max(wa.height, 1);
    const tw = aspect >= 1 ? 96 : Math.max(44, Math.round(96 * aspect));
    const th = aspect >= 1 ? Math.round(clamp(96 / aspect, 40, 72)) : 96;
    const root = h('div', 'wm-layouts');
    root.style.setProperty('--cols', set.length > 4 || set.length === 3 ? 3 : 2);
    set.forEach((zones, li) => {
      const tile = h('div', 'wm-layout', { role: 'group', 'aria-label': `Layout ${li + 1}` });
      tile.dataset.layout = li;
      tile.style.width = `${tw}px`;
      tile.style.height = `${th}px`;
      const key = h('span', 'wm-key wm-layout-key');
      key.textContent = String(li + 1);
      tile.append(key);
      zones.forEach((z, zi) => {
        const g = 2; // half of the 4 px gap between zones
        const x0 = z.x * tw + (z.x > 0.001 ? g : 0), x1 = (z.x + z.w) * tw - (z.x + z.w < 0.999 ? g : 0);
        const y0 = z.y * th + (z.y > 0.001 ? g : 0), y1 = (z.y + z.h) * th - (z.y + z.h < 0.999 ? g : 0);
        const b = h('button', 'wm-zone', { type: 'button', 'aria-label': `Zone ${zi + 1} of layout ${li + 1}` });
        b.dataset.layout = li;
        b.dataset.zone = zi;
        Object.assign(b.style, { left: `${x0}px`, top: `${y0}px`, width: `${x1 - x0}px`, height: `${y1 - y0}px` });
        const n = h('span', 'wm-key');
        n.textContent = String(zi + 1);
        b.append(n);
        tile.append(b);
      });
      root.append(tile);
    });
    return root;
  }
  function openLayouts(win, keyboard = false) {
    const st = S.get(win);
    if (!st || !st.resizable || st.minimized) return;
    closeLayouts();
    const wa = workArea(), set = layoutsFor(wa);
    const el = buildLayouts(set, wa);
    el.classList.add('wm-layouts-flyout', 'acrylic', 'surface-flyout');
    el.classList.toggle('keyboard', keyboard);
    el.setAttribute('role', 'dialog');
    el.setAttribute('aria-label', 'Snap layouts');
    Shell.layer('flyouts').append(el);
    const anchor = (st.btnMax || st.titlebar || win.el).getBoundingClientRect();
    const W = el.offsetWidth, H = el.offsetHeight;
    let top = anchor.bottom + 4;
    if (top + H > wa.height - 8) top = Math.max(8, anchor.top - H - 4);
    el.style.left = `${Math.round(clamp(anchor.left + anchor.width / 2 - W / 2, 8, window.innerWidth - W - 8))}px`;
    el.style.top = `${Math.round(top)}px`;
    const f = layouts = { el, win, set, keyboard, chosen: -1, timer: 0 };
    el.addEventListener('click', (e) => {
      const b = e.target.closest('.wm-zone');
      if (b) pickZone(f, +b.dataset.layout, +b.dataset.zone);
    });
    el.addEventListener('pointerenter', () => clearTimeout(f.timer));
    el.addEventListener('pointerleave', () => scheduleCloseLayouts());
    el.addEventListener('keydown', (e) => layoutsKey(e, f));
    Shell.transient.open('wm-snaplayouts', { close: closeLayouts, elements: [el, st.btnMax], group: 'snaplayouts' });
    Shell.animate(el, [{ opacity: 0, transform: 'translateY(-8px)' }, { opacity: 1, transform: 'none' }], { duration: 167 });
    if (keyboard) el.querySelector('.wm-zone')?.focus();
  }
  function scheduleCloseLayouts() {
    const f = layouts;
    if (!f || f.keyboard) return;
    clearTimeout(f.timer);
    f.timer = setTimeout(closeLayouts, LAYOUTS_GRACE);
  }
  function closeLayouts() {
    const f = layouts;
    if (!f) return;
    layouts = null;
    clearTimeout(f.timer);
    Shell.transient.close('wm-snaplayouts');
    f.el.style.pointerEvents = 'none';
    Shell.animate(f.el, [{ opacity: 1 }, { opacity: 0 }], { duration: 83, easing: Shell.ease.out, fill: 'forwards' }).then(() => f.el.remove());
  }
  function pickZone(f, li, zi) {
    const win = f.win;
    closeLayouts();
    if (!S.has(win)) return;
    snapTo(win, f.set[li][zi]);
    focusWin(win);
    startAssist(win, f.set[li].filter((_, i) => i !== zi), f.keyboard);
  }
  function layoutsKey(e, f) {
    const zones = [...f.el.querySelectorAll('.wm-zone')];
    const i = zones.indexOf(document.activeElement);
    if (e.key === 'Escape') { const win = f.win; closeLayouts(); if (S.has(win)) focusWin(win); }
    else if (/^[1-9]$/.test(e.key)) {
      const n = +e.key - 1;
      if (f.chosen < 0 && f.set[n]) {
        f.chosen = n;
        f.el.classList.add('phase-zone');
        const tile = f.el.querySelector(`.wm-layout[data-layout="${n}"]`);
        tile.classList.add('chosen');
        tile.querySelector('.wm-zone').focus();
      } else if (f.chosen >= 0 && f.set[f.chosen][n]) pickZone(f, f.chosen, n);
    } else if (e.key.startsWith('Arrow') && zones.length) {
      const d = e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : -1;
      zones[(Math.max(i, 0) + d + zones.length) % zones.length].focus();
    } else return;
    e.preventDefault();
    e.stopPropagation();
  }

  /* ---------- Snap Assist ---------- */
  /* After a snap, the remaining zones of the layout are offered one at a time: the first shows
     the other windows of this desktop as thumbnails, the rest wait as empty acrylic. */
  function startAssist(anchor, zones, keyboard = false) {
    endAssist();
    const candidates = list().filter((w) => w !== anchor);
    if (!zones.length || !candidates.length) return;
    assist = { anchor, zones: [...zones], candidates, panels: [], keyboard };
    Shell.transient.open('wm-snapassist', {
      close: endAssist,
      group: 'snapassist',
      elements: [(e) => !!assist && assist.panels.some((p) => p.contains(e.target))],
    });
    renderAssist();
  }
  function renderAssist() {
    const a = assist;
    const fresh = !a.panels.length;
    a.panels.forEach((p) => p.remove());
    a.panels = a.zones.map((z, i) => {
      const r = shrink(zoneRect(z), ZONE_INSET);
      const p = h('div', `wm-assist-zone acrylic${i ? ' pending' : ''}`, { tabindex: '-1' });
      Object.assign(p.style, geo(r));
      if (i === 0) fillAssist(p, r);
      Shell.layer('flyouts').append(p);
      if (fresh || i === 0) Shell.animate(p, [{ opacity: 0, transform: 'scale(0.98)' }, { opacity: 1, transform: 'none' }], { duration: 167 });
      return p;
    });
    /* Keyboard snaps start on the first thumbnail; pointer snaps focus the panel so arrows and
       Escape work without drawing a focus rectangle. */
    const first = a.panels[0];
    (a.keyboard ? first?.querySelector('.wm-assist-item') : first)?.focus({ preventScroll: true });
  }
  function fillAssist(panel, r) {
    const items = assist.candidates, gap = 16, pad = 24, chrome = 16 + 28; // item padding, header + gap
    const W = r.w - 2 * pad, H = r.h - 2 * pad;
    let best = { size: 0, cols: 1 };
    for (let cols = 1; cols <= items.length; cols++) {
      const rows = Math.ceil(items.length / cols);
      const cw = (W - (cols - 1) * gap) / cols - 16;
      const ch = (H - (rows - 1) * gap) / rows - chrome;
      const size = Math.min(cw, ch / 0.625, 320);
      if (size > best.size) best = { size, cols };
    }
    const tw = Math.max(40, Math.floor(best.size)), th = Math.round(tw * 0.625);
    const grid = h('div', 'wm-assist-grid', { role: 'list', 'aria-label': 'Snap assist' });
    grid.style.width = `${Math.min(W, best.cols * (tw + 16) + (best.cols - 1) * gap)}px`;
    for (const w of items) {
      const item = h('div', 'wm-assist-item', { role: 'listitem button', tabindex: '0', 'aria-label': w.title });
      const head = h('div', 'wm-assist-head');
      const title = h('span', 'wm-assist-title');
      title.textContent = w.title;
      const x = h('button', 'wm-assist-close', { type: 'button', tabindex: '-1', 'aria-label': 'Close' });
      x.innerHTML = GLYPH.close;
      head.append(Shell.icon(w.icon, { size: 16 }), title, x);
      const thumb = h('div', 'wm-assist-thumb');
      Object.assign(thumb.style, { width: `${tw}px`, height: `${th}px` });
      const wr = visibleRect(w);
      thumb.append(snapshot(w, Math.min(tw / wr.w, th / wr.h)));
      item.append(head, thumb);
      item.addEventListener('click', (e) => { if (e.target.closest('.wm-assist-close')) close(w); else pickAssist(w, false); });
      item.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pickAssist(w, true); }
        else if (e.key === 'Delete') close(w);
      });
      grid.append(item);
    }
    panel.append(grid);
    panel.addEventListener('keydown', (e) => {
      const all = [...panel.querySelectorAll('.wm-assist-item')];
      const i = all.indexOf(document.activeElement);
      if (e.key === 'Escape') { const anchor = assist?.anchor; endAssist(); if (anchor && S.has(anchor)) focusWin(anchor); }
      else if (e.key.startsWith('Arrow') && all.length) all[(Math.max(i, 0) + (e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : -1) + all.length) % all.length].focus();
      else return;
      e.preventDefault();
      e.stopPropagation();
    });
  }
  function pickAssist(win, keyboard) {
    const a = assist;
    if (!a || !S.has(win)) return;
    a.keyboard = keyboard;
    const zone = a.zones.shift();
    a.candidates = a.candidates.filter((w) => w !== win);
    snapTo(win, zone);
    focusWin(win);
    if (a.zones.length && a.candidates.length) renderAssist(); else endAssist();
  }
  function endAssist() {
    const a = assist;
    if (!a) return;
    assist = null;
    Shell.transient.close('wm-snapassist');
    a.panels.forEach((p) => {
      p.style.pointerEvents = 'none';
      Shell.animate(p, [{ opacity: 1 }, { opacity: 0 }], { duration: 83, easing: Shell.ease.out, fill: 'forwards' }).then(() => p.remove());
    });
  }

  /* ---------- snapshots ---------- */
  /* A static, scaled copy of the window for previews. Form values and canvases are copied; Mica
     is pinned to the wallpaper patch the window sits on so the copy can be scaled. */
  function snapshot(win, scale = 0.2) {
    const st = S.get(win);
    const frame = win.el.querySelector(':scope > .window-frame');
    const r = st ? visibleRect(win) : { x: 0, y: 0, w: win.el.offsetWidth || 800, h: win.el.offsetHeight || 600 };
    const s = Number.isFinite(scale) && scale > 0 ? scale : 0.2;
    const wrap = h('div', 'window-snapshot', { 'aria-hidden': 'true' });
    wrap.inert = true;
    ['active', 'maximized', 'snapped', 'titlebar-tall', 'ghost'].forEach((c) => wrap.classList.toggle(c, win.el.classList.contains(c)));
    Object.assign(wrap.style, { width: `${Math.round(r.w * s)}px`, height: `${Math.round(r.h * s)}px` });
    const copy = frame.cloneNode(true);
    copy.querySelectorAll('[id]').forEach((n) => n.removeAttribute('id'));
    const from = frame.querySelectorAll('input, textarea, select, canvas');
    const to = copy.querySelectorAll('input, textarea, select, canvas');
    from.forEach((src, i) => {
      const dst = to[i];
      if (!dst) return;
      if (src.tagName === 'CANVAS') { try { dst.getContext('2d').drawImage(src, 0, 0); } catch { /* tainted or webgl */ } }
      else if (src.type === 'checkbox' || src.type === 'radio') dst.checked = src.checked;
      else dst.value = src.value;
    });
    Object.assign(copy.style, { width: `${r.w}px`, height: `${r.h}px`, transform: `scale(${s})` });
    if (copy.matches('.mica, .mica-alt')) {
      Object.assign(copy.style, {
        backgroundAttachment: 'scroll',
        backgroundSize: `${window.innerWidth}px ${window.innerHeight}px`,
        backgroundPosition: `${-r.x}px ${-r.y}px`,
      });
    }
    wrap.append(copy);
    return wrap;
  }

  /* ---------- open ---------- */
  function open(o = {}) {
    const kind = o.titlebar || 'standard';
    const win = { id: `win-${nextId++}`, appId: o.appId || 'app', title: o.title || '', icon: o.icon || 'object/app-generic', desktop: current, el: null, body: null };
    const st = {
      opts: o, mode: 'normal', zone: null, minimized: false,
      resizable: o.resizable !== false, maximizable: o.maximizable !== false, minimizable: o.minimizable !== false,
      minW: o.minWidth ?? 200, minH: o.minHeight ?? 120,
      tbh: kind === 'none' ? 0 : kind === 'tall' ? 48 : 32,
    };
    S.set(win, st);

    const el = h('section', 'window', { tabindex: '-1', 'aria-label': win.title, 'data-app': win.appId });
    el.classList.toggle('titlebar-tall', kind === 'tall');
    const bg = { mica: 'mica', 'mica-alt': 'mica-alt', solid: 'window-solid', none: 'window-clear' }[o.background || 'mica'] || 'mica';
    const frame = h('div', `window-frame ${bg}`);
    const body = h('div', 'window-body');
    if (o.content) body.append(o.content);
    if (kind !== 'none') frame.append(buildTitlebar(win, st, o));
    frame.append(body);
    el.append(frame);
    if (st.resizable) {
      for (const edge of ['n', 's', 'e', 'w', 'nw', 'ne', 'sw', 'se']) {
        const r = h('div', `wm-resize ${edge}`, { 'aria-hidden': 'true' });
        r.dataset.edge = edge;
        el.append(r);
      }
    }
    Object.assign(win, { el, body });
    st.frame = frame;

    el.addEventListener('pointerdown', () => focusWin(win, { pointer: true }), true);
    /* The title bar, caption buttons and plain client surface (a status bar) never take keyboard
       focus, as Win32's non-client area: the press keeps the caret where it was, or brings back the
       window's last focused control when it activates the window. Selectable text keeps its drag. */
    el.addEventListener('mousedown', (e) => {
      const t = e.target.closest?.(TABBABLE);
      if (t && t !== el && !t.classList.contains('window-caption-btn')) return;
      for (let n = e.target; n && n !== el; n = n.parentElement) {
        const u = getComputedStyle(n).userSelect;
        if (u === 'none') break;
        if (u !== 'auto') return;
      }
      e.preventDefault();
      focusWin(win);
    });
    el.addEventListener('focusin', (e) => {
      if (!S.has(win)) return;
      st.lastFocus = e.target;
      if (focusedWin !== win) focusWin(win, { pointer: true });
    });
    el.addEventListener('pointerdown', (e) => {
      const r = e.target.closest?.('.wm-resize');
      if (r && r.parentElement === el) beginResize(win, e, r.dataset.edge);
    });
    el.addEventListener('dblclick', (e) => {
      const r = e.target.closest?.('.wm-resize');
      if (r && (r.dataset.edge === 'n' || r.dataset.edge === 's')) stretchVertically(win);
    });
    el.addEventListener('keydown', (e) => cycleTab(el, e));
    /* A click in a ghost's client area (the frame's wash covers the inert content) asks what to do. */
    frame.addEventListener('click', (e) => { if (st.ghost && !st.titlebar?.contains(e.target)) hangDialog(win); });

    Object.assign(win, {
      setTitle(t) {
        st.realTitle = String(t ?? '');
        win.title = st.ghost ? `${st.realTitle} (Not Responding)` : st.realTitle;
        if (st.titleEl) st.titleEl.textContent = win.title;
        if (st.ghostTitle) st.ghostTitle.textContent = win.title;
        el.setAttribute('aria-label', win.title);
        emit('title', win);
      },
      setIcon(name) {
        win.icon = name;
        st.iconEl?.replaceChildren(Shell.icon(name, { size: 16 }));
        emit('icon', win);
      },
      focus: () => focusWin(win),
      close: (o) => close(win, o || {}),
      minimize: () => minimize(win),
      restore: () => restore(win),
      maximize: () => maximize(win),
      toggleMaximize: () => toggleMaximize(win),
      isMaximized: () => S.get(win)?.mode === 'max',
      isMinimized: () => !!S.get(win)?.minimized,
      isFocused: () => focusedWin === win,
      snapTo: (zone) => snapTo(win, zone),
      snapshot: (scale) => snapshot(win, scale),
      setNotResponding: (on, o) => setNotResponding(win, on, o || {}),
      isNotResponding: () => !!S.get(win)?.ghost,
    });

    /* Placement: centred, each further window on this desktop cascades down-right. */
    const wa = workArea();
    const n = list().length % 6;
    const size = fit(win, { x: 0, y: 0, w: o.width || 800, h: o.height || 600 });
    st.rect = fit(win, {
      ...size,
      x: o.x ?? clamp(Math.round((wa.width - size.w) / 2) + n * CASCADE, 0, wa.width - size.w),
      y: o.y ?? clamp(Math.round((wa.height - size.h) / 2) + n * CASCADE, 0, wa.height - size.h),
    });
    windows.push(win);
    Shell.layer('windows').append(el);
    render(win);
    restack();
    emit('open', win);
    focusWin(win);
    Shell.animate(el, [{ opacity: 0, transform: 'scale(0.9)' }, { opacity: 1, transform: 'none' }], { duration: 250 });
    return win;
  }
  function captionButton(kind, label) {
    const b = h('button', `window-caption-btn ${kind}`, { type: 'button', tabindex: '-1', 'aria-label': label });
    b.innerHTML = GLYPH[kind];
    return b;
  }
  function buildTitlebar(win, st, o) {
    const tb = h('div', 'window-titlebar');
    st.titlebar = tb;
    if (o.titlebarContent) {
      const c = h('div', 'window-titlebar-content');
      c.append(o.titlebarContent);
      tb.append(c);
    } else {
      st.iconEl = h('span', 'window-titlebar-icon');
      st.iconEl.append(Shell.icon(win.icon, { size: 16 }));
      st.titleEl = h('span', 'window-title');
      st.titleEl.textContent = win.title;
      tb.append(st.iconEl, st.titleEl);
      st.iconEl.addEventListener('click', () => { const r = st.iconEl.getBoundingClientRect(); systemMenu(win, r.left, r.bottom + 4); });
      st.iconEl.addEventListener('dblclick', () => close(win));
    }
    tb.append(h('div', 'window-titlebar-fill'));

    /* Without minimize and maximize Windows shows close alone; with one of them the other is
       shown disabled. */
    const cap = h('div', 'window-caption');
    if (st.minimizable || st.maximizable) {
      st.btnMin = captionButton('min', 'Minimize');
      st.btnMin.disabled = !st.minimizable;
      st.btnMin.dataset.tooltip = 'Minimize';
      st.btnMax = captionButton('max', 'Maximize');
      st.btnMax.disabled = !st.maximizable;
      cap.append(st.btnMin, st.btnMax);
      st.btnMin.addEventListener('click', () => minimize(win));
      st.btnMax.addEventListener('click', () => toggleMaximize(win));
      const btn = st.btnMax;
      btn.addEventListener('pointerenter', () => {
        clearTimeout(st.layoutsTimer);
        if (layouts?.win === win) { clearTimeout(layouts.timer); return; }
        if (!btn.disabled && st.resizable) st.layoutsTimer = setTimeout(() => openLayouts(win), LAYOUTS_DELAY);
      });
      btn.addEventListener('pointerleave', () => { clearTimeout(st.layoutsTimer); if (layouts?.win === win) scheduleCloseLayouts(); });
      btn.addEventListener('pointerdown', () => { clearTimeout(st.layoutsTimer); closeLayouts(); });
    }
    const btnClose = captionButton('close', 'Close');
    btnClose.dataset.tooltip = 'Close';
    btnClose.addEventListener('click', () => close(win));
    cap.append(btnClose);
    tb.append(cap);

    tb.addEventListener('pointerdown', (e) => { if (e.button === 0 && !e.target.closest(NO_DRAG)) beginMove(win, e); });
    tb.addEventListener('dblclick', (e) => { if (!e.target.closest(NO_DRAG) && st.maximizable) toggleMaximize(win); });
    tb.addEventListener('contextmenu', (e) => {
      if (e.target.closest(NO_DRAG) && !e.target.closest('.window-titlebar-icon')) return;
      e.preventDefault();
      systemMenu(win, e.clientX, e.clientY);
    });
    return tb;
  }

  /* ---------- desktops ---------- */
  const desktopList = () => desktops.map((d, index) => ({ index, name: d.name }));
  const emitDesktops = () => Shell.emit('desktops', { current, desktops: desktopList() });
  function setDesktop(i) {
    if (i === current || !desktops[i]) return;
    const dir = i > current ? 1 : -1;
    current = i;
    endAssist();
    closeLayouts();
    for (const w of windows) {
      const off = w.desktop !== current;
      w.el.classList.toggle('off-desktop', off);
      if (!off && !S.get(w).minimized) {
        Shell.animate(w.el, [{ transform: `translateX(${dir * 80}px)`, opacity: 0 }, { transform: 'none', opacity: 1 }], { duration: 250 });
      }
    }
    const top = topVisible(null);
    if (top) focusWin(top); else blurAll();
    emitDesktops();
  }
  function addDesktop() {
    desktops.push({ name: `Desktop ${desktops.length + 1}` });
    emitDesktops();
    return desktops.length - 1;
  }
  /* Its windows move to the desktop on its left (the next one when it is the first). */
  function removeDesktop(i) {
    if (desktops.length < 2 || !desktops[i]) return false;
    const to = i > 0 ? i - 1 : 0;
    for (const w of windows) {
      if (w.desktop === i) { w.desktop = to; emit('desktop', w); } else if (w.desktop > i) w.desktop -= 1;
    }
    desktops.splice(i, 1);
    desktops.forEach((d, k) => { if (/^Desktop \d+$/.test(d.name)) d.name = `Desktop ${k + 1}`; });
    const next = current === i ? to : current > i ? current - 1 : current;
    current = -1; // force setDesktop to re-show
    setDesktop(next);
    return true;
  }
  function renameDesktop(i, name) {
    if (!desktops[i]) return;
    desktops[i].name = String(name || '').trim() || `Desktop ${i + 1}`;
    emitDesktops();
  }
  function moveToDesktop(win, i) {
    if (!S.has(win) || !desktops[i] || win.desktop === i) return;
    win.desktop = i;
    win.el.classList.toggle('off-desktop', i !== current);
    if (focusedWin === win && i !== current) focusNext(win);
    emit('desktop', win);
  }

  /* ---------- show desktop, minimize all ---------- */
  function showDesktop() {
    const open = list().filter((w) => !S.get(w).minimized);
    if (open.length) {
      open.forEach((w) => minimize(w, { refocus: false }));
      shownDesktop = open;
      return;
    }
    const back = (shownDesktop || []).filter((w) => S.has(w) && onDesk(w));
    shownDesktop = null;
    back.forEach((w) => unminimize(w));
    if (back.length) focusWin(back[back.length - 1]);
  }
  const minimizeAll = (keep) => list().forEach((w) => { if (w !== keep) minimize(w, { refocus: false }); });
  function restoreAll() {
    const back = list().filter((w) => S.get(w).minimized);
    back.forEach((w) => unminimize(w));
    if (back.length) focusWin(back[back.length - 1]);
  }

  /* ---------- keyboard ---------- */
  /* Win+Arrow walks one window through the snap positions as Windows 11 does on one monitor. */
  const KEY_SNAP = {
    ArrowLeft: { normal: 'left', max: 'left', left: 'right', right: 'normal', 'top-left': 'top-right', 'top-right': 'top-left', 'bottom-left': 'bottom-right', 'bottom-right': 'bottom-left', top: 'left', bottom: 'left', custom: 'left' },
    ArrowRight: { normal: 'right', max: 'right', right: 'left', left: 'normal', 'top-right': 'top-left', 'top-left': 'top-right', 'bottom-right': 'bottom-left', 'bottom-left': 'bottom-right', top: 'right', bottom: 'right', custom: 'right' },
    ArrowUp: { normal: 'max', left: 'top-left', right: 'top-right', 'bottom-left': 'left', 'bottom-right': 'right', 'top-left': 'max', 'top-right': 'max', top: 'max', bottom: 'top', custom: 'max' },
    ArrowDown: { max: 'normal', normal: 'min', left: 'bottom-left', right: 'bottom-right', 'top-left': 'left', 'top-right': 'right', 'bottom-left': 'min', 'bottom-right': 'min', top: 'bottom', bottom: 'min', custom: 'normal' },
  };
  function keySnap(key) {
    const win = focusedWin;
    if (!win) return false;
    const st = S.get(win);
    const from = st.mode === 'snap' ? nameOf(st.zone) || 'custom' : st.mode;
    const to = KEY_SNAP[key][from];
    if (!to) return true;
    if (to === 'min') { if (st.minimizable) minimize(win); }
    else if (to === 'max') maximize(win);
    else if (to === 'normal') restore(win);
    else {
      snapTo(win, to);
      if (S.get(win).mode === 'snap' && (key === 'ArrowLeft' || key === 'ArrowRight') && (to === 'left' || to === 'right')) startAssist(win, restOf(NAMED[to]), true);
    }
    return true;
  }
  function keyMenu() {
    const win = focusedWin;
    if (!win) return false;
    const r = (S.get(win).titlebar || win.el).getBoundingClientRect();
    systemMenu(win, r.left + 8, r.top + Math.min(r.height, 32));
    return true;
  }
  const K = Shell.keys;
  ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].forEach((k) => K.on(`Win+${k}`, () => keySnap(k)));
  K.on('Shift+Win+ArrowUp', () => { if (focusedWin) stretchVertically(focusedWin); });
  K.on('Ctrl+Alt+Shift+ArrowUp', () => { if (focusedWin) stretchVertically(focusedWin); });
  K.on('Win+D', () => showDesktop());
  K.on('Win+M', () => minimizeAll(null));
  K.on('Shift+Win+M', () => restoreAll());
  K.on('Ctrl+Alt+Shift+M', () => restoreAll());
  /* Win+Home minimizes all but the active window; pressed again with that window still active,
     it brings back the windows it minimized. */
  let homeKept = null;
  K.on('Win+Home', () => {
    const win = focusedWin;
    if (!win) return;
    if (homeKept?.win === win) {
      homeKept.others.forEach((w) => { if (w.desktop === current) unminimize(w); });
      homeKept = null;
      focusWin(win);
      return;
    }
    const others = list().filter((w) => w !== win && !S.get(w).minimized);
    minimizeAll(win);
    homeKept = others.length ? { win, others } : null;
  });
  K.on('Win+Z', () => { if (!focusedWin) return false; openLayouts(focusedWin, true); return true; });
  K.on('Alt+F4', () => { if (focusedWin) close(focusedWin); else Shell.emit('alt-f4-desktop'); });
  K.on('Alt+ ', () => keyMenu());
  K.on('Ctrl+Win+ArrowLeft', () => setDesktop(Math.max(0, current - 1)));
  K.on('Ctrl+Win+ArrowRight', () => setDesktop(Math.min(desktops.length - 1, current + 1)));
  K.on('Ctrl+Win+D', () => setDesktop(addDesktop()));
  K.on('Ctrl+Win+F4', () => removeDesktop(current));

  /* ---------- responsiveness ---------- */
  /* Like a display change on Windows: normal windows come back fully on screen, zones rescale. */
  window.addEventListener('resize', () => {
    closeLayouts();
    const wa = workArea();
    for (const w of windows) {
      const st = S.get(w);
      if (st.mode === 'normal') {
        const r = fit(w, st.rect);
        st.rect = { ...r, x: clamp(r.x, 0, wa.width - r.w), y: clamp(r.y, 0, wa.height - r.h) };
      }
      render(w);
    }
    if (assist) renderAssist();
  });

  /* ---------- boot ---------- */
  function debugSnapLayouts() {
    let win = null;
    try { win = Shell.apps?.get?.('explorer') ? Shell.apps.launch('explorer') : null; } catch (e) { console.warn('[wm] explorer did not open', e); }
    if (!win || !S.has(win)) {
      const content = h('div', 'wm-demo');
      content.textContent = 'Point to Maximize to see snap layouts.';
      win = open({ appId: 'wm-demo', title: 'Snap layouts', icon: 'object/app-generic', width: 720, height: 460, content });
    }
    setTimeout(() => openLayouts(win), 300);
  }
  /* ?open=cursors: every pointer of css/cursors.css in one window; pointing at a tile shows it. */
  function debugCursors() {
    const grid = h('div', 'cursor-gallery');
    for (const [name, file, label, cls] of [['arrow', 'arrow', 'Normal select'], ['help', 'help', 'Help select'],
      ['arrow', 'working-00', 'Working in background', 'app-starting'], ['busy', 'busy-00', 'Busy', 'cursor-busy-spin'],
      ['precision', 'precision', 'Precision select'], ['text', 'text', 'Text select'], ['unavailable', 'unavailable', 'Unavailable'],
      ['ns', 'resize-ns', 'Vertical resize'], ['ew', 'resize-ew', 'Horizontal resize'], ['nwse', 'resize-nwse', 'Diagonal resize 1'],
      ['nesw', 'resize-nesw', 'Diagonal resize 2'], ['move', 'move', 'Move'], ['link', 'link', 'Link select'], ['split', 'split', 'Column divider'],
      ['drag-move', 'drag-move', 'Drag: move'], ['drag-copy', 'drag-copy', 'Drag: copy'], ['drag-link', 'drag-link', 'Drag: link']]) {
      const tile = h('div', `cursor-tile ${cls || ''}`.trim(), { 'data-cursor': name });
      tile.style.cursor = `var(--cursor-${name})`;
      const img = h('img', '', { src: `assets/cursors/${file}.svg`, alt: '' });
      tile.append(img, document.createTextNode(label));
      grid.append(tile);
    }
    open({ appId: 'cursors-demo', title: 'Mouse pointers', icon: 'object/app-generic', width: 640, height: 520, content: grid });
  }
  Shell.on('boot', () => {
    ['desktop', 'wallpaper'].forEach((n) => Shell.layer(n)?.addEventListener('pointerdown', blurAll));
    if (Shell.debugOpen.includes('snaplayouts')) setTimeout(debugSnapLayouts, 200);
    if (Shell.debugOpen.includes('cursors')) setTimeout(debugCursors, 200);
  });

  Shell.wm = {
    open,
    list,
    all: () => [...windows],
    focused: () => focusedWin,
    byApp: (appId) => list().filter((w) => w.appId === appId),
    showDesktop,
    currentDesktop: () => current,
    setDesktop,
    desktops: desktopList,
    addDesktop,
    removeDesktop,
    renameDesktop,
    moveToDesktop,
    workArea,
  };
})();
