/* UI primitives shared by every surface: the Windows 11 context menu (Shell.menu), tooltips
   (Shell.tooltip), placement next to an anchor (Shell.ui.position) and the small behaviours
   behind the WinUI control classes styled in css/ui.css (slider fill and value tip, TextBox
   clear button, ComboBox, Expander, NavigationView list, SelectorBar pill).
   Metrics: docs/windows11-reference/winui-controls.json (MenuFlyout, ComboBox, ToolTip,
   NavigationView); timing: tier-a.json. Contract: CONTRACT.md, "ui". */
(() => {
  'use strict';

  const Shell = (window.Shell = window.Shell || {});
  const ui = (Shell.ui = Shell.ui || {});

  const SUBMENU_DELAY = 400; // SPI_GETMENUSHOWDELAY default; WinUI's cascading menus read it
  const TIP_DELAY = 500, TIP_RESHOW = 100; // TTDT_INITIAL and TTDT_RESHOW (initial / 5)
  const NORMAL = 250, FAST = 167, FASTER = 83;
  const PILL_MS = 400; // NavigationView/SelectorBar indicator: stretch toward the target, then settle

  const layer = (name) => Shell.layer?.(name) || document.body;
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(v, hi));
  const reduced = () => Shell.reducedMotion?.() ?? false;
  const h = (tag, className, props, attrs) => {
    const n = document.createElement(tag);
    if (className) n.className = className;
    if (props) Object.assign(n, props);
    if (attrs) for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
    return n;
  };
  const taskbarTop = () => innerHeight - (Shell.layer?.('taskbar')?.offsetHeight || 0);
  const fadeOut = (node) => {
    node.style.pointerEvents = 'none';
    Shell.animate(node, [{ opacity: 1 }, { opacity: 0 }], { duration: FASTER, easing: Shell.ease.out, fill: 'forwards' })
      .then(() => node.remove());
  };

  /* Keyboard vs pointer: a menu opened from the keyboard focuses its first item. */
  let keyboardModality = false;
  let pointer = { x: -1, y: -1 };
  window.addEventListener('keydown', () => { keyboardModality = true; }, true);
  window.addEventListener('pointerdown', () => { keyboardModality = false; }, true);
  document.addEventListener('pointermove', (e) => { pointer = { x: e.clientX, y: e.clientY }; }, { passive: true });

  /* ---------- placement ---------- */
  /* Places a position:fixed node beside `anchor` (a DOMRect or {left, top, right, bottom}) on
     `placement` = side[-align] (top|bottom|left|right, start|end|center). Flips to the other
     side when it does not fit and that side has more room, flips the alignment before
     clamping, and keeps surfaces anchored above the taskbar out of it. */
  ui.position = (node, anchor, placement = 'bottom-start', { gap = 4, margin = 8 } = {}) => {
    const a = {
      left: anchor.left ?? anchor.x, top: anchor.top ?? anchor.y,
      right: anchor.right ?? (anchor.x + (anchor.width || 0)), bottom: anchor.bottom ?? (anchor.y + (anchor.height || 0)),
    };
    const w = node.offsetWidth, ht = node.offsetHeight;
    const floor = a.top < taskbarTop() ? taskbarTop() : innerHeight;
    const b = { l: margin, t: margin, r: innerWidth - margin, b: floor - margin };
    let [side, align = 'center'] = placement.split('-');
    const vertical = side === 'top' || side === 'bottom';
    const room = { top: a.top - gap - b.t, bottom: b.b - a.bottom - gap, left: a.left - gap - b.l, right: b.r - a.right - gap };
    const flip = { top: 'bottom', bottom: 'top', left: 'right', right: 'left' }[side];
    if (room[side] < (vertical ? ht : w) && room[flip] > room[side]) side = flip;
    const cross = (lo, hi, size, min, max) => {
      if (align === 'start') return lo + size <= max || hi - size < min ? lo : hi - size;
      if (align === 'end') return hi - size >= min || lo + size > max ? hi - size : lo;
      return (lo + hi - size) / 2;
    };
    let x, y;
    if (vertical) {
      y = side === 'bottom' ? a.bottom + gap : a.top - gap - ht;
      x = cross(a.left, a.right, w, b.l, b.r);
    } else {
      x = side === 'right' ? a.right + gap : a.left - gap - w;
      y = cross(a.top, a.bottom, ht, b.t, b.b);
    }
    x = Math.round(clamp(x, b.l, b.r - w));
    y = Math.round(clamp(y, b.t, b.b - ht));
    node.style.left = `${x}px`;
    node.style.top = `${y}px`;
    return { x, y, side, placement: `${side}-${align}` };
  };

  /* WinUI's menu reveal: the surface slides out from behind the edge that faces its anchor,
     clipped at that edge, while it fades in. */
  function reveal(node, dir) {
    if (reduced()) return;
    const d = Math.round(Math.min(node.offsetHeight / 2, 56));
    const out = '-48px';
    const from = dir === 'up'
      ? { transform: `translateY(${d}px)`, clipPath: `inset(${out} ${out} ${d}px ${out})` }
      : { transform: `translateY(${-d}px)`, clipPath: `inset(${d}px ${out} ${out} ${out})` };
    Shell.animate(node, [from, { transform: 'translateY(0)', clipPath: `inset(${out} ${out} ${out} ${out})` }], { duration: NORMAL, easing: Shell.ease.in });
    Shell.animate(node, [{ opacity: 0 }, { opacity: 1 }], { duration: FASTER, easing: 'linear' });
  }

  /* ---------- context menu ---------- */
  let menuSeq = 0;
  let session = null; // the open menu: {id, levels: [{el, parentRow}], timers, ...}
  let guard = null; // a press on the open menu's anchor closes it; the click that follows must not reopen it
  const itemOf = new WeakMap(); // row element -> item
  const rectKey = (r) => [r.left ?? r.x, r.top ?? r.y, r.width, r.height].map((v) => Math.round(v || 0)).join(',');

  /* Items also take accessKey: 'A' (Win32's '&A'): the letter runs the item and is underlined
     while keyboard cues show. */
  Shell.menu = {
    show(opts = {}) {
      const { x = 0, y = 0, items = [], anchorRect = null, placement = 'bottom-start', minWidth = 0, onClose } = opts;
      const key = anchorRect ? rectKey(anchorRect) : '';
      if (key && guard && guard.key === key && performance.now() < guard.until) {
        guard = null;
        if (onClose) queueMicrotask(onClose); // after the caller has stored the handle
        return { close() {} };
      }
      guard = null;
      const s = { id: `menu-${++menuSeq}`, levels: [], timers: {}, onClose, anchorRect, key, minWidth, restore: document.activeElement };
      Shell.transient.open(s.id, { group: 'menu', close: () => closeSession(s), elements: [(e) => pressInside(s, e)] });
      session = s;
      const root = openLevel(s, items, null);
      const place = () => ui.position(root.el, anchorRect || { left: x, right: x, top: y, bottom: y }, placement,
        anchorRect ? { gap: 4 } : { gap: 0, margin: 4 });
      const res = place();
      /* A menu opened while the UI font is still loading changes height when it lands; place it again
         so a menu above its anchor keeps its bottom edge there. */
      if (document.fonts?.status === 'loading') document.fonts.ready.then(() => { if (session === s) place(); });
      const up = res.side === 'top';
      root.el.classList.toggle('up', up);
      reveal(root.el, up ? 'up' : 'down');
      s.onKey = (e) => menuKey(s, e);
      window.addEventListener('keydown', s.onKey, true);
      if (keyboardModality) { setCues(s); setKbd(s, true); focusEdge(root, 1); } else root.el.focus({ preventScroll: true });
      return { close: () => Shell.transient.close(s.id) };
    },
    close() { if (session) Shell.transient.close(session.id); },
    isOpen: () => !!session,
  };

  function pressInside(s, e) {
    if (s.levels.some((l) => l.el.contains(e.target))) return true;
    const r = s.anchorRect;
    if (r && e.clientX >= (r.left ?? r.x) && e.clientX <= (r.right ?? r.x + r.width) && e.clientY >= (r.top ?? r.y) && e.clientY <= (r.bottom ?? r.y + r.height)) {
      guard = { key: s.key, until: performance.now() + 600 };
    }
    return false;
  }

  /* Focus follows the pointer (as in WinUI) but the focus visual and the keyboard plate only
     show once keys are used; moving the pointer over a menu hides them again. */
  function setKbd(s, on) {
    s.kbd = on;
    s.levels.forEach((l) => l.el.classList.toggle('kbd', on));
  }
  /* Access-key underlines (keyboard cues) show once the keyboard opened or drove the menu and,
     as in Win32, stay for the rest of the menu's life. */
  function setCues(s) {
    s.cues = true;
    s.levels.forEach((l) => l.el.classList.add('cues'));
  }

  function openLevel(s, items, parentRow) {
    const level = { el: buildMenu(s, items.filter(Boolean), s.levels.length), parentRow };
    level.el.classList.toggle('kbd', !!s.kbd);
    level.el.classList.toggle('cues', !!s.cues);
    s.levels.push(level);
    layer('menus').append(level.el);
    return level;
  }

  function buildMenu(s, items, depth) {
    const menu = h('div', 'menu acrylic surface-flyout', { tabIndex: -1 }, { role: 'menu' });
    if (depth === 0 && s.minWidth) menu.style.minWidth = `${s.minWidth}px`;
    const bar = items.find((i) => i.commandBar)?.commandBar?.filter(Boolean) || [];
    const entries = items.filter((i) => !i.commandBar);
    const hasCheck = entries.some((i) => 'checked' in i || i.radio);
    const hasIcon = entries.some((i) => i.icon && !i.separator);
    if (bar.length) {
      const row = h('div', 'menu-commandbar', null, { role: 'group' });
      bar.forEach((c) => row.append(commandButton(s, c)));
      menu.append(row, h('div', 'menu-separator menu-cmd-sep', null, { role: 'separator' }));
    }
    const list = h('div', 'menu-list');
    entries.forEach((item) => list.append(item.separator
      ? h('div', 'menu-separator', null, { role: 'separator' })
      : item.header ? h('div', 'menu-header', { textContent: item.header }, { role: 'presentation' }) // jump list "Pinned"/"Recent"
        : menuRow(s, item, depth, hasCheck, hasIcon)));
    menu.append(list);
    menu.addEventListener('contextmenu', (e) => e.preventDefault());
    menu.addEventListener('mousedown', (e) => e.preventDefault()); // focus stays where the menu put it
    menu.addEventListener('pointerenter', () => { if (s.levels[depth + 1] || depth > 0) clearTimer(s, 'close'); });
    menu.addEventListener('pointermove', () => { if (s.kbd) setKbd(s, false); });
    return menu;
  }

  function commandButton(s, c) {
    const b = h('button', 'menu-command', { type: 'button', tabIndex: -1, disabled: !!c.disabled }, { 'aria-label': c.label || '' });
    if (c.label) b.dataset.tooltip = c.label;
    if (c.icon) b.append(Shell.icon(c.icon, { size: 16 }));
    b.addEventListener('click', () => activate(s, c));
    b.addEventListener('pointerenter', () => {
      if (!c.disabled) b.focus({ preventScroll: true });
      clearTimer(s, 'open');
      if (s.levels[1]) setTimer(s, 'close', () => closeLevels(s, 1), SUBMENU_DELAY);
    });
    return b;
  }

  function menuRow(s, item, depth, hasCheck, hasIcon) {
    const role = item.radio ? 'menuitemradio' : 'checked' in item ? 'menuitemcheckbox' : 'menuitem';
    const row = h('div', 'menu-item', { tabIndex: -1 }, { role });
    itemOf.set(row, item);
    if (role !== 'menuitem') row.setAttribute('aria-checked', String(!!item.checked));
    if (item.accessKey) row.setAttribute('aria-keyshortcuts', item.accessKey.toUpperCase());
    if (item.disabled) { row.classList.add('disabled'); row.setAttribute('aria-disabled', 'true'); }
    if (item.submenu) { row.setAttribute('aria-haspopup', 'menu'); row.setAttribute('aria-expanded', 'false'); }
    if (hasCheck) {
      const c = h('span', 'menu-check');
      if (item.checked) c.append(item.radio ? h('span', 'menu-radio') : Shell.icon('fluent/checkmark', { size: 16 }));
      row.append(c);
    }
    if (hasIcon) {
      const ic = h('span', 'menu-icon');
      if (item.icon) ic.append(Shell.icon(item.icon, { size: 16 }));
      row.append(ic);
    }
    row.append(menuLabel(item));
    if (item.shortcut) row.append(h('span', 'menu-shortcut', { textContent: item.shortcut }));
    if (item.submenu) {
      const ch = h('span', 'menu-chevron');
      ch.append(Shell.icon('fluent/chevron-right', { size: 12 }));
      row.append(ch);
    }
    row.addEventListener('pointerenter', () => hoverRow(s, depth, row));
    row.addEventListener('pointerleave', () => clearTimer(s, 'open'));
    row.addEventListener('click', () => invoke(s, depth, row, false));
    return row;
  }

  /* The access key's letter is underlined where it first appears, preferring the same case
     ('Terminal (Admin)' + 'A' underlines Admin, as Windows' '&Admin'). */
  function menuLabel(item) {
    const text = item.label ?? '', k = item.accessKey;
    const label = h('span', 'menu-label');
    let i = k ? text.indexOf(k) : -1;
    if (k && i < 0) i = text.toLowerCase().indexOf(k.toLowerCase());
    if (i < 0) label.textContent = text;
    else label.append(text.slice(0, i), h('u', 'menu-ak', { textContent: text[i] }), text.slice(i + 1));
    return label;
  }

  const setTimer = (s, name, fn, ms) => { clearTimer(s, name); s.timers[name] = setTimeout(fn, ms); };
  const clearTimer = (s, name) => { clearTimeout(s.timers[name]); delete s.timers[name]; };

  function hoverRow(s, depth, row) {
    if (!s.levels[depth]) return;
    const item = itemOf.get(row);
    if (!item.disabled) row.focus({ preventScroll: true });
    clearTimer(s, 'open');
    const child = s.levels[depth + 1];
    if (child && child.parentRow === row) { clearTimer(s, 'close'); return; }
    if (child) setTimer(s, 'close', () => closeLevels(s, depth + 1), SUBMENU_DELAY);
    if (item.submenu && !item.disabled) setTimer(s, 'open', () => openSubmenu(s, depth, row, false), SUBMENU_DELAY);
  }

  function invoke(s, depth, row, fromKeyboard) {
    const item = itemOf.get(row);
    if (!item || item.disabled) return;
    if (item.submenu) openSubmenu(s, depth, row, fromKeyboard);
    else activate(s, item);
  }

  function activate(s, item) {
    if (item.disabled) return;
    Shell.transient.close(s.id);
    item.action?.();
  }

  function openSubmenu(s, depth, row, focusFirst) {
    clearTimer(s, 'open');
    clearTimer(s, 'close');
    const existing = s.levels[depth + 1];
    if (existing && existing.parentRow === row) { if (focusFirst) focusEdge(existing, 1); return; }
    closeLevels(s, depth + 1);
    const level = openLevel(s, itemOf.get(row).submenu, row);
    row.classList.add('open');
    row.setAttribute('aria-expanded', 'true');
    const m = s.levels[depth].el.getBoundingClientRect(), r = row.getBoundingClientRect();
    /* first plate of the submenu level with the parent's plate; overlap the parent's 4 px padding */
    ui.position(level.el, { left: m.left + 4, right: m.right - 4, top: r.top - 3, bottom: r.bottom + 3 }, 'right-start', { gap: 0, margin: 4 });
    reveal(level.el, 'down');
    if (focusFirst) focusEdge(level, 1);
  }

  function closeLevels(s, from) {
    while (s.levels.length > from) {
      const l = s.levels.pop();
      fadeOut(l.el);
      if (l.parentRow) { l.parentRow.classList.remove('open'); l.parentRow.setAttribute('aria-expanded', 'false'); }
    }
  }

  function closeSession(s) {
    Object.keys(s.timers).forEach((k) => clearTimer(s, k));
    window.removeEventListener('keydown', s.onKey, true);
    const hadFocus = s.levels.some((l) => l.el.contains(document.activeElement));
    closeLevels(s, 0);
    if (session === s) session = null;
    if (hadFocus && s.restore?.isConnected) s.restore.focus?.({ preventScroll: true });
    s.onClose?.();
  }

  /* Keyboard stops of one level, top to bottom: the command row (one stop) and enabled rows. */
  function stops(level) {
    const rows = [...level.el.querySelectorAll('.menu-list > .menu-item:not(.disabled)')];
    const cmds = [...level.el.querySelectorAll('.menu-command:not(:disabled)')];
    const cmd = cmds.includes(document.activeElement) ? document.activeElement : cmds[0];
    if (!cmd) return rows;
    return level.el.classList.contains('up') ? [...rows, cmd] : [cmd, ...rows];
  }

  function focusStop(node) {
    if (!node) return;
    node.focus({ preventScroll: true });
    node.scrollIntoView?.({ block: 'nearest' });
  }

  function focusEdge(level, dir) {
    const rows = [...level.el.querySelectorAll('.menu-list > .menu-item:not(.disabled)')];
    focusStop(dir > 0 ? rows[0] : rows[rows.length - 1]);
  }

  function menuKey(s, e) {
    if (session !== s || !s.levels.length) return;
    const level = s.levels.find((l) => l.el.contains(document.activeElement)) || s.levels[s.levels.length - 1];
    const depth = s.levels.indexOf(level);
    const active = level.el.contains(document.activeElement) ? document.activeElement : null;
    const inBar = !!active?.classList.contains('menu-command');
    const row = active?.closest('.menu-item');
    const k = e.key;
    let handled = true;
    if (k === 'ArrowDown' || k === 'ArrowUp' || k === 'Tab') {
      const list = stops(level);
      const i = list.indexOf(active);
      const dir = k === 'ArrowUp' || (k === 'Tab' && e.shiftKey) ? -1 : 1;
      focusStop(i < 0 ? list[dir > 0 ? 0 : list.length - 1] : list[(i + dir + list.length) % list.length]);
    } else if (k === 'Home' || k === 'End') {
      focusEdge(level, k === 'Home' ? 1 : -1);
    } else if (k === 'ArrowRight' || k === 'ArrowLeft') {
      const dir = k === 'ArrowRight' ? 1 : -1;
      if (inBar) {
        const cmds = [...level.el.querySelectorAll('.menu-command:not(:disabled)')];
        focusStop(cmds[(cmds.indexOf(active) + dir + cmds.length) % cmds.length]);
      } else if (dir > 0) {
        if (row && itemOf.get(row)?.submenu) invoke(s, depth, row, true);
      } else if (depth > 0) {
        closeLevels(s, depth);
        focusStop(level.parentRow);
      }
    } else if (k === 'Escape') {
      if (depth > 0) { closeLevels(s, depth); focusStop(level.parentRow); } else Shell.transient.close(s.id);
    } else if (k === 'Enter' || k === ' ') {
      if (inBar) active.click(); else if (row) invoke(s, depth, row, true);
    } else if (k.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
      /* Access keys: a unique match runs (a submenu opens on its first item), several cycle.
         A menu that declares access keys answers only to them; others match first letters. */
      const all = [...level.el.querySelectorAll('.menu-list > .menu-item')];
      const rows = all.filter((r) => !r.classList.contains('disabled'));
      const keyed = all.some((r) => itemOf.get(r)?.accessKey);
      const lk = k.toLowerCase();
      const hits = rows.filter((r) => (keyed ? (itemOf.get(r)?.accessKey || '').toLowerCase() === lk
        : (itemOf.get(r)?.label || '').toLowerCase().startsWith(lk)));
      if (hits.length === 1) invoke(s, depth, hits[0], true);
      else if (hits.length) focusStop(hits.find((r) => rows.indexOf(r) > rows.indexOf(row)) || hits[0]);
    } else handled = false;
    if (handled) {
      e.preventDefault();
      e.stopPropagation();
      if (session === s) { if (!s.kbd) setKbd(s, true); if (!s.cues) setCues(s); }
    }
  }

  /* Menus, combo lists and tooltips do not survive a change of viewport size. */
  let viewport = `${innerWidth}x${innerHeight}`;
  window.addEventListener('resize', () => {
    const now = `${innerWidth}x${innerHeight}`;
    if (now === viewport) return;
    viewport = now;
    Shell.transient?.closeAll('menu');
    hideTip();
  });

  /* ---------- tooltips ---------- */
  /* Any [data-tooltip] element: shown after 500 ms of hover (100 ms when another tip just
     closed), above the taskbar for taskbar elements, below the pointer elsewhere, after
     500 ms of keyboard focus below the element. Hidden on press, key, wheel and leave. */
  const tip = { node: null, target: null, hover: null, source: '', timer: 0, focusTimer: 0, hiddenAt: -1e9, muted: null };

  function showTip(target, text, { placement, rect } = {}) {
    if (!text) return;
    clearTimeout(tip.timer);
    if (!tip.node) {
      tip.node = h('div', 'tooltip acrylic', { hidden: true }, { role: 'tooltip' });
      layer('tooltips').append(tip.node);
    }
    const node = tip.node, wasShown = !node.hidden;
    node.textContent = text;
    node.hidden = false;
    tip.target = target;
    const bar = !rect && target?.closest?.('#taskbar');
    if (bar) {
      const r = target.getBoundingClientRect(), t = bar.getBoundingClientRect().top;
      ui.position(node, { left: r.left, right: r.right, top: t, bottom: t }, 'top', { gap: 8 });
    } else if (rect || placement === 'top' || placement === 'bottom' || pointer.x < 0) {
      ui.position(node, rect || target.getBoundingClientRect(), placement || 'bottom', { gap: 8 });
    } else {
      ui.position(node, { left: pointer.x, right: pointer.x, top: pointer.y, bottom: pointer.y + 20 }, 'bottom-start', { gap: 0 });
    }
    if (!wasShown) Shell.animate(node, [{ opacity: 0 }, { opacity: 1 }], { duration: FAST, easing: 'linear' });
  }

  function hideTip() {
    clearTimeout(tip.timer);
    tip.timer = 0;
    if (tip.node && !tip.node.hidden) { tip.node.hidden = true; tip.hiddenAt = performance.now(); }
    tip.target = null;
    tip.source = '';
  }

  Shell.tooltip = {
    show(target, text, opts) { showTip(target, text, opts); tip.source = 'api'; },
    hide: hideTip,
  };

  /* tip.muted outlives pointerout: a flyout opening under a still pointer fires out/over
     without the pointer moving, so only a pointermove outside the element unmutes it. */
  function leaveHover() {
    if (tip.source !== 'api') hideTip();
    tip.hover = null;
  }

  document.addEventListener('pointerover', (e) => {
    if (e.pointerType === 'touch') return;
    const t = e.target.closest?.('[data-tooltip]') || null;
    if (t === tip.hover) return;
    if (tip.hover) leaveHover();
    if (!t) return;
    tip.hover = t;
    const delay = performance.now() - tip.hiddenAt < 400 ? TIP_RESHOW : TIP_DELAY;
    clearTimeout(tip.timer);
    tip.timer = setTimeout(() => {
      if (!t.isConnected || tip.hover !== t || tip.muted === t || tip.source === 'api') return;
      showTip(t, t.dataset.tooltip);
      tip.source = 'hover';
    }, delay);
  });
  document.addEventListener('pointerout', (e) => { if (!e.relatedTarget) { tip.muted = null; if (tip.hover) leaveHover(); } });
  window.addEventListener('pointerdown', () => {
    if (tip.source !== 'api') hideTip();
    tip.muted = tip.hover; // no tooltip again until the pointer leaves the element
  }, true);
  window.addEventListener('keydown', () => { if (tip.source === 'hover') hideTip(); }, true);
  window.addEventListener('wheel', () => { if (tip.source !== 'api') hideTip(); }, { passive: true, capture: true });
  window.addEventListener('blur', () => hideTip());
  document.addEventListener('pointermove', (e) => {
    if (tip.target && !tip.target.isConnected) hideTip();
    if (!tip.muted) return;
    const r = tip.muted.getBoundingClientRect();
    if (!tip.muted.isConnected || e.clientX < r.left || e.clientX >= r.right || e.clientY < r.top || e.clientY >= r.bottom) tip.muted = null;
  }, { passive: true });
  document.addEventListener('focusin', (e) => {
    const t = e.target.closest?.('[data-tooltip]');
    if (!t || !t.matches(':focus-visible')) return;
    clearTimeout(tip.focusTimer);
    tip.focusTimer = setTimeout(() => {
      if (document.activeElement !== t || tip.source === 'api') return;
      showTip(t, t.dataset.tooltip, { placement: 'bottom' });
      tip.source = 'focus';
    }, TIP_DELAY);
  });
  document.addEventListener('focusout', () => { clearTimeout(tip.focusTimer); if (tip.source === 'focus') hideTip(); });

  /* ---------- Slider ---------- */
  /* --fill is the value as a percentage (the contract); --fill-x ends the accent at the thumb's
     centre, which travels 10 px inside each end of the track. */
  const ratioOf = (input) => {
    const min = parseFloat(input.min) || 0, max = input.max === '' ? 100 : parseFloat(input.max);
    return max > min ? clamp((parseFloat(input.value) - min) / (max - min), 0, 1) : 0;
  };
  ui.slider = (input) => {
    const r = ratioOf(input);
    input.style.setProperty('--fill', `${(r * 100).toFixed(2)}%`);
    input.style.setProperty('--fill-x', `calc(10px + (100% - 20px) * ${r.toFixed(4)})`);
    return input;
  };
  /* Windows shows the value above the thumb while it is dragged. */
  let sliderDrag = null;
  const sliderTip = (s) => {
    const r = s.getBoundingClientRect(), x = r.left + 10 + (r.width - 20) * ratioOf(s), cy = r.top + r.height / 2;
    showTip(s, s.value, { rect: { left: x, right: x, top: cy - 10, bottom: cy + 10 }, placement: 'top' });
    tip.source = 'api';
  };
  document.addEventListener('pointerdown', (e) => {
    const s = e.target.closest?.('input.slider');
    if (!s || s.disabled || e.button !== 0) return;
    sliderDrag = s;
    requestAnimationFrame(() => { if (sliderDrag === s) sliderTip(s); });
  }, true);
  const onSliderValue = (e) => {
    if (!e.target.matches?.('input.slider')) return;
    ui.slider(e.target);
    if (sliderDrag === e.target) sliderTip(e.target);
  };
  document.addEventListener('input', onSliderValue, true);
  document.addEventListener('change', onSliderValue, true); // owners that set .value programmatically dispatch change
  const endSlider = () => { if (sliderDrag) { sliderDrag = null; hideTip(); } };
  window.addEventListener('pointerup', endSlider, true);
  window.addEventListener('pointercancel', endSlider, true);

  /* ---------- TextBox ---------- */
  /* Shell.ui.textbox(input, {clear}) adds WinUI's delete button, shown while the box has focus
     and text. The button is a sibling placed over the input's right edge, so the input keeps
     whatever layout its owner gave it. */
  ui.textbox = (input, { clear = false } = {}) => {
    if (!input.classList.contains('searchbox')) input.classList.add('textbox');
    if (!clear || input.dataset.clear) return input;
    input.dataset.clear = '1';
    const btn = h('button', 'textbox-clear', { type: 'button', tabIndex: -1, hidden: true }, { 'aria-label': 'Clear' });
    btn.append(Shell.icon('fluent/dismiss', { size: 12 }));
    btn.addEventListener('mousedown', (e) => e.preventDefault()); // keep focus in the input
    btn.addEventListener('click', () => {
      input.value = '';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.focus();
    });
    const place = () => {
      const show = document.activeElement === input && input.value !== '' && !input.disabled && !input.readOnly;
      if (show && !btn.isConnected && input.parentNode) input.after(btn);
      btn.hidden = !show;
      input.classList.toggle('has-clear', show);
      if (!show) return;
      const inset = input.classList.contains('searchbox') ? 32 : 0;
      btn.style.left = `${input.offsetLeft + input.offsetWidth - 34 - inset}px`;
      btn.style.top = `${input.offsetTop + 4}px`;
      btn.style.height = `${input.offsetHeight - 8}px`;
    };
    input.addEventListener('focus', place);
    input.addEventListener('input', place);
    input.addEventListener('blur', () => setTimeout(place));
    return input;
  };

  /* ---------- ComboBox ---------- */
  /* Shell.ui.combo(el, {options: [{value, label}], value, onChange, placeholder}) -> handle.
     The control is as wide as its widest option (WinUI's default); the list opens over the
     control with the selected item centred on it and grows out of that row. */
  let comboSeq = 0;
  ui.combo = (node, { options = [], value, onChange, placeholder = '' } = {}) => {
    let opts = options.slice();
    let current = value !== undefined ? value : opts[0]?.value;
    let popup = null, onKey = null;
    const id = `combo-${++comboSeq}`;
    node.classList.add('combo');
    if (node.tagName === 'BUTTON') node.type = 'button';
    else if (!node.hasAttribute('tabindex')) node.tabIndex = 0;
    node.setAttribute('role', 'combobox');
    node.setAttribute('aria-haspopup', 'listbox');
    node.setAttribute('aria-expanded', 'false');
    const box = h('span', 'combo-value');
    const chevron = h('span', 'combo-chevron');
    chevron.append(Shell.icon('fluent/chevron-down', { size: 12 }));
    node.replaceChildren(box, chevron);

    const disabled = () => node.disabled || node.getAttribute('aria-disabled') === 'true';
    const index = () => opts.findIndex((o) => o.value === current);
    const render = () => {
      box.replaceChildren(...opts.map((o) => h('span', o.value === current ? 'current' : '', { textContent: o.label })));
      if (index() < 0) box.append(h('span', 'current placeholder', { textContent: placeholder }));
    };
    const commit = (v) => { if (v === current) return; current = v; render(); onChange?.(v); };
    const close = () => Shell.transient.close(id);

    const open = () => {
      if (popup || disabled() || !opts.length) return;
      popup = h('div', 'combo-popup acrylic surface-flyout', { tabIndex: -1 }, { role: 'listbox' });
      opts.forEach((o) => {
        const it = h('div', 'combo-item', { tabIndex: -1, textContent: o.label }, { role: 'option' });
        if (o.value === current) { it.classList.add('selected'); it.setAttribute('aria-selected', 'true'); }
        it.addEventListener('pointerenter', () => it.focus({ preventScroll: true }));
        it.addEventListener('click', () => { commit(o.value); close(); });
        popup.append(it);
      });
      popup.addEventListener('mousedown', (e) => e.preventDefault());
      popup.addEventListener('contextmenu', (e) => e.preventDefault());
      popup.addEventListener('pointermove', () => popup.classList.remove('kbd'));
      popup.classList.toggle('kbd', keyboardModality);
      layer('menus').append(popup);
      const r = node.getBoundingClientRect();
      const floor = r.top < taskbarTop() ? taskbarTop() : innerHeight;
      popup.style.width = `${Math.max(r.width + 10, 80)}px`;
      popup.style.maxHeight = `${Math.min(15 * 36 + 6, floor - 16)}px`; // ComboBoxPopupMaxNumberOfItems
      const sel = popup.querySelector('.selected') || popup.firstElementChild;
      sel.scrollIntoView({ block: 'nearest' });
      const ht = popup.offsetHeight;
      const selTop = sel.offsetTop - popup.scrollTop;
      popup.style.top = `${Math.round(clamp(r.top + r.height / 2 - (selTop + sel.offsetHeight / 2), 8, floor - 8 - ht))}px`;
      popup.style.left = `${Math.round(clamp(r.left - 5, 8, innerWidth - 8 - popup.offsetWidth))}px`; // item text over the control's text
      if (!reduced()) {
        const below = ht - selTop - sel.offsetHeight;
        Shell.animate(popup, [{ clipPath: `inset(${selTop}px 0 ${below}px 0 round 4px)` }, { clipPath: 'inset(-48px -48px -48px -48px round 0px)' }], { duration: NORMAL, easing: Shell.ease.in });
        Shell.animate(popup, [{ opacity: 0 }, { opacity: 1 }], { duration: FASTER, easing: 'linear' });
      }
      Shell.transient.open(id, { group: 'menu', elements: [popup, node], close: teardown });
      node.setAttribute('aria-expanded', 'true');
      node.classList.add('open');
      onKey = (e) => popupKey(e);
      window.addEventListener('keydown', onKey, true);
      sel.focus({ preventScroll: true });
    };

    function teardown() {
      const p = popup;
      popup = null;
      window.removeEventListener('keydown', onKey, true);
      node.setAttribute('aria-expanded', 'false');
      node.classList.remove('open');
      if (p.contains(document.activeElement)) node.focus({ preventScroll: true });
      fadeOut(p);
    }

    function popupKey(e) {
      const items = [...popup.querySelectorAll('.combo-item')];
      const i = items.indexOf(document.activeElement);
      const k = e.key;
      let handled = true;
      if (k === 'Escape' || k === 'Tab' || (e.altKey && k === 'ArrowUp')) close();
      else if (k === 'ArrowDown' || k === 'ArrowUp') focusStop(items[clamp(i + (k === 'ArrowDown' ? 1 : -1), 0, items.length - 1)]);
      else if (k === 'Home' || k === 'End') focusStop(k === 'Home' ? items[0] : items[items.length - 1]);
      else if (k === 'Enter' || k === ' ') { if (i >= 0) commit(opts[i].value); close(); }
      else if (k.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
        const hits = items.filter((it) => it.textContent.toLowerCase().startsWith(k.toLowerCase()));
        focusStop(hits.find((it) => items.indexOf(it) > i) || hits[0]);
      } else handled = false;
      if (handled) { e.preventDefault(); e.stopPropagation(); popup?.classList.add('kbd'); }
    }

    node.addEventListener('click', () => (popup ? close() : open()));
    node.addEventListener('keydown', (e) => {
      if (popup || disabled()) return;
      if (e.key === 'Enter' || e.key === ' ' || e.key === 'F4' || (e.altKey && e.key === 'ArrowDown')) { e.preventDefault(); open(); }
      else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        const n = clamp(index() + (e.key === 'ArrowDown' ? 1 : -1), 0, opts.length - 1);
        if (opts[n]) commit(opts[n].value);
      }
    });
    render();
    return {
      el: node,
      get value() { return current; },
      set(v) { current = v; render(); },
      setOptions(list, v = current) { opts = list.slice(); current = v; render(); },
      open,
      close() { if (popup) close(); },
    };
  };

  /* ---------- Expander ---------- */
  /* Markup: .expander > .expander-header + .expander-content. The content slides down from
     under the header (WinUI's ExpandDown); the rows below move at once, as in WinUI. */
  ui.expander = (node, { open, onToggle } = {}) => {
    const header = node.querySelector(':scope > .expander-header');
    const content = node.querySelector(':scope > .expander-content');
    if (!header || !content) return null;
    if (!header.querySelector('.expander-chevron')) {
      const c = h('span', 'expander-chevron');
      c.append(Shell.icon('fluent/chevron-down', { size: 12 }));
      header.append(c);
    }
    if (header.tagName !== 'BUTTON' && !header.hasAttribute('tabindex')) { header.tabIndex = 0; header.setAttribute('role', 'button'); }
    const isOpen = () => node.classList.contains('open');
    if (open !== undefined) node.classList.toggle('open', !!open);
    header.setAttribute('aria-expanded', String(isOpen()));
    let token = 0;
    const set = (on) => {
      if (on === isOpen()) return;
      const t = ++token;
      node.classList.toggle('open', on);
      node.classList.toggle('closing', !on);
      header.setAttribute('aria-expanded', String(on));
      content.getAnimations().forEach((a) => a.cancel());
      const ht = content.offsetHeight;
      const shut = { transform: `translateY(${-ht}px)`, clipPath: `inset(${ht}px 0 0 0)` };
      const shown = { transform: 'translateY(0)', clipPath: 'inset(0 0 0 0)' };
      Shell.animate(content, on ? [shut, shown] : [shown, shut], on ? { duration: NORMAL, easing: Shell.ease.in } : { duration: FAST, easing: Shell.ease.out })
        .then(() => { if (t === token) node.classList.remove('closing'); });
      onToggle?.(on);
    };
    header.addEventListener('click', (e) => {
      const ctl = e.target.closest('button, a, input, select, textarea, label, .combo');
      if (ctl && ctl !== header && header.contains(ctl)) return; // controls in the header act alone
      set(!isOpen());
    });
    if (header.tagName !== 'BUTTON') {
      header.addEventListener('keydown', (e) => {
        if ((e.key === 'Enter' || e.key === ' ') && e.target === header) { e.preventDefault(); set(!isOpen()); }
      });
    }
    return { open: () => set(true), close: () => set(false), toggle: () => set(!isOpen()), isOpen };
  };

  /* ---------- selection pill (NavigationView, SelectorBar) ---------- */
  /* The resting pill is CSS on the selected item; a move animates a floating copy that
     stretches to the new item, then lets its trailing edge catch up. */
  function slidePill(container, from, to, axis) {
    if (reduced() || !from?.isConnected || from === to) return;
    container.querySelectorAll(':scope > .ui-pill').forEach((p) => p.remove());
    const c = container.getBoundingClientRect();
    const box = (it) => {
      const r = it.getBoundingClientRect();
      const x = r.left - c.left - container.clientLeft + container.scrollLeft;
      const y = r.top - c.top - container.clientTop + container.scrollTop;
      return axis === 'y'
        ? { left: x, top: y + (r.height - 16) / 2, width: 3, height: 16 }
        : { left: x + (r.width - 16) / 2, top: y + r.height - 3, width: 16, height: 3 };
    };
    const a = box(from), b = box(to);
    const [pos, len] = axis === 'y' ? ['top', 'height'] : ['left', 'width'];
    const lo = Math.min(a[pos], b[pos]), hi = Math.max(a[pos] + a[len], b[pos] + b[len]);
    const pill = h('span', 'ui-pill');
    Object.assign(pill.style, axis === 'y' ? { left: `${a.left}px`, width: '3px' } : { top: `${a.top}px`, height: '3px' });
    container.append(pill);
    container.classList.add('pill-moving');
    const frame = (p, l, easing) => ({ [pos]: `${p}px`, [len]: `${l}px`, ...(easing ? { easing } : {}) });
    Shell.animate(pill, [frame(a[pos], a[len], Shell.ease.standard), frame(lo, hi - lo, Shell.ease.standard), frame(b[pos], b[len])], { duration: PILL_MS, easing: 'linear' })
      .then(() => {
        pill.remove();
        if (!container.querySelector(':scope > .ui-pill')) container.classList.remove('pill-moving');
      });
  }

  /* ---------- NavigationView list ---------- */
  /* Shell.ui.navList(container, {onSelect(item, index)}) -> {select(itemOrIndex, {animate, silent}), selected()}.
     onSelect runs on every invoke (Settings returns to a page's top when its item is clicked again). */
  ui.navList = (container, { onSelect } = {}) => {
    container.classList.add('nav-list');
    const items = () => [...container.querySelectorAll('.nav-item')];
    const usable = (it) => !it.disabled && it.getAttribute('aria-disabled') !== 'true';
    const selected = () => container.querySelector('.nav-item.selected');
    const select = (target, { animate = true, silent = false } = {}) => {
      const item = typeof target === 'number' ? items()[target] : target;
      if (!item) return;
      const prev = selected();
      if (prev !== item) {
        prev?.classList.remove('selected');
        prev?.removeAttribute('aria-current');
        item.classList.add('selected');
        item.setAttribute('aria-current', 'page');
        if (animate && prev) slidePill(container, prev, item, 'y');
      }
      if (!silent) onSelect?.(item, items().indexOf(item));
    };
    container.addEventListener('click', (e) => {
      const it = e.target.closest('.nav-item');
      if (it && container.contains(it) && usable(it)) select(it);
    });
    container.addEventListener('keydown', (e) => {
      const it = e.target.closest?.('.nav-item');
      if (!it || !container.contains(it)) return;
      if ((e.key === 'Enter' || e.key === ' ') && it.tagName !== 'BUTTON' && it.tagName !== 'A') { e.preventDefault(); select(it); return; }
      if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(e.key)) return;
      const list = items().filter((i) => usable(i) && i.offsetParent !== null);
      const i = list.indexOf(it);
      const n = e.key === 'Home' ? 0 : e.key === 'End' ? list.length - 1 : clamp(i + (e.key === 'ArrowDown' ? 1 : -1), 0, list.length - 1);
      e.preventDefault();
      list[n]?.focus();
    });
    return { select, selected };
  };

  /* ---------- SelectorBar (.segmented) ---------- */
  /* Any .segmented container: clicking a child selects it, slides the pill and dispatches a
     `select` event {item, index} on the container. */
  const segPrev = new WeakMap();
  const segItems = (bar) => [...bar.children].filter((c) => !c.classList.contains('ui-pill'));
  document.addEventListener('pointerdown', (e) => {
    const b = e.target.closest?.('.segmented > :not(.ui-pill)');
    if (b) segPrev.set(b.parentElement, b.parentElement.querySelector(':scope > .selected'));
  }, true);
  document.addEventListener('click', (e) => {
    const b = e.target.closest?.('.segmented > :not(.ui-pill)');
    if (!b || b.disabled || b.getAttribute('aria-disabled') === 'true') return;
    const bar = b.parentElement;
    const prev = segPrev.get(bar) || bar.querySelector(':scope > .selected');
    segItems(bar).forEach((c) => { c.classList.toggle('selected', c === b); c.setAttribute('aria-selected', String(c === b)); });
    segPrev.set(bar, b);
    if (prev && prev !== b) slidePill(bar, prev, b, 'x');
    bar.dispatchEvent(new CustomEvent('select', { detail: { item: b, index: segItems(bar).indexOf(b) } }));
  });

  /* ---------- debug surfaces: ?open=menu, ?open=menu-view, ?open=ui-controls ---------- */
  function demoMenu(kind) {
    const fileMenu = [
      { commandBar: [
        { icon: 'fluent/cut', label: 'Cut' }, { icon: 'fluent/copy', label: 'Copy' }, { icon: 'fluent/rename', label: 'Rename' },
        { icon: 'fluent/share', label: 'Share', disabled: true }, { icon: 'fluent/delete', label: 'Delete' },
      ] },
      { label: 'Open', shortcut: 'Enter' },
      { label: 'Open with', icon: 'fluent/apps', submenu: [
        { label: 'Notepad', icon: 'object/app-notepad' }, { label: 'Terminal', icon: 'object/app-terminal' },
        { separator: true }, { label: 'Choose another app' },
      ] },
      { separator: true },
      { label: 'Pin to Start', icon: 'fluent/pin' },
      { label: 'Copy as path', icon: 'fluent/document', shortcut: 'Ctrl+Shift+C' },
      { label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter' },
      { separator: true },
      { label: 'Show more options', icon: 'fluent/open', shortcut: 'Shift+F10' },
    ];
    const deskMenu = [
      { label: 'View', icon: 'fluent/grid', submenu: [
        { label: 'Large icons', radio: true, checked: false, shortcut: 'Ctrl+Shift+2' },
        { label: 'Medium icons', radio: true, checked: true, shortcut: 'Ctrl+Shift+3' },
        { label: 'Small icons', radio: true, checked: false, shortcut: 'Ctrl+Shift+4' },
        { separator: true },
        { label: 'Auto arrange icons', checked: false }, { label: 'Align icons to grid', checked: true },
        { separator: true }, { label: 'Show desktop icons', checked: true },
      ] },
      { label: 'Sort by', icon: 'fluent/arrow-sort', submenu: [{ label: 'Name' }, { label: 'Size' }, { label: 'Item type' }, { label: 'Date modified' }] },
      { label: 'Refresh', icon: 'fluent/arrow-clockwise' },
      { separator: true },
      { label: 'Paste', icon: 'fluent/clipboard-paste', disabled: true },
      { label: 'New', icon: 'fluent/add', submenu: [{ label: 'Folder', icon: 'object/folder' }] },
      { separator: true },
      { label: 'Display settings', icon: 'fluent/desktop' }, { label: 'Personalize', icon: 'fluent/paint-brush' },
      { separator: true },
      { label: 'Show more options', icon: 'fluent/open', shortcut: 'Shift+F10' },
    ];
    Shell.menu.show({ x: Math.round(innerWidth * 0.3), y: Math.round(innerHeight * 0.18), items: kind === 'menu' ? fileMenu : deskMenu });
    const s = session;
    const row = s?.levels[0].el.querySelectorAll('.menu-list > .menu-item')[kind === 'menu' ? 1 : 0];
    if (row) setTimeout(() => openSubmenu(s, 0, row, false), 300);
  }

  function controlsGallery() {
    const root = h('div', 'ui-gallery');
    const toggle = (checked, disabled) => `<label class="toggle"><input type="checkbox" role="switch"${checked ? ' checked' : ''}${disabled ? ' disabled' : ''}><span class="toggle-track"><span class="toggle-thumb"></span></span><span class="toggle-text"></span></label>`;
    root.innerHTML = `
      <section><h3>Button</h3><div class="ui-row">
        <button class="btn">Standard</button><button class="btn accent">Accent</button><button class="btn subtle">Subtle</button>
        <button class="btn icon-only" data-tooltip="Settings" data-icon="fluent/settings" aria-label="Settings"></button>
        <button class="btn subtle icon-only" data-tooltip="More options" data-icon="fluent/more-horizontal" aria-label="More options"></button>
        <button class="btn" disabled>Disabled</button><button class="btn accent" disabled>Disabled</button>
        <button class="hyperlink" type="button">Hyperlink button</button></div></section>
      <section><h3>Toggle switch</h3><div class="ui-row">${toggle(false)}${toggle(true)}${toggle(false, true)}${toggle(true, true)}</div></section>
      <section><h3>Slider</h3><div class="ui-row"><input type="range" class="slider" value="62" aria-label="Volume"><input type="range" class="slider" value="30" disabled aria-label="Disabled"></div></section>
      <section><h3>Check box and radio button</h3><div class="ui-row">
        <label><input type="checkbox" class="checkbox">Unchecked</label><label><input type="checkbox" class="checkbox" checked>Checked</label>
        <label><input type="checkbox" class="checkbox" data-indeterminate>Indeterminate</label><label><input type="checkbox" class="checkbox" checked disabled>Disabled</label>
      </div><div class="ui-row">
        <label><input type="radio" class="radio" name="ui-r">Light</label><label><input type="radio" class="radio" name="ui-r" checked>Dark</label>
        <label><input type="radio" class="radio" name="ui-r2" disabled>Disabled</label><label><input type="radio" class="radio" name="ui-r2" checked disabled>Disabled</label>
      </div></section>
      <section><h3>Text box</h3><div class="ui-row">
        <input class="textbox" placeholder="Name" aria-label="Name"><input class="textbox searchbox" placeholder="Find a setting" aria-label="Find a setting">
        <input class="textbox" value="Disabled" disabled aria-label="Disabled"></div></section>
      <section><h3>Combo box</h3><div class="ui-row"><button class="ui-combo-a"></button><button class="ui-combo-b"></button></div></section>
      <section><h3>Expander</h3>
        <div class="expander open"><div class="expander-header"><span>Night light settings</span></div><div class="expander-content">Strength and schedule go here.</div></div>
        <div class="expander"><div class="expander-header"><span>Advanced display</span></div><div class="expander-content">Display information, refresh rate.</div></div></section>
      <section><h3>Navigation view</h3><nav class="ui-nav">
        <button class="nav-item selected" data-icon="fluent/desktop">System</button><button class="nav-item" data-icon="fluent/bluetooth">Bluetooth &amp; devices</button>
        <button class="nav-item" data-icon="fluent/wifi">Network &amp; internet</button><button class="nav-item" data-icon="fluent/paint-brush">Personalization</button>
        <button class="nav-item" data-icon="fluent/apps">Apps</button></nav></section>
      <section><h3>Progress</h3><div class="ui-row"><div class="progress-ring"></div><div class="progress-ring determinate" style="--value: 70"></div>
        <div class="progress-bar indeterminate"></div><progress class="progress-bar"></progress><div class="progress-bar" style="--value: 40"></div></div></section>
      <section><h3>Selector bar and badge</h3><div class="ui-row"><div class="segmented"><button class="selected">All</button><button>Apps</button><button>Documents</button><button>Settings</button><button>Folders</button></div>
        <span class="badge">3</span><span class="badge"></span><span class="badge critical">12</span></div></section>
      <section><h3>Card and menu</h3><div class="card-group"><div class="card">Card with the default background and stroke</div><button class="card">Clickable card</button></div>
        <div class="ui-row"><button class="btn ui-open-menu">Show a context menu</button></div></section>`;
    root.querySelectorAll('[data-icon]').forEach((n) => n.prepend(Shell.icon(n.dataset.icon, { size: 16 })));
    root.querySelectorAll('[data-indeterminate]').forEach((n) => { n.indeterminate = true; });
    root.querySelectorAll('input.slider').forEach(ui.slider);
    root.querySelectorAll('input.textbox').forEach((n) => ui.textbox(n, { clear: true }));
    root.querySelectorAll('.expander').forEach((n) => ui.expander(n));
    ui.navList(root.querySelector('.ui-nav'));
    ui.combo(root.querySelector('.ui-combo-a'), { options: ['Light', 'Dark', 'Custom'].map((l) => ({ value: l.toLowerCase(), label: l })), value: 'dark' });
    ui.combo(root.querySelector('.ui-combo-b'), { options: ['100%', '125% (Recommended)', '150%', '175%'].map((l) => ({ value: l, label: l })), value: '125% (Recommended)' });
    root.querySelector('.ui-open-menu').addEventListener('click', (e) => {
      const r = e.currentTarget.getBoundingClientRect();
      Shell.menu.show({ anchorRect: r, items: [{ label: 'View', icon: 'fluent/grid', submenu: [{ label: 'Medium icons', radio: true, checked: true }, { label: 'Show desktop icons', checked: true }] }, { label: 'Refresh', icon: 'fluent/arrow-clockwise' }] });
    });
    let win = null;
    try { win = Shell.wm?.open?.({ appId: 'ui-gallery', title: 'Controls', icon: 'object/app-settings', width: 760, height: 680, content: root }); } catch { win = null; }
    if (!win) { root.classList.add('ui-gallery-panel', 'mica', 'surface-flyout'); layer('overlays').append(root); }
  }

  Shell.on?.('boot', () => setTimeout(() => {
    const want = Shell.debugOpen || [];
    if (want.includes('ui-controls')) controlsGallery();
    if (want.includes('menu')) demoMenu('menu');
    else if (want.includes('menu-view')) demoMenu('menu-view');
  }, 0));
})();
