/* Shell dialogs (owner: dialogs): the Win32 MessageBox and TaskDialog, Run (Win+R), property
   sheets, Open with, the common Open/Save dialog, the UAC consent prompt on the secure desktop,
   and the file-operation engine with its progress, "Replace or Skip Files" and "Destination
   Folder Access Denied" dialogs.
   Every dialog is a window from Shell.wm.open, modal to its owner where Windows makes it modal
   (the owner is covered by a blocker; a click on it flashes the dialog's frame). UAC is the one
   exception: it lives on the secure desktop, an overlay over the frozen, dimmed screen.
   Classic comctl32 dialogs stay light in dark mode, as Windows 11 23H2 draws them; Open with,
   the file dialog and UAC follow the theme. Every function returns a Promise.
   Contract: CONTRACT.md "dialogs". */
(() => {
  'use strict';

  const Shell = window.Shell;
  const D = (Shell.dialogs = Shell.dialogs || {});

  /* ---------- small helpers ---------- */
  const PROPS = ['value', 'checked', 'disabled', 'type', 'tabIndex', 'htmlFor', 'readOnly', 'id', 'title', 'placeholder'];
  const h = (tag, cls, attrs, ...kids) => {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v == null || v === false) continue;
      if (k === 'text') n.textContent = v;
      else if (typeof v === 'function') n.addEventListener(k.slice(2), v);
      else if (PROPS.includes(k)) n[k] = v;
      else n.setAttribute(k, v === true ? '' : v);
    }
    for (const kid of kids.flat(Infinity)) if (kid != null && kid !== false) n.append(kid);
    return n;
  };
  const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), Math.max(lo, hi));
  const area = () => Shell.wm?.workArea?.() || { width: innerWidth, height: innerHeight - 48 };
  const get = (id) => Shell.fs?.get?.(id) || null;
  const kids = (id) => Shell.fs?.children?.(id) || [];
  const alive = (win) => !!win && !!Shell.wm?.all?.().includes(win);
  const winOf = (node) => (node instanceof Node && Shell.wm?.all?.().find((w) => w.el.contains(node))) || null;
  const icon = (name, size = 16, className = '') => Shell.icon(name, { size, className });
  const ICON = {
    info: 'object/dialog-info', warning: 'object/dialog-warning', error: 'object/dialog-error',
    question: 'object/dialog-question', shield: 'object/dialog-shield',
  };
  let seq = 0;

  /* ---------- names, types, paths and sizes as Explorer prints them ---------- */
  const TYPES = { txt: 'Text Document', png: 'PNG File', jpg: 'JPG File', pdf: 'PDF File', xlsx: 'XLSX File',
    zip: 'Compressed (zipped) Folder', gz: 'GZ File', exe: 'Application', lnk: 'Shortcut' };
  const hidesExt = (it) => it?.kind === 'file' && !!it.ext && it.name.length > it.ext.length + 1
    && it.name.toLowerCase().endsWith(`.${it.ext.toLowerCase()}`);
  const shown = (it) => (hidesExt(it) ? it.name.slice(0, -(it.ext.length + 1)) : it?.name || '');
  const typeOf = (it) => ({ folder: 'File folder', drive: 'Local Disk', system: 'System Folder', shortcut: 'Shortcut' }[it.kind]
    || TYPES[it.ext] || (it.ext ? `${it.ext.toUpperCase()} File` : 'File'));
  const isContainer = (it) => !!it && (it.kind === 'folder' || it.kind === 'drive' || it.id === 'root');
  const iconOf = (it) => {
    if (!it) return 'object/folder';
    if (it.id === 'recycle') return kids('recycle').length ? 'object/recycle-full' : 'object/recycle-empty';
    if (it.icon) return it.icon;
    return it.kind === 'folder' ? 'object/folder' : 'object/document';
  };
  const driveLetter = (it) => it?.name.match(/\(([A-Z]:)\)/)?.[1] || '';
  function osPath(id) {
    const special = { home: 'Home', root: 'This PC', recycle: 'Recycle Bin' };
    if (special[id]) return special[id];
    const parts = [];
    for (const it of Shell.fs?.path?.(id) || []) {
      if (it.id === 'root') continue;
      if (it.kind === 'drive') parts.push(driveLetter(it) || it.name);
      else if (it.known) parts.push('C:', 'Users', 'Mihawk', it.name);
      else parts.push(it.name);
    }
    const p = parts.join('\\');
    return /^[A-Z]:$/.test(p) ? `${p}\\` : p;
  }
  function walk(id, out = []) {
    kids(id).forEach((c) => { out.push(c); if (isContainer(c)) walk(c.id, out); });
    return out;
  }
  const SHELL_FOLDERS = { desktop: 'desktop', downloads: 'downloads', personal: 'documents', documents: 'documents',
    'my pictures': 'pictures', pictures: 'pictures', 'my music': 'music', music: 'music', 'my video': 'videos',
    videos: 'videos', recyclebinfolder: 'recycle', mycomputerfolder: 'root', profile: 'home', 'this pc': 'root' };
  /* A typed location -> item id: a full path, a shell: folder, a drive or a folder name. */
  function resolvePath(text, { files = false } = {}) {
    const want = text.trim().replace(/^"|"$/g, '').replace(/[\\/]+$/, '').replace(/\//g, '\\').toLowerCase();
    if (!want) return null;
    if (want.startsWith('shell:')) return SHELL_FOLDERS[want.slice(6)] || null;
    if (SHELL_FOLDERS[want]) return SHELL_FOLDERS[want];
    if (['.', '~', '%userprofile%', 'c:\\users\\mihawk'].includes(want)) return 'home';
    if (want === 'recycle bin') return 'recycle';
    const all = walk('root').filter((c) => files || isContainer(c));
    const norm = (p) => p.toLowerCase().replace(/\\$/, '');
    const hit = all.find((c) => norm(osPath(c.id)) === want)
      || (files && all.find((c) => c.kind !== 'folder' && norm(osPath(c.parent) + '\\' + shown(c)) === want))
      || all.find((c) => c.name.toLowerCase() === want || shown(c).toLowerCase() === want);
    return hit?.id || null;
  }
  const bytes = (n) => `${Math.round(n || 0).toLocaleString('en-US')} bytes`;
  /* Windows' three significant digits, truncated: 2.08 KB, 20.8 KB, 208 KB, 0.97 MB. */
  function sig3(n) {
    n = n || 0;
    if (n < 1024) return bytes(n);
    const u = ['KB', 'MB', 'GB', 'TB'];
    let v = n / 1024, i = 0;
    while (v >= 1000 && i < u.length - 1) { v /= 1024; i++; }
    const d = v >= 100 ? 0 : v >= 10 ? 1 : 2;
    return `${(Math.floor(v * 10 ** d) / 10 ** d).toFixed(d)} ${u[i]}`;
  }
  const sizeLine = (n) => (n < 1024 ? bytes(n) : `${sig3(n)} (${bytes(n)})`);
  const onDisk = (n) => Math.ceil((n || 0) / 4096) * 4096;
  const fmtKB = (n) => `${Math.ceil((n || 0) / 1024).toLocaleString('en-US')} KB`;
  const longDate = (ms) => {
    const d = new Date(ms);
    return `${d.toLocaleDateString('en-US', { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' })}, ${d.toLocaleTimeString('en-US')}`;
  };
  const shortDate = (ms) => (ms ? new Date(ms).toLocaleString('en-US', { month: 'numeric', day: 'numeric', year: 'numeric', hour: 'numeric', minute: '2-digit' }).replace(',', '') : '');
  function treeSize(it) {
    if (!it) return 0;
    if (it.kind === 'folder' || it.kind === 'drive') return kids(it.id).reduce((s, c) => s + treeSize(c), 0);
    return it.size || 0;
  }
  const counts = (id) => walk(id).reduce((c, it) => (isContainer(it) ? c.folders++ : c.files++, c), { files: 0, folders: 0 });

  /* ---------- windows: placement, the classic look, modality and the flash ---------- */
  const owned = new Map(); // owner window -> its modal dialogs, top last
  const ownerOf = new Map();
  const forced = new WeakSet(); // closed with their owner: no veto
  const flashing = new Map();

  function measure(node, width) {
    const host = h('div', 'dlg-measure');
    host.style.width = `${width}px`;
    document.body.append(host);
    host.append(node);
    const b = node.getBoundingClientRect();
    const r = { w: Math.ceil(b.width), h: Math.ceil(b.height) };
    host.remove();
    return r;
  }
  const textWidth = (text, cls = 'dlg-measure-text') => measure(h('span', cls, { text }), 4000).w;

  /* -> {win, done, finish(value)}. `done` resolves with the value given to finish(), or with
     o.dismiss (default null) when the window is closed some other way. o.canClose() -> false
     vetoes a close that is not a finish (Windows' MessageBox without Cancel). */
  function dialog(o) {
    let resolve, settled = false, value = o.dismiss ?? null;
    const done = new Promise((r) => { resolve = r; });
    if (!Shell.wm?.open) { resolve(value); return { win: null, done, finish() {} }; }
    const wa = area();
    const owner = alive(o.owner) ? o.owner : null;
    const classic = o.classic !== false;
    o.content.classList.add('dlg');
    if (classic) o.content.dataset.theme = 'light';
    const w = Math.min(o.width, wa.width);
    const ht = Math.min(o.height ?? measure(o.content, w - 2).h + 34, wa.height);
    let { x, y } = o;
    if (x == null || y == null) {
      const r = owner && !owner.isMinimized?.() ? owner.el.getBoundingClientRect() : { left: 0, top: 0, width: wa.width, height: wa.height };
      x = Math.round(clamp(r.left + (r.width - w) / 2, 0, wa.width - w));
      y = Math.round(clamp(r.top + (r.height - ht) / 2, 0, wa.height - ht));
    }
    const appId = o.appId || owner?.appId || 'dialog';
    /* The window's icon is what the taskbar and Alt+Tab show: the program's colour icon, never the
       message icon or a line glyph (the caption icon is hidden unless o.captionIcon). */
    const win = Shell.wm.open({
      appId, title: o.title || '', icon: o.icon || Shell.apps.get(appId)?.icon || owner?.icon || 'object/app-generic',
      width: w, height: ht, x, y, content: o.content, titlebarContent: o.titlebarContent,
      minWidth: o.minWidth ?? Math.min(w, 200), minHeight: o.minHeight ?? Math.min(ht, 120),
      resizable: !!o.resizable, maximizable: false, minimizable: !!o.minimizable, background: o.background || 'solid',
      onClose: () => {
        if (!settled && !forced.has(win) && o.canClose && !o.canClose()) { flash(win); return false; }
        win.dlgHadFocus = win.isFocused?.();
        settled = true;
        o.onDismiss?.();
        resolve(value);
        return undefined;
      },
    });
    win.el.classList.add('dlg-window');
    if (!o.captionIcon) win.el.classList.add('dlg-noicon');
    const frame = win.el.querySelector(':scope > .window-frame');
    if (classic && frame) { frame.dataset.theme = 'light'; frame.classList.add('dlg-frame'); }
    if (owner && o.modal !== false) makeModal(win, owner);
    return { win, done, finish(v) { value = v; settled = true; win.close(); } };
  }

  /* The disabled owner (EnableWindow FALSE): its frame goes inert so nothing in it can take focus
     (an app that refocuses its editor on activation would otherwise fight the dialog), and a
     blocker over it turns presses into a flash of the dialog. */
  const ownerFocus = new Map();
  function makeModal(win, owner) {
    ownerOf.set(win, owner);
    owned.set(owner, [...(owned.get(owner) || []), win]);
    if (owner.el.querySelector(':scope > .dlg-blocker')) return;
    const frame = owner.el.querySelector(':scope > .window-frame');
    const had = document.activeElement;
    if (frame?.contains(had)) ownerFocus.set(owner, had);
    if (frame) frame.inert = true;
    const b = h('div', 'dlg-blocker', { 'aria-hidden': 'true' });
    const swallow = (e) => { e.preventDefault(); e.stopPropagation(); };
    ['contextmenu', 'dblclick', 'click', 'wheel'].forEach((t) => b.addEventListener(t, swallow, { passive: false }));
    owner.el.append(b);
  }
  const topOf = (owner) => { const k = owned.get(owner); return k?.[k.length - 1] || null; };
  const deepest = (owner) => { let t = topOf(owner); while (t && topOf(t)) t = topOf(t); return t; };
  function release(win) {
    const owner = ownerOf.get(win);
    (owned.get(win) || []).slice().forEach((k) => { forced.add(k); k.close(); });
    if (!owner) return;
    ownerOf.delete(win);
    const rest = (owned.get(owner) || []).filter((k) => k !== win);
    if (rest.length) owned.set(owner, rest);
    else {
      owned.delete(owner);
      owner.el.querySelector(':scope > .dlg-blocker')?.remove();
      const frame = owner.el.querySelector(':scope > .window-frame');
      if (frame) frame.inert = false;
    }
    /* Windows hands activation back to the owner (or the next dialog on it), to the control that had focus. */
    if (!win.dlgHadFocus) return;
    setTimeout(() => {
      const next = rest[rest.length - 1] || owner;
      if (!alive(next) || next.isMinimized()) return;
      next.focus();
      const back = !rest.length && ownerFocus.get(owner);
      ownerFocus.delete(owner);
      if (back?.isConnected) back.focus({ preventScroll: true });
    });
  }
  /* The disabled owner: a press on it activates and flashes its dialog (the ding is silent). */
  window.addEventListener('pointerdown', (e) => {
    const b = e.target.closest?.('.dlg-blocker');
    if (!b) return;
    e.preventDefault();
    e.stopPropagation();
    const owner = [...owned.keys()].find((o) => o.el === b.parentElement);
    const top = owner && deepest(owner);
    if (!top) return;
    if (top.isMinimized()) top.restore();
    top.focus();
    flash(top);
  }, true);
  Shell.on('window', ({ type, win }) => {
    if (type === 'close') { release(win); return; }
    const k = owned.get(win);
    if (!k?.length) return;
    if (type === 'focus') setTimeout(() => { const top = deepest(win); if (alive(top) && win.isFocused()) top.focus(); });
    else if (type === 'minimize') k.forEach((d) => d.minimize());
    else if (type === 'restore' && !win.isMinimized()) k.forEach((d) => d.isMinimized() && d.restore());
    else if (type === 'desktop') k.forEach((d) => Shell.wm.moveToDesktop?.(d, win.desktop));
  });
  /* FlashWindow: the frame and title bar blink between active and inactive four times. */
  function flash(win) {
    if (!win?.el) return;
    clearInterval(flashing.get(win));
    let n = 0;
    win.el.classList.add('dlg-flashing');
    const t = setInterval(() => {
      n++;
      win.el.classList.toggle('dlg-flash', n % 2 === 1 && n < 8);
      if (n >= 8) { clearInterval(t); flashing.delete(win); win.el.classList.remove('dlg-flashing'); }
    }, 70);
    flashing.set(win, t);
  }
  /* A close the dialog does not allow: the caption button greys out, as with MB_YESNO. */
  const disableClose = (win) => { const b = win?.el.querySelector('.window-caption-btn.close'); if (b) b.disabled = true; };

  /* ---------- classic controls ---------- */
  /* '&Yes' -> Y underlined while keyboard cues show; the letter presses it. */
  function mnemonic(label, holder) {
    const i = label.indexOf('&');
    if (i < 0 || i === label.length - 1) return [label];
    holder.dataset.key = label[i + 1].toLowerCase();
    return [label.slice(0, i), h('u', null, { text: label[i + 1] }), label.slice(i + 2)];
  }
  const plain = (label) => label.replace('&', '');
  function btn(label, { def, cls = '', on, disabled, shield } = {}) {
    const b = h('button', `dlg-btn ${cls}`.trim(), { type: 'button', disabled });
    if (shield) b.append(icon(ICON.shield, 16, 'dlg-btn-shield'));
    b.append(h('span', null, null, ...mnemonic(label, b)));
    if (def) b.dataset.default = '';
    if (on) b.addEventListener('click', on);
    return b;
  }
  function check(label, { checked, indeterminate, disabled, on, radio, name } = {}) {
    const input = h('input', radio ? 'dlg-radio' : 'dlg-check', { type: radio ? 'radio' : 'checkbox', checked: !!checked, disabled, name });
    if (indeterminate) input.indeterminate = true;
    const l = h('label', 'dlg-checkline');
    l.append(input, h('span', null, null, ...mnemonic(label, l)));
    if (on) input.addEventListener('change', () => on(input.checked));
    l.input = input;
    return l;
  }
  const edit = (value = '', attrs = {}) => h('input', 'dlg-edit', { type: 'text', spellcheck: 'false', value, ...attrs });

  /* The classic drop-down list under a combo box: square, 1 px border, Highlight selection,
     slides open; Up/Down/Home/End move, Enter picks, Esc/Tab/F4/Alt+Up close. */
  function dropList(anchor, items, current, onPick, onClose) {
    const id = `dlg-dd-${++seq}`;
    const list = h('div', 'dlg dlg-dropdown', { role: 'listbox' });
    const scope = anchor.closest('[data-theme]');
    if (scope && scope !== document.documentElement) list.dataset.theme = scope.dataset.theme;
    let hi = Math.max(0, items.findIndex((i) => i.value === current));
    const set = (i) => {
      hi = clamp(i, 0, items.length - 1);
      [...list.children].forEach((r, k) => r.classList.toggle('hi', k === hi));
      list.children[hi]?.scrollIntoView({ block: 'nearest' });
    };
    items.forEach((it, i) => {
      const row = h('div', 'dlg-dd-item', { role: 'option', text: it.label });
      row.addEventListener('pointerenter', () => set(i));
      row.addEventListener('click', () => { onPick(it.value); close(); });
      list.append(row);
    });
    list.addEventListener('mousedown', (e) => e.preventDefault());
    Shell.layer('menus').append(list);
    const r = anchor.getBoundingClientRect();
    const room = innerHeight - r.bottom - 8;
    list.style.minWidth = `${Math.round(r.width)}px`;
    list.style.maxHeight = `${Math.max(120, Math.min(30 * 20 + 2, Math.max(room, r.top - 8)))}px`;
    const below = list.offsetHeight <= room || room >= r.top;
    list.style.left = `${Math.round(clamp(r.left, 4, innerWidth - list.offsetWidth - 4))}px`;
    list.style.top = `${Math.round(below ? r.bottom : r.top - list.offsetHeight)}px`;
    set(hi);
    Shell.animate(list, [{ clipPath: below ? 'inset(0 0 100% 0)' : 'inset(100% 0 0 0)' }, { clipPath: 'inset(0 0 0 0)' }], { duration: 167 });
    const key = (e) => {
      const k = e.key;
      if (k === 'ArrowDown' && !e.altKey) set(hi + 1);
      else if (k === 'ArrowUp' && !e.altKey) set(hi - 1);
      else if (k === 'Home' || k === 'PageUp') set(k === 'Home' ? 0 : hi - 10);
      else if (k === 'End' || k === 'PageDown') set(k === 'End' ? items.length - 1 : hi + 10);
      else if (k === 'Enter') { onPick(items[hi]?.value); close(); }
      else if (k === 'Escape' || k === 'Tab' || k === 'F4' || (e.altKey && k === 'ArrowUp')) close();
      else return;
      e.preventDefault();
      e.stopPropagation();
    };
    window.addEventListener('keydown', key, true);
    function close() { Shell.transient.close(id); }
    Shell.transient.open(id, {
      group: 'menu', elements: [list, anchor],
      close: () => { window.removeEventListener('keydown', key, true); list.remove(); onClose?.(); },
    });
    return { close };
  }
  /* Editable combo box (Run's Open:, File name:): an edit with the drop-down button. */
  function editCombo(input, items) {
    const wrap = h('span', 'dlg-combo');
    const arrow = h('button', 'dlg-combo-btn', { type: 'button', tabIndex: -1, 'aria-label': 'Open the list' }, icon('fluent/chevron-down', 12));
    wrap.append(input, arrow);
    let dd = null;
    const open = () => {
      if (dd) { dd.close(); return; }
      const list = items();
      if (!list.length) return;
      wrap.classList.add('open');
      dd = dropList(wrap, list.map((v) => ({ value: v, label: v })), input.value, (v) => {
        if (v == null) return;
        input.value = v;
        input.dispatchEvent(new Event('input', { bubbles: true }));
        input.select();
      }, () => { dd = null; wrap.classList.remove('open'); });
    };
    arrow.addEventListener('mousedown', (e) => e.preventDefault());
    arrow.addEventListener('click', () => { input.focus(); open(); });
    input.addEventListener('keydown', (e) => {
      if ((e.altKey && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) || e.key === 'F4') { e.preventDefault(); open(); }
      else if (!dd && !e.altKey && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) {
        const list = items();
        if (!list.length) return;
        e.preventDefault();
        const i = list.indexOf(input.value);
        input.value = list[clamp(i + (e.key === 'ArrowDown' ? 1 : -1), 0, list.length - 1)] ?? list[0];
        input.dispatchEvent(new Event('input', { bubbles: true }));
        input.select();
      }
    });
    wrap.input = input;
    return wrap;
  }
  /* Drop-down list combo box (CBS_DROPDOWNLIST): looks like a field, opens the same list. */
  function selectBox(options, value, onChange) {
    let cur = value ?? options[0]?.value;
    const b = h('button', 'dlg-select', { type: 'button', role: 'combobox', 'aria-haspopup': 'listbox' });
    const txt = h('span', 'dlg-select-text');
    b.append(txt, icon('fluent/chevron-down', 12, 'dlg-select-chev'));
    const render = () => { txt.textContent = options.find((o) => o.value === cur)?.label ?? ''; };
    const pick = (v) => { if (v === undefined || v === cur) return; cur = v; render(); onChange?.(v); };
    let dd = null;
    const open = () => {
      if (dd) { dd.close(); return; }
      b.classList.add('open');
      dd = dropList(b, options, cur, pick, () => { dd = null; b.classList.remove('open'); });
    };
    b.addEventListener('click', open);
    b.addEventListener('keydown', (e) => {
      if (dd) return;
      if ((e.altKey && e.key === 'ArrowDown') || e.key === 'F4') { e.preventDefault(); open(); }
      else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        const i = options.findIndex((o) => o.value === cur);
        pick(options[clamp(i + (e.key === 'ArrowDown' ? 1 : -1), 0, options.length - 1)]?.value);
      }
    });
    render();
    return { el: b, get value() { return cur; }, set(v) { cur = v; render(); } };
  }
  /* Win32 dialog keyboard: Enter presses the default push button (the focused one, else the
     dialog's), Esc runs onEscape, a mnemonic letter (Alt+letter, or the letter alone outside a
     text field) presses its control, Alt shows the underlines, Ctrl+PgUp/PgDn switch tabs. */
  function dialogKeys(root, { onEscape } = {}) {
    const mark = () => {
      const f = document.activeElement?.closest?.('.dlg-btn');
      const target = f && root.contains(f) ? f : root.querySelector('.dlg-btn[data-default]');
      root.querySelectorAll('.dlg-btn.is-default').forEach((b) => { if (b !== target) b.classList.remove('is-default'); });
      target?.classList.add('is-default');
    };
    root.addEventListener('focusin', mark);
    root.addEventListener('focusout', () => setTimeout(mark));
    mark();
    root.addEventListener('keydown', (e) => {
      if (e.defaultPrevented) return;
      const t = e.target;
      if (e.key === 'Alt') { root.classList.add('dlg-cues'); return; }
      if (e.key === 'Tab' || e.key.startsWith('Arrow')) root.classList.add('dlg-cues');
      /* The push buttons of a button row are one dialog group: arrows move focus (and the
         default ring) through it and wrap, as GetNextDlgGroupItem does. */
      if (e.key.startsWith('Arrow') && !e.altKey && !e.ctrlKey && !e.shiftKey && t.matches?.('.dlg-foot .dlg-btn')) {
        const group = [...t.closest('.dlg-foot').querySelectorAll('.dlg-btn')].filter((b) => !b.disabled && b.checkVisibility());
        const d = e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : -1;
        group[(group.indexOf(t) + d + group.length) % group.length]?.focus();
        e.preventDefault();
        return;
      }
      if (e.key === 'Enter' && !e.altKey && !e.ctrlKey) {
        if (t.tagName === 'TEXTAREA') return;
        if (t.matches?.('button:not([role="tab"]), a, [role="button"]')) { e.preventDefault(); if (!t.disabled) t.click(); return; }
        const b = root.querySelector('.dlg-btn.is-default');
        if (b && !b.disabled) { e.preventDefault(); b.click(); }
      } else if (e.key === 'Escape') {
        if (onEscape) { e.preventDefault(); onEscape(); }
      } else if (e.ctrlKey && (e.key === 'PageDown' || e.key === 'PageUp')) {
        const tabs = [...root.querySelectorAll('.dlg-tab')];
        const i = tabs.findIndex((x) => x.classList.contains('sel'));
        const to = tabs[(i + (e.key === 'PageDown' ? 1 : tabs.length - 1)) % tabs.length];
        if (to) { e.preventDefault(); to.click(); to.focus(); }
      } else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey
        && (e.altKey || !t.matches?.('input:not([type="checkbox"]):not([type="radio"]), textarea, .dlg-list'))) {
        const k = e.key.toLowerCase();
        const hit = [...root.querySelectorAll('[data-key]')].find((x) => x.dataset.key === k
          && !x.disabled && !x.input?.disabled && x.checkVisibility({ visibilityProperty: true }));
        if (!hit) return;
        e.preventDefault();
        root.classList.add('dlg-cues');
        if (hit.tagName === 'LABEL' && hit.htmlFor) document.getElementById(hit.htmlFor)?.focus();
        else if (hit.tagName === 'LABEL') { hit.input?.focus(); hit.input?.click(); }
        else { hit.focus(); hit.click(); }
      }
    });
  }
  /* A property sheet: classic tabs over a white page; every page shares the tallest page's size. */
  function sheet(pages, start = 0) {
    const el = h('div', 'dlg-sheet');
    const strip = h('div', 'dlg-tabs', { role: 'tablist' });
    const pane = h('div', 'dlg-tabpane');
    let cur = 0;
    const tabs = pages.map((p, i) => {
      const t = h('button', 'dlg-tab', { type: 'button', role: 'tab', tabIndex: -1, text: p.label, onclick: () => select(i) });
      t.addEventListener('keydown', (e) => {
        if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') {
          e.preventDefault();
          select((cur + (e.key === 'ArrowRight' ? 1 : pages.length - 1)) % pages.length, true);
        }
      });
      strip.append(t);
      p.node.classList.add('dlg-page');
      pane.append(p.node);
      return t;
    });
    function select(i, focus) {
      cur = i;
      tabs.forEach((t, k) => { t.classList.toggle('sel', k === i); t.setAttribute('aria-selected', String(k === i)); t.tabIndex = k === i ? 0 : -1; });
      pages.forEach((p, k) => p.node.classList.toggle('shown', k === i));
      if (focus) tabs[i].focus();
    }
    select(start);
    el.append(strip, pane);
    return el;
  }
  const hr = () => h('div', 'dlg-hr');
  const row = (label, ...value) => [h('div', 'dlg-k', { text: label }), h('div', 'dlg-v', null, ...value)];
  /* Window-name characters Windows refuses, with its balloon tip. */
  const BAD = /[\\/:*?"<>|]/g;
  function guardName(input) {
    input.addEventListener('input', () => {
      if (!BAD.test(input.value)) return;
      const at = input.selectionStart;
      input.value = input.value.replace(BAD, '');
      input.setSelectionRange(at - 1, at - 1);
      Shell.tooltip?.show?.(input, 'A file name can\'t contain any of the following characters:\n\\ / : * ? " < > |', { placement: 'bottom' });
      setTimeout(() => Shell.tooltip?.hide?.(), 3000);
    });
  }

  /* ---------- MessageBox ---------- */
  const MNEMONICS = { Yes: '&Yes', No: '&No', Abort: '&Abort', Retry: '&Retry', Ignore: '&Ignore', 'Try Again': '&Try Again', Continue: '&Continue' };
  /* `app` (optional, an app id): the program an ownerless box belongs to; the taskbar groups it there. */
  D.messageBox = ({ title = '', text = '', icon: kind, buttons = ['OK'], defaultButton, owner, app } = {}) => {
    const wa = area();
    const iconName = ICON[kind];
    const root = h('div', 'dlg-msg');
    const textEl = h('div', 'dlg-msg-text', { text: String(text) });
    root.append(h('div', 'dlg-msg-body', null, iconName && icon(iconName, 32, 'dlg-msg-icon'), textEl));
    const def = buttons.includes(defaultButton) ? defaultButton : buttons[0];
    const cancel = buttons.includes('Cancel') ? 'Cancel' : buttons.length === 1 ? buttons[0] : null;
    const btns = buttons.map((b) => btn(MNEMONICS[b] || b, { def: b === def, on: () => s.finish(b) }));
    root.append(h('div', 'dlg-foot dlg-msg-foot', null, btns));
    /* Width: the text unwrapped up to 5/8 of the screen, never narrower than the buttons or the caption. */
    const iconW = iconName ? 42 : 0;
    const textW = Math.max(...String(text).split('\n').map((l) => textWidth(l)));
    const maxText = Math.max(160, Math.round(wa.width * 5 / 8) - 54 - iconW);
    const width = Math.ceil(Math.max(Math.min(textW, maxText) + iconW + 54 + 2, buttons.length * 98 + 14 + 2,
      textWidth(title) + 48 + 46 + 24, 180));
    const s = dialog({ title, appId: app, owner, width, content: root, canClose: () => !!cancel || buttons.length === 1 });
    if (!s.win) return s.done;
    if (!cancel && buttons.length > 1) disableClose(s.win);
    dialogKeys(root, { onEscape: () => { if (cancel) s.finish(cancel); } });
    root.addEventListener('keydown', (e) => {
      if (!e.ctrlKey || !['c', 'C', 'Insert'].includes(e.key)) return;
      e.preventDefault();
      const line = '---------------------------';
      try { navigator.clipboard?.writeText(`${line}\n${title}\n${line}\n${text}\n${line}\n${buttons.join('   ')}   \n${line}\n`).catch(() => {}); } catch { /* no clipboard */ }
    });
    btns[buttons.indexOf(def)]?.focus();
    return s.done;
  };

  /* ---------- TaskDialog ---------- */
  D.taskDialog = ({ title = '', instruction = '', content = '', icon: kind, commandLinks = [], buttons, checkbox, owner, defaultButton, app } = {}) => {
    const list = buttons || (commandLinks.length ? [] : ['OK']); // command links alone: no button row, as the hang dialog
    const iconName = ICON[kind];
    const root = h('div', 'dlg-task');
    const col = h('div', 'dlg-task-col');
    if (instruction) col.append(h('div', 'dlg-task-instr', { text: instruction }));
    if (content) col.append(h('div', 'dlg-task-content', { text: content }));
    let chk = null;
    const result = (button) => ({ button, checked: !!chk?.input.checked });
    const links = commandLinks.map((c) => h('button', 'dlg-cmdlink', { type: 'button', onclick: () => s.finish(result(c.label)) },
      icon('fluent/arrow-right', 16, 'dlg-cmdlink-arrow'),
      h('span', 'dlg-cmdlink-text', null, h('span', 'dlg-cmdlink-label', { text: c.label }), c.note && h('span', 'dlg-cmdlink-note', { text: c.note })),
    ));
    if (links.length) col.append(h('div', 'dlg-task-links', null, links));
    root.append(h('div', 'dlg-task-main', null, iconName && icon(iconName, 32, 'dlg-task-icon'), col));
    const def = list.includes(defaultButton) ? defaultButton : list[0];
    const btns = list.map((b) => btn(MNEMONICS[b] || b, { def: !links.length && b === def, on: () => s.finish(result(b)) }));
    if (checkbox) chk = check(typeof checkbox === 'string' ? checkbox : checkbox.label, { checked: !!checkbox.checked });
    if (chk || btns.length) root.append(h('div', 'dlg-foot dlg-task-foot', null, chk, h('span', 'dlg-fill'), btns));
    const iw = iconName ? 42 : 0;
    const width = clamp(Math.max(textWidth(instruction, 'dlg-measure-instr') + iw + 44, links.length ? 440 : 360), 360, 560);
    const cancel = list.includes('Cancel') ? 'Cancel' : list.length <= 1 ? (list[0] ?? null) : undefined;
    const s = dialog({ title, appId: app, owner, width, content: root, dismiss: { button: null, checked: false } });
    if (!s.win) return s.done;
    dialogKeys(root, { onEscape: () => { if (cancel !== undefined) s.finish(result(cancel)); } });
    root.addEventListener('keydown', (e) => {
      const i = links.indexOf(document.activeElement);
      if (i < 0 || (e.key !== 'ArrowDown' && e.key !== 'ArrowUp')) return;
      e.preventDefault();
      links[clamp(i + (e.key === 'ArrowDown' ? 1 : -1), 0, links.length - 1)].focus();
    });
    (links[0] || btns[list.indexOf(def)])?.focus();
    return s.done;
  };

  /* ---------- Run ---------- */
  const ALIASES = { calc: 'calculator', wt: 'terminal', cmd: 'terminal', powershell: 'terminal', pwsh: 'terminal',
    control: 'settings', 'explorer.exe': 'explorer', 'task manager': 'taskmgr' };
  const MS_SETTINGS = { display: 'system/display', sound: 'system/sound', notifications: 'system/notifications',
    powersleep: 'system/power', storagesense: 'system/storage', about: 'system/about', clipboard: 'system/clipboard',
    multitasking: 'system/multitasking', bluetooth: 'bluetooth', network: 'network', 'network-wifi': 'network/wifi',
    personalization: 'personalization', 'personalization-background': 'personalization/background',
    colors: 'personalization/colors', themes: 'personalization/themes', 'personalization-start': 'personalization/start',
    taskbar: 'personalization/taskbar', appsfeatures: 'apps/installed', defaultapps: 'apps/default', yourinfo: 'accounts',
    dateandtime: 'time/date', gaming: 'gaming', 'gaming-gamemode': 'gaming/game-mode', easeofaccess: 'accessibility',
    privacy: 'privacy', windowsupdate: 'update' };
  /* -> the app to start for a command line, or null: app ids and names, aliases, ms-settings:,
     folders and files. Does not run anything. */
  function commandTarget(raw) {
    const cmd = raw.trim();
    const m = cmd.match(/^"([^"]*)"\s*(.*)$/) || cmd.match(/^(\S+)\s*(.*)$/);
    const head = (m?.[1] || cmd).toLowerCase(), rest = (m?.[2] || '').trim();
    if (head.startsWith('ms-settings:')) {
      const page = head.slice(12);
      return Shell.apps.get('settings') ? { name: 'Settings', run: () => Shell.apps.launch('settings', page ? { page: MS_SETTINGS[page] || 'home' } : {}) } : null;
    }
    const exe = head.replace(/\.exe$/, '');
    const id = ALIASES[head] || ALIASES[exe] || exe;
    const app = Shell.apps.get(id) || Shell.apps.list().find((a) => (a.name || '').toLowerCase() === exe);
    if (app) {
      const args = {};
      if (rest && app.id === 'explorer') args.folder = resolvePath(rest) || undefined;
      if (rest && app.id === 'notepad') { const f = resolvePath(rest, { files: true }); if (f && get(f)?.kind === 'file') args.file = f; }
      return { name: app.name, icon: app.icon, run: () => Shell.apps.launch(app.id, args) };
    }
    const target = resolvePath(cmd, { files: true });
    const it = get(target) || (target === 'home' ? { id: 'home', kind: 'system' } : null);
    if (!it) return null;
    return { name: shown(it), run: () => Shell.fs.open(it.id) };
  }
  let runSession = null;
  /* run({createTask?, owner?}): the shell's Run (Win+R, bottom-left), or with createTask Task
     Manager's "Create new task" (centred and modal on its owner, with the administrator check
     box). Task Manager calls run() bare from its "Run new task" button, so a bare call made from
     inside a Task Manager window gets that variant. Both share Run's history (RunMRU). */
  D.run = (o) => {
    if (o === undefined) {
      const from = winOf(window.event?.target); // the click (or key) that asked, never a stale focus
      o = from?.appId === 'taskmgr' ? { createTask: true, owner: from } : {};
    }
    const create = !!o.createTask, owner = alive(o.owner) ? o.owner : null;
    if (!create && runSession && alive(runSession.win)) { runSession.win.focus(); return runSession.done; }
    const history = Shell.store.get('run-history', []);
    const root = h('div', 'dlg-run');
    const field = edit(history[0] || '', { id: `dlg-run-${++seq}`, 'aria-label': 'Open' });
    const combo = editCombo(field, () => Shell.store.get('run-history', []));
    const label = h('label', 'dlg-run-label', { htmlFor: field.id });
    label.append(...mnemonic('&Open:', label));
    const adminChk = create && check('Create this task with &administrative privileges.');
    const ok = btn('OK', { def: true, on: () => exec(!!adminChk?.input.checked) });
    const cancel = btn('Cancel', { on: () => s.finish(null) });
    const browse = btn('&Browse...', { on: async () => {
      const r = await D.fileOpen({ title: 'Browse', owner: s.win, folder: 'c', filters: [{ label: 'Programs', exts: ['exe', 'lnk', 'bat', 'cmd'] }, { label: 'All Files', exts: ['*'] }] });
      if (!r) { field.focus(); return; }
      field.value = `"${osPath(r.parent)}\\${r.name}"`.replace('\\\\', '\\');
      sync();
      ok.focus();
    } });
    root.append(
      h('div', 'dlg-run-top', null,
        h('div', 'dlg-run-intro', null, icon('object/app-run', 32), h('p', null, { text: 'Type the name of a program, folder, document, or Internet resource, and Windows will open it for you.' })),
        h('div', 'dlg-run-open', null, label, combo),
        adminChk && h('div', 'dlg-run-admin', null, icon(ICON.shield, 16), adminChk)),
      h('div', 'dlg-foot', null, ok, cancel, browse),
    );
    const sync = () => { ok.disabled = !field.value.trim(); };
    field.addEventListener('input', sync);
    sync();
    async function exec(admin) {
      const cmd = field.value.trim();
      if (!cmd) return;
      const t = commandTarget(cmd);
      if (!t) {
        await D.messageBox({ title: cmd, icon: 'error', owner: s.win,
          text: `Windows cannot find '${cmd}'. Make sure you typed the name correctly, and then try again.` });
        if (alive(s.win)) { field.focus(); field.select(); }
        return;
      }
      if (admin && !(await D.uac({ app: t.name, icon: t.icon, publisher: 'Mihawk OS', verified: true }))) return;
      Shell.store.set('run-history', [cmd, ...Shell.store.get('run-history', []).filter((c) => c.toLowerCase() !== cmd.toLowerCase())].slice(0, 26));
      s.finish(cmd);
      t.run();
    }
    root.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && e.ctrlKey && e.shiftKey) { e.preventDefault(); exec(true); }
    }, true);
    const wa = area();
    const width = Math.min(397, wa.width);
    const ht = measure(root, width - 2).h + 34;
    const s = create
      ? dialog({ title: 'Create new task', icon: 'object/app-run', owner, width, content: root, captionIcon: true })
      : dialog({ title: 'Run', icon: 'object/app-run', appId: 'run', width, content: root, captionIcon: true,
        x: Math.min(12, Math.max(0, wa.width - width)), y: Math.max(0, wa.height - ht - 12) });
    if (!create) runSession = s;
    if (!s.win) return s.done;
    dialogKeys(root, { onEscape: () => s.finish(null) });
    field.focus();
    field.select();
    return s.done;
  };
  Shell.keys?.on?.('Win+R', () => { D.run({}); });

  /* ---------- Properties ---------- */
  const propsOpen = new Map(); // item id -> session
  const attrs = () => Shell.store.get('dialogs-attributes', {});
  const defaults = () => Shell.store.get('dialogs-default-apps', {});
  function opensWith(it) {
    const id = defaults()[it.ext] || (it.ext === 'txt' ? 'notepad' : null);
    return id && Shell.apps.get(id) ? Shell.apps.get(id) : null;
  }
  D.properties = (id, { owner } = {}) => {
    const it = get(id);
    if (!it) return Promise.resolve(false);
    if (id === 'root') { Shell.apps.launch('settings', { page: 'system/about' }); return Promise.resolve(false); }
    const open = propsOpen.get(id);
    if (open && alive(open.win)) { open.win.focus(); return open.done; }
    let dirty = false;
    const root = h('div', 'dlg-props');
    const apply = btn('&Apply', { disabled: true, on: () => commit() });
    const touch = () => { dirty = true; apply.disabled = false; };
    const commits = [];
    const recycle = id === 'recycle';
    const title = recycle ? 'Recycle Bin Properties' : `${it.kind === 'drive' ? it.name : shown(it)} Properties`;

    /* The icon + name box heading every General page; renaming happens on Apply/OK. */
    function head(value, fixed) {
      const name = fixed ? h('div', 'dlg-props-name', { text: value }) : edit(value, { 'aria-label': 'Name' });
      if (!fixed) {
        guardName(name);
        name.addEventListener('input', touch);
        commits.push(() => {
          const v = name.value.trim();
          if (!v) return;
          if (it.kind === 'drive') { const L = driveLetter(it); if (v !== it.name.replace(/\s*\([A-Z]:\)$/, '')) Shell.fs.rename(it.id, L ? `${v} (${L})` : v); }
          else if (v !== shown(it)) Shell.fs.rename(it.id, hidesExt(it) ? `${v}.${it.ext}` : v);
        });
      }
      return h('div', 'dlg-props-head', null, icon(iconOf(it), 32), name);
    }
    function attributes(folder) {
      const a = attrs()[id] || {};
      const ro = check('&Read-only', { checked: !!a.ro, indeterminate: folder && a.ro == null, on: touch });
      const hid = check('H&idden', { checked: !!a.hidden, on: touch });
      commits.push(() => Shell.store.set('dialogs-attributes', { ...attrs(), [id]: { ...(attrs()[id] || {}), ro: ro.input.indeterminate ? undefined : ro.input.checked, hidden: hid.input.checked } }));
      const adv = btn('A&dvanced...', { cls: 'dlg-btn-sm', on: () => advancedAttributes(s.win) });
      return row('Attributes:', folder
        ? h('div', 'dlg-attrs folder', null, h('div', null, null, ro, h('span', 'dlg-dim', { text: '(Only applies to files in folder)' })), h('div', null, null, hid, adv))
        : h('div', 'dlg-attrs', null, ro, hid, adv));
    }
    const dates = (created = true) => [
      ...(created ? row('Created:', longDate(it.created || it.modified - 86400000 * 3)) : []),
      ...row('Modified:', longDate(it.modified)),
      ...row('Accessed:', longDate(Date.now())),
    ];
    const grid = (...nodes) => h('div', 'dlg-grid', null, ...nodes);
    const pages = [];

    if (recycle) {
      const drives = kids('root').filter((d) => d.kind === 'drive');
      const listEl = h('div', 'dlg-list dlg-rb-list', { tabIndex: 0, role: 'listbox' },
        h('div', 'dlg-list-head', null, h('span', null, { text: 'Recycle Bin Location' }), h('span', null, { text: 'Space Available' })),
        drives.map((d, i) => h('div', `dlg-list-row${i ? '' : ' sel'}`, { role: 'option' },
          h('span', 'dlg-list-name', null, icon(iconOf(d), 16), d.name), h('span', null, { text: sig3(d.free || 0) }))));
      listEl.addEventListener('click', (e) => {
        const r = e.target.closest('.dlg-list-row');
        if (r) listEl.querySelectorAll('.dlg-list-row').forEach((x) => x.classList.toggle('sel', x === r));
      });
      const rb = Shell.store.get('dialogs-recycle', { custom: true, max: 11253, confirm: false });
      const name = `dlg-rb-${++seq}`;
      const custom = check('&Custom size:', { radio: true, name, checked: rb.custom, on: () => { size.disabled = false; touch(); } });
      const now = check("Don't move files to the &Recycle Bin. Remove files immediately when deleted.", { radio: true, name, checked: !rb.custom, on: () => { size.disabled = true; touch(); } });
      const size = edit(String(rb.max), { 'aria-label': 'Maximum size (MB)', disabled: !rb.custom });
      size.addEventListener('input', () => { size.value = size.value.replace(/\D/g, ''); touch(); });
      const confirm = check('&Display delete confirmation dialog', { checked: rb.confirm, on: touch });
      commits.push(() => Shell.store.set('dialogs-recycle', { custom: custom.input.checked, max: Number(size.value) || 0, confirm: confirm.input.checked }));
      pages.push({ label: 'General', node: h('div', null, null, listEl,
        h('fieldset', 'dlg-group', null, h('legend', null, { text: 'Settings for selected location' }),
          custom, h('div', 'dlg-rb-size', null, h('span', null, { text: 'Maximum size (MB):' }), size), now),
        confirm) });
    } else if (it.kind === 'drive') {
      const used = (it.size || 0) - (it.free || 0);
      const pct = it.size ? used / it.size : 0;
      const L = driveLetter(it);
      const big = (n) => [h('span', 'dlg-num', { text: bytes(n) }), h('span', 'dlg-num-short', { text: sig3(n) })];
      const legend = (cls, label, n) => [h('div', 'dlg-k', null, h('span', `dlg-swatch ${cls}`), label), h('div', 'dlg-v dlg-nums', null, big(n))];
      const ring = h('div', 'dlg-ring', { role: 'img', 'aria-label': `${Math.round(pct * 100)}% used` });
      ring.style.setProperty('--p', `${(pct * 100).toFixed(1)}%`);
      const settingsBtn = (label) => btn(label, { cls: 'dlg-btn-sm', on: () => Shell.apps.get('settings') && Shell.apps.launch('settings', { page: 'system/storage' }) });
      pages.push({ label: 'General', node: h('div', null, null,
        head(it.name.replace(/\s*\([A-Z]:\)$/, '')), hr(),
        grid(...row('Type:', 'Local Disk'), ...row('File system:', 'NTFS')), hr(),
        grid(...legend('used', 'Used space:', used), ...legend('free', 'Free space:', it.free || 0)), hr(),
        grid(h('div', 'dlg-k', { text: 'Capacity:' }), h('div', 'dlg-v dlg-nums', null, big(it.size || 0))),
        h('div', 'dlg-drive-chart', null, h('div', 'dlg-ring-wrap', null, ring, h('div', null, { text: `Drive ${L}` })),
          h('div', 'dlg-drive-btns', null, settingsBtn('Disk Clea&nup'), settingsBtn('Details'))),
        hr(),
        check('Compress this drive to save disk space', { on: touch }),
        check('Allow files on this drive to have contents indexed in addition to file properties', { checked: true, on: touch })) });
    } else if (it.kind === 'folder' || it.kind === 'system') {
      const total = treeSize(it), c = counts(it.id);
      pages.push({ label: 'General', node: h('div', null, null,
        head(shown(it), it.kind === 'system'), hr(),
        grid(...row('Type:', typeOf(it)), ...row('Location:', it.known ? 'C:\\Users\\Mihawk' : it.parent ? osPath(it.parent) : 'Desktop'),
          ...(it.kind === 'folder' ? [...row('Size:', sizeLine(total)), ...row('Size on disk:', sizeLine(onDisk(total))),
            ...row('Contains:', `${c.files.toLocaleString('en-US')} Files, ${c.folders.toLocaleString('en-US')} Folders`)] : [])),
        hr(), grid(...row('Created:', longDate(it.modified - 86400000 * 30))),
        it.kind === 'folder' && hr(), it.kind === 'folder' && grid(...attributes(true))) });
    } else {
      const lnk = it.kind === 'shortcut';
      const size = lnk ? 1372 : it.size || 0;
      const ow = !lnk && it.ext !== 'exe' && opensWith(it);
      const owName = h('span', 'dlg-ow-name');
      const renderOw = (app) => owName.replaceChildren(...(app ? [icon(app.icon || 'object/app-generic', 16), app.name] : ['Pick an app']));
      renderOw(ow);
      const change = btn('&Change...', { cls: 'dlg-btn-sm', on: async () => {
        const app = await D.openWith(id, { owner: s.win, change: true });
        if (app) renderOw(Shell.apps.get(app));
      } });
      pages.push({ label: 'General', node: h('div', null, null,
        head(shown(it)), hr(),
        grid(...row('Type of file:', lnk ? 'Shortcut (.lnk)' : `${typeOf(it)}${it.ext ? ` (.${it.ext})` : ''}`),
          ...(lnk || it.ext === 'exe' ? row('Description:', shown(it)) : row('Opens with:', h('div', 'dlg-ow', null, owName, change)))),
        hr(),
        grid(...row('Location:', osPath(it.parent)), ...row('Size:', sizeLine(size)), ...row('Size on disk:', sizeLine(onDisk(size)))),
        hr(), grid(...dates()), hr(), grid(...attributes(false))) });
      if (lnk) {
        const app = Shell.apps.get(it.target);
        const exe = app ? `C:\\Program Files\\Mihawk OS\\${app.id}.exe` : osPath(it.target);
        const target = edit(it.args?.title ? `"${exe}" "${it.args.title}"` : `"${exe}"`, { 'aria-label': 'Target' });
        const start = edit('"C:\\Program Files\\Mihawk OS"', { 'aria-label': 'Start in' });
        const hot = edit('None', { 'aria-label': 'Shortcut key', readOnly: true });
        hot.addEventListener('keydown', (e) => {
          if (e.key === 'Tab' || e.key === 'Escape' || e.key === 'Enter') return;
          e.preventDefault();
          const k = e.key.length === 1 ? e.key.toUpperCase() : e.key;
          if (e.key === 'Backspace' || e.key === 'Delete') hot.value = 'None';
          else if (k.length === 1 || /^F\d+$/.test(k)) hot.value = `Ctrl + Alt + ${k}`;
          touch();
        });
        const run = selectBox([{ value: 'normal', label: 'Normal window' }, { value: 'min', label: 'Minimized' }, { value: 'max', label: 'Maximized' }], 'normal', touch);
        const comment = edit('', { 'aria-label': 'Comment' });
        [target, start, comment].forEach((n) => n.addEventListener('input', touch));
        /* Change Icon...: the pick shows in both page heads now and lands on the item on Apply. */
        let newIcon = null;
        const heads = () => root.querySelectorAll('.dlg-props-head > .icon');
        commits.push(() => {
          if (!newIcon || newIcon === it.icon) return;
          it.icon = newIcon;
          Shell.fs.rename(it.id, it.name); // fs has no icon setter: a same-name rename saves and emits 'fs'
        });
        pages.push({ label: 'Shortcut', node: h('div', null, null,
          h('div', 'dlg-props-head', null, icon(iconOf(it), 32), h('div', 'dlg-props-name', { text: shown(it) })), hr(),
          grid(...row('Target type:', 'Application'), ...row('Target location:', app ? 'Mihawk OS' : osPath(get(it.target)?.parent)),
            ...row('Target:', target)), hr(),
          grid(...row('Start in:', start), ...row('Shortcut key:', hot), ...row('Run:', run.el), ...row('Comment:', comment)),
          h('div', 'dlg-row-btns', null,
            btn('Open &File Location', { cls: 'dlg-btn-sm', on: () => { Shell.apps.launch('explorer', app ? { folder: 'f-c1' } : { folder: get(it.target)?.parent, select: it.target }); } }),
            btn('Change &Icon...', { cls: 'dlg-btn-sm', on: async () => {
              const pick = await changeIcon(s.win, newIcon || iconOf(it));
              if (!pick) return;
              newIcon = pick;
              heads().forEach((n) => n.replaceWith(icon(pick, 32)));
              touch();
            } }),
            btn('A&dvanced...', { cls: 'dlg-btn-sm', on: () => advancedShortcut(s.win) }))) });
      }
    }
    if (it.kind === 'file' || it.kind === 'shortcut') pages.push({ label: 'Details', node: detailsPage(it) });

    const ok = btn('OK', { def: true, on: () => { commit(); s.finish(true); } });
    function commit() { if (dirty) commits.forEach((f) => f()); dirty = false; apply.disabled = true; }
    root.append(sheet(pages, it.kind === 'shortcut' ? 1 : 0), h('div', 'dlg-foot dlg-props-foot', null, ok, btn('Cancel', { on: () => s.finish(false) }), apply));
    const s = dialog({ title, icon: iconOf(it), appId: 'properties', owner, width: 380, content: root, dismiss: false });
    propsOpen.set(id, s);
    s.done.then(() => { if (propsOpen.get(id) === s) propsOpen.delete(id); });
    if (!s.win) return s.done;
    dialogKeys(root, { onEscape: () => s.finish(false) });
    const first = root.querySelector('.dlg-page.shown .dlg-edit');
    if (first) { first.focus(); first.select(); } else root.querySelector('.dlg-tab')?.focus();
    return s.done;
  };
  function detailsPage(it) {
    const groups = [
      ['Description', [['Title', ''], ['Subject', ''], ['Rating', ''], ['Tags', ''], ['Comments', '']]],
      ['Origin', [['Authors', it.kind === 'file' ? 'Mihawk' : ''], ['Date created', shortDate(it.modified - 86400000 * 3)], ['Copyright', '']]],
      ['File', [['Name', it.name], ['Item type', typeOf(it)], ['Folder path', osPath(it.parent)],
        ['Date created', shortDate(it.modified - 86400000 * 3)], ['Date modified', shortDate(it.modified)],
        ['Size', it.kind === 'folder' ? '' : sig3(it.size || 0)], ['Attributes', 'A'], ['Owner', 'MIHAWK-PC\\Mihawk'], ['Computer', 'MIHAWK-PC (this PC)']]],
    ];
    const list = h('div', 'dlg-list dlg-details', { tabIndex: 0, role: 'grid' },
      h('div', 'dlg-list-head', null, h('span', null, { text: 'Property' }), h('span', null, { text: 'Value' })),
      groups.map(([g, rows]) => [h('div', 'dlg-list-group', { text: g }),
        rows.map(([k, v]) => h('div', 'dlg-list-row', null, h('span', null, { text: k }), h('span', null, { text: v })))]));
    list.addEventListener('click', (e) => {
      const r = e.target.closest('.dlg-list-row');
      if (r) list.querySelectorAll('.dlg-list-row').forEach((x) => x.classList.toggle('sel', x === r));
    });
    return h('div', 'dlg-details-page', null, list);
  }
  /* Advanced Attributes (General > Advanced...) and Advanced Properties (Shortcut > Advanced...). */
  function smallSheet(title, owner, body) {
    const root = h('div', 'dlg-small', null, h('div', 'dlg-small-body', null, body));
    const ok = btn('OK', { def: true, on: () => s.finish(true) });
    root.append(h('div', 'dlg-foot dlg-small-foot', null, ok, btn('Cancel', { on: () => s.finish(false) })));
    const s = dialog({ title, icon: 'object/app-generic', owner, width: 410, content: root, dismiss: false });
    if (s.win) { dialogKeys(root, { onEscape: () => s.finish(false) }); ok.focus(); }
    return s.done;
  }
  const advancedAttributes = (owner) => smallSheet('Advanced Attributes', owner, [
    h('p', null, { text: 'Choose the settings you want for this file.' }),
    h('fieldset', 'dlg-group', null, h('legend', null, { text: 'File attributes' }),
      check('File is &ready for archiving', { checked: true }),
      check('Allow this file to have contents &indexed in addition to file properties', { checked: true })),
    h('fieldset', 'dlg-group', null, h('legend', null, { text: 'Compress or Encrypt attributes' }),
      check('&Compress contents to save disk space'), check('&Encrypt contents to secure data')),
  ]);
  /* Change Icon: the icons of a file, four rows that scroll sideways; -> icon name or null. */
  const ICON_CHOICES = ['app-generic', 'app-game', 'app-store', 'app-explorer', 'app-notepad', 'app-terminal', 'app-calculator',
    'app-settings', 'app-taskmgr', 'app-run', 'app-controls', 'folder', 'folder-open', 'documents', 'downloads', 'pictures',
    'music', 'videos', 'desktop', 'games', 'this-pc', 'drive', 'network-wired', 'document', 'text', 'image', 'recycle-empty',
    'recycle-full', 'user-avatar'].map((n) => `object/${n}`);
  function changeIcon(owner, current) {
    let cur = Math.max(0, ICON_CHOICES.indexOf(current));
    const okBtn = () => list.closest('.dlg-small')?.querySelector('.dlg-btn[data-default]');
    const cells = ICON_CHOICES.map((n, i) => h('div', 'dlg-icon-cell', { role: 'option', onclick: () => pick(i), ondblclick: () => okBtn()?.click() }, icon(n, 32)));
    const list = h('div', 'dlg-list dlg-icon-grid', { tabIndex: 0, role: 'listbox', 'aria-label': 'Select an icon from the list below' }, cells);
    function pick(i) {
      cur = clamp(i, 0, cells.length - 1);
      cells.forEach((c, k) => { c.classList.toggle('sel', k === cur); c.setAttribute('aria-selected', String(k === cur)); });
      cells[cur].scrollIntoView({ block: 'nearest', inline: 'nearest' });
    }
    list.addEventListener('keydown', (e) => {
      const d = { ArrowDown: 1, ArrowUp: -1, ArrowRight: 4, ArrowLeft: -4 }[e.key];
      if (!d) return;
      e.preventDefault();
      e.stopPropagation();
      pick(cur + d);
    });
    const file = edit('%SystemRoot%\\System32\\imageres.dll', { 'aria-label': 'Look for icons in this file' });
    const browse = btn('&Browse...', { cls: 'dlg-btn-sm', on: async () => {
      const r = await D.fileOpen({ title: 'Change Icon', owner: winOf(browse), folder: 'f-c0',
        filters: [{ label: 'Icon files', exts: ['ico', 'exe', 'dll'] }, { label: 'All files', exts: ['*'] }] });
      if (r) file.value = `${osPath(r.parent)}\\${r.name}`;
    } });
    const done = smallSheet('Change Icon', owner, [
      h('div', null, { text: 'Look for icons in this file:' }), h('div', 'dlg-icon-file', null, file, browse),
      h('div', 'dlg-icon-label', { text: 'Select an icon from the list below:' }), list]);
    pick(cur);
    list.focus();
    return done.then((ok) => (ok ? ICON_CHOICES[cur] : null));
  }
  const advancedShortcut = (owner) => smallSheet('Advanced Properties', owner, [
    h('p', null, { text: 'Choose the advanced properties you want for this shortcut.' }),
    check('&Run as administrator'),
    check('Run in &separate memory space', { disabled: true }),
  ]);

  /* ---------- Open with (Windows 11 22H2+) ---------- */
  D.openWith = (id, { owner, change = false } = {}) => {
    const it = get(id);
    const ext = it?.ext || (it?.kind === 'shortcut' ? 'lnk' : '');
    const apps = Shell.apps.list().filter((a) => !['game', 'settings', 'taskmgr', 'controls'].includes(a.id));
    const rec = (a) => (ext === 'txt' && a.id === 'notepad') || defaults()[ext] === a.id;
    apps.sort((a, b) => rec(b) - rec(a) || (a.name || '').localeCompare(b.name || ''));
    const root = h('div', 'dlg-ow-modern');
    let chosen = opensWith(it || {})?.id || apps[0]?.id || null;
    const list = h('div', 'dlg-ow-list', { role: 'listbox', tabIndex: 0, 'aria-label': 'Apps' });
    const rows = apps.map((a) => h('div', 'dlg-ow-item', { role: 'option', 'data-id': a.id,
      onclick: () => select(a.id), ondblclick: () => go(change ? 'always' : 'once') },
    icon(a.icon || 'object/app-generic', 24), h('span', null, { text: a.name })));
    const recRows = rows.filter((r, i) => rec(apps[i])), more = rows.filter((r, i) => !rec(apps[i]));
    if (recRows.length) list.append(h('div', 'dlg-ow-section', { text: 'Recommended apps' }), ...recRows);
    if (more.length) list.append(h('div', 'dlg-ow-section', { text: recRows.length ? 'Other apps' : 'Suggested apps' }), ...more);
    function select(appId, scroll) {
      chosen = appId;
      rows.forEach((r) => { const on = r.dataset.id === appId; r.classList.toggle('sel', on); r.setAttribute('aria-selected', String(on)); });
      if (scroll) rows.find((r) => r.dataset.id === appId)?.scrollIntoView({ block: 'nearest' });
      once.disabled = always.disabled = !chosen;
    }
    list.addEventListener('keydown', (e) => {
      const i = apps.findIndex((a) => a.id === chosen);
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); select(apps[clamp(i + (e.key === 'ArrowDown' ? 1 : -1), 0, apps.length - 1)].id, true); }
      else if (e.key === 'Home' || e.key === 'End') { e.preventDefault(); select(apps[e.key === 'Home' ? 0 : apps.length - 1].id, true); }
      else if (e.key === 'Enter') { e.preventDefault(); go(change ? 'always' : 'once'); }
    });
    const pc = h('button', 'hyperlink dlg-ow-pc', { type: 'button', text: 'Choose an app on your PC', onclick: async () => {
      const r = await D.fileOpen({ title: 'Open with...', owner: s.win, folder: 'f-c1', filters: [{ label: 'Programs', exts: ['exe'] }] });
      if (r) Shell.notify?.({ app: 'File Explorer', title: 'This app can\'t run on your PC', body: `${r.name} isn't a Mihawk OS app. Pick an app from the list.` });
    } });
    const once = h('button', 'btn accent', { type: 'button', text: change ? 'Set default' : 'Just once', onclick: () => go(change ? 'always' : 'once') });
    const always = h('button', 'btn', { type: 'button', text: 'Always', onclick: () => go('always') });
    const heading = change ? `Select an app to open .${ext || 'this type of'} files` : `Select an app to open this${ext ? ` .${ext}` : ''} file`;
    root.append(
      h('div', 'dlg-ow-top', null, h('h2', 'dlg-ow-title', { text: heading }), it && !change && h('div', 'dlg-ow-file', null, icon(iconOf(it), 16), h('span', null, { text: it.name })), list, pc),
      h('div', 'dlg-ow-foot', null, change ? [once] : [once, always]),
    );
    function go(mode) {
      if (!chosen) return;
      if (mode === 'always' && ext) Shell.store.set('dialogs-default-apps', { ...defaults(), [ext]: chosen });
      s.finish(chosen); // the caller opens the file with it (CONTRACT: -> app id or null)
    }
    const wa = area();
    const s = dialog({ title: 'Open with', appId: owner?.appId || 'openwith', owner, classic: false,
      titlebarContent: h('span', 'dlg-ow-tb'), width: Math.min(480, wa.width), height: Math.min(600, wa.height), content: root });
    if (!s.win) return s.done;
    select(chosen, true);
    root.addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.preventDefault(); s.finish(null); } });
    list.focus();
    return s.done;
  };

  /* ---------- the common file dialog (Open / Save As) ---------- */
  D.fileOpen = (o = {}) => fileDialog('open', o);
  D.fileSave = (o = {}) => fileDialog('save', o);
  const filterLabel = (f) => `${f.label} (${f.exts.map((e) => (e === '*' ? '*.*' : `*.${e}`)).join(';')})`;
  const KNOWN = ['desktop', 'downloads', 'documents', 'pictures', 'music', 'videos'];
  function fileDialog(mode, o) {
    const save = mode === 'save';
    const ext = (o.ext || '').replace(/^\./, '');
    const filters = save
      ? [...(ext ? [{ label: TYPES[ext] ? `${TYPES[ext]}s` : `${ext.toUpperCase()} Files`, exts: [ext] }] : []), { label: 'All files', exts: ['*'] }]
      : (o.filters?.length ? o.filters : [{ label: 'All files', exts: ['*'] }]);
    const st = {
      loc: isContainer(get(o.folder)) ? o.folder : Shell.store.get('dialogs-last-folder', 'documents'),
      back: [], fwd: [], sel: null, sort: { key: 'name', dir: 1 }, view: 'details', query: '', filter: 0,
      open: new Set(['root']), navKey: null, // the nav node last navigated by: the tree has one selection
    };
    if (!isContainer(get(st.loc)) && st.loc !== 'home') st.loc = 'documents';
    const root = h('div', 'dlg-fd');

    /* address row */
    const iconBtn = (name, label, on) => h('button', 'dlg-fd-ib', { type: 'button', 'aria-label': label, 'data-tooltip': label, onclick: on }, icon(name, 16));
    const bBack = iconBtn('fluent/arrow-left', 'Back', () => back());
    const bFwd = iconBtn('fluent/arrow-right', 'Forward', () => fwd());
    const bUp = iconBtn('fluent/arrow-up', 'Up', () => up());
    const crumbs = h('div', 'dlg-fd-crumbs');
    const addrEdit = h('input', 'dlg-fd-addr-edit', { type: 'text', spellcheck: 'false', 'aria-label': 'Address' });
    const addr = h('div', 'dlg-fd-addr', null, crumbs, addrEdit, iconBtn('fluent/arrow-clockwise', 'Refresh', () => render()));
    const search = h('input', 'dlg-fd-search', { type: 'search', spellcheck: 'false' });
    const searchWrap = h('label', 'dlg-fd-searchbox', null, search, icon('fluent/search', 16));
    /* command bar */
    const organize = h('button', 'dlg-fd-cmd', { type: 'button' }, 'Organize', icon('fluent/chevron-down', 12));
    const newFolder = h('button', 'dlg-fd-cmd', { type: 'button', text: 'New folder', onclick: () => makeFolder() });
    const viewBtn = h('button', 'dlg-fd-cmd dlg-fd-view', { type: 'button', 'aria-label': 'Change your view', 'data-tooltip': 'Change your view' }, icon('fluent/text-bullet-list', 16), icon('fluent/chevron-down', 12));
    /* panes */
    const nav = h('div', 'dlg-fd-nav', { role: 'tree', 'aria-label': 'Navigation pane' });
    const head = h('div', 'dlg-fd-head');
    const rows = h('div', 'dlg-fd-rows');
    const empty = h('div', 'dlg-fd-empty');
    const listEl = h('div', 'dlg-list dlg-fd-list', { tabIndex: 0, role: 'grid', 'aria-label': 'Items' }, head, rows, empty);
    /* bottom */
    const nameInput = edit(o.name || '', { id: `dlg-fd-name-${++seq}` });
    const nameCombo = editCombo(nameInput, () => Shell.store.get('dialogs-file-mru', []));
    const nameLabel = h('label', 'dlg-fd-label', { htmlFor: nameInput.id });
    nameLabel.append(...mnemonic('File &name:', nameLabel));
    const typeSel = selectBox(filters.map((f, i) => ({ value: i, label: filterLabel(f) })), 0, (v) => { st.filter = v; render(); });
    const okBtn = btn(save ? '&Save' : '&Open', { def: true, on: () => accept() });
    const cancelBtn = btn('Cancel', { on: () => s.finish(null) });
    const bottom = save
      ? h('div', 'dlg-fd-bottom save', null,
        nameLabel, nameCombo, h('span', 'dlg-fd-label', { text: 'Save as type:' }), typeSel.el,
        h('div', 'dlg-fd-btns', null, okBtn, cancelBtn))
      : h('div', 'dlg-fd-bottom', null, nameLabel, nameCombo, typeSel.el, h('div', 'dlg-fd-btns', null, okBtn, cancelBtn));
    root.append(
      h('div', 'dlg-fd-addrrow', null, bBack, bFwd, bUp, addr, searchWrap),
      h('div', 'dlg-fd-cmdbar', null, organize, newFolder, h('span', 'dlg-fill'), viewBtn),
      h('div', 'dlg-fd-main', null, nav, listEl),
      bottom,
    );

    /* ---- model ---- */
    const where = () => (st.loc === 'home' ? { id: 'home', name: 'Home', icon: 'fluent/home' } : get(st.loc));
    const canHold = () => { const l = get(st.loc); return !!l && (l.kind === 'folder' || l.kind === 'drive'); };
    const matches = (it) => {
      const f = filters[st.filter];
      if (isContainer(it) || f.exts.includes('*')) return true;
      return f.exts.includes(it.kind === 'shortcut' ? 'lnk' : (it.ext || '').toLowerCase());
    };
    function items() {
      let list;
      if (st.query) {
        const q = st.query.toLowerCase();
        const base = st.loc === 'home' ? KNOWN.flatMap((k) => walk(k)) : walk(st.loc);
        list = base.filter((i) => i.name.toLowerCase().includes(q));
      } else list = st.loc === 'home' ? KNOWN.map(get).filter(Boolean) : kids(st.loc);
      list = list.filter((i) => i.kind !== 'system' && matches(i));
      const { key, dir } = st.sort;
      const val = (i) => (key === 'date' ? i.modified || 0 : key === 'type' ? typeOf(i) : key === 'size' ? (isContainer(i) ? -1 : i.size || 0) : shown(i).toLowerCase());
      return list.sort((a, b) => (isContainer(b) - isContainer(a)) || (val(a) > val(b) ? dir : val(a) < val(b) ? -dir : 0));
    }

    /* ---- render ---- */
    function renderCrumbs() {
      crumbs.replaceChildren();
      const loc = where();
      crumbs.append(h('span', 'dlg-fd-crumb-icon', null, icon(loc?.id === 'home' ? 'fluent/home' : iconOf(loc), 16)));
      const path = st.loc === 'home' ? [{ id: 'home', name: 'Home' }] : (Shell.fs.path(st.loc) || []);
      path.forEach((p, i) => {
        crumbs.append(h('button', 'dlg-fd-chev', { type: 'button', tabIndex: -1, 'aria-label': 'Folders', onclick: (e) => crumbMenu(p.id === 'home' ? null : p.parent, e.currentTarget) }, icon('fluent/chevron-right', 12)));
        crumbs.append(h('button', 'dlg-fd-crumb', { type: 'button', tabIndex: i === path.length - 1 ? 0 : -1, text: p.name, onclick: () => go(p.id) }));
      });
      const last = path[path.length - 1];
      if (last && last.id !== 'home' && kids(last.id).some(isContainer)) crumbs.append(h('button', 'dlg-fd-chev', { type: 'button', tabIndex: -1, 'aria-label': 'Folders', onclick: (e) => crumbMenu(last.id, e.currentTarget) }, icon('fluent/chevron-right', 12)));
    }
    /* The breadcrumb bar is one tab stop (its last crumb); Left/Right walk its buttons. */
    crumbs.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
      const list = [...crumbs.querySelectorAll('button')];
      const to = list[list.indexOf(document.activeElement) + (e.key === 'ArrowRight' ? 1 : -1)];
      e.preventDefault();
      e.stopPropagation();
      if (!to) return;
      list.forEach((b) => { b.tabIndex = b === to ? 0 : -1; });
      to.focus();
    });
    function crumbMenu(parent, anchor) {
      const subs = parent ? kids(parent).filter(isContainer) : KNOWN.map(get).filter(Boolean);
      if (!subs.length && parent) return;
      anchor.classList.add('open');
      Shell.menu?.show?.({ anchorRect: anchor.getBoundingClientRect(), placement: 'bottom-start',
        items: (parent ? subs : [{ id: 'home', name: 'Home', icon: 'fluent/home' }, { id: 'root', name: 'This PC', icon: 'object/this-pc' }, ...subs])
          .map((f) => ({ label: f.name, icon: f.icon || iconOf(f), action: () => go(f.id) })),
        onClose: () => anchor.classList.remove('open') });
    }
    /* One tree, one selection: the node navigated by, else the first node showing the folder.
       The selected node is the pane's tab stop. */
    function renderNav() {
      const hadFocus = nav.contains(document.activeElement);
      nav.replaceChildren();
      const nodes = [];
      const item = (it, depth, key, { pin, expandable } = {}) => {
        const open = st.open.has(it.id);
        const el = h('div', 'dlg-fd-navitem', { role: 'treeitem', tabIndex: -1, 'aria-level': depth + 1, 'aria-expanded': expandable ? String(open) : null });
        Object.assign(el, { key, itemId: it.id, depth, expandable });
        el.style.paddingLeft = `${4 + depth * 16}px`;
        const chev = h('span', `dlg-fd-navchev${open ? ' open' : ''}`, null, expandable ? icon('fluent/chevron-right', 12) : null);
        chev.addEventListener('click', (e) => { e.stopPropagation(); if (expandable) toggleNode(it.id); });
        el.append(chev, icon(it.id === 'home' ? 'fluent/home' : iconOf(it), 16), h('span', 'dlg-fd-navlabel', { text: it.name }));
        if (pin) el.append(icon('fluent/pin', 12, 'dlg-fd-pin'));
        el.addEventListener('click', () => navGo(el));
        nav.append(el);
        nodes.push(el);
        if (open && expandable) kids(it.id).filter(isContainer).sort((a, b) => (a.kind === 'drive') - (b.kind === 'drive') || a.name.localeCompare(b.name))
          .forEach((c) => item(c, depth + 1, `tree:${c.id}`, { expandable: kids(c.id).some(isContainer) }));
      };
      item({ id: 'home', name: 'Home' }, 0, 'home');
      nav.append(h('div', 'dlg-fd-navsep'));
      KNOWN.map(get).filter(Boolean).forEach((k) => item(k, 0, `pin:${k.id}`, { pin: true }));
      nav.append(h('div', 'dlg-fd-navsep'));
      const pc = get('root');
      if (pc) item(pc, 0, 'tree:root', { expandable: true });
      const sel = nodes.find((n) => n.key === st.navKey && n.itemId === st.loc) || nodes.find((n) => n.itemId === st.loc);
      sel?.classList.add('sel');
      sel?.setAttribute('aria-selected', 'true');
      (sel || nodes[0]).tabIndex = 0;
      if (hadFocus) (sel || nodes[0]).focus({ preventScroll: true });
      sel?.scrollIntoView({ block: 'nearest' });
    }
    function navGo(node) { st.navKey = node.key; if (node.itemId === st.loc) renderNav(); else go(node.itemId); }
    function toggleNode(id) { if (st.open.has(id)) st.open.delete(id); else st.open.add(id); renderNav(); }
    /* Tree keys: Up/Down/Home/End move (and open, as the dialog's tree does), Right expands then
       steps in, Left collapses then steps out, a letter jumps to the next node starting with it. */
    nav.addEventListener('keydown', (e) => {
      const list = [...nav.querySelectorAll('.dlg-fd-navitem')];
      const cur = e.target.closest?.('.dlg-fd-navitem'), i = list.indexOf(cur);
      if (i < 0 || e.ctrlKey || e.altKey || e.metaKey) return;
      const k = e.key;
      let to = null;
      if (k === 'ArrowDown' || k === 'ArrowUp') to = list[i + (k === 'ArrowDown' ? 1 : -1)];
      else if (k === 'Home' || k === 'End') to = k === 'Home' ? list[0] : list[list.length - 1];
      else if (k === 'ArrowRight') { if (cur.expandable && !st.open.has(cur.itemId)) toggleNode(cur.itemId); else if (cur.expandable) to = list[i + 1]; }
      else if (k === 'ArrowLeft') { if (cur.expandable && st.open.has(cur.itemId)) toggleNode(cur.itemId); else to = list.slice(0, i).reverse().find((n) => n.depth < cur.depth); }
      else if (k === 'Enter' || k === ' ') to = cur;
      else if (k.length === 1) {
        const hit = (n) => n.querySelector('.dlg-fd-navlabel').textContent.toLowerCase().startsWith(k.toLowerCase());
        to = list.slice(i + 1).find(hit) || list.find(hit);
      } else return;
      e.preventDefault();
      e.stopPropagation();
      if (to) navGo(to);
    });
    const COLS = [['name', 'Name'], ['date', 'Date modified'], ['type', 'Type'], ['size', 'Size']];
    function renderList() {
      head.replaceChildren(...COLS.map(([k, label], i) => h('button', `dlg-fd-col${st.sort.key === k ? ` sorted ${st.sort.dir > 0 ? 'asc' : 'desc'}` : ''}`,
        { type: 'button', tabIndex: -1, onclick: () => { st.sort = { key: k, dir: st.sort.key === k ? -st.sort.dir : 1 }; renderList(); } },
        h('span', null, { text: label }), st.sort.key === k && icon(st.sort.dir > 0 ? 'fluent/chevron-up' : 'fluent/chevron-down', 12, 'dlg-fd-sort'),
        h('span', 'dlg-fd-grip', { 'data-col': i, 'aria-hidden': 'true' }))));
      const list = items();
      listEl.classList.toggle('icons', st.view === 'icons');
      rows.replaceChildren(...list.map((it) => {
        const r = h('div', `dlg-fd-row${st.sel === it.id ? ' sel' : ''}${Shell.clipboard?.get?.()?.cut && Shell.clipboard.get().ids.includes(it.id) ? ' cut' : ''}`,
          { role: 'row', 'data-id': it.id },
          h('span', 'dlg-fd-name', null, icon(iconOf(it), st.view === 'icons' ? 48 : 16), h('span', 'dlg-fd-text', { text: shown(it) })),
          h('span', null, { text: shortDate(it.modified) }), h('span', null, { text: typeOf(it) }),
          h('span', 'dlg-fd-size', { text: isContainer(it) ? '' : fmtKB(it.size) }));
        r.addEventListener('click', () => select(it.id));
        r.addEventListener('dblclick', () => activate(it));
        r.addEventListener('contextmenu', (e) => { e.preventDefault(); e.stopPropagation(); select(it.id); itemMenu(it, e.clientX, e.clientY); });
        return r;
      }));
      empty.textContent = list.length ? '' : st.query ? 'No items match your search.' : 'This folder is empty.';
    }
    function render() {
      if (st.loc !== 'home' && !get(st.loc)) st.loc = 'documents';
      renderCrumbs();
      renderNav();
      renderList();
      bBack.disabled = !st.back.length;
      bFwd.disabled = !st.fwd.length;
      bUp.disabled = st.loc === 'home' || st.loc === 'root';
      newFolder.disabled = !canHold();
      search.placeholder = `Search ${where()?.name || ''}`;
      s?.win?.setTitle?.(o.title || (save ? 'Save As' : 'Open'));
    }

    /* ---- navigation and selection ---- */
    function go(id, { history = true } = {}) {
      if (!id || id === st.loc) { render(); return; }
      if (history) { st.back.push(st.loc); st.fwd = []; }
      st.loc = id;
      st.sel = null;
      st.query = '';
      search.value = '';
      Shell.fs.path(id).forEach((p) => st.open.add(p.id));
      render();
    }
    const back = () => { if (st.back.length) { st.fwd.push(st.loc); go(st.back.pop(), { history: false }); } };
    const fwd = () => { if (st.fwd.length) { st.back.push(st.loc); go(st.fwd.pop(), { history: false }); } };
    const up = () => { const p = get(st.loc)?.parent; if (p) go(p); else if (st.loc !== 'home') go('home'); };
    function select(id, scroll) {
      st.sel = id;
      rows.querySelectorAll('.dlg-fd-row').forEach((r) => r.classList.toggle('sel', r.dataset.id === id));
      const it = get(id);
      if (it && !isContainer(it)) nameInput.value = shown(it);
      if (scroll) rows.querySelector('.dlg-fd-row.sel')?.scrollIntoView({ block: 'nearest' });
    }
    function activate(it) { if (isContainer(it)) go(it.id); else { nameInput.value = shown(it); accept(); } }
    listEl.addEventListener('click', (e) => { if (!e.target.closest('.dlg-fd-row, .dlg-fd-head, input')) select(null); });
    /* Header dividers: a drag sizes the column on their left; the list scrolls sideways when wider. */
    head.addEventListener('pointerdown', (e) => {
      const grip = e.target.closest('.dlg-fd-grip');
      if (!grip || e.button !== 0) return;
      e.preventDefault();
      e.stopPropagation();
      const cells = [...head.children].filter((c) => c.checkVisibility());
      const widths = cells.map((c) => c.getBoundingClientRect().width);
      const i = cells.indexOf(grip.parentElement), x0 = e.clientX, w0 = widths[i];
      grip.setPointerCapture(e.pointerId);
      const move = (ev) => {
        widths[i] = Math.max(24, w0 + ev.clientX - x0);
        listEl.style.setProperty('--cols', widths.map((w) => `${Math.round(w)}px`).join(' '));
        listEl.classList.add('sized');
      };
      const up = () => ['pointermove', 'pointerup', 'pointercancel'].forEach((t) => grip.removeEventListener(t, t === 'pointermove' ? move : up));
      grip.addEventListener('pointermove', move);
      grip.addEventListener('pointerup', up);
      grip.addEventListener('pointercancel', up);
    });
    head.addEventListener('click', (e) => { if (e.target.closest('.dlg-fd-grip')) e.stopPropagation(); }, true); // a resize is not a sort
    listEl.addEventListener('keydown', (e) => {
      const list = items(), i = list.findIndex((x) => x.id === st.sel);
      const k = e.key;
      if (k === 'ArrowDown' || k === 'ArrowUp') select(list[clamp(i + (k === 'ArrowDown' ? 1 : -1), 0, list.length - 1)]?.id, true);
      else if (k === 'Home' || k === 'End') select(list[k === 'Home' ? 0 : list.length - 1]?.id, true);
      else if (k === 'Enter' && i >= 0) activate(list[i]);
      else if (k === 'Backspace' || (e.altKey && k === 'ArrowUp')) up();
      else if (k === 'F2' && i >= 0) rename(list[i]);
      else if (k === 'Delete' && i >= 0) remove(list[i]);
      else if ((k === 'ContextMenu' || (e.shiftKey && k === 'F10')) && i >= 0) {
        const r = rows.querySelector('.dlg-fd-row.sel')?.getBoundingClientRect();
        itemMenu(list[i], r ? r.left + 24 : 0, r ? r.bottom : 0);
      } else if (k.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
        const hits = list.filter((x) => shown(x).toLowerCase().startsWith(k.toLowerCase()));
        select((hits.find((x) => list.indexOf(x) > i) || hits[0])?.id, true);
      } else return;
      e.preventDefault();
      e.stopPropagation();
    });
    /* F6 / Shift+F6 cycle the panes: address bar, search, navigation pane, items, file name. */
    const panes = () => [crumbs.querySelector('[tabindex="0"]'), search, nav.querySelector('[tabindex="0"]'), listEl, nameInput]
      .filter((p) => p?.checkVisibility());
    const editAddress = () => { addr.classList.add('editing'); addrEdit.value = osPath(st.loc); addrEdit.focus(); addrEdit.select(); };
    root.addEventListener('keydown', (e) => {
      if (e.altKey && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) { e.preventDefault(); (e.key === 'ArrowLeft' ? back : fwd)(); }
      else if (e.key === 'F6') {
        e.preventDefault();
        const list = panes(), a = document.activeElement;
        const i = list.findIndex((p) => p.contains(a) || (p.parentNode === crumbs && crumbs.contains(a)));
        list[i < 0 ? (e.shiftKey ? list.length - 1 : 0) : (i + (e.shiftKey ? list.length - 1 : 1)) % list.length]?.focus();
      } else if ((e.altKey && e.key.toLowerCase() === 'd') || (e.ctrlKey && e.key.toLowerCase() === 'l')) { e.preventDefault(); editAddress(); }
      else if (e.key === 'F5') { e.preventDefault(); render(); }
      else if (e.ctrlKey && e.key.toLowerCase() === 'e') { e.preventDefault(); search.focus(); }
      else if (e.ctrlKey && e.shiftKey && e.key.toLowerCase() === 'n') { e.preventDefault(); makeFolder(); }
    });
    listEl.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      Shell.menu?.show?.({ x: e.clientX, y: e.clientY, items: [
        { label: 'View', icon: 'fluent/grid', submenu: [
          { label: 'Medium icons', radio: true, checked: st.view === 'icons', action: () => { st.view = 'icons'; renderList(); } },
          { label: 'Details', radio: true, checked: st.view === 'details', action: () => { st.view = 'details'; renderList(); } }] },
        { label: 'Sort by', icon: 'fluent/arrow-sort', submenu: COLS.map(([k, label]) => ({ label, radio: true, checked: st.sort.key === k, action: () => { st.sort = { key: k, dir: 1 }; renderList(); } })) },
        { label: 'Refresh', icon: 'fluent/arrow-clockwise', action: () => render() },
        { separator: true },
        { label: 'Paste', icon: 'fluent/clipboard-paste', disabled: !Shell.clipboard?.canPaste?.(st.loc), action: () => { Shell.clipboard.paste(st.loc); } },
        { label: 'New folder', icon: 'fluent/folder-add', disabled: !canHold(), action: () => makeFolder() },
        { separator: true },
        { label: 'Properties', icon: 'fluent/wrench', disabled: st.loc === 'home', action: () => D.properties(st.loc, { owner: s.win }) },
      ] });
    });
    function itemMenu(it, x, y) {
      const editable = it.kind === 'file' || it.kind === 'shortcut' || (it.kind === 'folder' && !it.known);
      Shell.menu?.show?.({ x, y, items: [
        { commandBar: [
          { icon: 'fluent/cut', label: 'Cut', disabled: !editable, action: () => Shell.clipboard?.set?.([it.id], { cut: true }) },
          { icon: 'fluent/copy', label: 'Copy', disabled: it.kind === 'drive', action: () => Shell.clipboard?.set?.([it.id]) },
          { icon: 'fluent/rename', label: 'Rename', disabled: !editable, action: () => rename(it) },
          { icon: 'fluent/delete', label: 'Delete', disabled: !editable, action: () => remove(it) }] },
        { label: isContainer(it) ? 'Open' : save ? 'Select' : 'Open', icon: 'fluent/open', action: () => activate(it) },
        { separator: true },
        { label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter', action: () => D.properties(it.id, { owner: s.win }) },
      ] });
    }
    function rename(it) {
      const r = rows.querySelector(`.dlg-fd-row[data-id="${CSS.escape(it.id)}"] .dlg-fd-text`);
      if (!r || it.known || it.kind === 'drive') return;
      const box = h('input', 'dlg-fd-rename', { type: 'text', spellcheck: 'false', value: shown(it) });
      guardName(box);
      r.replaceWith(box);
      box.focus();
      box.select();
      let done = false;
      const end = (keep) => {
        if (done) return;
        done = true;
        const v = box.value.trim();
        if (keep && v && v !== shown(it)) Shell.fs.rename(it.id, hidesExt(it) ? `${v}.${it.ext}` : v);
        renderList();
        select(it.id);
        listEl.focus();
      };
      box.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); end(e.key === 'Enter'); }
      });
      box.addEventListener('blur', () => end(true));
    }
    function remove(it) {
      if (it.known || it.kind === 'drive' || it.kind === 'system') return;
      D.fileOperation({ op: 'delete', ids: [it.id] });
    }
    function makeFolder() {
      if (!canHold()) return;
      const f = Shell.fs.create(st.loc, { name: Shell.fs.uniqueName(st.loc, 'New folder'), kind: 'folder', icon: 'object/folder' });
      renderList();
      select(f.id, true);
      rename(f);
    }
    /* address bar: a click on its empty part edits the path */
    addr.addEventListener('click', (e) => { if (!e.target.closest('button')) editAddress(); });
    addrEdit.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); addrEdit.blur(); }
      else if (e.key === 'Enter') {
        e.preventDefault();
        e.stopPropagation();
        const id = resolvePath(addrEdit.value);
        if (id) { addrEdit.blur(); go(id); return; }
        D.messageBox({ title: o.title || (save ? 'Save As' : 'Open'), icon: 'error', owner: s.win,
          text: `Windows can't find '${addrEdit.value}'. Check the spelling and try again.` }).then(() => addrEdit.focus());
      }
    });
    addrEdit.addEventListener('blur', () => addr.classList.remove('editing'));
    search.addEventListener('input', () => { st.query = search.value.trim(); renderList(); });
    search.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); e.stopPropagation(); listEl.focus(); } });
    organize.addEventListener('click', () => {
      const it = get(st.sel);
      const editable = it && (it.kind === 'file' || it.kind === 'shortcut' || (it.kind === 'folder' && !it.known));
      organize.classList.add('open');
      Shell.menu?.show?.({ anchorRect: organize.getBoundingClientRect(), placement: 'bottom-start', onClose: () => organize.classList.remove('open'), items: [
        { label: 'Cut', icon: 'fluent/cut', shortcut: 'Ctrl+X', disabled: !editable, action: () => Shell.clipboard?.set?.([it.id], { cut: true }) },
        { label: 'Copy', icon: 'fluent/copy', shortcut: 'Ctrl+C', disabled: !it, action: () => Shell.clipboard?.set?.([it.id]) },
        { label: 'Paste', icon: 'fluent/clipboard-paste', shortcut: 'Ctrl+V', disabled: !Shell.clipboard?.canPaste?.(st.loc), action: () => Shell.clipboard.paste(st.loc) },
        { separator: true },
        { label: 'Layout', icon: 'fluent/grid', submenu: [
          { label: 'Medium icons', radio: true, checked: st.view === 'icons', action: () => { st.view = 'icons'; renderList(); } },
          { label: 'Details', radio: true, checked: st.view === 'details', action: () => { st.view = 'details'; renderList(); } }] },
        { separator: true },
        { label: 'Delete', icon: 'fluent/delete', disabled: !editable, action: () => remove(it) },
        { label: 'Rename', icon: 'fluent/rename', disabled: !editable, action: () => rename(it) },
        { label: 'Properties', icon: 'fluent/wrench', disabled: st.loc === 'home' && !it, action: () => D.properties(it ? it.id : st.loc, { owner: s.win }) },
      ] });
    });
    viewBtn.addEventListener('click', () => { st.view = st.view === 'details' ? 'icons' : 'details'; renderList(); });

    /* ---- accept ---- */
    const titleText = () => o.title || (save ? 'Save As' : 'Open');
    const remember = (parent, name) => {
      Shell.store.set('dialogs-last-folder', parent);
      Shell.store.set('dialogs-file-mru', [name, ...Shell.store.get('dialogs-file-mru', []).filter((n) => n !== name)].slice(0, 12));
    };
    async function accept() {
      const raw = nameInput.value.trim().replace(/^"|"$/g, '');
      if (!raw) { const it = get(st.sel); if (it && isContainer(it)) go(it.id); return; }
      const folder = raw.includes('\\') || raw.includes(':') ? resolvePath(raw) : kids(st.loc).find((i) => isContainer(i) && i.name.toLowerCase() === raw.toLowerCase())?.id;
      if (folder) { nameInput.value = ''; go(folder); return; }
      if (save) {
        if (/[\\/:*?"<>|]/.test(raw)) { await D.messageBox({ title: titleText(), icon: 'warning', owner: s.win, text: `${raw}\nThe file name is not valid.` }); nameInput.focus(); return; }
        if (!canHold()) { await D.messageBox({ title: titleText(), icon: 'warning', owner: s.win, text: `You can't save here. Please choose another location.` }); return; }
        const f = filters[st.filter];
        const name = f.exts.includes('*') || raw.toLowerCase().endsWith(`.${f.exts[0]}`) ? raw : `${raw}.${f.exts[0]}`;
        const existing = kids(st.loc).find((i) => i.name.toLowerCase() === name.toLowerCase());
        if (existing && isContainer(existing)) { nameInput.value = ''; go(existing.id); return; }
        if (existing) {
          const r = await D.messageBox({ title: 'Confirm Save As', icon: 'warning', owner: s.win, buttons: ['Yes', 'No'], defaultButton: 'No',
            text: `${name} already exists.\nDo you want to replace it?` });
          if (r !== 'Yes') { nameInput.focus(); return; }
        }
        remember(st.loc, shown({ kind: 'file', name, ext: name.split('.').pop() }));
        s.finish({ parent: st.loc, name, ...(existing ? { id: existing.id } : {}) });
        return;
      }
      const pool = raw.includes('\\') ? [get(resolvePath(raw, { files: true }))].filter(Boolean)
        : (st.loc === 'home' ? KNOWN.flatMap(kids) : kids(st.loc));
      const it = pool.find((i) => !isContainer(i) && (raw.includes('\\') || i.name.toLowerCase() === raw.toLowerCase() || shown(i).toLowerCase() === raw.toLowerCase()));
      if (!it) {
        await D.messageBox({ title: titleText(), icon: 'warning', owner: s.win, text: `${raw}\nFile not found.\nCheck the file name and try again.` });
        nameInput.focus();
        nameInput.select();
        return;
      }
      remember(it.parent, shown(it));
      s.finish({ parent: it.parent, name: it.name, id: it.id });
    }

    const wa = area();
    let s = null;
    render();
    s = dialog({ title: titleText(), owner: o.owner, classic: false,
      width: Math.min(960, wa.width - 24), height: Math.min(600, wa.height - 24), minWidth: Math.min(360, wa.width), minHeight: Math.min(320, wa.height),
      resizable: true, content: root, appId: o.app || o.owner?.appId || 'filedialog' });
    if (!s.win) return s.done;
    const off = Shell.on('fs', () => { if (alive(s.win)) render(); });
    const offClip = Shell.on('clipboard', () => { if (alive(s.win)) renderList(); });
    s.done.then(() => { off(); offClip(); });
    dialogKeys(root, { onEscape: () => s.finish(null) });
    nameInput.focus();
    nameInput.select();
    return s.done;
  }

  /* ---------- UAC: the secure desktop ---------- */
  let uacChain = Promise.resolve();
  D.uac = (o = {}) => {
    const p = uacChain.then(() => consent(o));
    uacChain = p.catch(() => {});
    return p;
  };
  /* The secure desktop is a document of its own (a full-screen iframe): keys typed on it never
     reach this window's listeners, so Shell.keys, the bare Win key, Alt+Tab and every other shell
     hotkey are dead until Yes or No, and the frozen shell under the dim takes no pointer or focus. */
  const UAC_INERT = ['desktop', 'windows', 'taskbar', 'flyouts'];
  function secureDesktop(build) {
    const frame = h('iframe', 'dlg-uac-frame', { title: 'User Account Control', tabIndex: -1 });
    const sheets = [...document.querySelectorAll('link[rel="stylesheet"]')].map((l) => `<link rel="stylesheet" href="${l.href}">`).join('');
    frame.srcdoc = `<!doctype html><html><head><meta charset="utf-8">${sheets}</head><body></body></html>`;
    frame.addEventListener('load', () => {
      const fdoc = frame.contentDocument;
      for (const a of document.documentElement.attributes) fdoc.documentElement.setAttribute(a.name, a.value); // theme, accent, motion
      fdoc.documentElement.style.background = fdoc.body.style.background = 'transparent';
      build(fdoc);
    }, { once: true });
    const inert = UAC_INERT.map(Shell.layer).filter((l) => l && !l.inert);
    inert.forEach((l) => { l.inert = true; });
    Shell.layer('overlays').append(frame);
    frame.focus(); // keys go to the frame's document from now on, even before the card is in it
    return { frame, release: () => inert.forEach((l) => { l.inert = false; }) };
  }
  function consent({ app = 'App', publisher, verified, icon: appIcon, path } = {}) {
    const known = (verified ?? !!publisher) && !!publisher;
    return new Promise((resolve) => {
      Shell.transient?.closeAll?.();
      Shell.tooltip?.hide?.();
      const before = document.activeElement;
      const screen = h('div', 'dlg-uac-screen');
      const qid = `dlg-uac-q-${++seq}`;
      const card = h('div', `dlg-uac-card${known ? '' : ' unknown'}`, { role: 'alertdialog', 'aria-modal': 'true', 'aria-labelledby': qid });
      const more = h('div', 'dlg-uac-more', { hidden: true },
        h('div', null, { text: `Program location: "${path || `C:\\Program Files\\${app}\\${app.replace(/\s+/g, '')}.exe`}"` }),
        known && h('div', 'dlg-uac-cert', { text: 'Show information about the publisher\'s certificate' }));
      const toggle = h('button', 'hyperlink inline dlg-uac-toggle', { type: 'button', text: 'Show more details', onclick: () => {
        more.hidden = !more.hidden;
        toggle.textContent = more.hidden ? 'Show more details' : 'Show fewer details';
      } });
      const yes = h('button', `btn${known ? ' accent' : ''}`, { type: 'button', onclick: () => finish(true) }, h('span', null, null, ...mnemonic('&Yes', h('span'))));
      const no = h('button', `btn${known ? '' : ' accent'}`, { type: 'button', onclick: () => finish(false) }, h('span', null, null, ...mnemonic('&No', h('span'))));
      card.append(
        h('div', 'dlg-uac-head', null, h('div', 'dlg-uac-caption', { text: 'User Account Control' }),
          h('div', 'dlg-uac-q', { id: qid, text: known ? 'Do you want to allow this app to make changes to your device?'
            : 'Do you want to allow this app from an unknown publisher to make changes to your device?' })),
        h('div', 'dlg-uac-body', null,
          h('div', 'dlg-uac-app', null, icon(appIcon || 'object/app-generic', 32), h('span', null, { text: app })),
          h('div', null, { text: known ? `Verified publisher: ${publisher}` : 'Publisher: Unknown' }),
          h('div', null, { text: 'File origin: Hard drive on this computer' }),
          more, toggle),
        h('div', 'dlg-uac-foot', null, yes, no),
      );
      screen.append(card);
      const stops = () => [...card.querySelectorAll('button')].filter((b) => b.checkVisibility());
      const key = (e) => {
        if (e.key === 'Tab') {
          e.preventDefault();
          const list = stops(), i = list.indexOf(card.ownerDocument.activeElement);
          list[(i + (e.shiftKey ? list.length - 1 : 1)) % list.length]?.focus();
        } else if (e.key === 'Escape') { e.preventDefault(); finish(false); }
        else if ((e.altKey || !e.ctrlKey) && (e.key === 'y' || e.key === 'Y')) { e.preventDefault(); finish(true); }
        else if ((e.altKey || !e.ctrlKey) && (e.key === 'n' || e.key === 'N')) { e.preventDefault(); finish(false); }
        else if (e.key === 'Alt') card.classList.add('dlg-cues');
        else if (e.key !== 'Enter' && e.key !== ' ') e.preventDefault();
      };
      const focusCard = () => (known ? yes : no).focus();
      let ended = false, fdoc = null;
      const desk = secureDesktop((d) => {
        fdoc = d;
        d.body.append(screen);
        d.addEventListener('keydown', key, true);
        Shell.animate(desk.frame, [{ opacity: 0 }, { opacity: 1 }], { duration: 250 });
        Shell.animate(card, [{ opacity: 0, transform: 'scale(0.96)' }, { opacity: 1, transform: 'none' }], { duration: 250 });
        if (!ended) focusCard();
      });
      /* Something in the frozen shell that grabs focus (a timer) gives it straight back. */
      const refocus = (e) => { if (e.target !== desk.frame && fdoc) focusCard(); };
      document.addEventListener('focusin', refocus, true);
      function finish(v) {
        if (ended) return;
        ended = true;
        fdoc?.removeEventListener('keydown', key, true);
        document.removeEventListener('focusin', refocus, true);
        desk.frame.style.pointerEvents = 'none';
        desk.release();
        Shell.animate(desk.frame, [{ opacity: 1 }, { opacity: 0 }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' }).then(() => desk.frame.remove());
        if (before?.isConnected && before !== document.body) before.focus?.({ preventScroll: true });
        else document.activeElement?.blur?.(); // the keyboard leaves the secure desktop
        resolve(v);
      }
    });
  }

  /* ---------- file operations ---------- */
  const PROTECTED = ['c', 'f-c0', 'f-c1', 'f-c2'];
  const driveOf = (id) => (Shell.fs.path(id) || []).find((p) => p.kind === 'drive')?.id || 'c';
  const nameClash = (src, to) => kids(to).find((i) => i.id !== src.id && i.name.toLowerCase() === src.name.toLowerCase()) || null;
  const stemExt = (it) => (hidesExt(it) ? [it.name.slice(0, -(it.ext.length + 1)), it.ext] : [it.name, null]);
  /* fs has no overwrite: a replaced item leaves every listing and search for good. */
  function hardDelete(id) {
    const it = get(id);
    if (!it) return;
    walk(id).forEach((c) => { c.deleted = true; });
    it.deleted = true;
    Shell.fs.move(id, null);
  }
  function conflictsOf(src, to, out = []) {
    const clash = src.parent === to ? null : nameClash(src, to);
    if (!clash) return out;
    if (src.kind === 'folder' && clash.kind === 'folder') kids(src.id).forEach((c) => conflictsOf(c, clash.id, out));
    else out.push({ src, dest: clash });
    return out;
  }
  const opWord = { copy: ['Copying', 'copy'], move: ['Moving', 'move'], delete: ['Deleting', 'delete'] };
  const nameOfFolder = (id) => (id === 'recycle' ? 'Recycle Bin' : shown(get(id)) || osPath(id));
  /* "Copying 3 items from Downloads to Desktop", the folder names as links that open them. */
  function headline(op, n, from, to) {
    const link = (id) => h('button', 'dlg-link', { type: 'button', text: nameOfFolder(id), onclick: () => Shell.apps.launch('explorer', { folder: id }) });
    return h('div', 'dlg-op-line', null, `${opWord[op][0]} ${n} item${n === 1 ? '' : 's'} from `, link(from), ...(to && op !== 'delete' ? [' to ', link(to)] : []));
  }

  D.fileOperation = async ({ op = 'copy', ids = [], to, owner } = {}) => {
    const srcs = ids.map(get).filter((it) => it && it.kind !== 'drive' && it.kind !== 'system' && !it.known && !it.deleted);
    if (!srcs.length || !Shell.fs) return { done: [], items: [] };
    if (op === 'delete') return runOp({ op, srcs, to: null, decisions: new Map(), owner });
    const dest = get(to);
    if (!dest || !(dest.kind === 'folder' || dest.kind === 'drive')) return { done: [], items: [] };
    /* not into itself, not a move onto its own folder */
    const ok = srcs.filter((s) => !Shell.fs.path(to).some((p) => p.id === s.id) && !(op === 'move' && s.parent === to));
    if (!ok.length) return { done: [], items: [] };
    if (PROTECTED.includes(to) || Shell.fs.path(to).some((p) => PROTECTED.includes(p.id) && p.id !== 'c')) {
      const go = await accessDenied(op, ok, to, owner);
      if (!go) return { done: [], items: [] };
    }
    const conflicts = ok.flatMap((s) => conflictsOf(s, to));
    const decisions = new Map();
    if (conflicts.length) {
      const choice = await replaceOrSkip(op, ok, to, conflicts, owner);
      if (!choice) return { done: [], items: [] };
      conflicts.forEach((c) => decisions.set(c.src.id, choice.get?.(c.src.id) || choice));
    }
    return runOp({ op, srcs: ok, to, decisions, owner });
  };

  /* Windows' Destination Folder Access Denied: Continue elevates through UAC. */
  function accessDenied(op, srcs, to, owner) {
    const dest = get(to);
    const root = h('div', 'dlg-conflict');
    const res = (v) => () => s.finish(v);
    root.append(
      h('div', 'dlg-conflict-main', null,
        h('div', 'dlg-conflict-title', { text: `You'll need to provide administrator permission to ${opWord[op][1]} to this folder` }),
        h('div', 'dlg-denied-item', null, icon(iconOf(dest), 32), h('div', null, null,
          h('div', 'dlg-strong', { text: dest.name }), h('div', 'dlg-dim', { text: `Date created: ${shortDate(dest.modified)}` })))),
      h('div', 'dlg-foot', null,
        btn('&Continue', { def: true, shield: true, on: res('continue') }), btn('&Skip', { on: res(null) }), btn('Cancel', { on: res(null) })),
    );
    const s = dialog({ title: 'Destination Folder Access Denied', icon: 'object/folder', appId: 'explorer', owner, width: 464, content: root });
    if (!s.win) return Promise.resolve(false);
    dialogKeys(root, { onEscape: () => s.finish(null) });
    root.querySelector('.dlg-btn')?.focus();
    return s.done.then((v) => (v === 'continue' ? D.uac({ app: 'File Operation', icon: 'object/app-explorer', publisher: 'Mihawk OS', verified: true, path: 'C:\\Windows\\explorer.exe' }) : false));
  }

  /* Replace or Skip Files -> 'replace' | 'skip' | Map(srcId -> 'replace'|'skip'|'both') | null */
  function replaceOrSkip(op, srcs, to, conflicts, owner) {
    const one = conflicts.length === 1;
    const from = srcs[0].parent;
    const root = h('div', 'dlg-conflict');
    const option = (glyph, cls, label, on) => h('button', `dlg-cmdlink dlg-choice ${cls}`, { type: 'button', onclick: on },
      icon(glyph, 20, 'dlg-choice-icon'), h('span', 'dlg-cmdlink-label', { text: label }));
    const opts = [
      option('fluent/checkmark', 'replace', one ? 'Replace the file in the destination' : 'Replace the files in the destination', () => s.finish('replace')),
      option('fluent/arrow-clockwise', 'skip', one ? 'Skip this file' : 'Skip these files', () => s.finish('skip')),
      option('fluent/copy', 'decide', one ? 'Compare info for both files' : 'Let me decide for each file', () => s.finish('decide')),
    ];
    root.append(h('div', 'dlg-conflict-main', null,
      headline(op, srcs.length, from, to),
      h('div', 'dlg-conflict-title', { text: one ? `The destination already has a file named "${conflicts[0].src.name}"` : `The destination has ${conflicts.length} files with the same names` }),
      h('div', 'dlg-choices', null, opts)));
    const s = dialog({ title: 'Replace or Skip Files', icon: 'object/folder', appId: 'explorer', owner, width: 476, content: root });
    if (!s.win) return Promise.resolve(null);
    root.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { e.preventDefault(); s.finish(null); }
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        const i = opts.indexOf(document.activeElement);
        opts[clamp(i + (e.key === 'ArrowDown' ? 1 : -1), 0, opts.length - 1)].focus();
      }
    });
    opts[0].focus();
    return s.done.then((v) => (v === 'decide' ? decideEach(op, from, to, conflicts, owner) : v));
  }

  /* "N File Conflicts": keep the copied file, the one already there, or both (numbered). */
  function decideEach(op, from, to, conflicts, owner) {
    const n = conflicts.length;
    const root = h('div', 'dlg-decide');
    const side = (it, which) => {
      const c = h('input', 'dlg-check', { type: 'checkbox', 'aria-label': `${which}: ${it.name}` });
      const cell = h('label', 'dlg-decide-cell', null, c, icon(iconOf(it), 32),
        h('div', null, null, h('div', null, { text: shortDate(it.modified) }), h('div', 'dlg-dim', { text: sig3(it.size || 0) })));
      cell.input = c;
      return cell;
    };
    const pairs = conflicts.map((c) => ({ c, a: side(c.src, 'Copied'), b: side(c.dest, 'Existing') }));
    const allA = h('input', 'dlg-check', { type: 'checkbox' }), allB = h('input', 'dlg-check', { type: 'checkbox' });
    const sync = () => {
      allA.checked = pairs.every((p) => p.a.input.checked);
      allA.indeterminate = !allA.checked && pairs.some((p) => p.a.input.checked);
      allB.checked = pairs.every((p) => p.b.input.checked);
      allB.indeterminate = !allB.checked && pairs.some((p) => p.b.input.checked);
      cont.disabled = !pairs.every((p) => p.a.input.checked || p.b.input.checked);
    };
    allA.addEventListener('change', () => { pairs.forEach((p) => { p.a.input.checked = allA.checked; }); sync(); });
    allB.addEventListener('change', () => { pairs.forEach((p) => { p.b.input.checked = allB.checked; }); sync(); });
    const same = pairs.filter((p) => p.c.src.size === p.c.dest.size && p.c.src.modified === p.c.dest.modified);
    const skipSame = check(`Skip ${same.length} file${same.length === 1 ? '' : 's'} with the same date and size`, { disabled: !same.length, on: (v) => {
      same.forEach((p) => { p.a.input.checked = false; p.b.input.checked = v || p.b.input.checked; });
      sync();
    } });
    const cont = btn('&Continue', { def: true, disabled: true, on: () => s.finish(new Map(pairs.map((p) => [p.c.src.id, p.a.input.checked && p.b.input.checked ? 'both' : p.a.input.checked ? 'replace' : 'skip']))) });
    root.append(
      h('div', 'dlg-conflict-main', null,
        h('div', 'dlg-conflict-title', { text: 'Which files do you want to keep?' }),
        h('div', null, { text: 'If you select both versions, the copied file will have a number added to its name.' }),
        h('div', 'dlg-decide-grid', null,
          h('label', 'dlg-decide-colhead', null, allA, `Files from ${nameOfFolder(from)}`),
          h('label', 'dlg-decide-colhead', null, allB, `Files already in ${nameOfFolder(to)}`),
          pairs.map((p) => [h('div', 'dlg-decide-name', { text: p.c.src.name }), p.a, p.b]))),
      h('div', 'dlg-foot', null, skipSame, h('span', 'dlg-fill'), cont, btn('Cancel', { on: () => s.finish(null) })),
    );
    [...pairs.flatMap((p) => [p.a.input, p.b.input])].forEach((c) => c.addEventListener('change', sync));
    const s = dialog({ title: `${n} File Conflict${n === 1 ? '' : 's'}`, icon: 'object/folder', appId: 'explorer', owner, width: 560, content: root });
    if (!s.win) return Promise.resolve(null);
    dialogKeys(root, { onEscape: () => s.finish(null) });
    pairs[0].a.input.focus();
    return s.done;
  }

  /* Runs the plan: instant when it would take under a second, otherwise with the progress
     dialog (x% complete, Pause/Cancel, More details with the speed graph). Each item lands in
     the file system the moment its bytes are through, so Cancel keeps what is done. */
  function runOp({ op, srcs, to, decisions, owner }) {
    const done = [], made = [];
    const place = (src, toId) => {
      const clash = src.parent === toId ? null : nameClash(src, toId);
      let how = null;
      if (clash) {
        if (src.kind === 'folder' && clash.kind === 'folder') {
          kids(src.id).forEach((c) => place(c, clash.id));
          if (op === 'move' && !kids(src.id).length) hardDelete(src.id);
          return clash;
        }
        how = decisions.get(src.id) || 'skip';
        if (how === 'skip') return null;
        if (how === 'replace') hardDelete(clash.id);
      }
      const unique = () => { const [stem, ext] = stemExt(src); return Shell.fs.uniqueName(toId, stem, ext); };
      if (op === 'copy') {
        const n = Shell.fs.copy(src.id, toId);
        if (n && how === 'both') Shell.fs.rename(n.id, unique());
        return n;
      }
      if (how === 'both') Shell.fs.rename(src.id, unique());
      Shell.fs.move(src.id, toId);
      return get(src.id);
    };
    const perform = (src) => {
      const r = op === 'delete' ? (Shell.fs.remove(src.id), get(src.id)) : place(src, to);
      done.push(src.id);
      if (r) made.push(r);
    };
    const weight = (it) => (op === 'delete' ? 4e6 : Math.max(isContainer(it) ? treeSize(it) + 4096 * (walk(it.id).length + 1) : it.size || 0, 4096));
    const sizes = srcs.map(weight);
    const total = sizes.reduce((a, b) => a + b, 0);
    const sameDrive = op === 'move' && srcs.every((x) => driveOf(x.id) === driveOf(to));
    const speedBase = op === 'delete' ? 90e6 : driveOf(to) === 'd' || srcs.some((x) => driveOf(x.id) === 'd') ? 46e6 : 52e6;
    if (sameDrive || total / speedBase < 1) {
      srcs.forEach(perform);
      return Promise.resolve({ done, items: made });
    }
    return new Promise((resolve) => progressDialog({ op, srcs, to, sizes, total, speedBase, perform, owner, end: () => resolve({ done, items: made }) }));
  }

  function progressDialog({ op, srcs, to, sizes, total, speedBase, perform, owner, end }) {
    const root = h('div', 'dlg-prog');
    const pctEl = h('div', 'dlg-prog-pct', { text: '0% complete' }); // measured before the first draw: it must hold its line
    const bar = h('div', 'dlg-prog-bar', { role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100' }, h('div', 'dlg-prog-fill'));
    const pause = h('button', 'dlg-prog-ib', { type: 'button', 'aria-label': 'Pause', 'data-tooltip': 'Pause', onclick: () => setPaused(!paused) }, icon('object/dialog-pause', 16));
    const cancel = h('button', 'dlg-prog-ib', { type: 'button', 'aria-label': 'Cancel', 'data-tooltip': 'Cancel', onclick: () => stop() }, icon('fluent/dismiss', 16));
    const svgNS = 'http://www.w3.org/2000/svg';
    const graph = document.createElementNS(svgNS, 'svg');
    graph.setAttribute('class', 'dlg-prog-graph');
    graph.setAttribute('preserveAspectRatio', 'none');
    const areaPath = document.createElementNS(svgNS, 'path');
    areaPath.setAttribute('class', 'dlg-prog-area');
    const line = document.createElementNS(svgNS, 'line');
    line.setAttribute('class', 'dlg-prog-speedline');
    graph.append(areaPath, line);
    const speedLabel = h('div', 'dlg-prog-speed');
    const graphBox = h('div', 'dlg-prog-graphbox', null, graph, speedLabel);
    const nameEl = h('span'), timeEl = h('span'), leftEl = h('span');
    const details = h('div', 'dlg-prog-details', null,
      h('div', 'dlg-k', { text: 'Name:' }), nameEl, h('div', 'dlg-k', { text: 'Time remaining:' }), timeEl, h('div', 'dlg-k', { text: 'Items remaining:' }), leftEl);
    const chev = h('span', 'dlg-prog-chev', null, icon('fluent/chevron-down', 12));
    const moreLabel = h('span', null, { text: 'More details' });
    const moreBtn = h('button', 'dlg-prog-more', { type: 'button', 'aria-expanded': 'false', onclick: () => setExpanded(!expanded) }, chev, moreLabel);
    root.append(
      h('div', 'dlg-prog-main', null,
        headline(op, srcs.length, srcs[0].parent, to),
        h('div', 'dlg-prog-row', null, pctEl, h('div', 'dlg-prog-btns', null, pause, cancel)),
        bar, graphBox, details),
      h('div', 'dlg-foot dlg-prog-foot', null, moreBtn),
    );
    let expanded = false, paused = false, stopped = false, moved = 0, next = 0, speed = speedBase, lastPct = -1, last = performance.now(), started = last;
    const samples = [];
    const W = 400, H = 80;
    graph.setAttribute('viewBox', `0 0 ${W} ${H}`);
    const s = dialog({ title: '0% complete', icon: 'object/folder', appId: 'explorer', owner, width: 466, content: root,
      minimizable: true, x: undefined, y: undefined });
    if (!s.win) { srcs.forEach(perform); end(); return; }
    s.done.then(() => { stopped = true; Shell.taskbar?.setProgress?.('explorer', null); end(); });
    const collapsedH = s.win.el.offsetHeight;
    root.classList.add('expanded');
    const expandedH = Math.min(measure(root.cloneNode(true), 464).h + 34, area().height);
    root.classList.remove('expanded');
    function setExpanded(on) {
      expanded = on;
      root.classList.toggle('expanded', on);
      /* wm has no resize API: a class with an !important height (dialogs.css) grows the window. */
      s.win.el.style.setProperty('--dlg-h', `${on ? Math.max(expandedH, collapsedH) : collapsedH}px`);
      s.win.el.classList.toggle('dlg-grown', on);
      moreLabel.textContent = on ? 'Fewer details' : 'More details';
      moreBtn.setAttribute('aria-expanded', String(on));
      chev.replaceChildren(icon(on ? 'fluent/chevron-up' : 'fluent/chevron-down', 12));
      draw();
    }
    function setPaused(on) {
      paused = on;
      root.classList.toggle('paused', on);
      pause.replaceChildren(icon(on ? 'fluent/play' : 'object/dialog-pause', 16));
      pause.setAttribute('aria-label', on ? 'Resume' : 'Pause');
      pause.dataset.tooltip = on ? 'Resume' : 'Pause';
      lastPct = -1;
      draw();
    }
    function stop() { stopped = true; s.finish(null); }
    const fmtSpeed = (b) => `${(b / 1048576).toFixed(1)} MB/s`;
    function remaining(sec) {
      if (performance.now() - started < 1000) return 'Calculating...';
      if (sec < 60) return `About ${Math.max(1, Math.round(sec))} second${Math.round(sec) === 1 ? '' : 's'}`;
      const m = Math.round(sec / 60);
      return `About ${m} minute${m === 1 ? '' : 's'}`;
    }
    function draw() {
      const frac = Math.min(1, moved / total);
      const pct = Math.floor(frac * 100);
      if (pct !== lastPct) {
        lastPct = pct;
        s.win.setTitle(`${paused ? 'Paused - ' : ''}${pct}% complete`);
        pctEl.textContent = `${paused ? 'Paused - ' : ''}${pct}% complete`;
        bar.setAttribute('aria-valuenow', String(pct));
        Shell.taskbar?.setProgress?.('explorer', frac, paused ? 'paused' : 'normal');
      }
      root.style.setProperty('--frac', String(frac));
      const top = Math.max(...samples.map((p) => p[1]), speedBase) * 1.3;
      const y = (v) => (H - (v / top) * H).toFixed(1);
      areaPath.setAttribute('d', samples.length ? `M0 ${H} ${samples.map(([f, v]) => `L${(f * W).toFixed(1)} ${y(v)}`).join(' ')} L${(frac * W).toFixed(1)} ${H} Z` : '');
      const cur = paused ? 0 : speed;
      line.setAttribute('x1', '0'); line.setAttribute('x2', String(W));
      line.setAttribute('y1', y(cur)); line.setAttribute('y2', y(cur));
      speedLabel.textContent = `Speed: ${fmtSpeed(cur)}`;
      speedLabel.style.top = `${(Number(y(cur)) / H) * 100}%`;
      nameEl.textContent = srcs[Math.min(next, srcs.length - 1)].name;
      timeEl.textContent = paused ? 'Paused' : remaining((total - moved) / Math.max(speed, 1));
      const left = srcs.length - next;
      leftEl.textContent = `${left} (${sig3(Math.max(0, total - moved))})`;
    }
    function tick() {
      if (stopped || !alive(s.win)) return;
      const now = performance.now(), dt = Math.min(0.5, (now - last) / 1000);
      last = now;
      if (!paused) {
        /* the copy engine's speed wanders around its rate, as the real graph does */
        speed = clamp(speed + (speedBase * (0.92 + Math.random() * 0.16) - speed) * 0.35, speedBase * 0.6, speedBase * 1.4);
        moved = Math.min(total, moved + speed * dt);
        samples.push([moved / total, speed]);
        let edge = sizes.slice(0, next + 1).reduce((a, b) => a + b, 0);
        while (next < srcs.length && moved >= edge - 0.5) { perform(srcs[next]); next++; edge += sizes[next] || 0; }
      }
      draw();
      if (next >= srcs.length) { stopped = true; setTimeout(() => s.finish('done'), 150); return; }
      setTimeout(tick, 100);
    }
    dialogKeys(root, { onEscape: () => stop() });
    draw();
    setTimeout(tick, 100);
    cancel.focus();
    progressHooks.expand = () => setExpanded(true);
  }
  const progressHooks = {};

  /* ---------- review captures: ?open=<name> ---------- */
  Shell.on('boot', () => {
    const want = (n) => Shell.debugOpen.includes(n);
    if (!Shell.debugOpen.some((n) => /^(msgbox|taskdialog|run|createtask|properties|changeicon|openwith|fileopen|filesave|uac|copyprogress|conflict)/.test(n))) return;
    setTimeout(() => {
      if (want('msgbox')) D.messageBox({ title: 'Notepad', icon: 'warning', buttons: ['Yes', 'No', 'Cancel'], app: 'notepad',
        text: 'The text in the Untitled file has changed.\n\nDo you want to save the changes?' });
      if (want('msgbox-error')) D.messageBox({ title: 'launcher.exe', icon: 'error', text: 'The code execution cannot proceed because launcher64.dll was not found. Reinstalling the program may fix this problem.' });
      if (want('msgbox-info')) D.messageBox({ title: 'Mihawk OS', icon: 'info', text: 'The prefix for Example Game is ready.' });
      if (want('taskdialog')) D.taskDialog({ title: 'Mihawk OS', icon: 'question', instruction: 'How do you want to start Example Game?',
        content: 'The game can take the whole screen or run in a window on the desktop.',
        commandLinks: [{ label: 'Play in full screen', note: 'Best performance. Press Win to return to the desktop.' },
          { label: 'Play in a window', note: 'Keep the taskbar and other windows in view.' }], checkbox: 'Don\'t ask me again' });
      if (want('run')) D.run({});
      if (want('createtask')) D.run({ createTask: true, owner: Shell.apps.get('taskmgr') ? Shell.apps.launch('taskmgr') : null });
      if (want('properties')) D.properties('f-readme');
      if (want('properties-shortcut') || want('changeicon')) D.properties('sc-game');
      if (want('changeicon')) setTimeout(() => [...document.querySelectorAll('.dlg-props .dlg-btn')].find((b) => b.textContent === 'Change Icon...')?.click(), 100);
      if (want('properties-folder')) D.properties('documents');
      if (want('properties-drive')) D.properties('c');
      if (want('properties-recycle')) D.properties('recycle');
      if (want('openwith')) D.openWith('f-doc1');
      if (want('fileopen')) D.fileOpen({ title: 'Open', folder: 'documents', filters: [{ label: 'Text Documents', exts: ['txt'] }, { label: 'All files', exts: ['*'] }] });
      if (want('filesave')) D.fileSave({ title: 'Save As', name: '*.txt', ext: 'txt', folder: 'documents' });
      if (want('uac')) D.uac({ app: 'Terminal', icon: 'object/app-terminal', publisher: 'Mihawk OS', verified: true });
      if (want('uac-unknown')) D.uac({ app: 'setup.exe', publisher: null, verified: false, path: 'C:\\Users\\Mihawk\\Downloads\\setup.exe' });
      if (want('copyprogress') || want('copyprogress-details')) {
        D.fileOperation({ op: 'copy', ids: ['f-dl0', 'f-dl2'], to: 'desktop' });
        if (want('copyprogress-details')) setTimeout(() => progressHooks.expand?.(), 300);
      }
      if (want('conflict') || want('conflict-decide')) {
        Shell.fs.create('desktop', { name: 'Notes.txt', kind: 'file', ext: 'txt', icon: 'object/document', size: 1800 });
        Shell.fs.create('desktop', { name: 'Budget 2026.xlsx', kind: 'file', ext: 'xlsx', icon: 'object/document', size: 46e3 });
        const ids = ['f-doc1', 'f-doc0'];
        if (want('conflict')) D.fileOperation({ op: 'copy', ids, to: 'desktop' });
        else decideEach('copy', 'documents', 'desktop', ids.map((id) => ({ src: get(id), dest: kids('desktop').find((i) => i.name === get(id).name) })), null);
      }
    }, 150);
  });
})();
