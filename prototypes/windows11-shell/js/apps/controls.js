/* Windows controls: the Win32 common controls as Windows 11 (22H2/23H2) draws them in a classic,
   non-WinUI program. The gallery is the visual target of the Wine msstyles theme
   (themes/windows11/source): every section names the msstyles class/part it shows and the
   theme.ini section that draws it, and says what that theme does today where it differs.
   Owns: the app `controls` ("Windows controls", All apps only) and its window; the specimens
   (push, split and command-link buttons, check boxes, radio buttons, group box, edits, combo
   boxes, list box, up-down, list views with header, tree view, trackbar, progress bars, classic
   and overlay scroll bars, tab control, toolbar and rebar, menu bar with popup menus, status bar,
   tooltips, date picker, month calendar) and their behaviour; the classic popups they open
   (menu, drop-down list, calendar, tooltip) in the menus and tooltips layers; the Light/Dark
   switch, which shows what Windows does for classic programs in dark mode (only the controls
   that opt into the dark Explorer themes follow it); the sample "game launcher" dialog, inline
   and as its own window.
   Colours: css/apps/controls.css (the classic aero palette, not WinUI resources: see its header).
   Debug captures: ?open=controls, controls-dark, controls-<page> where page is buttons, text,
   lists, ranges, bars, dates or dialog. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const SHOW_DELAY = 400; // SPI_GETMENUSHOWDELAY: hover time before a submenu opens
  const TIP_DELAY = 500, TIP_RESHOW = 100; // TTDT_INITIAL, TTDT_RESHOW

  /* ---------- helpers ---------- */
  function h(tag, cls, props, kids) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    for (const [k, v] of Object.entries(props || {})) {
      if (v == null || v === false) continue;
      if (k === 'text') el.textContent = v;
      else el.setAttribute(k, v === true ? '' : String(v));
    }
    if (kids) el.append(...kids.filter(Boolean));
    return el;
  }
  const icon = (name, size = 16) => Shell.icon(name, { size });
  const tri = (dir) => h('span', `w32-tri ${dir}`, { 'aria-hidden': 'true' });
  const chev = () => h('span', 'w32-chev', { 'aria-hidden': 'true' }, [icon('fluent/chevron-down', 12)]);
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const appear = (el) => Shell.animate(el, [{ opacity: 0 }, { opacity: 1 }], { duration: 167, easing: 'linear' });
  const sameDay = (a, b) => a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
  /* lists, trees and calendars draw their focus rectangle only once a key has been used */
  const kbdCues = (el) => {
    el.addEventListener('keydown', () => el.classList.add('kbd'));
    el.addEventListener('pointerdown', () => el.classList.remove('kbd'));
  };
  /* keeps `item` inside the visible part of the scrolling `box` (below a sticky header of `top` px) */
  function ensureVisible(box, item, top = 0) {
    if (!item) return;
    const y = item.offsetTop, b = y + item.offsetHeight;
    if (y - top < box.scrollTop) box.scrollTop = y - top;
    else if (b > box.scrollTop + box.clientHeight) box.scrollTop = b - box.clientHeight;
  }
  /* press and hold repeats, as scroll arrows and up-down buttons do: once, then every 50 ms after 400 ms */
  function autoRepeat(btn, fn) {
    btn.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || btn.disabled) return;
      e.preventDefault();
      fn();
      let t = setTimeout(function tick() { fn(); t = setTimeout(tick, 50); }, 400);
      const stop = () => {
        clearTimeout(t);
        window.removeEventListener('pointerup', stop, true);
        window.removeEventListener('pointercancel', stop, true);
        btn.removeEventListener('pointerleave', stop);
      };
      window.addEventListener('pointerup', stop, true);
      window.addEventListener('pointercancel', stop, true);
      btn.addEventListener('pointerleave', stop);
    });
  }

  let G = null; // the open gallery: {win, root, dark, followSystem, show, cleanups}
  const galleryDark = () => !!G?.dark;

  /* Registers a classic popup with Shell.transient: one menu-group surface at a time, closed by a
     press outside it. The returned dismiss() drops the registration without calling closeFn. */
  let seq = 0;
  function lightDismiss(contains, closeFn) {
    const id = `controls-popup-${++seq}`;
    let done = false;
    Shell.transient.open(id, { group: 'menu', elements: [(e) => contains(e.target)], close: () => { if (!done) { done = true; closeFn(); } } });
    return () => { if (!done) { done = true; Shell.transient.close(id); } };
  }

  /* ---------- classic popup menu (MENU / MENU_POPUP*) ---------- */
  /* items: {label, kbd?, checked?, radio?, disabled?, separator?, submenu?, action?}. Drawn in
     `host` (inline, positioned inside it) or floating in the menus layer. Keyboard: arrows,
     Enter/Space, Esc, Right/Left for submenus (or onNavigate(+1/-1) for a menu bar), first letter. */
  function classicMenu(items, { host = null, anchor, dark = false, keyboard = false, onNavigate, onClose }) {
    const levels = [];
    const layer = host || Shell.layer('menus');
    let timer = 0, closed = false;

    function build(list, depth) {
      const L = { el: h('div', `w32 w32-menu${host ? '' : ' floating'}${dark ? ' w32-dark' : ''}`, { role: 'menu' }), rows: [], hot: -1, depth, parent: -1 };
      for (const it of list) {
        if (it.separator) { L.el.append(h('div', 'w32-msep', { role: 'separator' })); continue; }
        const role = it.radio ? 'menuitemradio' : 'checked' in it ? 'menuitemcheckbox' : 'menuitem';
        const row = h('div', `w32-mi${it.disabled ? ' disabled' : ''}`, {
          role, 'aria-disabled': it.disabled ? 'true' : null, 'aria-haspopup': it.submenu ? 'menu' : null,
          'aria-checked': role === 'menuitem' ? null : String(!!it.checked),
        }, [h('span', `chk${it.checked ? (it.radio ? ' dot' : ' on') : ''}`), h('span', null, { text: it.label }),
          h('span', 'kbd', { text: it.kbd || '' }), h('span', 'sub', null, it.submenu ? [tri('right')] : [])]);
        const i = L.rows.length;
        L.rows.push({ row, it });
        row.addEventListener('pointerenter', () => hover(L, i));
        row.addEventListener('click', () => activate(L, i, false));
        L.el.append(row);
      }
      L.el.addEventListener('mousedown', (e) => e.preventDefault()); // focus stays where it was, as with Win32 menus
      L.el.addEventListener('contextmenu', (e) => e.preventDefault());
      return L;
    }
    const setHot = (L, i) => { L.hot = i; L.rows.forEach((r, k) => r.row.classList.toggle('hot', k === i)); };
    const trim = (depth) => { while (levels.length > depth) levels.pop().el.remove(); };
    function hover(L, i) {
      setHot(L, i);
      clearTimeout(timer);
      if (levels[L.depth + 1]?.parent === i) return;
      const { it } = L.rows[i];
      timer = setTimeout(() => { trim(L.depth + 1); if (it.submenu && !it.disabled) openSub(L, i, false); }, SHOW_DELAY);
    }
    function openSub(L, i, kbd) {
      trim(L.depth + 1);
      const sub = build(L.rows[i].it.submenu, L.depth + 1);
      sub.parent = i;
      levels.push(sub);
      layer.append(sub.el);
      const row = L.rows[i].row;
      if (host) {
        let x = L.el.offsetLeft + L.el.offsetWidth - 3;
        if (x + sub.el.offsetWidth > host.clientWidth) x = Math.max(0, L.el.offsetLeft - sub.el.offsetWidth + 3);
        sub.el.style.left = `${x}px`;
        sub.el.style.top = `${L.el.offsetTop + row.offsetTop - 4}px`;
      } else {
        const m = L.el.getBoundingClientRect(), r = row.getBoundingClientRect();
        Shell.ui.position(sub.el, { left: m.left + 3, right: m.right - 3, top: r.top - 4, bottom: r.bottom }, 'right-start', { gap: 0, margin: 4 });
      }
      appear(sub.el);
      if (kbd) setHot(sub, 0);
    }
    function activate(L, i, kbd) {
      const { it } = L.rows[i];
      if (it.disabled) return;
      if (it.submenu) { clearTimeout(timer); if (levels[L.depth + 1]?.parent !== i) openSub(L, i, kbd); return; }
      close(false);
      it.action?.();
    }
    const step = (L, dir) => (L.hot < 0 ? (dir > 0 ? 0 : L.rows.length - 1) : (L.hot + dir + L.rows.length) % L.rows.length);
    function onKey(e) {
      const w = host?.closest('.window'), a = document.activeElement;
      if (w && a && a !== document.body && !w.contains(a)) return; // an inline menu answers keys only in its own window
      const L = levels[levels.length - 1], k = e.key;
      if (k === 'ArrowDown' || k === 'ArrowUp') setHot(L, step(L, k === 'ArrowDown' ? 1 : -1));
      else if (k === 'ArrowRight') {
        const r = L.rows[L.hot];
        if (r?.it.submenu && !r.it.disabled) openSub(L, L.hot, true); else onNavigate?.(1);
      } else if (k === 'ArrowLeft') { if (L.depth > 0) trim(L.depth); else onNavigate?.(-1); }
      else if (k === 'Enter' || k === ' ') { if (L.hot >= 0) activate(L, L.hot, true); }
      else if (k === 'Escape') { if (L.depth > 0) trim(L.depth); else close(true); }
      else if (k === 'Tab' || k === 'Alt' || k === 'F10') close(true);
      else if (k.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
        const hits = L.rows.map((r, i) => i).filter((i) => L.rows[i].it.label.toLowerCase().startsWith(k.toLowerCase()));
        if (hits.length === 1) activate(L, hits[0], true);
        else if (hits.length) setHot(L, hits.find((i) => i > L.hot) ?? hits[0]);
      } else return;
      e.preventDefault();
      e.stopPropagation();
    }
    function close(byKey) {
      if (closed) return;
      closed = true;
      clearTimeout(timer);
      trim(0);
      window.removeEventListener('keydown', onKey, true);
      onClose?.({ byKey });
    }

    const root = build(items, 0);
    levels.push(root);
    layer.append(root.el);
    if (host) {
      const hr = host.getBoundingClientRect();
      const x = Math.round(anchor.left - hr.left - host.clientLeft);
      root.el.style.left = `${Math.max(0, Math.min(x, host.clientWidth - root.el.offsetWidth))}px`;
      root.el.style.top = `${Math.round(anchor.bottom - hr.top - host.clientTop)}px`;
    } else Shell.ui.position(root.el, anchor, 'bottom-start', { gap: 0, margin: 4 });
    appear(root.el);
    if (keyboard) setHot(root, 0);
    window.addEventListener('keydown', onKey, true);
    return { close: () => close(false), contains: (t) => levels.some((L) => L.el.contains(t)) };
  }

  /* A button that drops a floating classic menu (toolbar drop-downs, the split button's arrow). */
  function dropMenu(btn, items, part = btn) {
    let menu = null;
    const open = (keyboard) => {
      if (menu) { menu.close(); return; }
      btn.classList.add('open');
      let dismiss = null;
      menu = classicMenu(items(), {
        anchor: btn.getBoundingClientRect(), dark: galleryDark(), keyboard,
        onClose: ({ byKey }) => { btn.classList.remove('open'); menu = null; dismiss?.(); if (byKey) btn.focus(); },
      });
      const m = menu;
      dismiss = lightDismiss((t) => btn.contains(t) || m.contains(t), () => m.close());
    };
    part.addEventListener('pointerdown', (e) => { if (e.button === 0) { e.preventDefault(); e.stopPropagation(); open(false); } });
    btn.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowDown' || e.key === 'F4' || (part === btn && (e.key === 'Enter' || e.key === ' '))) { e.preventDefault(); open(true); }
    });
  }

  /* Menu bar (MENU_BARITEM): opens on press, hover switches menus while one is open, arrows walk it. */
  function menuBar(menus, host) {
    const bar = h('div', 'w32-menubar', { role: 'menubar' });
    let open = null;
    const btns = menus.map((m, i) => {
      const b = h('button', 'w32-mbitem', { type: 'button', role: 'menuitem', 'aria-haspopup': 'menu', 'aria-expanded': 'false', tabindex: i ? '-1' : '0', text: m.label });
      b.addEventListener('pointerdown', (e) => { if (e.button !== 0) return; e.preventDefault(); if (open?.i === i) open.menu.close(); else show(i, false); });
      b.addEventListener('pointerenter', () => { if (open && open.i !== i) show(i, false); });
      b.addEventListener('keydown', (e) => {
        if (e.key === 'ArrowDown' || e.key === 'Enter' || e.key === ' ') { e.preventDefault(); show(i, true); }
        else if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { e.preventDefault(); focusBtn(i + (e.key === 'ArrowRight' ? 1 : -1)); }
      });
      bar.append(b);
      return b;
    });
    function focusBtn(i) {
      const b = btns[(i + btns.length) % btns.length];
      btns.forEach((x) => { x.tabIndex = x === b ? 0 : -1; });
      b.focus();
    }
    function show(i, keyboard) {
      open?.menu.close();
      const b = btns[i], rec = { i, menu: null, dismiss: null };
      b.classList.add('open');
      b.setAttribute('aria-expanded', 'true');
      rec.menu = classicMenu(menus[i].items(), {
        host, anchor: b.getBoundingClientRect(), dark: galleryDark(), keyboard,
        onNavigate: (d) => show((i + d + btns.length) % btns.length, true),
        onClose: ({ byKey }) => {
          b.classList.remove('open');
          b.setAttribute('aria-expanded', 'false');
          rec.dismiss?.();
          if (open === rec) open = null;
          if (byKey) focusBtn(i);
        },
      });
      rec.dismiss = lightDismiss((t) => bar.contains(t) || rec.menu.contains(t), () => rec.menu.close());
      open = rec;
    }
    return { el: bar, show, isOpen: () => !!open };
  }

  /* ---------- combo boxes (COMBOBOX) ---------- */
  /* The drop-down list window (ComboLBox): the hot item follows the pointer and the arrow keys
     while focus stays in the combo box. */
  function dropList(anchor, options, index, { dark = false, inline = false, onPick }) {
    const el = h('div', `w32 w32-droplist w32-sb${inline ? ' inline' : ''}${dark ? ' w32-dark' : ''}`, { role: 'listbox' });
    let hot = -1;
    const items = options.map((o, i) => {
      const it = h('div', 'w32-dl-item', { role: 'option', text: o });
      it.addEventListener('pointerenter', () => setHot(i));
      it.addEventListener('click', () => onPick(i));
      el.append(it);
      return it;
    });
    el.addEventListener('mousedown', (e) => e.preventDefault());
    function setHot(i) {
      hot = clamp(i, 0, items.length - 1);
      items.forEach((it, k) => { it.classList.toggle('hot', k === hot); it.setAttribute('aria-selected', String(k === hot)); });
      ensureVisible(el, items[hot]);
    }
    if (!inline) {
      Shell.layer('menus').append(el);
      const r = anchor.getBoundingClientRect();
      el.style.minWidth = `${Math.round(r.width)}px`;
      Shell.ui.position(el, r, 'bottom-start', { gap: 0, margin: 4 });
    }
    setHot(index);
    return { el, setHot, get hot() { return hot; } };
  }

  /* CBS_DROPDOWN (editable) or CBS_DROPDOWNLIST. Keys as Win32: arrows change the selection
     while closed, F4 / Alt+Down open, arrows move the hot item while open, Enter takes it, Esc
     cancels; a drop-down list also selects by first letter. */
  function comboBox({ options, value = 0, editable = false, disabled = false, label, onChange }) {
    let index = value, list = null, dismiss = null;
    const text = editable
      ? h('input', null, { type: 'text', value: options[index], 'aria-label': label, spellcheck: 'false', autocomplete: 'off', disabled })
      : h('span', 'w32-ddl-text', { text: options[index] });
    const btn = editable ? h('button', 'w32-combo-btn', { type: 'button', tabindex: '-1', 'aria-label': `Open ${label}`, disabled }, [chev()]) : null;
    const el = editable
      ? h('div', 'w32-combo', { role: 'combobox', 'aria-expanded': 'false', 'aria-label': label }, [text, btn])
      : h('button', 'w32-ddl', { type: 'button', role: 'combobox', 'aria-expanded': 'false', 'aria-label': label, disabled }, [text, chev()]);
    const focusEl = editable ? text : el;
    function set(i) {
      index = clamp(i, 0, options.length - 1);
      if (editable) { text.value = options[index]; text.select(); } else text.textContent = options[index];
      onChange?.(index);
      el.dispatchEvent(new Event('change', { bubbles: true }));
    }
    function open() {
      if (list || disabled) return;
      el.classList.add('open');
      el.setAttribute('aria-expanded', 'true');
      const cur = editable ? options.findIndex((o) => o.toLowerCase() === text.value.trim().toLowerCase()) : index;
      list = dropList(el, options, Math.max(0, cur), { dark: !!el.closest('.w32-dark'), onPick: (i) => { set(i); close(); } });
      appear(list.el);
      dismiss = lightDismiss((t) => el.contains(t) || !!list?.el.contains(t), () => close());
    }
    function close() {
      if (!list) return;
      list.el.remove();
      list = null;
      el.classList.remove('open');
      el.setAttribute('aria-expanded', 'false');
      const d = dismiss; dismiss = null; d?.();
    }
    el.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || disabled || (editable && !btn.contains(e.target))) return;
      e.preventDefault();
      focusEl.focus();
      if (list) close(); else open();
    });
    focusEl.addEventListener('keydown', (e) => {
      const k = e.key, vertical = k === 'ArrowDown' || k === 'ArrowUp';
      if (k === 'F4' || (vertical && e.altKey)) { if (list) { set(list.hot); close(); } else open(); }
      else if (list && vertical) list.setHot(list.hot + (k === 'ArrowDown' ? 1 : -1));
      else if (list && k === 'Enter') { set(list.hot); close(); }
      else if (list && k === 'Escape') close();
      else if (!list && vertical) set(index + (k === 'ArrowDown' ? 1 : -1));
      else if (!list && !editable && (k === 'Home' || k === 'End')) set(k === 'Home' ? 0 : options.length - 1);
      else if (!editable && k.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
        const n = options.length, c = k.toLowerCase();
        const at = [...Array(n).keys()].map((d) => (index + 1 + d) % n).find((i) => options[i].toLowerCase().startsWith(c));
        if (at === undefined) return;
        if (list) list.setHot(at); else set(at);
      } else return;
      e.preventDefault();
      e.stopPropagation();
    });
    focusEl.addEventListener('blur', close); // Win32 closes the list when focus leaves the combo box
    return { el, close };
  }

  /* ---------- list box (LISTBOX), single selection ---------- */
  function listBox(options, sel, label) {
    const el = h('div', 'w32-listbox w32-sb', { role: 'listbox', tabindex: '0', 'aria-label': label });
    kbdCues(el);
    const items = options.map((o, i) => {
      const it = h('div', 'w32-lb-item', { role: 'option', text: o });
      it.addEventListener('pointerdown', () => set(i));
      el.append(it);
      return it;
    });
    function set(i) {
      sel = clamp(i, 0, items.length - 1);
      items.forEach((it, k) => { it.classList.toggle('sel', k === sel); it.classList.toggle('focus', k === sel); it.setAttribute('aria-selected', String(k === sel)); });
      ensureVisible(el, items[sel]);
    }
    el.addEventListener('keydown', (e) => {
      const page = Math.max(1, Math.floor(el.clientHeight / 17) - 1);
      let to = { ArrowDown: sel + 1, ArrowUp: sel - 1, Home: 0, End: items.length - 1, PageDown: sel + page, PageUp: sel - page }[e.key];
      if (to === undefined && e.key.length === 1 && !e.ctrlKey && !e.altKey) {
        to = [...items.keys()].map((d) => (sel + 1 + d) % items.length).find((i) => options[i].toLowerCase().startsWith(e.key.toLowerCase()));
      }
      if (to === undefined) return;
      e.preventDefault();
      set(to);
    });
    set(sel);
    return el;
  }

  /* ---------- up-down (SPIN) with its buddy edit ---------- */
  function upDown({ value, min, max, label }) {
    const input = h('input', null, { type: 'text', value: String(value), inputmode: 'numeric', role: 'spinbutton', 'aria-label': label, 'aria-valuemin': min, 'aria-valuemax': max, 'aria-valuenow': value, spellcheck: 'false' });
    const up = h('button', null, { type: 'button', tabindex: '-1', 'aria-label': 'Increase' }, [tri('up')]);
    const down = h('button', null, { type: 'button', tabindex: '-1', 'aria-label': 'Decrease' }, [tri('down')]);
    const set = (v) => {
      value = clamp(v, min, max);
      input.value = String(value);
      input.setAttribute('aria-valuenow', value);
      input.dispatchEvent(new Event('input', { bubbles: true }));
    };
    autoRepeat(up, () => { input.focus(); set(value + 1); });
    autoRepeat(down, () => { input.focus(); set(value - 1); });
    input.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
      e.preventDefault();
      set(value + (e.key === 'ArrowUp' ? 1 : -1));
    });
    input.addEventListener('change', () => { const n = parseInt(input.value, 10); set(Number.isFinite(n) ? n : value); });
    return h('div', 'w32-updown', null, [input, h('div', 'w32-spin', null, [up, down])]);
  }

  /* ---------- trackbar (TRACKBAR): native range drawn as the bottom-pointing thumb with tics ---------- */
  function trackbar({ min = 0, max = 10, value = 5, tick = 1, disabled = false, label, onInput }) {
    const input = h('input', null, { type: 'range', min, max, step: 1, value, disabled, 'aria-label': label });
    const el = h('div', 'w32-trackbar', null, [input, h('div', 'w32-tics', { 'aria-hidden': 'true' })]);
    el.style.setProperty('--steps', String((max - min) / tick));
    if (onInput) input.addEventListener('input', () => onInput(Number(input.value)));
    return el;
  }

  const progress = (state, value, extra = '') => h('div', `w32-progress ${state} ${extra}`.trim(), {
    role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': state === 'marquee' ? null : value, style: `--value: ${value}`,
  }, [h('i')]);

  /* ---------- push buttons, check boxes, radio buttons ---------- */
  const button = (label, { cls = '', disabled = false, def = false } = {}) =>
    h('button', `w32-btn${def ? ' default' : ''} ${cls}`.trim(), { type: 'button', disabled, text: label });
  function check(label, { type = 'checkbox', checked = false, mixed = false, disabled = false, name, cls = '' } = {}) {
    const input = h('input', null, { type, name, checked, disabled });
    if (mixed) input.indeterminate = true;
    return h('label', `w32-check${type === 'radio' ? ' radio' : ''} ${cls}`.trim(), null, [input, h('span', 'w32-box', { 'aria-hidden': 'true' }), label ? h('span', 'w32-label', { text: label }) : null]);
  }
  /* BS_AUTO3STATE: unchecked -> checked -> indeterminate */
  function threeState(label) {
    const el = check(label);
    const input = el.querySelector('input');
    let state = 0;
    input.addEventListener('change', () => { state = (state + 1) % 3; input.checked = state === 1; input.indeterminate = state === 2; });
    return el;
  }
  /* a row of forced-state specimens (inert): [caption, element] */
  const states = (list) => h('div', 'w32-states', { inert: true }, list.map(([cap, el]) => h('figure', null, null, [el, h('figcaption', null, { text: cap })])));

  /* ---------- classic tooltip (TOOLTIP / TTP_STANDARD), live on toolbar buttons ---------- */
  let tipNode = null, tipTimer = 0, tipHidden = -1e9;
  function hideTip() {
    clearTimeout(tipTimer);
    if (tipNode?.isConnected) { tipNode.remove(); tipHidden = performance.now(); }
  }
  function classicTip(el, text) {
    let x = 0, y = 0;
    el.addEventListener('pointermove', (e) => { x = e.clientX; y = e.clientY; });
    el.addEventListener('pointerenter', (e) => {
      x = e.clientX; y = e.clientY;
      clearTimeout(tipTimer);
      tipTimer = setTimeout(() => {
        if (!el.isConnected) return;
        tipNode = tipNode || h('div', 'w32 w32-tip', { role: 'tooltip' });
        tipNode.textContent = text;
        tipNode.classList.toggle('w32-dark', galleryDark());
        Shell.layer('tooltips').append(tipNode);
        Shell.ui.position(tipNode, { left: x, right: x, top: y, bottom: y + 20 }, 'bottom-start', { gap: 0, margin: 4 }); // below the arrow cursor
        appear(tipNode);
      }, performance.now() - tipHidden < 400 ? TIP_RESHOW : TIP_DELAY);
    });
    el.addEventListener('pointerleave', hideTip);
    el.addEventListener('pointerdown', hideTip);
  }

  /* ---------- Windows 11 overlay scroll bars over a scrolling view ---------- */
  function thinScroller(content, label) {
    const view = h('div', 'w32-thin-view', { tabindex: '0', 'aria-label': label }, content);
    const bars = ['v', 'h'].map((axis) => {
      const thumb = h('div', 'w32-thin-thumb');
      const track = h('div', 'w32-thin-track', null, [thumb]);
      const dec = h('button', 'w32-thin-arrow', { type: 'button', tabindex: '-1', 'aria-hidden': 'true' }, [tri(axis === 'v' ? 'up' : 'left')]);
      const inc = h('button', 'w32-thin-arrow', { type: 'button', tabindex: '-1', 'aria-hidden': 'true' }, [tri(axis === 'v' ? 'down' : 'right')]);
      const bar = h('div', `w32-thin-bar ${axis}`, { 'aria-hidden': 'true' }, [dec, track, inc]);
      const v = axis === 'v';
      const by = (d) => view.scrollBy({ [v ? 'top' : 'left']: d });
      autoRepeat(dec, () => by(-16));
      autoRepeat(inc, () => by(16));
      track.addEventListener('pointerdown', (e) => {
        if (e.target !== track || e.button !== 0) return;
        const r = thumb.getBoundingClientRect();
        by(((v ? e.clientY < r.top : e.clientX < r.left) ? -1 : 1) * (v ? view.clientHeight : view.clientWidth));
      });
      thumb.addEventListener('pointerdown', (e) => {
        if (e.button !== 0) return;
        e.preventDefault();
        thumb.setPointerCapture(e.pointerId);
        bar.classList.add('dragging');
        const start = v ? e.clientY : e.clientX, from = v ? view.scrollTop : view.scrollLeft;
        const len = v ? track.clientHeight : track.clientWidth, t = v ? thumb.offsetHeight : thumb.offsetWidth;
        const range = (v ? view.scrollHeight - view.clientHeight : view.scrollWidth - view.clientWidth) / Math.max(1, len - t);
        const move = (ev) => { view[v ? 'scrollTop' : 'scrollLeft'] = from + ((v ? ev.clientY : ev.clientX) - start) * range; };
        const up = () => { bar.classList.remove('dragging'); thumb.removeEventListener('pointermove', move); thumb.removeEventListener('pointerup', up); thumb.removeEventListener('pointercancel', up); };
        thumb.addEventListener('pointermove', move);
        thumb.addEventListener('pointerup', up);
        thumb.addEventListener('pointercancel', up);
      });
      return { v, bar, track, thumb };
    });
    const el = h('div', 'w32-thin', null, [view, ...bars.map((b) => b.bar)]);
    function update() {
      for (const b of bars) {
        const size = b.v ? view.clientHeight : view.clientWidth, total = b.v ? view.scrollHeight : view.scrollWidth;
        b.bar.hidden = total <= size + 1;
        const len = b.v ? b.track.clientHeight : b.track.clientWidth;
        if (b.bar.hidden || !len) continue;
        const t = Math.min(len, Math.max(30, (len * size) / total)); // ScrollBarVerticalThumbMinHeight
        const off = (len - t) * ((b.v ? view.scrollTop : view.scrollLeft) / (total - size));
        b.thumb.style[b.v ? 'height' : 'width'] = `${t}px`;
        b.thumb.style[b.v ? 'top' : 'left'] = `${off}px`;
      }
    }
    let idle = 0;
    const wake = () => { el.classList.add('active'); clearTimeout(idle); idle = setTimeout(() => el.classList.remove('active'), 50); };
    view.addEventListener('scroll', () => { update(); wake(); }, { passive: true });
    el.addEventListener('pointermove', wake);
    const ro = new ResizeObserver(update);
    ro.observe(view);
    G?.cleanups.push(() => { ro.disconnect(); clearTimeout(idle); });
    return el;
  }

  /* ---------- month calendar (MONTHCAL) and date picker (DATEPICKER) ---------- */
  const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  /* onSelect(date, final): final on click, Enter or Today; arrows move the selection only */
  function monthCal({ value, floating = false, onSelect, onCancel }) {
    let sel = new Date(value.getFullYear(), value.getMonth(), value.getDate());
    let view = new Date(sel.getFullYear(), sel.getMonth(), 1), mode = 'month';
    const today = Shell.now();
    const prev = h('button', null, { type: 'button', tabindex: '-1', 'aria-label': 'Previous' }, [tri('left')]);
    const title = h('button', null, { type: 'button', tabindex: '-1' });
    const next = h('button', null, { type: 'button', tabindex: '-1', 'aria-label': 'Next' }, [tri('right')]);
    const grid = h('div', 'w32-mc-grid', { role: 'grid' });
    const foot = h('button', 'w32-mc-foot', { type: 'button', tabindex: '-1' });
    const el = h('div', `w32 w32-mc${floating ? ' floating' : ''}`, { tabindex: '0', role: 'group', 'aria-label': 'Calendar' }, [h('div', 'w32-mc-head', null, [prev, title, next]), grid, foot]);
    kbdCues(el);
    el.addEventListener('mousedown', (e) => { if (e.target !== el) { e.preventDefault(); el.focus(); } });
    function render() {
      if (mode === 'month') {
        title.textContent = view.toLocaleDateString('en-US', { month: 'long', year: 'numeric' });
        grid.className = 'w32-mc-grid';
        const cells = ['Su', 'Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa'].map((d) => h('div', 'w32-mc-dow', { text: d }));
        const start = new Date(view.getFullYear(), view.getMonth(), 1 - view.getDay());
        for (let i = 0; i < 42; i++) {
          const d = new Date(start.getFullYear(), start.getMonth(), start.getDate() + i);
          const b = h('button', `w32-mc-day${d.getMonth() !== view.getMonth() ? ' trail' : ''}${sameDay(d, sel) ? ' sel' : ''}${sameDay(d, today) ? ' today' : ''}`,
            { type: 'button', tabindex: '-1', 'aria-label': d.toDateString(), text: d.getDate() });
          b.addEventListener('click', () => pick(d, true));
          cells.push(b);
        }
        grid.replaceChildren(...cells);
      } else {
        title.textContent = String(view.getFullYear());
        grid.className = 'w32-mc-grid months';
        grid.replaceChildren(...MONTHS.map((m, i) => {
          const here = view.getFullYear();
          const b = h('button', `w32-mc-day${here === sel.getFullYear() && i === sel.getMonth() ? ' sel' : ''}${here === today.getFullYear() && i === today.getMonth() ? ' today' : ''}`, { type: 'button', tabindex: '-1', text: m });
          b.addEventListener('click', () => { view = new Date(here, i, 1); mode = 'month'; render(); });
          return b;
        }));
      }
      foot.textContent = `Today: ${today.toLocaleDateString('en-US')}`;
    }
    function pick(d, final) {
      sel = new Date(d.getFullYear(), d.getMonth(), d.getDate());
      view = new Date(sel.getFullYear(), sel.getMonth(), 1);
      mode = 'month';
      render();
      onSelect?.(sel, final);
    }
    const shift = (n) => { view = mode === 'month' ? new Date(view.getFullYear(), view.getMonth() + n, 1) : new Date(view.getFullYear() + n, 0, 1); render(); };
    prev.addEventListener('click', () => shift(-1));
    next.addEventListener('click', () => shift(1));
    title.addEventListener('click', () => { if (mode === 'month') { mode = 'year'; render(); } });
    foot.addEventListener('click', () => pick(today, true));
    el.addEventListener('keydown', (e) => {
      const days = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -7, ArrowDown: 7 }[e.key];
      if (days && mode === 'month') pick(new Date(sel.getFullYear(), sel.getMonth(), sel.getDate() + days), false);
      else if (e.key === 'PageUp' || e.key === 'PageDown') pick(new Date(sel.getFullYear(), sel.getMonth() + (e.key === 'PageDown' ? 1 : -1), sel.getDate()), false);
      else if (e.key === 'Enter') onSelect?.(sel, true);
      else if (e.key === 'Escape' && onCancel) onCancel();
      else return;
      e.preventDefault();
      e.stopPropagation();
    });
    render();
    return { el };
  }

  function datePicker({ value, long = false, disabled = false, label }) {
    let date = value, cal = null, dismiss = null;
    const fmt = (d) => (long ? d.toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric', year: 'numeric' }) : d.toLocaleDateString('en-US'));
    const text = h('span', null, { text: fmt(date) });
    const el = h('button', 'w32-dtp', { type: 'button', 'aria-label': label, 'aria-haspopup': 'dialog', 'aria-expanded': 'false', disabled }, [text, chev()]);
    function close(refocus) {
      if (!cal) return;
      cal.el.remove();
      cal = null;
      el.classList.remove('open');
      el.setAttribute('aria-expanded', 'false');
      const d = dismiss; dismiss = null; d?.();
      if (refocus) el.focus();
    }
    function open() {
      if (cal) return;
      el.classList.add('open');
      el.setAttribute('aria-expanded', 'true');
      cal = monthCal({
        value: date, floating: true,
        onSelect: (d, final) => { date = d; text.textContent = fmt(d); if (final) close(true); },
        onCancel: () => close(true),
      });
      Shell.layer('menus').append(cal.el);
      Shell.ui.position(cal.el, el.getBoundingClientRect(), 'bottom-start', { gap: 0, margin: 4 });
      appear(cal.el);
      cal.el.focus();
      const c = cal;
      dismiss = lightDismiss((t) => el.contains(t) || c.el.contains(t), () => close(false));
    }
    el.addEventListener('pointerdown', (e) => { if (e.button !== 0 || el.disabled) return; e.preventDefault(); el.focus(); if (cal) close(false); else open(); });
    el.addEventListener('keydown', (e) => { if (e.key === 'F4' || (e.altKey && e.key === 'ArrowDown')) { e.preventDefault(); open(); } });
    return el;
  }

  /* ---------- tab control (TAB) ---------- */
  function tabControl(tabs, sel = 0) {
    const list = h('div', 'w32-tablist', { role: 'tablist' });
    const pane = h('div', 'w32-tabpane', { role: 'tabpanel' });
    const bodies = tabs.map((t) => t.body());
    pane.append(...bodies);
    const btns = tabs.map((t, i) => {
      const b = h('button', 'w32-tab', { type: 'button', role: 'tab', tabindex: '-1' }, [h('span', null, { text: t.label })]);
      b.addEventListener('pointerdown', (e) => { if (e.button === 0) { e.preventDefault(); select(i, true); } });
      list.append(b);
      return b;
    });
    function select(i, focus) {
      sel = (i + btns.length) % btns.length;
      btns.forEach((b, k) => { b.classList.toggle('sel', k === sel); b.setAttribute('aria-selected', String(k === sel)); b.tabIndex = k === sel ? 0 : -1; });
      bodies.forEach((b, k) => { b.hidden = k !== sel; });
      if (focus) btns[sel].focus();
    }
    list.addEventListener('keydown', (e) => {
      const to = { ArrowRight: sel + 1, ArrowLeft: sel - 1, Home: 0, End: btns.length - 1 }[e.key];
      if (to === undefined) return;
      e.preventDefault();
      select(to, true);
    });
    const el = h('div', 'w32-tabctl', null, [list, pane]);
    el.addEventListener('keydown', (e) => {
      if (!e.ctrlKey || !['Tab', 'PageDown', 'PageUp'].includes(e.key)) return;
      e.preventDefault();
      select(sel + (e.key === 'PageUp' || (e.key === 'Tab' && e.shiftKey) ? -1 : 1), btns.includes(document.activeElement));
    });
    select(sel);
    return el;
  }

  /* ---------- multi-select model for the list views ---------- */
  /* Click, Ctrl+click, Shift+click, arrows (by `cols` in a grid), Home/End, Page keys, Ctrl+A;
     Ctrl+arrows move the focus without selecting. Selection is kept by item key across re-sorts. */
  function selectable(box, { rows, cols = () => 1, top = 0, selected = [] }) {
    const sel = new Set(selected);
    let focus = selected[0] ?? null, anchor = focus;
    const keyOf = (r) => r.dataset.key;
    function paint() {
      rows().forEach((r) => {
        const k = keyOf(r);
        r.classList.toggle('sel', sel.has(k));
        r.classList.toggle('focus', k === focus);
        r.setAttribute('aria-selected', String(sel.has(k)));
      });
    }
    function select(i, { ctrl = false, shift = false } = {}) {
      const list = rows();
      if (!list.length) return;
      i = clamp(i, 0, list.length - 1);
      const k = keyOf(list[i]);
      if (shift) {
        if (!ctrl) sel.clear();
        const a = Math.max(0, list.findIndex((r) => keyOf(r) === anchor));
        for (let n = Math.min(a, i); n <= Math.max(a, i); n++) sel.add(keyOf(list[n]));
      } else if (ctrl) { if (sel.has(k)) sel.delete(k); else sel.add(k); anchor = k; }
      else { sel.clear(); sel.add(k); anchor = k; }
      focus = k;
      paint();
      ensureVisible(box, list[i], top);
    }
    box.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 && e.button !== 2) return;
      if (e.target === box && (e.offsetX >= box.clientWidth || e.offsetY >= box.clientHeight)) return; // the scroll bars
      const r = e.target.closest('[data-key]');
      if (!r) { if (!e.target.closest('.w32-lv-head') && !e.ctrlKey) { sel.clear(); paint(); } return; }
      if (e.button === 2 && sel.has(keyOf(r))) return;
      select(rows().indexOf(r), { ctrl: e.ctrlKey, shift: e.shiftKey });
    });
    box.addEventListener('keydown', (e) => {
      const list = rows(), c = cols(), at = Math.max(0, list.findIndex((r) => keyOf(r) === focus));
      const page = Math.max(1, Math.floor((box.clientHeight - top) / (list[0]?.offsetHeight || 20)) - 1) * c;
      let i;
      switch (e.key) {
        case 'ArrowDown': i = at + c; break;
        case 'ArrowUp': i = at - c; break;
        case 'ArrowRight': if (c === 1) return; i = at + 1; break;
        case 'ArrowLeft': if (c === 1) return; i = at - 1; break;
        case 'Home': i = 0; break;
        case 'End': i = list.length - 1; break;
        case 'PageDown': i = at + page; break;
        case 'PageUp': i = at - page; break;
        case ' ': if (!e.ctrlKey) return; e.preventDefault(); select(at, { ctrl: true }); return;
        case 'a': case 'A':
          if (!e.ctrlKey) return;
          e.preventDefault();
          list.forEach((r) => sel.add(keyOf(r)));
          paint();
          return;
        default: return;
      }
      e.preventDefault();
      i = clamp(i, 0, list.length - 1);
      if (e.ctrlKey && !e.shiftKey) { focus = keyOf(list[i]); paint(); ensureVisible(box, list[i], top); }
      else select(i, { shift: e.shiftKey });
    });
    return { paint };
  }

  /* ---------- file-system data for the list and tree views ---------- */
  const TYPES = { txt: 'Text Document', pdf: 'PDF File', xlsx: 'XLSX File', zip: 'Compressed (zipped) Folder', gz: 'GZ File', exe: 'Application', png: 'PNG File' };
  const typeOf = (it) => (it.kind === 'folder' ? 'File folder' : it.kind === 'shortcut' ? 'Shortcut' : TYPES[it.ext] || `${String(it.ext || '').toUpperCase()} File`);
  const sizeOf = (it) => (it.kind === 'file' ? `${Math.max(1, Math.ceil((it.size || 0) / 1024)).toLocaleString('en-US')} KB` : it.kind === 'shortcut' ? '1 KB' : '');
  const dateFmt = new Intl.DateTimeFormat('en-US', { month: 'numeric', day: 'numeric', year: 'numeric', hour: 'numeric', minute: '2-digit' });
  const when = (ms) => dateFmt.format(new Date(ms)).replace(',', '');

  function listViewDetails() {
    const items = ['documents', 'downloads', 'desktop'].flatMap((p) => Shell.fs.children(p));
    const COLS = [
      { key: 'name', label: 'Name', w: 230, get: (it) => it.name.toLowerCase() },
      { key: 'date', label: 'Date modified', w: 150, get: (it) => it.modified },
      { key: 'type', label: 'Type', w: 180, get: typeOf },
      { key: 'size', label: 'Size', w: 90, num: true, get: (it) => (it.kind === 'file' ? it.size || 0 : -1) },
    ];
    let sort = { key: 'name', dir: 1 };
    const box = h('div', 'w32-lv w32-sb', { role: 'grid', tabindex: '0', 'aria-label': 'Files', 'aria-multiselectable': 'true' });
    box.style.setProperty('--cols', COLS.map((c) => `${c.w}px`).join(' '));
    const head = h('div', 'w32-lv-head', { role: 'row' });
    const body = h('div', 'w32-lv-rows', { role: 'rowgroup' });
    box.append(h('div', 'w32-lv-grid', null, [head, body]));
    kbdCues(box);
    const sorted = [...items].sort((a, b) => a.name.localeCompare(b.name));
    const model = selectable(box, { rows: () => [...body.children], top: 25, selected: sorted[1] ? [sorted[1].id] : [] });
    function render() {
      const col = COLS.find((c) => c.key === sort.key);
      const cmp = (a, b) => { const x = col.get(a), y = col.get(b); return x > y ? 1 : x < y ? -1 : 0; };
      items.sort((a, b) => ((b.kind === 'folder') - (a.kind === 'folder') || cmp(a, b) || a.name.localeCompare(b.name)) * sort.dir); // Explorer keeps folders together
      head.replaceChildren(...COLS.map((c) => {
        const on = c.key === sort.key;
        const b = h('button', `w32-hd${c.num ? ' num' : ''}`, { type: 'button', tabindex: '-1', role: 'columnheader', 'aria-sort': on ? (sort.dir > 0 ? 'ascending' : 'descending') : null },
          [h('span', null, { text: c.label }), on ? h('i', `sort${sort.dir < 0 ? ' desc' : ''}`, { 'aria-hidden': 'true' }) : null]);
        b.addEventListener('click', () => { sort = { key: c.key, dir: on ? -sort.dir : 1 }; render(); });
        return b;
      }));
      body.replaceChildren(...items.map((it) => h('div', 'w32-lv-row', { role: 'row', 'data-key': it.id }, [
        h('span', null, { role: 'gridcell' }, [icon(it.icon, 16), h('b', null, { text: it.name })]),
        h('span', 'dim', { role: 'gridcell', text: when(it.modified) }),
        h('span', 'dim', { role: 'gridcell', text: typeOf(it) }),
        h('span', 'num dim', { role: 'gridcell', text: sizeOf(it) }),
      ])));
      model.paint();
    }
    render();
    return box;
  }

  function listViewIcons() {
    const box = h('div', 'w32-lv icons w32-sb', { role: 'listbox', tabindex: '0', 'aria-label': 'This PC', 'aria-multiselectable': 'true' });
    kbdCues(box);
    Shell.fs.children('root').forEach((it) => box.append(h('div', 'w32-lv-icon', { role: 'option', 'data-key': it.id }, [icon(it.icon, 32), h('span', null, { text: it.name })])));
    const model = selectable(box, { rows: () => [...box.children], cols: () => Math.max(1, Math.floor((box.clientWidth - 8) / 80)), selected: ['documents'] });
    model.paint();
    return box;
  }

  /* static item states for the theme (LISS_HOT, LISS_SELECTED, LISS_HOTSELECTED, LISS_SELECTEDNOTFOCUS) */
  function itemStates() {
    const row = (label, k) => h('div', 'w32-lv-row', { style: `--cols: 1fr; background: var(--it-${k}); border-color: var(--it-${k}-border)` },
      [h('span', null, null, [icon('object/folder', 16), h('b', null, { text: label })])]);
    return h('div', 'w32-lv w32-itemstates', { inert: true }, [h('div', 'w32-lv-rows', null,
      [row('Hot', 'hot'), row('Selected', 'sel'), row('Selected and hot', 'hotsel'), row('Selected, not focused', 'nofocus')])]);
  }

  function treeView() {
    const box = h('div', 'w32-tree w32-sb', { role: 'tree', tabindex: '0', 'aria-label': 'Folders' });
    kbdCues(box);
    const kids = (id) => Shell.fs.children(id).filter((c) => c.kind === 'folder' || c.kind === 'drive');
    const open = new Set(['root', 'pictures']);
    let sel = 'documents', rows = [];
    function render() {
      rows = [];
      const walk = (id, depth, parent) => {
        const it = Shell.fs.get(id);
        if (!it) return;
        const ch = kids(id), isOpen = open.has(id) && ch.length > 0;
        const exp = h('span', `w32-tv-exp${ch.length ? (isOpen ? ' open' : '') : ' none'}`, { 'aria-hidden': 'true' }, [icon('fluent/chevron-right', 12)]);
        const row = h('div', `w32-tv-row${sel === id ? ' sel' : ''}`, {
          role: 'treeitem', 'data-id': id, 'aria-level': depth + 1, 'aria-selected': String(sel === id), 'aria-expanded': ch.length ? String(isOpen) : null,
        }, [exp, icon(it.icon, 16), h('span', null, { text: it.name })]);
        row.style.paddingLeft = `${2 + depth * 17}px`;
        rows.push({ id, row, parent, hasKids: ch.length > 0, isOpen });
        if (isOpen) ch.forEach((c) => walk(c.id, depth + 1, id));
      };
      ['root', 'recycle'].forEach((r) => walk(r, 0, null));
      box.replaceChildren(...rows.map((r) => r.row));
    }
    const toggle = (id) => { if (open.has(id)) open.delete(id); else open.add(id); render(); };
    const select = (id) => { sel = id; render(); ensureVisible(box, rows.find((r) => r.id === id)?.row); };
    box.addEventListener('pointerdown', (e) => {
      const row = e.target.closest('.w32-tv-row');
      if (!row || e.button !== 0) return;
      if (e.target.closest('.w32-tv-exp')) toggle(row.dataset.id); else select(row.dataset.id);
    });
    box.addEventListener('dblclick', (e) => { const row = e.target.closest('.w32-tv-row'); if (row && !e.target.closest('.w32-tv-exp')) toggle(row.dataset.id); });
    box.addEventListener('keydown', (e) => {
      const i = rows.findIndex((r) => r.id === sel), cur = rows[i];
      if (!cur) return;
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') select(rows[clamp(i + (e.key === 'ArrowDown' ? 1 : -1), 0, rows.length - 1)].id);
      else if (e.key === 'Home' || e.key === 'End') select(rows[e.key === 'Home' ? 0 : rows.length - 1].id);
      else if (e.key === 'ArrowRight') { if (cur.hasKids && !cur.isOpen) toggle(cur.id); else if (cur.isOpen) select(rows[i + 1].id); }
      else if (e.key === 'ArrowLeft') { if (cur.isOpen) toggle(cur.id); else if (cur.parent) select(cur.parent); }
      else if (e.key === 'Enter' && cur.hasKids) toggle(cur.id);
      else return;
      e.preventDefault();
    });
    render();
    return box;
  }

  /* ---------- gallery sections ---------- */
  /* dark: the dark Explorer theme a program applies for the control to follow dark mode, or
     null when Windows keeps the control light. */
  function section({ title, part, ini, dark = null, darkTag, note, wide = false, body }) {
    const stage = h('div', 'cg-stage w32', { 'data-dark': dark ? 'follows' : 'light' });
    stage.append(...[].concat(body(stage)).filter(Boolean));
    return h('section', `cg-section${wide ? ' wide' : ''}`, { 'aria-label': title }, [
      h('header', 'cg-head', null, [
        h('h3', 'cg-title', { text: title }),
        h('span', `cg-tag${dark ? ' follows' : ''}`, { text: darkTag || (dark ? `Dark with ${dark}` : 'Stays light in dark mode') }),
      ]),
      h('p', 'cg-part', null, [h('code', null, { text: part }), document.createTextNode(ini ? '  ·  theme.ini ' : '  ·  not in theme.ini'), ini ? h('code', null, { text: ini }) : null]),
      stage,
      note ? h('p', 'cg-note', { text: note }) : null,
    ]);
  }
  const sub = (text) => h('p', 'w32-sub', { text });
  const row = (...kids) => h('div', 'w32-row', null, kids);
  const col = (...kids) => h('div', 'w32-col', null, kids);
  function form(pairs) {
    return h('div', 'w32-form', null, pairs.flatMap(([label, ctl]) => {
      const id = `cg-f${++seq}`;
      const target = ctl.matches('input, button') ? ctl : ctl.querySelector('input, button');
      if (target && !target.id) target.id = id;
      return [h('label', null, { for: target?.id || null, text: label }), ctl];
    }));
  }
  function edit(value, props = {}) { const e = h('input', `w32-edit ${props.cls || ''}`.trim(), { type: props.type || 'text', spellcheck: 'false', autocomplete: 'off', ...props, cls: null }); e.value = value; return e; }

  const GAMES = ['Example Game', 'Demo Game', 'Sample Game', 'Test Game', 'Arcade Game', 'Puzzle Game', 'Racing Game', 'Strategy Game', 'Platform Game', 'Card Game', 'Rhythm Game', 'Adventure Game', 'Sports Game', 'Board Game'];
  const RESOLUTIONS = ['1280 x 720', '1600 x 900', '1920 x 1080', '2560 x 1440', '3840 x 2160'];
  const LINES = ['Example Game launch notes', '', 'Prefix: D:\\Wine prefixes\\example-game', 'Renderer: Vulkan through RADV on the PS5 GPU', 'Controller: DualSense, read as an XInput pad', 'Audio: 48 kHz stereo through AudioOut', 'Saves: D:\\Wine prefixes\\example-game\\drive_c\\users\\Mihawk\\AppData\\LocalLow', 'Last run: 2026-10-14 21:03, 2 h 14 min'];

  /* ---------- pages ---------- */
  function pageButtons() {
    const cbMatrix = () => {
      const t = h('table', 'w32-matrix', { inert: true });
      const cols = [['Normal', ''], ['Hot', 'is-hot'], ['Pressed', 'is-pressed'], ['Disabled', '']];
      t.append(h('tr', null, null, [h('th'), ...cols.map(([c]) => h('th', null, { scope: 'col', text: c }))]));
      return { t, cols };
    };
    const checkMatrix = (type, rowsDef) => {
      const { t, cols } = cbMatrix();
      rowsDef.forEach(([label, opts]) => t.append(h('tr', null, null, [h('th', null, { scope: 'row', text: label }),
        ...cols.map(([c, cls]) => h('td', null, null, [check('', { type, ...opts, cls, disabled: c === 'Disabled' })]))])));
      return t;
    };
    const split = h('button', 'w32-btn w32-split', { type: 'button', 'aria-haspopup': 'menu' }, [h('span', null, { text: 'Save' }), h('span', 'w32-split-arrow', { 'aria-hidden': 'true' }, [tri('down')])]);
    dropMenu(split, () => [{ label: 'Save' }, { label: 'Save as...' }, { label: 'Save a copy' }, { separator: true }, { label: 'Save all', disabled: true }], split.lastChild);
    const link = (title, note, disabled) => h('button', 'w32-cmdlink', { type: 'button', disabled }, [icon('fluent/arrow-right', 16), h('b', null, { text: title }), h('small', null, { text: note })]);
    return [
      section({
        title: 'Push button', part: 'BUTTON / BP_PUSHBUTTON', ini: '[Button.Pushbutton]', dark: 'DarkMode_Explorer',
        note: 'Wine theme today: rest #FFFFFF with a #D1D1D1 border, hot #F9F9F9, pressed #F0F0F0 (neutral), default 2 px #0067C0. Focus moves the default border to the focused button.',
        body: () => [
          states([['Normal', button('Button')], ['Hot', button('Button', { cls: 'is-hot' })], ['Pressed', button('Button', { cls: 'is-pressed' })],
            ['Disabled', button('Button', { disabled: true })], ['Default', button('Button', { def: true })]]),
          sub('Live: Tab moves focus, Space presses, the split part opens a menu'),
          h('div', 'w32-row w32-defscope', null, [button('OK', { def: true }), button('Cancel'), button('Apply', { disabled: true }), split]),
        ],
      }),
      section({
        title: 'Check box', part: 'BUTTON / BP_CHECKBOX', ini: '[Button.Checkbox]',
        note: 'Windows 11 fills a checked box with blue. Wine theme today: a dark check mark on a white box, hot border #0067C0.',
        body: () => [
          checkMatrix('checkbox', [['Unchecked', {}], ['Checked', { checked: true }], ['Mixed', { mixed: true }]]),
          sub('Live'),
          col(check('Vertical sync', { checked: true }), check('Show frame rate'), threeState('Three-state (click to cycle)'), check('Disabled', { disabled: true }), check('Disabled, checked', { disabled: true, checked: true })),
        ],
      }),
      section({
        title: 'Radio button', part: 'BUTTON / BP_RADIOBUTTON', ini: '[Button.Radiobutton]',
        note: 'Wine theme today: a dark dot on a white circle; the dot turns blue only while hot.',
        body: () => [
          checkMatrix('radio', [['Unchecked', {}], ['Checked', { checked: true }]]),
          sub('Live: arrow keys move within the group'),
          col(check('Fullscreen', { type: 'radio', name: 'cg-mode', checked: true }), check('Borderless window', { type: 'radio', name: 'cg-mode' }),
            check('Windowed', { type: 'radio', name: 'cg-mode' }), check('Disabled', { type: 'radio', name: 'cg-mode2', disabled: true })),
        ],
      }),
      section({
        title: 'Group box', part: 'BUTTON / BP_GROUPBOX', ini: '[Button.Groupbox]',
        note: 'Square 1 px #DCDCDC frame. Wine theme today: rounded 3 px corners in #AEAEAE.',
        body: () => h('fieldset', 'w32-group', null, [h('legend', null, { text: 'Audio' }),
          col(check('Mute when in the background', { checked: true }), check('Use spatial sound'), check('48 kHz', { type: 'radio', name: 'cg-rate', checked: true }), check('44.1 kHz', { type: 'radio', name: 'cg-rate' }))]),
      }),
      section({
        title: 'Command link', part: 'BUTTON / BP_COMMANDLINK', ini: '[Button.Commandlink]',
        note: 'Wine theme today: title 26 26 26, hot 0 103 192, pressed 0 84 158.',
        body: () => col(link('Play now', 'Starts the game with the last settings'), link('Repair the game files', 'Checks every file and downloads what is missing'), link('Uninstall', 'Not available while the game is running', true)),
      }),
    ];
  }

  function pageText() {
    const edits = () => states([
      ['Normal', edit('Example Game')], ['Hot', edit('Example Game', { cls: 'is-hot' })], ['Focused', edit('Example Game', { cls: 'is-focus' })],
      ['Disabled', edit('Example Game', { disabled: true })], ['Read-only', edit('Example Game', { readonly: true })],
    ]);
    const ddlStates = () => {
      const mk = (cls, disabled) => comboBox({ options: RESOLUTIONS, value: 2, label: 'Resolution', disabled }).el;
      const hot = mk(); hot.classList.add('is-hot');
      const pressed = mk(); pressed.classList.add('is-pressed');
      const focus = mk(); focus.classList.add('is-focus');
      return states([['Normal', mk()], ['Hot', hot], ['Pressed', pressed], ['Focused', focus], ['Disabled', mk('', true)]]);
    };
    const openSpecimen = () => {
      const ddl = comboBox({ options: RESOLUTIONS, value: 2, label: 'Resolution (open)' });
      ddl.el.classList.add('open');
      ddl.el.inert = true;
      const list = dropList(ddl.el, RESOLUTIONS, 2, { inline: true, onPick: (i) => { ddl.el.querySelector('.w32-ddl-text').textContent = RESOLUTIONS[i]; } });
      return h('div', 'w32-stack', null, [ddl.el, list.el]);
    };
    const area = h('textarea', 'w32-edit w32-sb', { wrap: 'off', spellcheck: 'false', 'aria-label': 'Notes' });
    area.value = LINES.concat(LINES.slice(2).map((l) => `${l} (copy)`)).join('\r\n');
    return [
      section({
        title: 'Edit', part: 'EDIT / EP_EDITTEXT · EP_EDITBORDER_NOSCROLL', ini: '[Edit] [Edit.EditBorder_NoScroll]', dark: 'DarkMode_CFD',
        note: 'Wine theme today: border #AEAEAE, hot #0067C0, focused #00549E; disabled and read-only fill #F3F3F3.',
        body: () => [edits(), sub('Live'), form([
          ['Game name:', edit('Example Game')],
          ['Password:', edit('proton-ps5', { cls: 'password' })], // ES_PASSWORD: bullets without a page-level password field
          ['Search:', edit('', { placeholder: 'Search games' })],
          ['Disabled:', edit('Not available', { disabled: true })],
        ])],
      }),
      section({
        title: 'Multi-line edit', part: 'EDIT / EP_EDITBORDER_HVSCROLL', ini: '[Edit.EditBorder_HVScroll]', dark: 'DarkMode_Explorer',
        note: 'Classic 17 px scroll bars with arrows inside a multi-line edit (ES_MULTILINE, WS_VSCROLL, WS_HSCROLL).',
        body: () => area,
      }),
      section({
        title: 'Combo box', part: 'COMBOBOX / CP_READONLY · CP_BORDER · CP_DROPDOWNBUTTONRIGHT', ini: '[ComboBox.ReadOnly] [ComboBox.Border] [ComboBox.DropDownButtonRight]', dark: 'DarkMode_CFD',
        note: 'A drop-down list looks like a push button and highlights its text while focused. Wine theme today: read-only face #F3F3F3 with a #AEAEAE border; list highlight 0 103 192.',
        body: () => [ddlStates(), sub('Live: F4 or Alt+Down opens, arrows change the selection'), form([
          ['Drop-down list:', comboBox({ options: RESOLUTIONS, value: 2, label: 'Resolution' }).el],
          ['Drop-down:', comboBox({ options: ['-windowed', '-popupwindow', '-force-vulkan', '-screen-fullscreen 1'], value: 0, editable: true, label: 'Launch arguments' }).el],
          ['Disabled:', comboBox({ options: ['Speakers (PS5 Audio)'], value: 0, label: 'Output device', disabled: true }).el],
        ]), sub('Open (ComboLBox)'), openSpecimen()],
      }),
      section({
        title: 'List box', part: 'LISTBOX / LBCP_BORDER_VSCROLL', ini: '[ListBox]', dark: 'DarkMode_Explorer',
        note: 'Selection in the system Highlight colour. Wine theme today: border 174 174 174, Highlight 0 103 192.',
        body: () => listBox(GAMES, 0, 'Games'),
      }),
      section({
        title: 'Up-down', part: 'SPIN / SPNP_UP · SPNP_DOWN', ini: '[Spin] [Spin.Up] [Spin.Down]',
        note: 'Hold a button to repeat; arrow keys in the buddy edit step too. Wine theme today: fill 227 227 227, border 174 174 174.',
        body: () => form([['Frame rate limit:', upDown({ value: 60, min: 30, max: 240, label: 'Frame rate limit' })], ['Players:', upDown({ value: 1, min: 1, max: 4, label: 'Players' })]]),
      }),
    ];
  }

  function pageLists() {
    return [
      section({
        title: 'List view, details', part: 'LISTVIEW / LVP_LISTITEM · HEADER / HP_HEADERITEM (Explorer theme)', ini: '[ListView] [Header.HeaderItem]', dark: 'DarkMode_ItemsView', wide: true,
        note: 'Click a column to sort; Ctrl and Shift extend the selection; selection turns grey when the list loses focus. theme.ini has no Explorer::ListView, so Wine draws list selection in Highlight (0 103 192); its header is hot #E3F2FD, pressed #BBDEFB.',
        body: () => [listViewDetails(), sub('Item states'), itemStates()],
      }),
      section({
        title: 'List view, icons', part: 'LISTVIEW / LVP_LISTITEM (LV_VIEW_ICON)', ini: '[ListView]', dark: 'DarkMode_ItemsView',
        note: '32 px icons on a 75 px grid; selected labels show in full.',
        body: () => listViewIcons(),
      }),
      section({
        title: 'Tree view', part: 'TREEVIEW / TVP_TREEITEM · TVP_GLYPH · TVP_HOTGLYPH (Explorer theme)', ini: '[Explorer::TreeView.TreeItem] [Explorer::TreeView.Glyph]', dark: 'DarkMode_Explorer',
        note: 'Chevrons instead of plus boxes and lines; full-row hot and selected plates. Left and Right collapse, expand and walk the levels.',
        body: () => treeView(),
      }),
    ];
  }

  function pageRanges() {
    const value = h('span', 'w32-cap', { text: '6' });
    const pRow = (label, el) => [h('span', 'w32-cap', { text: label }), el];
    const classicBox = () => {
      const box = h('div', 'w32-scrollbox w32-sb', { tabindex: '0', 'aria-label': 'Classic scroll bars' });
      LINES.concat(LINES, LINES).forEach((l) => box.append(h('p', null, { text: l || ' ' })));
      return box;
    };
    const thinBox = () => thinScroller(LINES.concat(LINES, LINES).map((l) => h('p', null, { text: l || ' ' })), 'Overlay scroll bars');
    return [
      section({
        title: 'Trackbar', part: 'TRACKBAR / TKP_TRACK · TKP_THUMBBOTTOM · TKP_TICS', ini: '[TrackBar.Track] [TrackBar.Thumb] [TrackBar.Tics]',
        note: 'Blue thumb, black while hot, grey while dragged. Wine theme today: a white thumb with a #AEAEAE border, hot border #0067C0, pressed #00549E.',
        body: () => [
          form([
            ['Brightness:', h('div', 'w32-row', null, [trackbar({ value: 6, label: 'Brightness', onInput: (v) => { value.textContent = String(v); } }), value])],
            ['Disabled:', trackbar({ value: 3, disabled: true, label: 'Disabled' })],
          ]),
          sub('Thumb states'),
          h('div', 'w32-thumbs', { inert: true }, ['Normal', 'Hot', 'Pressed', 'Disabled'].map((s) => h('figure', null, null, [h('div', `w32-thumb ${s.toLowerCase()}`), h('figcaption', null, { text: s })]))),
        ],
      }),
      section({
        title: 'Progress bar', part: 'PROGRESS / PP_BAR · PP_FILL · PP_MOVEOVERLAY', ini: '[Progress.Bar] [Progress.Fill]',
        note: 'Green, yellow when paused, red on error; marquee for unknown length. Wine theme today: fill 0 103 192 (blue), paused 255 238 88, error 213 0 0, no marquee overlay.',
        body: () => h('div', 'w32-row', null, [
          h('div', 'w32-plist', null, [
            ...pRow('Normal', progress('normal', 63)), ...pRow('Paused', progress('paused', 40)), ...pRow('Error', progress('error', 72)), ...pRow('Marquee', progress('marquee', 0)),
          ]),
          progress('normal', 70, 'vert'),
        ]),
      }),
      section({
        title: 'Scroll bars', part: 'SCROLLBAR / SBP_ARROWBTN · SBP_THUMBBTNVERT · SBP_LOWERTRACKVERT', ini: '[ScrollBar.ArrowBtn] [ScrollBar.ThumbBtnVert]', dark: 'DarkMode_Explorer', wide: true,
        note: 'Classic programs get the 17 px bar with arrows (SM_CXVSCROLL). Windows 11\u2019s overlay bar (File Explorer, WinUI) is a thin line that widens into a 6 px thumb with arrows under the pointer. Wine theme today: ScrollbarWidth 17, a white thumb with a #AEAEAE border.',
        body: () => h('div', 'w32-row', { style: 'align-items: flex-start; gap: 24px' }, [
          col(h('span', 'w32-cap', { text: 'Classic (SCROLLBAR)' }), classicBox()),
          col(h('span', 'w32-cap', { text: 'Windows 11 overlay' }), thinBox()),
        ]),
      }),
    ];
  }

  function pageBars(page) {
    /* the menu bar specimen: a small Notepad-style window whose Edit menu is open on arrival */
    const state = { wrap: false, status: true };
    const text = h('div', 'w32-client-body', { text: LINES.join('\n') });
    const status = h('div', 'w32-status', null, [h('span'), h('span', null, { text: 'Ln 1, Col 1' }), h('span', null, { text: '100%' }), h('span', null, { text: 'Windows (CRLF)' }), h('span', null, { text: 'UTF-8' }), h('i', 'w32-gripper')]);
    const win = h('div', 'w32-menuwin');
    const menus = [
      { label: 'File', items: () => [{ label: 'New', kbd: 'Ctrl+N' }, { label: 'New Window', kbd: 'Ctrl+Shift+N' }, { label: 'Open...', kbd: 'Ctrl+O' }, { label: 'Save', kbd: 'Ctrl+S' }, { label: 'Save As...', kbd: 'Ctrl+Shift+S' }, { separator: true }, { label: 'Page Setup...' }, { label: 'Print...', kbd: 'Ctrl+P' }, { separator: true }, { label: 'Exit' }] },
      { label: 'Edit', items: () => [{ label: 'Undo', kbd: 'Ctrl+Z' }, { separator: true }, { label: 'Cut', kbd: 'Ctrl+X', disabled: true }, { label: 'Copy', kbd: 'Ctrl+C', disabled: true }, { label: 'Paste', kbd: 'Ctrl+V' }, { label: 'Delete', kbd: 'Del', disabled: true }, { separator: true }, { label: 'Find...', kbd: 'Ctrl+F' }, { label: 'Find Next', kbd: 'F3' }, { label: 'Replace...', kbd: 'Ctrl+H' }, { label: 'Go To...', kbd: 'Ctrl+G' }, { separator: true }, { label: 'Select All', kbd: 'Ctrl+A' }, { label: 'Time/Date', kbd: 'F5', action: () => { text.textContent += `\n${new Date().toLocaleString('en-US')}`; } }] },
      { label: 'Format', items: () => [{ label: 'Word Wrap', checked: state.wrap, action: () => { state.wrap = !state.wrap; text.style.whiteSpace = state.wrap ? 'pre-wrap' : 'pre'; } }, { label: 'Font...' }] },
      { label: 'View', items: () => [{ label: 'Zoom', submenu: [{ label: 'Zoom In', kbd: 'Ctrl+Plus' }, { label: 'Zoom Out', kbd: 'Ctrl+Minus' }, { label: 'Restore Default Zoom', kbd: 'Ctrl+0' }] }, { label: 'Status Bar', checked: state.status, action: () => { state.status = !state.status; status.hidden = !state.status; } }] },
      { label: 'Help', items: () => [{ label: 'View Help' }, { label: 'Send Feedback' }, { separator: true }, { label: 'About' }] },
    ];
    const bar = menuBar(menus, win);
    text.style.whiteSpace = 'pre';
    text.style.overflow = 'hidden';
    win.append(bar.el, text, status);
    page.onShow = () => requestAnimationFrame(() => { if (!bar.isOpen() && win.isConnected) bar.show(1, false); });

    const tb = (ic, label, opts = {}) => {
      const b = h('button', `w32-tbbtn${opts.checked ? ' checked' : ''}`, { type: 'button', tabindex: '-1', 'aria-label': label, 'aria-pressed': opts.check ? String(!!opts.checked) : null, 'aria-haspopup': opts.drop ? 'menu' : null, disabled: opts.disabled },
        [icon(ic, 16), opts.text ? h('span', null, { text: opts.text }) : null, opts.drop ? tri('down') : null]);
      classicTip(b, opts.kbd ? `${label} (${opts.kbd})` : label);
      if (opts.check) b.addEventListener('click', () => b.setAttribute('aria-pressed', String(b.classList.toggle('checked'))));
      if (opts.drop) dropMenu(b, opts.drop);
      return b;
    };
    const viewMode = { v: 'Details' };
    const toolbar = h('div', 'w32-toolbar', { role: 'toolbar', 'aria-label': 'Standard' }, [
      tb('fluent/document-add', 'New', { kbd: 'Ctrl+N' }), tb('fluent/folder-open', 'Open', { kbd: 'Ctrl+O' }), tb('fluent/save', 'Save', { kbd: 'Ctrl+S' }),
      h('span', 'w32-tbsep'),
      tb('fluent/cut', 'Cut', { kbd: 'Ctrl+X', disabled: true }), tb('fluent/copy', 'Copy', { kbd: 'Ctrl+C' }), tb('fluent/clipboard-paste', 'Paste', { kbd: 'Ctrl+V' }),
      h('span', 'w32-tbsep'),
      tb('fluent/text-font', 'Word wrap', { check: true, checked: true }), tb('fluent/print', 'Print', { kbd: 'Ctrl+P' }),
      tb('fluent/grid', 'View', { text: 'View', drop: () => ['Large icons', 'Small icons', 'List', 'Details'].map((l) => ({ label: l, radio: true, checked: viewMode.v === l, action: () => { viewMode.v = l; } })) }),
    ]);
    toolbar.firstChild.tabIndex = 0;
    toolbar.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
      const btns = [...toolbar.querySelectorAll('.w32-tbbtn:not(:disabled)')], i = btns.indexOf(document.activeElement);
      if (i < 0) return;
      e.preventDefault();
      const next = btns[(i + (e.key === 'ArrowRight' ? 1 : -1) + btns.length) % btns.length];
      btns.forEach((b) => { b.tabIndex = b === next ? 0 : -1; });
      next.focus();
    });
    const address = edit('D:\\Games\\Example Game', { 'aria-label': 'Address' });
    const rebar = h('div', 'w32-rebar', null, [
      h('div', 'w32-band', null, [h('span', 'w32-band-grip', { 'aria-hidden': 'true' }), toolbar]),
      h('div', 'w32-band grow', null, [h('span', 'w32-band-grip', { 'aria-hidden': 'true' }), h('label', 'w32-cap', { text: 'Address', for: (address.id = 'cg-address') }), address]),
    ]);

    const tabsDemo = () => tabControl([
      { label: 'General', body: () => col(check('Start with Windows'), check('Check for updates automatically', { checked: true }), form([['Language:', comboBox({ options: ['English (United States)', 'Français', 'Deutsch', '日本語'], value: 0, label: 'Language' }).el]])) },
      { label: 'Controller', body: () => col(check('Use the DualSense as an XInput pad', { checked: true }), check('Vibration', { checked: true }), form([['Dead zone:', trackbar({ value: 2, label: 'Dead zone' })]])) },
      { label: 'Advanced', body: () => col(check('Esync', { checked: true }), check('Large address aware'), form([['Log level:', comboBox({ options: ['Errors', 'Warnings', 'Everything'], value: 0, label: 'Log level' }).el]])) },
    ]);
    const tipSample = h('div', 'w32-row', { style: 'align-items: flex-start' }, [
      col(h('button', 'w32-tbbtn is-hot', { type: 'button', tabindex: '-1', inert: true, 'aria-label': 'Save' }, [icon('fluent/save', 16)]), h('div', 'w32-tip static', { text: 'Save (Ctrl+S)' })),
      h('div', 'w32-balloon', { role: 'note' }, [icon('fluent/info', 16), h('b', null, { text: 'Caps Lock is on' }), h('p', null, { text: 'Passwords are case sensitive, so check the key before you type.' })]),
    ]);
    return [
      section({
        title: 'Menu bar and menu', part: 'MENU / MENU_BARITEM · MENU_POPUPITEM · MENU_POPUPCHECK · MENU_POPUPSUBMENU · MENU_POPUPSEPARATOR', ini: '[Menu.BarItem] [Menu.PopupItem] [Menu.PopupCheck]', wide: true,
        darkTag: 'Menu bar stays light, popups go dark (SetPreferredAppMode)',
        note: 'Windows 11 rounds popup menus (8 px) and keeps the menu bar square. Hover switches menus while one is open; arrows, Enter, Esc and first letters work. Wine theme today: hot popup item solid 0 103 192 with white text.',
        body: () => win,
      }),
      section({
        title: 'Toolbar and rebar', part: 'TOOLBAR / TP_BUTTON · TP_DROPDOWNBUTTON · TP_SEPARATOR · REBAR / RP_GRIPPER · RP_BAND', ini: '[Toolbar.Button] [Rebar] [Rebar.Gripper]', wide: true,
        note: 'Hover a button for its tooltip; Word wrap is a check button; View drops a menu. Wine theme today: hot white with a #0067C0 border, pressed #00549E, toolbar fill 244 244 244.',
        body: () => rebar,
      }),
      section({
        title: 'Tab control', part: 'TAB / TABP_TOPTABITEM · TABP_PANE', ini: '[Tab.TopTabItem] [Tab.Pane]',
        note: 'The selected tab stands 2 px taller and joins the pane. Ctrl+Tab and the arrow keys switch tabs. Wine theme today: unselected #F3F3F3, hot with a #0067C0 edge.',
        body: () => tabsDemo(),
      }),
      section({
        title: 'Status bar', part: 'STATUS / SP_PANE · SP_GRIPPER', ini: '[Status.Pane] [Status.Gripper]',
        note: 'Panes divided by a light line, the size gripper in the corner.',
        body: () => h('div', 'w32-client', null, [h('div', 'w32-client-body', { text: '1,284 files verified' }),
          h('div', 'w32-status', null, [h('span', null, { text: 'Ready' }), h('span', null, { text: '14 items' }), h('span', null, { text: '1 item selected' }), h('i', 'w32-gripper')])]),
      }),
      section({
        title: 'Tooltips', part: 'TOOLTIP / TTP_STANDARD · TTP_BALLOON', ini: '[Tooltip.Standard] [Tooltip.Ballon]', dark: 'DarkMode_Explorer',
        note: 'Windows 11 rounds tooltips (4 px). Wine theme today: standard tooltip text 150 150 150.',
        body: () => tipSample,
      }),
    ];
  }

  function pageDates() {
    const now = Shell.now();
    return [
      section({
        title: 'Date picker', part: 'DATEPICKER / DP_DATEBORDER · DP_SHOWCALENDARBUTTONRIGHT',
        note: 'Not in theme.ini: Wine draws the date picker unthemed. F4 or Alt+Down opens the calendar; Enter takes the day.',
        body: () => form([
          ['Short date:', datePicker({ value: now, label: 'Short date' })],
          ['Long date:', datePicker({ value: now, long: true, label: 'Long date' })],
          ['Disabled:', datePicker({ value: now, disabled: true, label: 'Disabled date' })],
        ]),
      }),
      section({
        title: 'Month calendar', part: 'MONTHCAL / MC_BACKGROUND · MC_GRIDCELL · MC_GRIDCELLUPPER',
        note: 'Not in theme.ini. Today is outlined; click the title for the year view; arrows and Page keys move the selection.',
        body: () => monthCal({ value: now }).el,
      }),
    ];
  }

  /* ---------- the sample "game launcher" dialog ---------- */
  function launcherDialog({ onCancel, onPlay }) {
    const apply = button('Apply', { disabled: true });
    const play = button('Play', { def: true });
    const cancel = button('Cancel');
    const pct = h('span', 'w32-cap', { text: 'Verifying game files... 0%' });
    const bar = progress('normal', 0);
    const browse = button('Browse...');
    const prefix = edit('D:\\Wine prefixes\\example-game', { 'aria-label': 'Wine prefix' });
    const vol = h('span', 'w32-cap', { text: '80' });
    const general = () => col(
      h('fieldset', 'w32-group', null, [h('legend', null, { text: 'Game' }), form([
        ['Wine prefix:', h('div', 'w32-row', { style: 'flex-wrap: nowrap; gap: 6px' }, [prefix, browse])],
        ['Arguments:', comboBox({ options: ['-windowed', '-popupwindow', '-force-vulkan'], value: 2, editable: true, label: 'Launch arguments' }).el],
      ]), check('Start in offline mode')]),
      h('fieldset', 'w32-group', null, [h('legend', null, { text: 'Files' }), pct, bar]),
    );
    const graphics = () => col(
      form([['Resolution:', comboBox({ options: RESOLUTIONS, value: 2, label: 'Resolution' }).el], ['Frame rate limit:', upDown({ value: 60, min: 30, max: 240, label: 'Frame rate limit' })]]),
      h('fieldset', 'w32-group', null, [h('legend', null, { text: 'Display mode' }),
        check('Fullscreen', { type: 'radio', name: 'cg-dlg-mode', checked: true }), check('Borderless window', { type: 'radio', name: 'cg-dlg-mode' }), check('Windowed', { type: 'radio', name: 'cg-dlg-mode' })]),
      check('Vertical sync', { checked: true }), check('Show frame rate'),
    );
    const audio = () => col(
      form([['Volume:', h('div', 'w32-row', null, [trackbar({ max: 100, value: 80, tick: 10, label: 'Volume', onInput: (v) => { vol.textContent = String(v); } }), vol])],
        ['Output device:', comboBox({ options: ['Speakers (PS5 Audio)', 'Headset (DualSense)', 'HDMI (TV)'], value: 0, label: 'Output device' }).el]]),
      check('Mute when in the background', { checked: true }),
    );
    const head = h('div', 'w32-dialog-head', null, [icon('object/app-game', 32), col(h('b', null, { text: 'Example Game' }), h('span', 'w32-cap', { text: 'Launch options for the copy on D:' }))]);
    const el = h('div', 'w32 w32-dialog w32-defscope', { role: 'dialog', 'aria-label': 'Example Game launch options' }, [
      head,
      tabControl([{ label: 'General', body: general }, { label: 'Graphics', body: graphics }, { label: 'Audio', body: audio }]),
      h('div', 'w32-dialog-buttons', null, [play, cancel, apply]),
    ]);
    /* any change enables Apply, as a property sheet does */
    const dirty = (e) => { if (!e.target.closest('.w32-dialog-buttons')) apply.disabled = false; };
    el.addEventListener('input', dirty);
    el.addEventListener('change', dirty);
    apply.addEventListener('click', () => { apply.disabled = true; play.focus(); });
    play.addEventListener('click', () => onPlay?.());
    cancel.addEventListener('click', () => onCancel?.());
    browse.addEventListener('click', async () => {
      const r = await Shell.dialogs?.fileOpen?.({ title: 'Select the Wine prefix', folder: 'd', owner: el.closest('.window') });
      if (r?.parent) { prefix.value = Shell.fs.path(r.parent).map((i) => i.name).join('\\'); apply.disabled = false; }
    });
    /* Enter presses the default (or focused) button, Esc cancels */
    el.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { e.preventDefault(); onCancel?.(); }
      else if (e.key === 'Enter' && !e.target.closest('.w32-btn, .w32-cmdlink, textarea, [role="tab"], .w32-lv, .w32-tree, .w32-listbox, .w32-mc')) { e.preventDefault(); play.click(); }
    });
    let n = 0;
    const timer = setInterval(() => {
      if (!el.isConnected && n > 0) { clearInterval(timer); return; }
      n = Math.min(100, n + 3);
      bar.style.setProperty('--value', n);
      bar.setAttribute('aria-valuenow', n);
      pct.textContent = n < 100 ? `Verifying game files... ${n}%` : 'All 1,284 files verified.';
      if (n >= 100) clearInterval(timer);
    }, 150);
    G?.cleanups.push(() => clearInterval(timer));
    return el;
  }

  let dialogWin = null;
  function openDialogWindow() {
    if (dialogWin && Shell.wm.all().includes(dialogWin)) { dialogWin.focus(); return; }
    let win = null;
    const content = launcherDialog({ onCancel: () => win?.close(), onPlay: () => { win?.close(); Shell.apps.launch('game', { title: 'Example Game' }); } });
    content.classList.add('cg-window-dialog');
    win = dialogWin = Shell.wm.open({ appId: 'controls', title: 'Example Game launch options', icon: 'object/app-game', width: 460, height: 470, minWidth: 320, minHeight: 300, resizable: false, maximizable: false, minimizable: false, background: 'solid', content });
    setTimeout(() => content.querySelector('.w32-btn.default')?.focus({ preventScroll: true }));
  }

  function pageDialog() {
    const holder = h('div');
    const mount = () => holder.replaceChildren(launcherDialog({ onCancel: mount, onPlay: () => Shell.apps.launch('game', { title: 'Example Game' }) }));
    mount();
    const openBtn = button('Open in a window');
    openBtn.addEventListener('click', openDialogWindow);
    return [section({
      title: 'Sample dialog: game launcher', part: '#32770 dialog · property sheet', ini: '[Window.Dialog]', wide: true,
      darkTag: 'Stays light: classic dialogs are not themed dark',
      note: 'Every control together, as a classic launcher would use them: Tab walks the controls, Enter plays, Esc cancels, a change enables Apply. Browse opens the common file dialog when it is available. Dialog face: Windows 240 240 240; Wine theme today BtnFace 243 243 243.',
      body: () => h('div', 'cg-dialog-row', null, [holder, col(h('p', null, { text: 'The same dialog as its own fixed-size window, with a solid frame and no minimize or maximize buttons, as Windows draws a dialog box.' }), openBtn)]),
    })];
  }

  const PAGES = [
    { id: 'buttons', label: 'Buttons', build: pageButtons },
    { id: 'text', label: 'Text and choices', build: pageText },
    { id: 'lists', label: 'Lists', build: pageLists },
    { id: 'ranges', label: 'Ranges and scroll bars', build: pageRanges },
    { id: 'bars', label: 'Bars and menus', build: pageBars },
    { id: 'dates', label: 'Dates', build: pageDates },
    { id: 'dialog', label: 'Sample dialog', build: pageDialog },
  ];

  /* ---------- the gallery window ---------- */
  function setClassicDark(dark) {
    if (!G) return;
    G.dark = !!dark;
    G.root.dataset.classic = G.dark ? 'dark' : 'light';
    G.toggle.checked = G.dark;
    G.root.querySelectorAll('[data-dark="follows"], .w32-menu').forEach((el) => el.classList.toggle('w32-dark', G.dark));
  }

  function openControls(args = {}) {
    if (G) {
      if (G.win.isMinimized()) G.win.restore();
      G.win.focus();
      if (args.page) G.show(args.page);
      if (args.dark != null) { G.followSystem = false; setClassicDark(args.dark); }
      return G.win;
    }
    if (!Shell.wm?.open) { console.warn('[controls] Shell.wm is missing'); return null; }
    const tabs = h('div', 'segmented cg-pages', { role: 'tablist', 'aria-label': 'Control groups' },
      PAGES.map((p) => h('button', null, { type: 'button', role: 'tab', 'data-page': p.id, text: p.label })));
    const toggle = h('input', null, { type: 'checkbox', role: 'switch', 'aria-label': 'Dark mode for classic programs' });
    const theme = h('div', 'cg-theme', null, [h('span', null, { text: 'Dark mode' }),
      h('label', 'toggle', null, [toggle, h('span', 'toggle-track', null, [h('span', 'toggle-thumb')]), h('span', 'toggle-text')])]);
    const scroll = h('div', 'cg-scroll', null, [h('p', 'cg-intro', { text: 'Win32 controls as Windows 11 draws them in a classic program. Captions name the msstyles part and the Wine theme section that draws it; Dark mode shows which controls Windows lets follow it.' })]);
    const root = h('div', 'cg', { 'data-classic': 'light' }, [h('div', 'cg-top', null, [tabs, theme]), scroll]);

    const built = new Map();
    const g = { root, toggle, dark: false, followSystem: true, cleanups: [], page: null, win: null };
    G = g;
    g.show = (id) => {
      const p = PAGES.find((x) => x.id === id) || PAGES[0];
      for (const [k, el] of built) el.hidden = k !== p.id;
      if (!built.has(p.id)) {
        const el = h('div', 'cg-page', { 'data-page': p.id });
        el.append(...p.build(p));
        built.set(p.id, el);
        scroll.append(el);
        setClassicDark(g.dark);
      }
      tabs.querySelectorAll('[data-page]').forEach((b) => { const on = b.dataset.page === p.id; b.classList.toggle('selected', on); b.setAttribute('aria-selected', String(on)); });
      const sel = tabs.querySelector('.selected'); // keep the selected page tab in view when the bar scrolls
      if (sel && (sel.offsetLeft < tabs.scrollLeft || sel.offsetLeft + sel.offsetWidth > tabs.scrollLeft + tabs.clientWidth)) tabs.scrollLeft = sel.offsetLeft - 8;
      if (g.page !== p.id) scroll.scrollTop = 0;
      g.page = p.id;
      p.onShow?.();
    };
    tabs.addEventListener('select', (e) => g.show(e.detail.item.dataset.page));
    toggle.addEventListener('change', () => { g.followSystem = false; setClassicDark(toggle.checked); });
    scroll.addEventListener('wheel', () => Shell.transient.closeAll('menu'), { passive: true }); // popups do not follow their scrolled anchors
    g.cleanups.push(Shell.on('setting', ({ key, value }) => { if (key === 'theme' && g.followSystem) setClassicDark(value === 'dark'); }));

    g.win = Shell.wm.open({
      appId: 'controls', title: 'Windows controls', icon: 'object/app-controls', width: 1120, height: 780, minWidth: 360, minHeight: 320, content: root,
      onClose: () => { g.cleanups.forEach((f) => f()); hideTip(); Shell.transient.closeAll('menu'); if (G === g) G = null; },
    });
    if (Shell.wm.workArea().width < 600) g.win.maximize(); // phones: maximized, as Windows does on small tablets
    setClassicDark(args.dark ?? Shell.settings.theme === 'dark');
    if (args.dark != null) g.followSystem = false;
    g.show(args.page);
    return g.win;
  }

  Shell.apps.register({ id: 'controls', name: 'Windows controls', icon: 'object/app-controls', description: 'Win32 common controls as Windows 11 draws them in classic programs', open: openControls });

  Shell.on('boot', () => setTimeout(() => {
    const want = Shell.debugOpen.filter((n) => n === 'controls' || n.startsWith('controls-'));
    if (!want.length) return;
    const page = want.map((n) => n.slice('controls-'.length)).find((p) => PAGES.some((x) => x.id === p));
    openControls({ page, dark: want.includes('controls-dark') ? true : undefined });
  }, 50));
})();
