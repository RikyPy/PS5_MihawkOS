/* Task View (Win+Tab), the virtual desktops strip and the Alt+Tab switcher.
   Owns: Shell.taskView, the Alt+Tab / Alt+` switcher and their DOM in #overlays.
   Reads windows only through Shell.wm (list, all, focused, desktops, snapshot) and their
   on-screen rects. While Task View is open the windows layer is faded out, so thumbnails can
   fly from the windows' real positions into the grid without a double image, as Windows does.
   Contract: CONTRACT.md "taskview". */
(() => {
  'use strict';

  const Shell = window.Shell;
  const DUR = { normal: 250, fast: 167, faster: 83 };

  /* ---------- helpers ---------- */
  function h(tag, className, props = {}) {
    const n = document.createElement(tag);
    if (className) n.className = className;
    for (const [k, v] of Object.entries(props)) {
      if (k === 'text') n.textContent = v;
      else n.setAttribute(k, v);
    }
    return n;
  }
  const deskIndex = (d) => (d && typeof d === 'object' ? d.index : d) ?? 0;
  const currentDesk = () => deskIndex(Shell.wm?.currentDesktop?.());
  const desktops = () => {
    const list = Shell.wm?.desktops?.();
    return list && list.length ? list : [{ index: 0, name: 'Desktop 1' }];
  };
  const mru = (wins) => [...(wins || [])].reverse(); // wm lists bottom-to-top; Windows shows most recent first
  const windowsOn = (i) => (i === currentDesk()
    ? mru(Shell.wm?.list?.())
    : mru((Shell.wm?.all?.() || []).filter((w) => deskIndex(w.desktop) === i)));
  const clampAspect = (a) => Math.min(2.4, Math.max(0.55, a || 1.6));

  /* Where the window is on screen now, or null (minimized, on another desktop, not laid out). */
  function realRect(win) {
    if (win.isMinimized?.()) return null;
    const r = win.el?.getBoundingClientRect?.();
    return r && r.width > 1 && r.height > 1 ? r : null;
  }
  function winSize(win) {
    const w = win.el?.offsetWidth, ht = win.el?.offsetHeight;
    if (w > 1 && ht > 1) return { w, h: ht };
    // Hidden (minimized or on another desktop): its last geometry is usually in its inline style.
    const iw = parseFloat(win.el?.style.width), ih = parseFloat(win.el?.style.height);
    if (iw > 1 && ih > 1) return { w: iw, h: ih };
    const wa = Shell.wm?.workArea?.() || { width: innerWidth, height: innerHeight - 48 };
    return { w: wa.width * 0.6, h: wa.height * 0.6 };
  }

  /* Desktop names. shortcut: the contract gives wm no renameDesktop, so renamed names live here
     by index and shift if a desktop to their left closes; upgrade when wm offers renameDesktop. */
  const names = Shell.store.get('desktop-names', {});
  const deskName = (d) => names[d.index] || d.name || `Desktop ${d.index + 1}`;
  function setDeskName(d, value) {
    const v = value.trim();
    if (Shell.wm?.renameDesktop) { Shell.wm.renameDesktop(d.index, v || `Desktop ${d.index + 1}`); return; }
    if (v && v !== d.name) names[d.index] = v; else delete names[d.index];
    Shell.store.set('desktop-names', names);
  }

  /* FLIP: `node` is laid out at `to`; animate it from `from` (both viewport rects). */
  const flipTransform = (from, to) =>
    `translate(${from.left - to.left}px, ${from.top - to.top}px) scale(${from.width / to.width}, ${from.height / to.height})`;
  const fadeIn = (node, delay = 0) => Shell.animate(node,
    [{ opacity: 0, transform: 'scale(0.92)' }, { opacity: 1, transform: 'none' }],
    { duration: DUR.normal, delay, fill: 'backwards' });
  function fadeOutAndRemove(node) {
    node.style.pointerEvents = 'none';
    node.classList.add('tv-gone');
    Shell.animate(node, [{ opacity: 1 }, { opacity: 0, scale: '0.92' }],
      { duration: DUR.fast, easing: Shell.ease.out, fill: 'forwards' }).then(() => node.remove());
  }

  /* Left/Right walk the group in order; Up/Down go to the nearest item in that direction. */
  function spatial(nodes, current, key) {
    if (!current || !nodes.includes(current)) return nodes[0] || null;
    const dir = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[key];
    const mid = (n) => { const r = n.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };
    const [cx, cy] = mid(current);
    let best = null, bestScore = Infinity;
    for (const n of nodes) {
      if (n === current) continue;
      const [x, y] = mid(n);
      const along = (x - cx) * dir[0] + (y - cy) * dir[1];
      if (along <= 1) continue;
      const score = along + 2 * Math.abs((x - cx) * dir[1] + (y - cy) * dir[0]);
      if (score < bestScore) { best = n; bestScore = score; }
    }
    return best;
  }
  function stepInGroup(nodes, current, delta, wrap) {
    const i = nodes.indexOf(current);
    if (i < 0) return nodes[0] || null;
    const j = i + delta;
    if (wrap) return nodes[(j + nodes.length) % nodes.length];
    return nodes[j] || null;
  }

  /* Title row shared by Task View cards and Alt+Tab items: icon, title, close on hover. */
  function titleRow(win, className, onClose) {
    const row = h('div', className);
    row.append(Shell.icon(win.icon || 'object/app-generic', { size: 16 }), h('span', 'tv-title', { text: win.title || '' }));
    const x = h('button', 'tv-x', { type: 'button', tabindex: '-1', 'aria-label': 'Close', 'data-tooltip': 'Close' });
    x.appendChild(Shell.icon('fluent/dismiss', { size: 16 }));
    x.addEventListener('pointerdown', (e) => e.stopPropagation());
    x.addEventListener('click', (e) => { e.stopPropagation(); onClose(); });
    row.appendChild(x);
    return row;
  }

  /* A static copy of the window covering w x ht from its top edge (wm's snapshot; a window
     taller or wider than the clamped aspect is cropped, never letterboxed), or its icon on Mica. */
  function makeThumb(win, w, ht, className) {
    const box = h('div', className);
    box.style.width = `${w}px`;
    box.style.height = `${ht}px`;
    const sz = winSize(win);
    let snap = null;
    try { snap = win.snapshot?.(Math.max(w / sz.w, ht / sz.h)); } catch (e) { console.warn('[taskview] snapshot failed', e); }
    if (snap instanceof HTMLElement) box.appendChild(snap);
    else {
      box.classList.add('mica', 'tv-placeholder');
      box.appendChild(Shell.icon(win.icon || 'object/app-generic', { size: Math.round(Math.max(24, Math.min(64, ht / 3))) }));
    }
    return box;
  }

  /* Balanced rows: every thumbnail in a layout shares one height; the row count that gives the
     largest thumbnails wins (ties keep fewer rows); rows are centred, the block is centred. */
  function layoutRows(aspects, area, { pad, head, gap, maxH }) {
    const n = aspects.length;
    if (!n) return { thumbH: 0, rects: [] };
    let best = null;
    for (let rows = 1; rows <= n; rows++) {
      const base = Math.floor(n / rows), extra = n % rows;
      const split = [];
      for (let r = 0, k = 0; r < rows; r++) { const c = base + (r < extra ? 1 : 0); split.push([k, k + c]); k += c; }
      let th = Math.min(maxH, (area.h - gap * (rows - 1)) / rows - head - pad);
      for (const [a, b] of split) {
        const sum = aspects.slice(a, b).reduce((s, x) => s + x, 0);
        th = Math.min(th, (area.w - gap * (b - a - 1) - 2 * pad * (b - a)) / sum);
      }
      if (!best || th > best.th + 0.5) best = { th, split };
    }
    const thumbH = Math.max(24, Math.floor(best.th));
    const cardH = head + thumbH + pad;
    const blockH = best.split.length * cardH + gap * (best.split.length - 1);
    let y = area.y + Math.max(0, (area.h - blockH) / 2);
    const rects = [];
    for (const [a, b] of best.split) {
      const widths = aspects.slice(a, b).map((x) => Math.round(thumbH * x));
      const rowW = widths.reduce((s, w) => s + w + 2 * pad, 0) + gap * (b - a - 1);
      let x = area.x + (area.w - rowW) / 2;
      for (const w of widths) {
        rects.push({ x: Math.round(x), y: Math.round(y), w: w + 2 * pad, h: cardH, thumbW: w });
        x += w + 2 * pad + gap;
      }
      y += cardH + gap;
    }
    return { thumbH, rects };
  }

  /* ---------- Task View ---------- */
  let tv = null;
  let dismissedAt = 0;

  const isTaskViewButton = (e) => !!e.target.closest?.('[data-taskview], .taskview-button, [aria-label="Task View"]');
  const inMenus = (e) => !!Shell.layer('menus')?.contains(e.target);

  function openTaskView({ keyboard = false } = {}) {
    if (tv) return;
    switcherEnd(false);
    const root = h('div', 'tv-root', { role: 'dialog', 'aria-label': 'Task View', tabindex: '-1' });
    const backdrop = h('div', 'tv-backdrop');
    const grid = h('div', 'tv-grid', { role: 'group', 'aria-label': 'Open windows' });
    const strip = h('div', 'tv-strip', { role: 'group', 'aria-label': 'Desktops' });
    root.append(backdrop, grid, strip);
    Shell.layer('overlays').appendChild(root);
    tv = { root, backdrop, grid, strip, cards: new Map(), shown: currentDesk(), timers: [], offs: [], raf: 0 };
    Shell.transient.open('taskview', { close: teardown, elements: [root, isTaskViewButton, inMenus], group: 'flyout' });

    setScreenAspect();
    renderStrip();
    renderCards('windows');
    const layer = Shell.layer('windows');
    if (layer) layer.style.opacity = '0';

    Shell.animate(backdrop, [{ opacity: 0 }, { opacity: 1 }], { duration: DUR.normal });
    Shell.animate(strip, [{ transform: 'translateY(100%)', opacity: 0 }, { transform: 'none', opacity: 1 }],
      { duration: DUR.normal, delay: 50, fill: 'backwards' });

    root.addEventListener('click', (e) => {
      if (tv && !tv.suppressClick && (e.target === root || e.target === backdrop || e.target === grid)) closeTaskView();
    });
    root.addEventListener('contextmenu', (e) => e.preventDefault());
    const onKey = (e) => taskViewKey(e);
    const onResize = () => { if (!tv) return; setScreenAspect(); renderStrip(); renderCards('none'); };
    /* When a window closes or leaves the desktop, wm and the app (a tick later) move the keyboard into
       the next window, which is hidden behind Task View: Task View takes it back to where it was. */
    const onFocusIn = (e) => {
      if (!tv) return;
      if (root.contains(e.target)) tv.last = e.target;
      else if (Shell.layer('windows')?.contains(e.target)) (tv.last?.isConnected ? tv.last : root).focus({ preventScroll: true });
    };
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('resize', onResize);
    document.addEventListener('focusin', onFocusIn, true);
    tv.offs.push(() => window.removeEventListener('keydown', onKey, true), () => window.removeEventListener('resize', onResize),
      () => document.removeEventListener('focusin', onFocusIn, true));

    root.focus({ preventScroll: true });
    // Keyboard selection lands on the most recent window once its thumbnail has arrived.
    const first = grid.querySelector('.tv-card');
    if (keyboard && first) {
      const s = tv;
      setTimeout(() => { if (tv === s && document.activeElement === root && first.isConnected) first.focus({ preventScroll: true }); },
        Shell.reducedMotion() ? 0 : DUR.normal);
    }
  }

  function closeTaskView() {
    if (!tv) return;
    tv.byMe = true;
    if (Shell.transient.isOpen('taskview')) Shell.transient.close('taskview');
    else teardown();
  }

  /* Pick a window: Task View closes and that window comes to the front (then `after` runs,
     e.g. a snap, before the thumbnails fly back so they land where the window ends up). */
  function choose(win, after) {
    if (!tv) return;
    tv.chosen = win;
    tv.after = after;
    closeTaskView();
  }

  function teardown() {
    const s = tv;
    if (!s) return;
    tv = null;
    if (!s.byMe) dismissedAt = performance.now();
    s.offs.forEach((off) => off());
    s.timers.forEach(clearTimeout);
    cancelAnimationFrame(s.raf);
    const active = document.activeElement;
    const hadKeyboard = !active || active === document.body || s.root.contains(active);
    s.root.classList.add('tv-closing');
    if (s.root.contains(active)) active.blur();

    if (s.chosen) {
      if (s.chosen.isMinimized?.()) s.chosen.restore?.();
      s.chosen.focus?.();
    } else if (hadKeyboard) Shell.wm?.focused?.()?.focus?.(); // Esc, empty space, Win+Tab: back to the active window
    try { s.after?.(); } catch (e) { console.warn('[taskview]', e); }

    const order = Shell.wm?.list?.() || [];
    const anims = [];
    const live = [...s.cards.values()].filter(({ card }) => !card.classList.contains('tv-gone'));
    const targets = live.map(({ win }) => realRect(win)); // every read before the first write: one layout
    live.forEach(({ card, win, thumbRect }, i) => {
      const thumb = card.querySelector('.tv-thumb');
      const to = targets[i];
      card.style.zIndex = String(order.indexOf(win) + 2);
      Shell.animate(card.querySelector('.tv-head'), [{ opacity: 1 }, { opacity: 0 }], { duration: DUR.faster, easing: 'linear', fill: 'forwards' });
      if (to && thumb) {
        anims.push(Shell.animate(thumb, [{ transform: 'none' }, { transform: flipTransform(to, thumbRect) }],
          { duration: DUR.normal, easing: Shell.ease.in, fill: 'forwards' }));
      } else {
        anims.push(Shell.animate(card, [{ opacity: 1 }, { opacity: 0, transform: 'scale(0.92)' }],
          { duration: DUR.fast, easing: Shell.ease.out, fill: 'forwards' }));
      }
    });
    anims.push(Shell.animate(s.backdrop, [{ opacity: 1 }, { opacity: 0 }], { duration: DUR.normal, easing: Shell.ease.in, fill: 'forwards' }));
    anims.push(Shell.animate(s.strip, [{ transform: 'none', opacity: 1 }, { transform: 'translateY(100%)', opacity: 0 }],
      { duration: DUR.fast, easing: Shell.ease.out, fill: 'forwards' }));
    Promise.all(anims).then(() => {
      s.root.remove();
      const layer = Shell.layer('windows');
      if (layer && !tv) layer.style.opacity = '';
    });
  }

  function setScreenAspect() {
    tv.root.style.setProperty('--tv-screen', String(Math.max(1.2, innerWidth / innerHeight)));
  }

  function gridArea() {
    const W = tv.root.clientWidth, H = innerHeight;
    const mx = Math.min(64, Math.max(12, W * 0.04));
    const top = Math.min(56, Math.max(12, H * 0.05));
    const bottom = Math.min(32, Math.max(8, H * 0.03));
    return { x: mx, y: top, w: W - 2 * mx, h: tv.strip.offsetTop - top - bottom };
  }

  /* Lay the shown desktop's windows out. mode: 'windows' flies each thumbnail from its window,
     'reflow' moves surviving cards from where they were, 'swap' fades the new set in, 'none'
     places without motion. Cards that left the set fade out where they are. */
  function renderCards(mode) {
    const s = tv;
    const wins = windowsOn(s.shown);
    const small = innerWidth < 600;
    const pad = 8, head = small ? 32 : 36, gap = innerWidth < 800 ? 12 : 24;
    const area = gridArea();
    // Every rect is read before the first write, so building the grid costs one layout, not one per card.
    const items = wins.map((win) => {
      const sz = winSize(win), prev = s.cards.get(win.id);
      const from = mode === 'windows' ? realRect(win)
        : mode === 'reflow' && prev && !prev.card.classList.contains('tv-gone') ? prev.card.getBoundingClientRect() : null;
      return { win, sz, from, aspect: clampAspect(sz.w / sz.h) };
    });
    const activeCard = document.activeElement?.closest?.('.tv-card');
    const keep = activeCard && s.grid.contains(activeCard) ? activeCard.tvWin : null; // the card, or its close button
    const maxH = Math.min(area.h * 0.55, Math.max(0, ...items.map((i) => i.sz.h)));
    const { thumbH, rects } = layoutRows(items.map((i) => i.aspect), area, { pad, head, gap, maxH });

    const old = s.cards;
    s.cards = new Map();
    items.forEach(({ win, from }, i) => {
      const r = rects[i];
      const card = buildCard(win, r, pad, head, thumbH);
      const thumbRect = { left: r.x + pad, top: r.y + head, width: r.thumbW, height: thumbH };
      s.grid.appendChild(card);
      s.cards.set(win.id, { card, win, thumbRect });
      if (mode === 'windows') {
        if (from) {
          Shell.animate(card.querySelector('.tv-thumb'), [{ transform: flipTransform(from, thumbRect) }, { transform: 'none' }],
            { duration: DUR.normal });
          Shell.animate(card.querySelector('.tv-head'), [{ opacity: 0 }, { opacity: 1 }],
            { duration: DUR.fast, delay: 120, easing: 'linear', fill: 'backwards' });
        } else fadeIn(card, 60);
      } else if (mode === 'reflow' && from) {
        Shell.animate(card, [{ transform: flipTransform(from, { left: r.x, top: r.y, width: r.w, height: r.h }) },
          { transform: 'none' }], { duration: DUR.normal });
      } else if (mode !== 'none') fadeIn(card);
    });
    old.forEach(({ card, win }) => {
      if (mode !== 'swap' && mode !== 'windows' && s.cards.has(win.id) && !card.classList.contains('tv-dragging')) card.remove();
      else if (!card.classList.contains('tv-gone')) fadeOutAndRemove(card);
    });
    // The keyboard stays on the same window's new card, or (after Delete) on the card that took its place.
    const cards = [...s.cards.values()].map((e) => e.card);
    const sel = (keep && s.cards.get(keep.id)?.card) || (s.select != null ? cards[Math.min(s.select, cards.length - 1)] : null);
    s.select = null;
    if (sel) sel.focus({ preventScroll: true });
    else if (keep) s.root.focus({ preventScroll: true });
  }

  /* Windows keeps the keyboard in Task View, so Esc and the arrows always reach it. Rebuilt or
     removed nodes drop focus to <body>; this takes it back. (onFocusIn handles focus moved into a window.) */
  function keepKeyboard() {
    if (!tv) return;
    const a = document.activeElement;
    if (a && a !== document.body && (tv.root.contains(a) || Shell.layer('menus')?.contains(a))) return;
    tv.root.focus({ preventScroll: true });
  }

  /* Close a window from its card; a keyboard selection moves on to the next thumbnail. */
  function closeCard(win) {
    const card = tv?.cards.get(win.id)?.card;
    if (card && document.activeElement === card && card.matches(':focus-visible')) {
      tv.select = [...tv.grid.querySelectorAll('.tv-card:not(.tv-gone)')].indexOf(card);
    }
    win.close?.();
  }

  function buildCard(win, r, pad, head, thumbH) {
    const card = h('div', 'tv-card', { role: 'button', tabindex: '0', 'aria-label': win.title || 'Window' });
    card.style.cssText = `left:${r.x}px;top:${r.y}px;width:${r.w}px;--tv-pad:${pad}px;--tv-head:${head}px`;
    card.append(titleRow(win, 'tv-head', () => closeCard(win)), makeThumb(win, r.thumbW, thumbH, 'tv-thumb'));
    card.addEventListener('click', () => { if (tv && !tv.suppressClick) choose(win); });
    card.addEventListener('auxclick', (e) => { if (e.button === 1) closeCard(win); });
    card.addEventListener('contextmenu', (e) => cardMenu(e, win));
    card.addEventListener('pointerdown', (e) => dragCard(e, card, win));
    card.tvWin = win;
    return card;
  }

  function scheduleReflow() {
    if (!tv || tv.raf) return;
    tv.raf = requestAnimationFrame(() => { if (!tv) return; tv.raf = 0; renderCards('reflow'); });
  }

  function cardMenu(e, win) {
    e.preventDefault();
    e.stopPropagation();
    if (!Shell.menu?.show || !tv) return;
    const others = desktops().filter((d) => d.index !== deskIndex(win.desktop));
    Shell.menu.show({
      x: e.clientX, y: e.clientY,
      items: [
        { label: 'Snap left', action: () => choose(win, () => win.snapTo?.('left')) },
        { label: 'Snap right', action: () => choose(win, () => win.snapTo?.('right')) },
        { separator: true },
        {
          label: 'Move to', disabled: !Shell.wm?.moveToDesktop,
          submenu: [...others.map((d) => ({ label: deskName(d), action: () => moveTo(win, d.index) })),
            { label: 'New desktop', action: () => moveTo(win, 'new') }],
        },
        { separator: true },
        { label: 'Close', action: () => closeCard(win) },
      ],
    });
  }

  function moveTo(win, index) {
    if (!tv || !Shell.wm?.moveToDesktop) return false;
    let target = index;
    if (target === 'new') {
      if (!Shell.wm.addDesktop) return false;
      Shell.wm.addDesktop();
      target = desktops().length - 1;
      renderStrip({ added: target });
    }
    if (target === deskIndex(win.desktop)) return false;
    Shell.wm.moveToDesktop(win, target);
    scheduleReflow();
    return true;
  }

  /* Drag a card: it shrinks under the pointer; over a desktop it shrinks to tile size and the
     tile lights up; dropping there moves the window, anywhere else it springs back. */
  function dragCard(e, card, win) {
    if (e.button !== 0 || !tv) return;
    const s = tv;
    const x0 = e.clientX, y0 = e.clientY;
    let dragging = false, over = null;
    const move = (ev) => {
      const dx = ev.clientX - x0, dy = ev.clientY - y0;
      if (!dragging) {
        if (Math.hypot(dx, dy) < 4) return; // the system drag threshold
        dragging = true;
        s.drag = true;
        const r = card.getBoundingClientRect();
        card.style.transformOrigin = `${x0 - r.left}px ${y0 - r.top}px`;
        card.classList.add('tv-dragging');
        Shell.tooltip?.hide?.();
      }
      card.style.translate = `${dx}px ${dy}px`;
      const tile = document.elementFromPoint(ev.clientX, ev.clientY)?.closest?.('.tv-desk');
      const target = tile && tile.dataset.index !== String(deskIndex(win.desktop)) ? tile : null;
      card.classList.toggle('tv-over-desk', !!tile);
      if (target !== over) { over?.classList.remove('tv-drop'); over = target; over?.classList.add('tv-drop'); }
    };
    const end = (ev) => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', end);
      window.removeEventListener('pointercancel', end);
      if (!dragging || tv !== s) return;
      s.drag = false;
      s.suppressClick = true;
      setTimeout(() => { s.suppressClick = false; }, 0);
      over?.classList.remove('tv-drop');
      const index = over ? (over.dataset.index === 'new' ? 'new' : Number(over.dataset.index)) : null;
      if (ev.type === 'pointerup' && index !== null && moveTo(win, index)) return; // reflow fades the card out
      const back = Shell.animate(card, [{ translate: card.style.translate, scale: getComputedStyle(card).scale }, { translate: '0px 0px', scale: '1' }],
        { duration: DUR.fast });
      card.classList.remove('tv-dragging', 'tv-over-desk');
      card.style.translate = '';
      back.then(() => { card.style.transformOrigin = ''; });
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', end);
    window.addEventListener('pointercancel', end);
  }

  /* ---------- desktops strip ---------- */
  function renderStrip({ added, keepFocus = true } = {}) {
    const s = tv;
    const inStrip = s.strip.contains(document.activeElement);
    const had = document.activeElement?.closest?.('.tv-desk')?.dataset.index;
    const cur = currentDesk();
    const list = desktops();
    const row = h('div', 'tv-desks');
    list.forEach((d) => row.appendChild(deskTile(d, d.index === cur, list.length > 1)));
    row.appendChild(newDeskTile());
    const prev = h('button', 'tv-scroll', { type: 'button', tabindex: '-1', 'aria-label': 'Scroll left' });
    const next = h('button', 'tv-scroll', { type: 'button', tabindex: '-1', 'aria-label': 'Scroll right' });
    prev.appendChild(Shell.icon('fluent/chevron-left', { size: 16 }));
    next.appendChild(Shell.icon('fluent/chevron-right', { size: 16 }));
    const update = () => {
      prev.classList.toggle('off', row.scrollLeft <= 0);
      next.classList.toggle('off', row.scrollLeft + row.clientWidth >= row.scrollWidth - 1);
    };
    const page = (dir) => row.scrollBy({ left: dir * row.clientWidth * 0.8, behavior: Shell.reducedMotion() ? 'auto' : 'smooth' });
    prev.addEventListener('click', () => page(-1));
    next.addEventListener('click', () => page(1));
    row.addEventListener('scroll', update, { passive: true });
    row.addEventListener('wheel', (e) => { row.scrollLeft += e.deltaY || e.deltaX; e.preventDefault(); }, { passive: false });
    row.addEventListener('mouseleave', () => previewDesk(currentDesk()));
    s.strip.replaceChildren(prev, row, next);
    // Rebuilding drops focus to <body>: it stays on the same tile (New desktop after a click), else Task View keeps it.
    if (inStrip && keepFocus && had != null) row.querySelector(`.tv-desk[data-index="${had}"]`)?.focus({ preventScroll: true });
    keepKeyboard();
    markShown();
    if (added != null) {
      const tile = row.querySelector(`.tv-desk[data-index="${added}"]`);
      if (tile) { tile.scrollIntoView({ inline: 'nearest', block: 'nearest' }); fadeIn(tile); }
    }
    requestAnimationFrame(update);
  }

  function deskTile(d, current, closable) {
    const name = deskName(d);
    const tile = h('div', `tv-desk${current ? ' current' : ''}`, { role: 'button', tabindex: '0', 'aria-label': name, 'data-index': String(d.index) });
    if (current) tile.setAttribute('aria-current', 'true');
    const head = h('div', 'tv-desk-head');
    const label = h('span', 'tv-desk-name', { text: name });
    head.appendChild(label);
    if (closable) {
      const x = h('button', 'tv-x', { type: 'button', tabindex: '-1', 'aria-label': 'Close', 'data-tooltip': 'Close' });
      x.appendChild(Shell.icon('fluent/dismiss', { size: 16 }));
      x.addEventListener('click', (e) => { e.stopPropagation(); closeDesk(d); });
      head.appendChild(x);
    }
    tile.append(head, h('div', 'tv-desk-thumb'));
    label.addEventListener('click', (e) => { if (current) { e.stopPropagation(); renameDesk(tile, d); } });
    tile.addEventListener('click', () => switchDesk(d.index));
    tile.addEventListener('mouseenter', () => { if (!tv?.drag) previewDesk(d.index); });
    tile.addEventListener('focus', () => previewDesk(d.index));
    tile.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      e.stopPropagation();
      Shell.menu?.show?.({
        x: e.clientX, y: e.clientY,
        items: [
          { label: 'Rename', action: () => renameDesk(tile, d) },
          { label: 'Choose background', action: () => { closeTaskView(); Shell.apps.launch('settings', { page: 'personalization/background' }); } },
          { separator: true },
          { label: 'Close', disabled: !closable, action: () => closeDesk(d) },
        ],
      });
    });
    return tile;
  }

  function newDeskTile() {
    const tile = h('div', 'tv-desk tv-desk-new', { role: 'button', tabindex: '0', 'aria-label': 'New desktop', 'data-index': 'new' });
    const box = h('div', 'tv-desk-thumb');
    box.append(Shell.icon('fluent/add', { size: 20 }), h('span', 'tv-new-label', { text: 'New desktop' }));
    tile.append(h('div', 'tv-desk-head'), box);
    tile.addEventListener('click', () => {
      if (!tv || !Shell.wm?.addDesktop) return;
      Shell.wm.addDesktop();
      renderStrip({ added: desktops().length - 1 });
    });
    return tile;
  }

  function markShown() {
    tv?.strip.querySelectorAll('.tv-desk[data-index]').forEach((t) => t.classList.toggle('shown', t.dataset.index === String(tv.shown)));
  }

  /* Hovering (or focusing) a desktop shows its windows above, as Windows 11 does. */
  function previewDesk(i) {
    if (!tv) return;
    tv.timers.forEach(clearTimeout);
    tv.timers = [setTimeout(() => {
      if (!tv || tv.drag || tv.shown === i || !desktops().some((d) => d.index === i)) return;
      tv.shown = i;
      markShown();
      renderCards('swap');
    }, 250)];
  }

  function switchDesk(i) {
    if (!tv) return;
    if (i !== currentDesk()) {
      Shell.wm?.setDesktop?.(i);
      if (tv.shown !== i) { tv.shown = i; renderCards('none'); }
    }
    closeTaskView();
  }

  function closeDesk(d) {
    if (!tv || !Shell.wm?.removeDesktop || desktops().length < 2) return;
    Shell.wm.removeDesktop(d.index);
    if (names[d.index]) { delete names[d.index]; Shell.store.set('desktop-names', names); }
    if (!desktops().some((x) => x.index === tv.shown)) tv.shown = currentDesk();
    renderStrip({ keepFocus: false }); // the tile under the keyboard is gone; focusing its neighbour would preview it
    renderCards('reflow');
  }

  function renameDesk(tile, d) {
    const label = tile.querySelector('.tv-desk-name');
    if (!label) return;
    const input = h('input', 'tv-desk-input', { type: 'text', 'aria-label': 'Desktop name', maxlength: '50', spellcheck: 'false' });
    input.value = deskName(d);
    label.replaceWith(input);
    input.focus();
    input.select();
    let done = false;
    const finish = (save) => {
      if (done || !tv) return;
      done = true;
      if (save) setDeskName(d, input.value);
      renderStrip();
      tv.strip.querySelector(`.tv-desk[data-index="${d.index}"]`)?.focus({ preventScroll: true });
    };
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); finish(true); }
      else if (e.key === 'Escape') { e.preventDefault(); finish(false); }
    });
    input.addEventListener('blur', () => finish(true));
    input.addEventListener('click', (e) => e.stopPropagation());
    input.addEventListener('pointerdown', (e) => e.stopPropagation());
  }

  /* ---------- Task View keyboard ---------- */
  function taskViewKey(e) {
    if (!tv) return;
    const active = document.activeElement;
    if (active?.tagName === 'INPUT' && tv.root.contains(active)) return; // renaming
    if (Shell.layer('menus')?.contains(active)) return; // a menu has the keyboard
    const cards = [...tv.grid.querySelectorAll('.tv-card:not(.tv-gone)')];
    const desks = [...tv.strip.querySelectorAll('.tv-desk')];
    const all = [...cards, ...desks];
    const current = all.includes(active) ? active : null;
    const group = current && desks.includes(current) ? desks : cards;
    let next = null;
    switch (e.key) {
      case 'Escape': closeTaskView(); break;
      case 'ArrowLeft': case 'ArrowRight':
        next = current ? stepInGroup(group, current, e.key === 'ArrowLeft' ? -1 : 1, false) : all[0];
        break;
      case 'ArrowUp': case 'ArrowDown':
        next = spatial(all, current, e.key) || (current ? null : all[0]);
        break;
      case 'Tab':
        next = stepInGroup(all, current || all[all.length - 1], e.shiftKey ? -1 : 1, true);
        if (!current && !e.shiftKey) next = all[0];
        break;
      case 'Enter': case ' ':
        if (!current) return;
        current.click();
        break;
      case 'Delete':
        if (current?.tvWin) closeCard(current.tvWin);
        else return;
        break;
      case 'F2':
        if (current && desks.includes(current) && current.dataset.index !== 'new') {
          const d = desktops().find((x) => String(x.index) === current.dataset.index);
          if (d) renameDesk(current, d);
        } else return;
        break;
      default: return;
    }
    e.preventDefault();
    e.stopPropagation();
    next?.focus({ preventScroll: true });
    next?.scrollIntoView?.({ inline: 'nearest', block: 'nearest' });
  }

  /* ---------- Alt+Tab ---------- */
  let sw = null;

  function switcherStep(dir) {
    if (tv) return;
    if (!sw) {
      const wins = mru(Shell.wm?.list?.());
      if (!wins.length) return;
      const startAt = wins[0] === Shell.wm?.focused?.() && wins.length > 1 ? 1 : 0;
      sw = { wins, index: dir > 0 ? startAt : wins.length - 1, plate: null, sticky: false };
      // A quick Alt+Tab tap switches without flashing the plate, as Windows does.
      sw.timer = setTimeout(showSwitcher, 120);
      window.addEventListener('keydown', switcherKey, true);
      window.addEventListener('keyup', switcherKeyUp, true);
      window.addEventListener('pointerdown', switcherPointer, true);
      window.addEventListener('blur', switcherBlur);
      window.addEventListener('resize', buildSwitcher);
      return;
    }
    sw.index = (sw.index + dir + sw.wins.length) % sw.wins.length;
    if (sw.plate) selectSwitcher(); else showSwitcher();
  }

  function showSwitcher() {
    if (!sw || sw.plate) return;
    clearTimeout(sw.timer);
    buildSwitcher();
    Shell.animate(sw.plate, [{ opacity: 0 }, { opacity: 1 }], { duration: DUR.faster, easing: 'linear' });
  }

  function buildSwitcher() {
    if (!sw) return;
    if (!sw.wins.length) { switcherEnd(false); return; }
    const plate = h('div', 'at-plate acrylic surface-flyout', { role: 'listbox', 'aria-label': 'Task switcher' });
    const thumbH = Math.round(Math.max(64, Math.min(168, innerHeight * 0.15, innerWidth * 0.22)));
    sw.items = sw.wins.map((win, i) => {
      const sz = winSize(win);
      const w = Math.round(thumbH * Math.min(2, Math.max(0.9, sz.w / sz.h)));
      const item = h('div', 'at-item', { role: 'option', 'aria-label': win.title || 'Window' });
      item.style.width = `${Math.max(160, w + 16)}px`; // narrow windows keep a legible title; the plate wraps rows
      item.append(titleRow(win, 'at-head', () => win.close?.()), makeThumb(win, w, thumbH, 'at-thumb'));
      item.addEventListener('click', () => { if (sw) { sw.index = i; switcherEnd(true); } });
      item.addEventListener('auxclick', (e) => { if (e.button === 1) win.close?.(); });
      return item;
    });
    plate.append(...sw.items);
    if (sw.plate) sw.plate.replaceWith(plate); else Shell.layer('overlays').appendChild(plate);
    sw.plate = plate;
    selectSwitcher();
  }

  function selectSwitcher() {
    sw.items.forEach((item, i) => {
      item.classList.toggle('selected', i === sw.index);
      item.setAttribute('aria-selected', String(i === sw.index));
    });
    sw.items[sw.index]?.scrollIntoView({ block: 'nearest' });
  }

  function switcherKey(e) {
    if (!sw) return;
    if (e.key === 'Alt') { e.preventDefault(); return; }
    if (!e.altKey && !sw.sticky) { switcherEnd(true); return; } // Alt went up where we could not see it
    if (['Tab', '`', '~'].includes(e.key)) return; // stepped by the Shell.keys combos
    switch (e.key) {
      case 'Escape': switcherEnd(false); break;
      case 'Enter': case ' ': switcherEnd(true); break;
      case 'ArrowLeft': case 'ArrowRight':
        sw.index = (sw.index + (e.key === 'ArrowLeft' ? -1 : 1) + sw.wins.length) % sw.wins.length;
        if (sw.plate) selectSwitcher(); else showSwitcher();
        break;
      case 'ArrowUp': case 'ArrowDown': {
        if (!sw.plate) showSwitcher();
        const next = spatial(sw.items, sw.items[sw.index], e.key);
        if (next) { sw.index = sw.items.indexOf(next); selectSwitcher(); }
        break;
      }
      case 'Delete': sw.wins[sw.index]?.close?.(); break;
      default: return;
    }
    e.preventDefault();
    e.stopPropagation();
  }
  function switcherKeyUp(e) { if (sw && !sw.sticky && e.key === 'Alt') { e.preventDefault(); switcherEnd(true); } }
  function switcherPointer(e) { if (sw && !(sw.plate && sw.plate.contains(e.target))) switcherEnd(false); }
  function switcherBlur() { switcherEnd(false); }

  function switcherEnd(commit) {
    const s = sw;
    if (!s) return;
    sw = null;
    clearTimeout(s.timer);
    window.removeEventListener('keydown', switcherKey, true);
    window.removeEventListener('keyup', switcherKeyUp, true);
    window.removeEventListener('pointerdown', switcherPointer, true);
    window.removeEventListener('blur', switcherBlur);
    window.removeEventListener('resize', buildSwitcher);
    s.plate?.remove();
    const win = commit ? s.wins[s.index] : null;
    if (win) {
      if (win.isMinimized?.()) win.restore?.();
      win.focus?.();
    }
  }

  /* ---------- wiring ---------- */
  Shell.on('window', ({ type, win } = {}) => {
    if (tv && ['open', 'close', 'desktop'].includes(type)) { keepKeyboard(); scheduleReflow(); }
    if (tv && (type === 'title' || type === 'icon') && win) {
      const entry = tv.cards.get(win.id);
      const title = entry?.card.querySelector('.tv-title');
      if (title) title.textContent = win.title || '';
    }
    if (sw && type === 'close' && win) {
      const i = sw.wins.indexOf(win);
      if (i >= 0) {
        sw.wins.splice(i, 1);
        if (sw.index >= sw.wins.length) sw.index = Math.max(0, sw.wins.length - 1);
        if (sw.plate || !sw.wins.length) buildSwitcher();
      }
    }
  });

  Shell.taskView = {
    open: (opts) => openTaskView(opts),
    close: () => closeTaskView(),
    toggle(opts) {
      if (tv) { closeTaskView(); return; }
      if (performance.now() - dismissedAt < 400) return; // the pointerdown that light-dismissed us was this toggle's own click
      openTaskView(opts);
    },
    isOpen: () => !!tv,
  };

  Shell.keys.on('Win+Tab', () => Shell.taskView.toggle({ keyboard: true }));
  Shell.keys.on('Alt+Tab', () => switcherStep(1));
  Shell.keys.on('Alt+Shift+Tab', () => switcherStep(-1));
  Shell.keys.on('Alt+`', () => switcherStep(1));
  Shell.keys.on('Alt+Shift+~', () => switcherStep(-1));

  Shell.on('boot', () => {
    // Let apps opened by ?open=... mount their windows first.
    if (Shell.debugOpen.includes('taskview')) setTimeout(() => openTaskView({ keyboard: true }), 400);
    if (Shell.debugOpen.includes('alttab')) {
      setTimeout(() => {
        switcherStep(1);
        if (sw) { sw.sticky = true; showSwitcher(); }
      }, 400);
    }
  });
})();
