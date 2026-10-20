/* Small apps: Notepad, Calculator, Terminal and the game launcher, as Windows 11 (22H2/23H2)
   draws them.
   Owns: the app registrations `notepad`, `calculator`, `terminal` and `game`, and everything
   inside their windows: Notepad's title bar tabs, File/Edit/View menu bar, editor, find and
   replace panel, status bar, Open / Save as through Shell.dialogs' file dialogs and its
   "Do you want to save changes" dialog; Calculator's Standard mode (keypad and keyboard, memory,
   history/memory pane at wide sizes and flyouts at narrow ones, navigation pane, Keep on top
   compact view); Terminal's
   tab row, profiles menu and a tiny PowerShell / Command Prompt over Shell.fs (with start,
   taskmgr, hang and notify); the game toast and its splash window.
   Windows come from Shell.wm, menus and tooltips from js/ui.js, toasts from Shell.notify.
   Debug captures: ?open=notepad, notepad-save-prompt, calculator, terminal, terminal-help,
   terminal-hang. */
(() => {
  'use strict';

  const Shell = window.Shell;

  /* ---------- helpers ---------- */
  function h(tag, cls, props, kids) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    for (const [k, v] of Object.entries(props || {})) {
      if (v == null || v === false) continue;
      if (k === 'text') el.textContent = v;
      else if (k === 'html') el.innerHTML = v; // only this file's own label literals
      else el.setAttribute(k, v === true ? '' : String(v));
    }
    if (kids) el.append(...kids);
    return el;
  }
  function iconButton(cls, icon, label, size = 16) {
    const b = h('button', `btn subtle icon-only ${cls}`.trim(), { type: 'button', 'aria-label': label, 'data-tooltip': label });
    b.append(Shell.icon(icon, { size }));
    return b;
  }
  /* Phones get maximized windows, as Windows does on small tablets; the wm fits the rest. */
  function openWindow(opts) {
    if (!Shell.wm?.open) { console.warn('[small-apps] Shell.wm is missing'); return null; }
    const win = Shell.wm.open(opts);
    if (opts.maximizable !== false && Shell.wm.workArea().width < 600) win.maximize();
    return win;
  }
  /* A menu that drops from a button (menu bar items, the profiles chevron, settings). */
  function dropMenu(btn, items, onClose) {
    if (!Shell.menu?.show) return;
    const r = btn.getBoundingClientRect();
    Shell.menu.show({
      x: r.left, y: r.bottom, anchorRect: r, placement: 'bottom-start', items,
      onClose: () => { btn.classList.remove('open'); onClose?.(); },
    });
    if (Shell.menu.isOpen?.()) btn.classList.add('open');
  }
  /* Runs fn when this app's window gains focus; returns the unsubscribe. */
  const onFocus = (getWin, fn) => Shell.on('window', ({ type, win }) => { if (type === 'focus' && win === getWin()) setTimeout(fn); });

  /* ---------- title bar tabs (WinUI TabView, as Notepad and Terminal draw it) ---------- */
  function tabStrip(cls, { lead, addLabel, onAdd, onSelect, onClose, extra = [] }) {
    const list = h('div', 'sa-tab-list', { role: 'tablist' });
    const add = iconButton('sa-tab-add', 'fluent/add', addLabel);
    add.addEventListener('click', onAdd);
    const el = h('div', `sa-tabs ${cls}`, null, [...(lead ? [lead] : []), list, add, ...extra]);
    const tabs = new Map();
    return {
      el,
      add(id, title, icon) {
        const t = h('div', 'sa-tab', { role: 'tab', tabindex: '-1', 'aria-selected': 'false', 'data-tooltip': title });
        const label = h('span', 'sa-tab-title', { text: title });
        const close = h('button', 'sa-tab-close', { type: 'button', tabindex: '-1', 'aria-label': 'Close tab', 'data-tooltip': 'Close tab' },
          [Shell.icon('fluent/dismiss', { size: 16 }), h('span', 'sa-tab-dot')]);
        if (icon) t.append(Shell.icon(icon, { size: 16, className: 'sa-tab-icon' }));
        t.append(label, close);
        t.addEventListener('pointerdown', (e) => { if (e.button === 0 && !close.contains(e.target)) onSelect(id); });
        t.addEventListener('auxclick', (e) => { if (e.button === 1) onClose(id); }); // middle click closes, as TabView does
        close.addEventListener('click', () => onClose(id));
        list.append(t);
        tabs.set(id, { t, label });
        Shell.animate(t, [{ opacity: 0, transform: 'translateY(8px)' }, { opacity: 1, transform: 'none' }], { duration: 167 });
      },
      remove(id) { tabs.get(id)?.t.remove(); tabs.delete(id); },
      select(id) {
        for (const [k, { t }] of tabs) {
          t.classList.toggle('selected', k === id);
          t.setAttribute('aria-selected', String(k === id));
        }
        const t = tabs.get(id)?.t; // keep the selected tab in view when tabs overflow
        if (t && t.offsetLeft < list.scrollLeft) list.scrollLeft = t.offsetLeft - 8;
        else if (t && t.offsetLeft + t.offsetWidth > list.scrollLeft + list.clientWidth) list.scrollLeft = t.offsetLeft + t.offsetWidth - list.clientWidth + 8;
      },
      setTitle(id, title) {
        const x = tabs.get(id);
        if (x && x.label.textContent !== title) { x.label.textContent = title; x.t.dataset.tooltip = title; }
      },
      setDirty(id, dirty) { tabs.get(id)?.t.classList.toggle('dirty', dirty); },
    };
  }

  /* ======================= Notepad ======================= */
  const notepads = []; // open Notepad windows, most recently focused last
  const NP_FONTS = ['Consolas', 'Cascadia Mono', 'Courier New', 'Lucida Console'];
  const NP_SIZES = [8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 36];
  let npFolder = 'documents'; // where Open and Save as start, as Notepad remembers the last folder

  /* Windows 11 Notepad opens a file in a new tab of the window in use. */
  function openNotepad(args = {}) {
    const file = args.file ? Shell.fs.get(args.file) : null;
    const host = file && (notepads.find((n) => n.win.isFocused()) || notepads[notepads.length - 1]);
    if (host) { host.openFile(file); host.win.focus(); return host.win; }
    return createNotepad(file)?.win || null;
  }

  function createNotepad(file) {
    const np = { tabs: [], cur: null, zoom: 100, wrap: true, status: true, font: 'Consolas', size: 11, win: null, prompt: null };
    let seq = 0, winTitle = '', menuOwner = null, closing = false;
    /* a menu gives focus back to its button; typing belongs to the text unless the command moved it */
    const backToText = () => setTimeout(() => { if (menubar.contains(document.activeElement)) text.focus({ preventScroll: true }); });

    const strip = tabStrip('np-tabs', {
      lead: Shell.icon('object/app-notepad', { size: 16, className: 'np-app-icon' }),
      addLabel: 'Add new tab (Ctrl+N)',
      onAdd: () => addTab(null),
      onSelect: select,
      onClose: requestCloseTab,
    });

    /* menu bar: File, Edit, View; moving across the bar while a menu is open switches menus */
    const menubar = h('div', 'np-menubar', { role: 'menubar' });
    for (const name of ['File', 'Edit', 'View']) {
      const b = h('button', 'btn subtle np-menu-btn', { type: 'button', role: 'menuitem', 'aria-haspopup': 'menu', text: name });
      const open = () => {
        dropMenu(b, menus[name](), () => { if (menuOwner === b) menuOwner = null; backToText(); });
        if (Shell.menu?.isOpen?.()) menuOwner = b;
      };
      b.addEventListener('click', open);
      b.addEventListener('pointerenter', () => { if (menuOwner && menuOwner !== b) open(); });
      menubar.append(b);
    }
    const gear = iconButton('np-gear', 'fluent/settings', 'Settings');
    gear.addEventListener('click', () => dropMenu(gear, settingsMenu(), backToText));
    menubar.append(h('div', 'np-menubar-fill'), gear);

    const text = h('textarea', 'np-text', { spellcheck: 'false', autocomplete: 'off', 'aria-label': 'Text editor' });

    /* find and replace: the floating panel at the editor's top right */
    const findIn = h('input', 'textbox np-find-input', { type: 'text', placeholder: 'Find', 'aria-label': 'Find' });
    const replIn = h('input', 'textbox np-find-input', { type: 'text', placeholder: 'Replace', 'aria-label': 'Replace' });
    const toggleRepl = iconButton('np-find-toggle', 'fluent/chevron-down', 'Toggle replace');
    const prevBtn = iconButton('', 'fluent/arrow-up', 'Find previous (Shift+F3)');
    const nextBtn = iconButton('', 'fluent/arrow-down', 'Find next (F3)');
    const closeFind = iconButton('', 'fluent/dismiss', 'Close (Esc)');
    const replBtn = h('button', 'btn np-find-action', { type: 'button', text: 'Replace' });
    const replAllBtn = h('button', 'btn np-find-action', { type: 'button', text: 'Replace all' });
    const find = h('div', 'np-find acrylic surface-flyout', { role: 'search', hidden: true }, [
      h('div', 'np-find-row', null, [toggleRepl, findIn, prevBtn, nextBtn, closeFind]),
      h('div', 'np-find-row np-replace-row', null, [replIn, replBtn, replAllBtn]),
    ]);
    const editor = h('div', 'np-editor', null, [text, find]);

    const st = {
      pos: h('span', 'np-st'), chars: h('span', 'np-st'), zoom: h('span', 'np-st'),
      eol: h('span', 'np-st np-st-opt', { text: 'Windows (CRLF)' }), enc: h('span', 'np-st np-st-opt', { text: 'UTF-8' }),
    };
    const status = h('div', 'np-status', { role: 'status' }, [st.pos, st.chars, h('span', 'np-st-fill'), st.zoom, st.eol, st.enc]);
    const root = h('div', 'np', null, [menubar, editor, status]);

    /* ---- tabs ---- */
    function sync() {
      const t = np.cur;
      if (t) Object.assign(t, { text: text.value, sel: [text.selectionStart, text.selectionEnd], scroll: text.scrollTop });
    }
    function addTab(item, { focus = true } = {}) {
      const t = { id: `t${++seq}`, title: item ? item.name : 'Untitled', fileId: item?.id || null, text: item?.content ?? '', sel: [0, 0], scroll: 0 };
      t.saved = t.text;
      np.tabs.push(t);
      strip.add(t.id, t.title);
      if (focus) select(t.id);
      return t;
    }
    function select(id) {
      const t = np.tabs.find((x) => x.id === id);
      if (!t) return;
      if (t !== np.cur) {
        sync();
        np.cur = t;
        text.value = t.text;
        text.setSelectionRange(t.sel[0], t.sel[1]);
        text.scrollTop = t.scroll;
        strip.select(id);
        refresh();
      }
      text.focus({ preventScroll: true });
    }
    function removeTab(id) {
      const i = np.tabs.findIndex((x) => x.id === id);
      if (i < 0) return;
      const [t] = np.tabs.splice(i, 1);
      strip.remove(id);
      if (!np.tabs.length) { np.cur = null; np.win?.close(); return; }
      if (t === np.cur) { np.cur = null; select(np.tabs[Math.min(i, np.tabs.length - 1)].id); }
    }
    const dirty = (t) => t.text !== t.saved; // after sync()
    /* Closing a tab with unsaved changes asks first, as Notepad does; true when the tab closed. */
    async function requestCloseTab(id) {
      const t = np.tabs.find((x) => x.id === id);
      if (!t || np.prompt) return false;
      sync();
      if (dirty(t)) {
        select(t.id);
        const answer = await askToSave(t);
        if (answer === 'cancel' || (answer === 'save' && !(await save(false)))) return false;
      }
      removeTab(id);
      return true;
    }
    /* Closing the window asks for each unsaved tab in turn; Cancel keeps the window open. */
    async function requestCloseWindow() {
      for (const t of [...np.tabs]) {
        sync();
        if (np.tabs.includes(t) && dirty(t) && !(await requestCloseTab(t.id))) return;
      }
      closing = true;
      np.win?.close();
    }
    /* Notepad's ContentDialog: smoke over the whole window, "Notepad" title, Save (default) /
       Don't save / Cancel. Resolves 'save', 'dont' or 'cancel' (Esc). */
    function askToSave(t) {
      return new Promise((resolve) => {
        const back = document.activeElement;
        const uid = `np-dlg-${Date.now()}`;
        const buttons = [['save', 'Save', 'btn accent'], ['dont', "Don't save", 'btn'], ['cancel', 'Cancel', 'btn']].map(([v, label, cls]) => {
          const b = h('button', cls, { type: 'button', text: label });
          b.addEventListener('click', () => done(v));
          return b;
        });
        const dlg = h('div', 'np-dialog', { role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': `${uid}-t`, 'aria-describedby': `${uid}-d` }, [
          h('div', 'np-dialog-body', null, [
            h('h2', 'np-dialog-title', { id: `${uid}-t`, text: 'Notepad' }),
            h('p', 'np-dialog-text', { id: `${uid}-d`, text: `Do you want to save changes to ${t.title}?` }),
          ]),
          h('div', 'np-dialog-foot', null, buttons),
        ]);
        const smoke = h('div', 'np-smoke', null, [dlg]);
        const behind = [menubar, editor, status, strip.el];
        behind.forEach((el) => { el.inert = true; });
        smoke.addEventListener('mousedown', (e) => { if (!dlg.contains(e.target)) e.preventDefault(); }); // focus stays in the dialog
        dlg.addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); done('cancel'); } });
        np.prompt = smoke;
        root.append(smoke);
        if (np.win?.isMinimized()) np.win.restore(); // the question must be seen (taskbar "Close all windows")
        np.win?.focus();
        Shell.animate(smoke, [{ opacity: 0 }, { opacity: 1 }], { duration: 83, easing: 'linear' });
        Shell.animate(dlg, [{ transform: 'scale(1.05)' }, { transform: 'none' }], { duration: 250 });
        buttons[0].focus({ preventScroll: true });
        function done(v) {
          if (np.prompt !== smoke) return;
          np.prompt = null;
          behind.forEach((el) => { el.inert = false; });
          smoke.style.pointerEvents = 'none';
          Shell.animate(dlg, [{ transform: 'none' }, { transform: 'scale(1.05)' }], { duration: 167, easing: Shell.ease.out });
          Shell.animate(smoke, [{ opacity: 1 }, { opacity: 0 }], { duration: 167, easing: 'linear', fill: 'forwards' }).then(() => smoke.remove());
          (back?.isConnected && !back.closest('[inert]') && back !== document.body ? back : text).focus({ preventScroll: true });
          resolve(v);
        }
      });
    }
    const cycle = (d) => {
      const i = np.tabs.indexOf(np.cur);
      select(np.tabs[(i + d + np.tabs.length) % np.tabs.length].id);
    };
    np.addTab = addTab;
    np.openFile = (item) => {
      const open = np.tabs.find((t) => t.fileId === item.id);
      if (open) select(open.id); else addTab(item);
    };

    /* ---- state to screen ---- */
    function refresh() {
      const t = np.cur;
      if (!t) return;
      t.text = text.value;
      strip.setTitle(t.id, t.title);
      strip.setDirty(t.id, t.text !== t.saved);
      const title = `${t.title} - Notepad`;
      if (title !== winTitle && np.win) { winTitle = title; np.win.setTitle(title); }
      updateStatus();
    }
    function updateStatus() {
      const v = text.value, a = text.selectionStart, b = text.selectionEnd;
      const caret = text.selectionDirection === 'backward' ? a : b;
      const before = v.slice(0, caret);
      st.pos.textContent = `Ln ${before.split('\n').length}, Col ${caret - before.lastIndexOf('\n')}`;
      const count = (n) => `${n.toLocaleString('en-US')} character${n === 1 ? '' : 's'}`;
      st.chars.textContent = a === b ? count(v.length) : `${(b - a).toLocaleString('en-US')} of ${count(v.length)}`;
      st.zoom.textContent = `${np.zoom}%`;
    }
    function applyView() {
      text.style.fontFamily = `"${np.font}", Consolas, "Cascadia Mono", monospace`;
      text.style.fontSize = `${((np.size * 4) / 3) * (np.zoom / 100)}px`; // points at 96 DPI
      text.wrap = np.wrap ? 'soft' : 'off';
      status.hidden = !np.status;
      updateStatus();
    }
    const zoom = (z) => { np.zoom = Math.max(10, Math.min(500, Math.round(z / 10) * 10)); applyView(); };

    /* ---- commands ---- */
    function exec(cmd, arg) {
      text.focus({ preventScroll: true });
      document.execCommand(cmd, false, arg); // keeps the textarea's own undo stack
      refresh();
    }
    const paste = () => navigator.clipboard?.readText?.().then((t) => t && exec('insertText', t)).catch(() => text.focus());
    const selectAll = () => { text.focus(); text.select(); updateStatus(); };
    const timeDate = () => {
      const d = Shell.now();
      exec('insertText', `${d.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' })} ${d.toLocaleDateString('en-US')}`);
    };
    /* Where Save as writes: the common file dialog, or (without Shell.dialogs) a new name in
       Documents. -> {parent, name, item?} with item set when an existing file is overwritten. */
    async function saveTarget(t) {
      const stem = t.fileId ? t.title.replace(/\.[^.]*$/, '') : 'Untitled';
      if (!Shell.dialogs?.fileSave) return { parent: 'documents', name: Shell.fs.uniqueName('documents', stem, 'txt') };
      const folder = (t.fileId && Shell.fs.get(t.fileId)?.parent) || npFolder;
      const r = await Shell.dialogs.fileSave({ title: 'Save as', name: stem, ext: 'txt', folder, owner: np.win });
      if (!r) return null;
      const name = /\.[^.\\]+$/.test(r.name) ? r.name : `${r.name}.txt`;
      npFolder = r.parent;
      const item = (r.id && Shell.fs.get(r.id)) || Shell.fs.children(r.parent).find((i) => i.kind === 'file' && i.name.toLowerCase() === name.toLowerCase());
      return { parent: r.parent, name, item };
    }
    /* Saves the current tab (Save as for an untitled one); resolves false when cancelled. */
    async function save(as) {
      sync();
      const t = np.cur;
      if (!t) return false;
      let item = !as && t.fileId ? Shell.fs.get(t.fileId) : null;
      if (!item) {
        const to = await saveTarget(t);
        if (!to) return false;
        item = to.item || Shell.fs.create(to.parent, { name: to.name, kind: 'file', ext: (to.name.match(/\.([^.]+)$/)?.[1] || '').toLowerCase(), icon: 'object/document', size: 0, content: '' });
        Object.assign(t, { fileId: item.id, title: item.name });
        sync(); // typing may have gone on while the dialog was open
      }
      Object.assign(item, { content: t.text, size: new Blob([t.text]).size });
      Shell.fs.rename(item.id, item.name); // Shell.fs has no write(): rename persists the item and stamps its time
      t.saved = t.text;
      strip.setTitle(t.id, t.title);
      strip.setDirty(t.id, false);
      refresh();
      return true;
    }
    async function saveAll() {
      sync();
      const keep = np.cur;
      for (const t of np.tabs.filter(dirty)) { select(t.id); if (!(await save(false))) break; }
      if (keep && np.tabs.includes(keep)) select(keep.id);
    }
    async function openFile() {
      if (!Shell.dialogs?.fileOpen) { Shell.apps.launch('explorer', { folder: npFolder }); return; }
      const r = await Shell.dialogs.fileOpen({
        title: 'Open', folder: npFolder, owner: np.win,
        filters: [{ label: 'Text documents (*.txt)', exts: ['txt'] }, { label: 'All files (*.*)', exts: ['*'] }],
      });
      const item = r && ((r.id && Shell.fs.get(r.id)) || Shell.fs.children(r.parent).find((i) => i.name.toLowerCase() === String(r.name).toLowerCase()));
      if (!item || !notepads.includes(np)) return;
      npFolder = item.parent;
      np.openFile(item);
    }

    function showFind(replace) {
      const wasOpen = !find.hidden;
      find.hidden = false;
      find.classList.toggle('replace', replace);
      toggleRepl.replaceChildren(Shell.icon(replace ? 'fluent/chevron-up' : 'fluent/chevron-down', { size: 16 }));
      const sel = text.value.slice(text.selectionStart, text.selectionEnd);
      if (sel && !sel.includes('\n')) findIn.value = sel;
      if (!wasOpen) Shell.animate(find, [{ opacity: 0, transform: 'translateY(-8px)' }, { opacity: 1, transform: 'none' }], { duration: 167 });
      findIn.focus();
      findIn.select();
    }
    const hideFind = () => { find.hidden = true; text.focus(); };
    /* Case-insensitive and wrapping, as Notepad's defaults are. */
    function findNext(dir) {
      const q = findIn.value;
      if (!q) { showFind(find.classList.contains('replace')); return false; }
      const hay = text.value.toLowerCase(), needle = q.toLowerCase();
      let i = dir > 0 ? hay.indexOf(needle, text.selectionEnd) : text.selectionStart > 0 ? hay.lastIndexOf(needle, text.selectionStart - 1) : -1;
      if (i < 0) i = dir > 0 ? hay.indexOf(needle) : hay.lastIndexOf(needle);
      findIn.classList.toggle('np-nomatch', i < 0);
      if (i < 0) return false;
      const back = find.contains(document.activeElement) ? document.activeElement : null;
      text.focus({ preventScroll: true });
      text.setSelectionRange(i, i + q.length); // scrolls the match into view while focused
      back?.focus({ preventScroll: true });
      updateStatus();
      return true;
    }
    function replaceOne() {
      const q = findIn.value;
      if (!q) return;
      if (text.value.slice(text.selectionStart, text.selectionEnd).toLowerCase() === q.toLowerCase()) exec('insertText', replIn.value);
      findNext(1);
    }
    function replaceAll() {
      const q = findIn.value;
      if (!q) return;
      const re = new RegExp(q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'gi');
      const next = text.value.replace(re, () => replIn.value);
      if (next === text.value) return;
      text.focus();
      text.select();
      exec('insertText', next);
      findIn.focus();
    }

    const menus = {
      File: () => [
        { label: 'New tab', shortcut: 'Ctrl+N', action: () => addTab(null) },
        { label: 'New window', shortcut: 'Ctrl+Shift+N', action: () => createNotepad(null) },
        { label: 'Open', shortcut: 'Ctrl+O', action: openFile },
        { label: 'Save', shortcut: 'Ctrl+S', action: () => save(false) },
        { label: 'Save as', shortcut: 'Ctrl+Shift+S', action: () => save(true) },
        { label: 'Save all', shortcut: 'Ctrl+Alt+S', action: saveAll },
        { separator: true },
        { label: 'Close tab', shortcut: 'Ctrl+W', action: () => np.cur && requestCloseTab(np.cur.id) },
        { label: 'Close window', shortcut: 'Ctrl+Shift+W', action: () => np.win?.close() },
        { label: 'Exit', action: () => [...notepads].forEach((n) => n.win.close()) },
      ],
      Edit: () => {
        const none = text.selectionStart === text.selectionEnd;
        return [
          { label: 'Undo', shortcut: 'Ctrl+Z', action: () => exec('undo') },
          { separator: true },
          { label: 'Cut', shortcut: 'Ctrl+X', disabled: none, action: () => exec('cut') },
          { label: 'Copy', shortcut: 'Ctrl+C', disabled: none, action: () => exec('copy') },
          { label: 'Paste', shortcut: 'Ctrl+V', action: paste },
          { label: 'Delete', shortcut: 'Del', disabled: none, action: () => exec('delete') },
          { separator: true },
          { label: 'Find', shortcut: 'Ctrl+F', action: () => showFind(false) },
          { label: 'Find next', shortcut: 'F3', action: () => findNext(1) },
          { label: 'Find previous', shortcut: 'Shift+F3', action: () => findNext(-1) },
          { label: 'Replace', shortcut: 'Ctrl+H', action: () => showFind(true) },
          { separator: true },
          { label: 'Select all', shortcut: 'Ctrl+A', action: selectAll },
          { label: 'Time/Date', shortcut: 'F5', action: timeDate },
        ];
      },
      View: () => [
        { label: 'Zoom', submenu: [
          { label: 'Zoom in', shortcut: 'Ctrl+Plus', action: () => zoom(np.zoom + 10) },
          { label: 'Zoom out', shortcut: 'Ctrl+Minus', action: () => zoom(np.zoom - 10) },
          { label: 'Restore default zoom', shortcut: 'Ctrl+0', action: () => zoom(100) },
        ] },
        { label: 'Status bar', checked: np.status, action: () => { np.status = !np.status; applyView(); } },
        { label: 'Word wrap', checked: np.wrap, action: () => { np.wrap = !np.wrap; applyView(); } },
      ],
    };
    const settingsMenu = () => [
      { label: 'Font', submenu: NP_FONTS.map((f) => ({ label: f, radio: true, checked: np.font === f, action: () => { np.font = f; applyView(); } })) },
      { label: 'Size', submenu: NP_SIZES.map((s) => ({ label: String(s), radio: true, checked: np.size === s, action: () => { np.size = s; applyView(); } })) },
      { separator: true },
      { label: 'Word wrap', checked: np.wrap, action: () => { np.wrap = !np.wrap; applyView(); } },
      { label: 'Status bar', checked: np.status, action: () => { np.status = !np.status; applyView(); } },
    ];

    /* ---- input ---- */
    const KEYS = {
      'Ctrl+N': () => addTab(null), 'Ctrl+Shift+N': () => createNotepad(null),
      'Ctrl+W': () => np.cur && requestCloseTab(np.cur.id), 'Ctrl+Shift+W': () => np.win?.close(),
      'Ctrl+S': () => save(false), 'Ctrl+Shift+S': () => save(true), 'Ctrl+O': openFile,
      'Ctrl+F': () => showFind(false), 'Ctrl+H': () => showFind(true),
      F3: () => findNext(1), 'Shift+F3': () => findNext(-1), F5: timeDate,
      'Ctrl+=': () => zoom(np.zoom + 10), 'Ctrl++': () => zoom(np.zoom + 10), 'Ctrl+Shift++': () => zoom(np.zoom + 10),
      'Ctrl+-': () => zoom(np.zoom - 10), 'Ctrl+0': () => zoom(100),
      'Ctrl+Tab': () => cycle(1), 'Ctrl+Shift+Tab': () => cycle(-1), 'Ctrl+PageDown': () => cycle(1), 'Ctrl+PageUp': () => cycle(-1),
    };
    function onKey(e) {
      if (np.prompt) return; // the dialog owns the keyboard
      const fn = KEYS[Shell.keys.comboOf(e)];
      if (fn) { e.preventDefault(); e.stopPropagation(); fn(); }
      else if (e.key === 'Escape' && !find.hidden) { e.preventDefault(); hideFind(); }
    }
    root.addEventListener('keydown', onKey);
    strip.el.addEventListener('keydown', onKey);
    text.addEventListener('input', refresh);
    const onSel = () => { if (document.activeElement === text) updateStatus(); };
    document.addEventListener('selectionchange', onSel);
    editor.addEventListener('wheel', (e) => { if (e.ctrlKey) { e.preventDefault(); zoom(np.zoom + (e.deltaY < 0 ? 10 : -10)); } }, { passive: false });
    text.addEventListener('contextmenu', (e) => {
      if (!Shell.menu?.show) return;
      e.preventDefault();
      const none = text.selectionStart === text.selectionEnd;
      Shell.menu.show({ x: e.clientX, y: e.clientY, items: [
        { label: 'Cut', icon: 'fluent/cut', shortcut: 'Ctrl+X', disabled: none, action: () => exec('cut') },
        { label: 'Copy', icon: 'fluent/copy', shortcut: 'Ctrl+C', disabled: none, action: () => exec('copy') },
        { label: 'Paste', icon: 'fluent/clipboard-paste', shortcut: 'Ctrl+V', action: paste },
        { label: 'Delete', icon: 'fluent/delete', shortcut: 'Del', disabled: none, action: () => exec('delete') },
        { separator: true },
        { label: 'Select all', icon: 'fluent/select-all', shortcut: 'Ctrl+A', action: selectAll },
      ] });
    });
    findIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); findNext(e.shiftKey ? -1 : 1); } });
    findIn.addEventListener('input', () => findIn.classList.remove('np-nomatch'));
    replIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); replaceOne(); } });
    toggleRepl.addEventListener('click', () => showFind(!find.classList.contains('replace')));
    prevBtn.addEventListener('click', () => findNext(-1));
    nextBtn.addEventListener('click', () => findNext(1));
    closeFind.addEventListener('click', hideFind);
    replBtn.addEventListener('click', replaceOne);
    replAllBtn.addEventListener('click', replaceAll);

    const offs = [
      Shell.on('fs', ({ type, item }) => {
        if (type !== 'rename' || !item) return;
        for (const t of np.tabs) if (t.fileId === item.id) t.title = item.name;
        refresh();
      }),
      onFocus(() => np.win, () => {
        const i = notepads.indexOf(np);
        if (i >= 0) notepads.push(...notepads.splice(i, 1));
        if (np.prompt) { if (!np.prompt.contains(document.activeElement)) np.prompt.querySelector('button')?.focus({ preventScroll: true }); return; }
        if (!root.contains(document.activeElement) && !strip.el.contains(document.activeElement)) text.focus({ preventScroll: true });
      }),
    ];

    np.win = openWindow({
      appId: 'notepad', title: 'Untitled - Notepad', icon: 'object/app-notepad',
      width: 960, height: 640, minWidth: 320, minHeight: 240,
      titlebar: 'tall', titlebarContent: strip.el, background: 'mica', content: root,
      onClose() {
        if (!closing) {
          sync();
          if (np.prompt) return false;
          if (np.tabs.some(dirty)) { requestCloseWindow(); return false; }
        }
        document.removeEventListener('selectionchange', onSel);
        offs.forEach((off) => off());
        const i = notepads.indexOf(np);
        if (i >= 0) notepads.splice(i, 1);
      },
    });
    if (!np.win) { offs.forEach((off) => off()); document.removeEventListener('selectionchange', onSel); return null; }
    notepads.push(np);
    addTab(file);
    applyView();
    return np;
  }

  /* ======================= Calculator ======================= */
  /* Standard mode computes as Windows does: immediate execution (2 + 3 × 4 = 20), a repeated =
     repeats the last operation, % takes a percentage of the left operand for + and −, 16
     significant digits, and the error states Windows names. Pure state; the window renders it. */
  const OPS = { '+': (a, b) => a + b, '−': (a, b) => a - b, '×': (a, b) => a * b, '÷': (a, b) => a / b };
  const plain = (n) => { const r = Number(n.toPrecision(16)); return Object.is(r, -0) ? '0' : String(r); };
  function grouped(t) {
    if (t.includes('e')) return t.replace(/^(-?\d+)e/, '$1.e'); // Windows writes 1.e+21
    const [i, f] = t.split('.');
    const neg = i.startsWith('-');
    return (neg ? '-' : '') + (neg ? i.slice(1) : i).replace(/\B(?=(\d{3})+(?!\d))/g, ',') + (f === undefined ? '' : `.${f}`);
  }
  const fmt = (n) => grouped(plain(n));

  function calcEngine() {
    /* entry: the number shown, unformatted. typing: digits append to it. operand: it counts as the
       right operand of op. unary: the expression text of functions applied to it. done: an = ran. */
    const s = { entry: '0', acc: 0, op: null, typing: false, operand: false, unary: '', expr: '', done: false, last: null, error: '', history: [], memory: [] };
    const num = () => parseFloat(s.entry);
    const lead = () => (s.op ? `${fmt(s.acc)} ${s.op} ` : '');
    const reset = () => Object.assign(s, { entry: '0', acc: 0, op: null, typing: false, operand: false, unary: '', expr: '', done: false, error: '' });
    function fail(msg, expr) {
      Object.assign(s, { error: msg, entry: '0', acc: 0, op: null, typing: false, operand: false, unary: '', done: true, expr });
      return null;
    }
    function apply(op, a, b, expr) {
      if (op === '÷' && b === 0) return fail(a === 0 ? 'Result is undefined' : 'Cannot divide by zero', expr);
      const r = OPS[op](a, b);
      return isFinite(r) ? r : fail('Overflow', expr);
    }
    function digit(d) {
      if (s.error || s.done) { reset(); }
      if (!s.typing) { Object.assign(s, { entry: d === '.' ? '0.' : d, typing: true, operand: true, unary: '' }); return; }
      if (d === '.') { if (!s.entry.includes('.')) s.entry += '.'; return; }
      if (s.entry.replace(/[-.]/g, '').length >= 16) return;
      s.entry = s.entry === '0' ? d : s.entry === '-0' ? `-${d}` : s.entry + d;
    }
    function binary(op) {
      if (s.error) return;
      if (s.op && s.operand) {
        const r = apply(s.op, s.acc, num(), `${lead()}${s.unary || fmt(num())} ${op}`);
        if (r === null) return;
        s.acc = r;
        s.entry = plain(r);
      } else if (!s.op) s.acc = num();
      Object.assign(s, { op, typing: false, operand: false, unary: '', done: false, expr: `${fmt(s.acc)} ${op}` });
    }
    function equals() {
      if (s.error) { reset(); return; }
      let a, b, op, aText, bText;
      if (s.op) { a = s.acc; b = num(); op = s.op; aText = fmt(a); bText = s.unary || fmt(b); }
      else if (s.last) { a = num(); ({ op, b } = s.last); aText = s.unary || fmt(a); bText = fmt(b); }
      else { Object.assign(s, { expr: `${s.unary || fmt(num())} =`, typing: false, operand: false, unary: '', done: true }); return; }
      const expr = `${aText} ${op} ${bText} =`;
      const r = apply(op, a, b, expr);
      if (r === null) return;
      Object.assign(s, { entry: plain(r), expr, last: { op, b }, op: null, acc: r, typing: false, operand: false, unary: '', done: true });
      s.history.unshift({ expr, value: s.entry });
    }
    function unary(kind) {
      if (s.error) return;
      const x = num(), xt = s.unary || fmt(x);
      if (kind === 'neg' && s.typing) { // negating the number being typed edits it in place
        if (num() !== 0 || s.entry.includes('.')) s.entry = s.entry.startsWith('-') ? s.entry.slice(1) : `-${s.entry}`;
        return;
      }
      if (s.done) { s.op = null; s.done = false; } // a function on a result starts a new expression
      if (kind === 'pct') {
        const r = s.op === '+' || s.op === '−' ? (s.acc * x) / 100 : s.op ? x / 100 : 0;
        s.entry = plain(r);
        s.unary = fmt(r);
      } else {
        const t = { inv: `1/(${xt})`, sqr: `sqr(${xt})`, sqrt: `√(${xt})`, neg: `negate(${xt})` }[kind];
        if (kind === 'inv' && x === 0) return fail('Cannot divide by zero', lead() + t);
        if (kind === 'sqrt' && x < 0) return fail('Invalid input', lead() + t);
        const r = { inv: 1 / x, sqr: x * x, sqrt: Math.sqrt(x), neg: -x }[kind];
        if (!isFinite(r)) return fail('Overflow', lead() + t);
        s.entry = plain(r);
        s.unary = t;
      }
      Object.assign(s, { expr: lead() + s.unary, typing: false, operand: true });
    }
    function memory(kind, i) {
      if (s.error) return;
      const m = s.memory, x = num();
      if (kind === 'mc') { if (i == null) m.length = 0; else m.splice(i, 1); return; }
      if (kind === 'ms') m.unshift(x);
      else if (kind === 'mr') { if (!m.length) return; Object.assign(s, { entry: plain(m[i ?? 0]), unary: '' }); }
      else if (!m.length) m.push(kind === 'm+' ? x : -x);
      else m[i ?? 0] = Number(plain(m[i ?? 0] + (kind === 'm+' ? x : -x)));
      Object.assign(s, { typing: false, operand: true }); // the next digit starts a new number
    }
    function press(k) {
      if (/^[0-9.]$/.test(k)) digit(k);
      else if (k in OPS) binary(k);
      else if (k === '=') equals();
      else if (['pct', 'inv', 'sqr', 'sqrt', 'neg'].includes(k)) unary(k);
      else if (['mc', 'mr', 'm+', 'm-', 'ms'].includes(k)) memory(k);
      else if (k === 'c') { reset(); s.last = null; }
      else if (k === 'ce') { if (s.error || s.done) reset(); Object.assign(s, { entry: '0', typing: false, operand: true, unary: '' }); }
      else if (k === 'back') {
        if (s.error) reset();
        else if (s.done) s.expr = '';
        else if (s.typing) { s.entry = s.entry.slice(0, -1); if (s.entry === '' || s.entry === '-') s.entry = '0'; }
      }
    }
    const recall = (item) => { reset(); Object.assign(s, { entry: item.value, expr: item.expr, done: true }); };
    return { s, press, memory, recall, display: () => s.error || grouped(s.entry) };
  }

  /* Self-check of the engine on debug captures (?open=calculator); a mismatch is a console error. */
  function calcSelfCheck() {
    const alias = { '*': '×', '/': '÷', '-': '−', '%': 'pct', q: 'sqr', r: 'inv', n: 'neg', b: 'back' };
    [['2+3*4=', '20'], ['5+=', '10'], ['2+3==', '8'], ['1/3*3=', '1'], ['.1+.2=', '0.3'], ['50+10%', '5'], ['9q', '81'],
      ['4r', '0.25'], ['5/0=', 'Cannot divide by zero'], ['0/0=', 'Result is undefined'], ['12345678*1000=', '12,345,678,000'],
      ['7n+2=', '-5'], ['123b', '12'], ['2+3=4=', '7']].forEach(([keys, want]) => {
      const e = calcEngine();
      for (const ch of keys) e.press(alias[ch] || ch);
      if (e.display() !== want) console.error(`[small-apps] calculator self-check: ${keys} gave ${e.display()}, want ${want}`);
    });
  }

  const BACKSPACE = '<svg viewBox="0 0 20 20" width="20" height="20" aria-hidden="true"><path d="M7.6 4.5h8.4a1.5 1.5 0 0 1 1.5 1.5v8a1.5 1.5 0 0 1-1.5 1.5H7.6a1.5 1.5 0 0 1-1.12-.5L2.9 10.66a1 1 0 0 1 0-1.32l3.58-4.34a1.5 1.5 0 0 1 1.12-.5ZM10 7.5l5 5m0-5-5 5" fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"/></svg>';
  /* [key, label, accessible name (Windows Calculator's), style] row by row: 6 x 4 */
  const CALC_KEYS = [
    ['pct', '%', 'Percent', 'fn'], ['ce', 'CE', 'Clear entry', 'fn'], ['c', 'C', 'Clear', 'fn'], ['back', BACKSPACE, 'Backspace', 'fn'],
    ['inv', '<sup>1</sup>/<i>x</i>', 'Reciprocal', 'fn'], ['sqr', '<i>x</i><sup>2</sup>', 'Square', 'fn'], ['sqrt', '<sup>2</sup>√<i>x</i>', 'Square root', 'fn'], ['÷', '÷', 'Divide by', 'op'],
    ['7', '7', 'Seven', 'num'], ['8', '8', 'Eight', 'num'], ['9', '9', 'Nine', 'num'], ['×', '×', 'Multiply by', 'op'],
    ['4', '4', 'Four', 'num'], ['5', '5', 'Five', 'num'], ['6', '6', 'Six', 'num'], ['−', '−', 'Minus', 'op'],
    ['1', '1', 'One', 'num'], ['2', '2', 'Two', 'num'], ['3', '3', 'Three', 'num'], ['+', '+', 'Plus', 'op'],
    ['neg', '<sup>+</sup>/<sub>−</sub>', 'Positive negative', 'num'], ['0', '0', 'Zero', 'num'], ['.', '.', 'Decimal separator', 'num'], ['=', '=', 'Equals', 'eq'],
  ];
  const CALC_KEYMAP = { '+': '+', '-': '−', '*': '×', '/': '÷', '=': '=', Enter: '=', '%': 'pct', Escape: 'c', Delete: 'ce', Backspace: 'back', r: 'inv', R: 'inv', q: 'sqr', Q: 'sqr', '@': 'sqrt', F9: 'neg', '.': '.', ',': '.' };
  const CALC_CTRL = { m: 'ms', r: 'mr', p: 'm+', q: 'm-', l: 'mc' };
  const WIDE = 560; // the history and memory pane docks from this window width

  let calc = null; // Calculator is single-instance, as the Store app is

  function openCalculator() {
    if (calc) { calc.win.focus(); return calc.win; }
    const eng = calcEngine();
    const self = { win: null };
    let pane = 'history', flyout = false, navOpen = false, compact = false, full = null;
    const ids = { flyout: `calc-flyout-${Date.now()}`, nav: `calc-nav-${Date.now()}` };

    const navBtn = iconButton('calc-nav-btn', 'fluent/navigation', 'Open navigation');
    const histBtn = iconButton('calc-hist-btn', 'fluent/history', 'History (Ctrl+H)');
    const topBtn = iconButton('calc-top-btn', 'fluent/open', 'Keep on top (Alt+Up)');
    const head = h('div', 'calc-head', null, [navBtn, h('h1', 'calc-mode', { text: 'Standard' }), topBtn, h('span', 'calc-fill'), histBtn]);

    const exprEl = h('div', 'calc-expr');
    const valueEl = h('div', 'calc-value', { 'aria-live': 'polite' });
    const display = h('div', 'calc-display', null, [exprEl, valueEl]);

    const memBtns = {};
    const memRow = h('div', 'calc-memory', null, [
      ['mc', 'MC', 'Clear all memory (Ctrl+L)'], ['mr', 'MR', 'Memory recall (Ctrl+R)'], ['m+', 'M+', 'Memory add (Ctrl+P)'],
      ['m-', 'M-', 'Memory subtract (Ctrl+Q)'], ['ms', 'MS', 'Memory store (Ctrl+M)'],
    ].map(([k, label, tip]) => (memBtns[k] = h('button', 'btn subtle calc-mem', { type: 'button', text: label, 'data-tooltip': tip, 'aria-label': tip }))));
    const memFly = h('button', 'btn subtle calc-mem calc-mem-fly', { type: 'button', 'aria-label': 'Memory', 'data-tooltip': 'Memory' },
      [document.createTextNode('M'), Shell.icon('fluent/chevron-down', { size: 12 })]);
    memRow.append(memFly);

    const keyEls = {};
    const keypad = h('div', 'calc-keys', null, CALC_KEYS.map(([k, label, name, kind]) =>
      (keyEls[k] = h('button', `calc-key ${kind}`, { type: 'button', 'aria-label': name, html: `<span>${label}</span>` }))));

    const tabHist = h('button', 'calc-pivot-item selected', { type: 'button', role: 'tab', text: 'History' });
    const tabMem = h('button', 'calc-pivot-item', { type: 'button', role: 'tab', text: 'Memory' });
    const list = h('div', 'calc-list');
    const clearBtn = iconButton('calc-clear', 'fluent/delete', 'Clear all history');
    const side = h('aside', 'calc-side', null, [h('div', 'calc-pivot', { role: 'tablist' }, [tabHist, tabMem]), list, h('div', 'calc-side-foot', null, [clearBtn])]);

    const navClose = iconButton('calc-nav-btn', 'fluent/navigation', 'Close navigation');
    const navItem = (label, icon, selected) => {
      const b = h('button', `nav-item${selected ? ' selected' : ''}`, { type: 'button', disabled: !selected, 'aria-current': selected ? 'page' : null });
      b.append(Shell.icon(icon, { size: 16 }), h('span', null, { text: label }));
      return b;
    };
    const standardItem = navItem('Standard', 'fluent/calculator', true);
    // shortcut: only Standard mode exists in this POC; the other modes are listed disabled until built.
    const nav = h('nav', 'calc-nav acrylic', { 'aria-label': 'Navigation', hidden: true }, [
      navClose, h('div', 'calc-nav-header', { text: 'Calculator' }),
      standardItem, navItem('Scientific', 'fluent/calculator'), navItem('Date calculation', 'fluent/calendar'),
      h('div', 'calc-fill'), navItem('Settings', 'fluent/settings'),
    ]);

    const main = h('div', 'calc-main', null, [head, display, memRow, keypad]);
    const root = h('div', 'calc', { tabindex: '-1' }, [h('div', 'calc-layout', null, [main, side]), nav]);

    /* ---- render ---- */
    function fitValue() {
      valueEl.style.fontSize = '';
      const over = valueEl.scrollWidth / Math.max(1, valueEl.clientWidth);
      if (over > 1) valueEl.style.fontSize = `${Math.max(14, parseFloat(getComputedStyle(valueEl).fontSize) / over)}px`;
    }
    function render() {
      const s = eng.s, err = !!s.error;
      exprEl.textContent = s.expr;
      exprEl.scrollLeft = exprEl.scrollWidth; // long expressions show their end, as Windows scrolls them
      valueEl.textContent = eng.display();
      valueEl.classList.toggle('error', err);
      fitValue();
      for (const k of ['pct', 'inv', 'sqr', 'sqrt', '÷', '×', '−', '+', 'neg', '.']) keyEls[k].disabled = err;
      const empty = !s.memory.length;
      memBtns.mc.disabled = memBtns.mr.disabled = memFly.disabled = empty || err;
      memBtns['m+'].disabled = memBtns['m-'].disabled = memBtns.ms.disabled = err;
      renderList();
      if (self.win?.isFocused() && !root.contains(document.activeElement)) root.focus({ preventScroll: true });
    }
    function renderList() {
      const s = eng.s, hist = pane === 'history', items = hist ? s.history : s.memory;
      const tip = hist ? 'Clear all history' : 'Clear all memory';
      clearBtn.dataset.tooltip = tip;
      clearBtn.setAttribute('aria-label', tip);
      clearBtn.hidden = !items.length;
      tabHist.classList.toggle('selected', hist);
      tabMem.classList.toggle('selected', !hist);
      tabHist.setAttribute('aria-selected', String(hist));
      tabMem.setAttribute('aria-selected', String(!hist));
      if (!items.length) {
        list.replaceChildren(h('p', 'calc-empty', { text: hist ? "There's no history yet" : "There's nothing saved in memory" }));
        return;
      }
      list.replaceChildren(...items.map((it, i) => {
        const row = h('div', 'calc-item', { role: 'button', tabindex: '0' });
        if (hist) {
          row.append(h('div', 'calc-item-expr', { text: it.expr }), h('div', 'calc-item-value', { text: grouped(it.value) }));
          row.addEventListener('click', () => { eng.recall(it); closeFlyout(); render(); });
        } else {
          const acts = h('div', 'calc-item-actions', null, [['mc', 'MC', 'Clear memory item'], ['m+', 'M+', 'Add to memory item'], ['m-', 'M-', 'Subtract from memory item']].map(([k, label, tipText]) => {
            const b = h('button', 'btn calc-item-btn', { type: 'button', text: label, 'aria-label': tipText, 'data-tooltip': tipText });
            b.addEventListener('click', (e) => { e.stopPropagation(); eng.memory(k, i); render(); });
            return b;
          }));
          row.append(h('div', 'calc-item-value', { text: fmt(it) }), acts);
          row.addEventListener('click', () => { eng.memory('mr', i); closeFlyout(); render(); });
        }
        row.addEventListener('keydown', (e) => { if (e.key === 'Enter') row.click(); });
        return row;
      }));
    }

    /* ---- panes: the docked side pane (wide) doubles as the History / Memory flyout (narrow) ---- */
    const isWide = () => root.clientWidth >= WIDE;
    function showPane(p) { pane = p; renderList(); }
    function openFlyout(p) {
      if (isWide()) { showPane(p); return; }
      if (flyout && pane === p) { closeFlyout(); return; }
      showPane(p);
      if (flyout) return;
      flyout = true;
      side.classList.add('flyout', 'acrylic');
      side.style.top = `${memRow.offsetTop}px`;
      Shell.animate(side, [{ opacity: 0, transform: 'translateY(48px)' }, { opacity: 1, transform: 'none' }], { duration: 250 });
      Shell.transient.open(ids.flyout, {
        group: 'calc',
        elements: [side, histBtn, memFly],
        close: () => { flyout = false; side.classList.remove('flyout', 'acrylic'); side.style.top = ''; },
      });
    }
    const closeFlyout = () => { if (flyout) Shell.transient.close(ids.flyout); };
    function toggleNav() {
      if (navOpen) { Shell.transient.close(ids.nav); return; }
      navOpen = true;
      nav.hidden = false;
      Shell.animate(nav, [{ transform: 'translateX(-100%)' }, { transform: 'none' }], { duration: 250 });
      standardItem.focus({ preventScroll: true });
      Shell.transient.open(ids.nav, {
        group: 'calc',
        elements: [nav, navBtn],
        close: () => {
          navOpen = false;
          Shell.animate(nav, [{ transform: 'none' }, { transform: 'translateX(-100%)' }], { duration: 167, easing: Shell.ease.out })
            .then(() => { if (!navOpen) nav.hidden = true; });
          root.focus({ preventScroll: true });
        },
      });
    }

    /* ---- input ---- */
    function flash(el) {
      if (!el) return;
      el.classList.add('pressed');
      setTimeout(() => el.classList.remove('pressed'), 100);
    }
    function press(k) {
      eng.press(k);
      render();
    }
    CALC_KEYS.forEach(([k]) => keyEls[k].addEventListener('click', () => press(k)));
    Object.entries(memBtns).forEach(([k, b]) => b.addEventListener('click', () => press(k)));
    memFly.addEventListener('click', () => openFlyout('memory'));
    histBtn.addEventListener('click', () => openFlyout('history'));
    tabHist.addEventListener('click', () => showPane('history'));
    tabMem.addEventListener('click', () => showPane('memory'));
    clearBtn.addEventListener('click', () => {
      if (pane === 'history') eng.s.history.length = 0; else eng.memory('mc');
      render();
    });
    topBtn.addEventListener('click', () => setCompact(!compact));
    navBtn.addEventListener('click', toggleNav);
    navClose.addEventListener('click', toggleNav);
    standardItem.addEventListener('click', toggleNav);
    root.addEventListener('keydown', (e) => {
      if (e.altKey && !e.ctrlKey && !e.shiftKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
        e.preventDefault();
        setCompact(e.key === 'ArrowUp');
        return;
      }
      if (e.altKey || e.metaKey) return;
      if (e.key === 'Escape' && (navOpen || flyout)) { e.preventDefault(); Shell.transient.closeAll('calc'); root.focus(); return; }
      if (navOpen || e.target.closest?.('.calc-item')) return;
      let k;
      if (e.ctrlKey) {
        const c = e.key.toLowerCase();
        if (c === 'h') { e.preventDefault(); if (!compact) openFlyout('history'); return; }
        if (c === 'd' && e.shiftKey) { e.preventDefault(); eng.s.history.length = 0; render(); return; }
        k = e.shiftKey ? null : CALC_CTRL[c];
      } else k = /^[0-9]$/.test(e.key) ? e.key : CALC_KEYMAP[e.key];
      if (!k) return;
      e.preventDefault();
      flash(keyEls[k] || memBtns[k]);
      press(k);
    });

    const ro = new ResizeObserver(() => { fitValue(); if (flyout && isWide()) closeFlyout(); });
    const offFocus = onFocus(() => self.win, () => { if (!root.contains(document.activeElement)) root.focus({ preventScroll: true }); });

    /* Keep on top: Windows swaps the window for a compact overlay (close button only, always
       above other windows, no navigation, history or memory row) and back. The wm has no resize
       or topmost call, so the same content moves between two windows and the compact one
       re-pins its z-order after every window event. */
    const offTop = Shell.on('window', () => { if (compact && self.win?.el) self.win.el.style.zIndex = '1000'; });
    function mount(opts) {
      const win = openWindow({
        appId: 'calculator', title: 'Calculator', icon: 'object/app-calculator', background: 'mica', content: root, ...opts,
        onClose() { if (self.win === win) { ro.disconnect(); offFocus(); offTop(); Shell.transient.closeAll('calc'); calc = null; } },
      });
      if (win) self.win = win;
      return win;
    }
    function setCompact(on) {
      const old = self.win;
      if (on === compact || !old?.el) return;
      Shell.transient.closeAll('calc');
      const el = old.el, from = { x: el.offsetLeft, y: el.offsetTop, w: el.offsetWidth, h: el.offsetHeight };
      if (on) full = { ...from, max: old.isMaximized() };
      root.classList.toggle('compact', on);
      const win = mount(on
        ? { width: 320, height: 394, minWidth: 240, minHeight: 320, x: from.x, y: from.y, maximizable: false, minimizable: false }
        : { width: full.w, height: full.h, minWidth: 320, minHeight: 460, x: full.x, y: full.y });
      if (!win) { root.classList.toggle('compact', compact); return; }
      compact = on;
      if (on) win.el.style.zIndex = '1000';
      else if (full.max && !win.isMaximized()) win.maximize();
      const label = on ? 'Back to full view (Alt+Down)' : 'Keep on top (Alt+Up)';
      topBtn.dataset.tooltip = label;
      topBtn.setAttribute('aria-label', label);
      topBtn.replaceChildren(Shell.icon(on ? 'fluent/maximize' : 'fluent/open', { size: 16 }));
      el.style.visibility = 'hidden'; // the old frame leaves at once; the new one grows out of it
      old.close();
      win.el.getAnimations().forEach((a) => a.finish());
      const to = { x: win.el.offsetLeft, y: win.el.offsetTop, w: win.el.offsetWidth, h: win.el.offsetHeight };
      const geo = (r) => ({ left: `${r.x}px`, top: `${r.y}px`, width: `${r.w}px`, height: `${r.h}px` });
      Shell.animate(win.el, [geo(from), geo(to)], { duration: 250, easing: Shell.ease.in });
      root.focus({ preventScroll: true });
    }

    if (!mount({ width: 336, height: 540, minWidth: 320, minHeight: 460 })) { offFocus(); offTop(); return null; }
    calc = self;
    ro.observe(root);
    render();
    root.focus({ preventScroll: true });
    return self.win;
  }

  /* ======================= Terminal ======================= */
  const HOME = 'C:\\Users\\Mihawk';
  const WIN_VERSION = 'Mihawk OS prototype terminal';
  const PROFILES = {
    powershell: {
      name: 'Windows PowerShell', shortcut: 'Ctrl+Shift+1',
      banner: 'PowerShell-style shell (Mihawk OS prototype). Type help for the commands.\n\n',
      prompt: (cwd) => `PS ${cwd}> `,
    },
    cmd: {
      name: 'Command Prompt', shortcut: 'Ctrl+Shift+2',
      banner: `${WIN_VERSION} (Command Prompt style). Type help for the commands.\n\n`,
      prompt: (cwd) => `${cwd}>`,
    },
  };
  const TERM_HELP = [
    'Commands in this terminal:', '',
    '  cd <path>        Change the current directory (cd .., cd \\, cd ~, D:)',
    '  cls              Clear the screen',
    '  dir              List the items in the current directory',
    '  echo <text>      Write text to the output',
    '  hang [seconds]   Stop responding, for the given seconds or until the window is ended',
    '  help             Show this list',
    '  notify <text>    Show a notification',
    '  start <app>      Start an app by id or name, a file or a folder (start calc, start .)',
    '  taskmgr          Open Task Manager (also notepad, calc, explorer, wt)',
    '  ver              Show the Windows version',
    '  exit             Close this tab', '', '',
  ].join('\n');
  /* Programs the shell finds on PATH, and the names `start` knows besides app ids and names. */
  const TERM_EXES = ['taskmgr', 'notepad', 'calc', 'explorer', 'wt'];
  const TERM_ALIASES = { calc: 'calculator', wt: 'terminal', control: 'settings', 'ms-settings:': 'settings' };
  function termApp(name) {
    const n = name.toLowerCase().replace(/\.exe$/, '');
    return Shell.apps.get(TERM_ALIASES[n] || n) || Shell.apps.list().find((a) => a.name.toLowerCase() === n) || null;
  }

  /* C:\Users\Mihawk holds the known folders; the rest of C: and D: are their drive items. */
  function termList(id) {
    if (id === 'users') return [{ id: 'home', name: 'Mihawk', kind: 'folder' }];
    if (id === 'home') return Shell.fs.children('root').filter((i) => i.known);
    const kids = Shell.fs.children(id);
    return id === 'c' ? kids.filter((i) => i.name !== 'Users').concat({ id: 'users', name: 'Users', kind: 'folder' }) : kids;
  }
  const isDir = (i) => i.kind === 'folder' || i.kind === 'drive' || i.kind === 'system';
  const termName = (i) => (i.kind === 'shortcut' ? `${i.name}.lnk` : i.name);
  const termSize = (i) => (i.kind === 'shortcut' ? 1416 : Math.round(i.size || 0)); // synthetic .lnk size
  /* Resolves a path against cwd to {id, path}, or null when it does not exist. */
  function termResolve(cwd, arg) {
    let p = arg.replace(/\//g, '\\');
    if (p === '~' || p.startsWith('~\\')) p = HOME + p.slice(1);
    const parts = /^[a-z]:/i.test(p) ? [p.slice(0, 2).toUpperCase(), ...p.slice(2).split('\\')]
      : p.startsWith('\\') ? [cwd.slice(0, 2), ...p.split('\\')] : [...cwd.split('\\'), ...p.split('\\')];
    const segs = [];
    for (const seg of parts) {
      if (!seg || seg === '.') continue;
      if (seg === '..') { if (segs.length > 1) segs.pop(); } else segs.push(seg);
    }
    let id = { 'C:': 'c', 'D:': 'd' }[segs[0]];
    if (!id) return null;
    const names = [segs[0]];
    for (const seg of segs.slice(1)) {
      const hit = termList(id).find((i) => isDir(i) && i.name.toLowerCase() === seg.toLowerCase());
      if (!hit) return null;
      id = hit.id;
      names.push(hit.name);
    }
    return { id, path: names.length === 1 ? `${names[0]}\\` : names.join('\\') };
  }
  function psError(cmdlet, line, message, category, mark = line.length) {
    return `${cmdlet} : ${message}\nAt line:1 char:1\n+ ${line}\n+ ${'~'.repeat(mark)}\n    + CategoryInfo          : ${category}\n`;
  }
  function termDir(sess, target) {
    const ps = sess.profile === 'powershell';
    const items = termList(target.id).slice().sort((a, b) => (ps ? isDir(b) - isDir(a) : 0) || termName(a).localeCompare(termName(b)));
    const when = (ms) => new Date(ms || Date.now());
    if (ps) {
      if (!items.length) return '';
      const rows = items.map((i) => {
        const d = when(i.modified);
        const stamp = `${d.toLocaleDateString('en-US').padStart(10)}${d.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' }).padStart(10)}`;
        const mode = isDir(i) ? (i.known ? 'd-r---' : 'd-----') : '-a----';
        return `${mode}${stamp.padStart(28)}${(isDir(i) ? '' : String(termSize(i))).padStart(15)} ${termName(i)}`;
      });
      return `\n    Directory: ${target.path}\n\n\nMode                 LastWriteTime         Length Name\n----                 -------------         ------ ----\n${rows.join('\n')}\n\n\n`;
    }
    const line = (d, dir, size, name) => `${d.toLocaleDateString('en-US', { month: '2-digit', day: '2-digit', year: 'numeric' })}  ${d.toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit' })}    ${dir ? '<DIR>'.padEnd(14) : grouped(String(size)).padStart(14)} ${name}`;
    const drive = target.path[0];
    const rows = target.path.length > 3 ? [line(new Date(), true, 0, '.'), line(new Date(), true, 0, '..')] : [];
    items.forEach((i) => rows.push(line(when(i.modified), isDir(i), termSize(i), termName(i))));
    const files = items.filter((i) => !isDir(i));
    const bytes = files.reduce((n, i) => n + termSize(i), 0);
    const free = Math.round(Shell.fs.get(drive === 'D' ? 'd' : 'c')?.free || 0);
    return ` Volume in drive ${drive} ${drive === 'D' ? 'is PS5 Data' : 'has no label.'}\n Volume Serial Number is 6A1F-03C2\n\n Directory of ${target.path}\n\n${rows.join('\n')}\n`
      + `${String(files.length).padStart(16)} File(s) ${grouped(String(bytes)).padStart(14)} bytes\n`
      + `${String(rows.length - files.length).padStart(16)} Dir(s) ${grouped(String(free)).padStart(15)} bytes free\n`;
  }
  /* PSReadLine's default colours: command yellow, parameter grey, string cyan, number white. */
  function psTokens(line) {
    const out = [];
    const re = /("[^"]*"?|'[^']*'?)|(\s+)|(-[A-Za-z]\w*)|([^\s"']+)/g;
    let m, first = true;
    while ((m = re.exec(line))) {
      if (m[2]) { out.push([m[0], '']); continue; }
      out.push([m[0], m[1] ? 'str' : first ? 'cmd' : m[3] ? 'param' : /^-?\d+(\.\d+)?$/.test(m[0]) ? 'num' : '']);
      first = false;
    }
    return out;
  }

  function openTerminal({ command } = {}) {
    const sessions = [];
    let cur = null, seq = 0, winTitle = '', hung = false, hangTimer = 0;
    const self = { win: null };

    const profilesBtn = iconButton('term-profiles', 'fluent/chevron-down', 'Open a new tab', 12);
    const strip = tabStrip('term-tabs', {
      addLabel: 'New tab (Ctrl+Shift+T)',
      onAdd: () => addSession('powershell'),
      onSelect: select,
      onClose: closeSession,
      extra: [profilesBtn],
    });
    profilesBtn.addEventListener('click', () => dropMenu(profilesBtn, Object.entries(PROFILES).map(([id, p]) => ({
      label: p.name, icon: 'object/app-terminal', shortcut: p.shortcut, action: () => addSession(id),
    }))));

    const input = h('input', 'term-input', { type: 'text', autocomplete: 'off', spellcheck: 'false', 'aria-label': 'Terminal input' });
    const root = h('div', 'term', { 'data-theme': 'dark' }, [input]);

    /* ---- sessions (one per tab) ---- */
    function addSession(profile) {
      const p = PROFILES[profile];
      const out = h('div', 'term-out');
      const live = h('span', 'term-live');
      out.append(live);
      const screen = h('div', 'term-screen', { role: 'log', 'aria-label': p.name }, [out]);
      const sess = { id: `s${++seq}`, profile, cwd: HOME, cwdId: 'home', out, live, screen, history: [], hIndex: 0, draft: '' };
      sessions.push(sess);
      root.append(screen);
      strip.add(sess.id, p.name, 'object/app-terminal');
      screen.addEventListener('pointerup', () => { if (window.getSelection().isCollapsed) input.focus({ preventScroll: true }); });
      write(sess, p.banner);
      select(sess.id);
      return sess;
    }
    function select(id) {
      const sess = sessions.find((x) => x.id === id);
      if (!sess) return;
      if (cur && cur !== sess) cur.draft = input.value;
      cur = sess;
      for (const x of sessions) x.screen.hidden = x !== sess;
      strip.select(id);
      input.value = sess.draft;
      const title = PROFILES[sess.profile].name;
      if (self.win && title !== winTitle) { winTitle = title; self.win.setTitle(title); }
      input.focus({ preventScroll: true });
      renderLive();
    }
    function closeSession(id) {
      const i = sessions.findIndex((x) => x.id === id);
      if (i < 0) return;
      const [sess] = sessions.splice(i, 1);
      sess.screen.remove();
      strip.remove(id);
      if (!sessions.length) { cur = null; self.win?.close(); return; }
      if (sess === cur) { cur = null; select(sessions[Math.min(i, sessions.length - 1)].id); }
    }

    /* ---- screen ---- */
    function write(sess, text, cls) {
      if (!text) return;
      sess.out.insertBefore(cls ? h('span', cls, { text }) : document.createTextNode(text), sess.live);
    }
    const tokensOf = (sess, line) => (sess.profile === 'powershell' ? psTokens(line) : [[line, '']]);
    const span = (text, cls) => (cls ? h('span', `term-${cls}`, { text }) : document.createTextNode(text));
    function renderLive() {
      const sess = cur;
      if (!sess) return;
      if (hung) { sess.live.replaceChildren(h('span', 'term-cursor')); return; } // no prompt until the command ends
      const v = input.value, caret = input.selectionStart ?? v.length;
      const cursor = h('span', 'term-cursor');
      const kids = [document.createTextNode(PROFILES[sess.profile].prompt(sess.cwd))];
      let pos = 0, placed = false;
      for (const [t, cls] of tokensOf(sess, v)) {
        if (!placed && caret < pos + t.length) {
          kids.push(span(t.slice(0, caret - pos), cls), cursor, span(t.slice(caret - pos), cls));
          placed = true;
        } else kids.push(span(t, cls));
        pos += t.length;
      }
      if (!placed) kids.push(cursor);
      sess.live.replaceChildren(...kids);
      sess.screen.scrollTop = sess.screen.scrollHeight;
    }
    function freeze(sess, line, suffix = '') {
      write(sess, PROFILES[sess.profile].prompt(sess.cwd));
      tokensOf(sess, line).forEach(([t, cls]) => sess.out.insertBefore(span(t, cls), sess.live));
      write(sess, `${suffix}\n`);
    }

    /* ---- the shell ---- */
    function run(sess, line) {
      const ps = sess.profile === 'powershell';
      const args = (line.match(/"[^"]*"|'[^']*'|\S+/g) || []).map((a) => a.replace(/^(["'])(.*)\1$/, '$2'));
      const err = (text) => write(sess, text, 'term-err');
      if (!args.length) return;
      if (/^[a-z]:$/i.test(args[0]) && args.length === 1) args.splice(0, 1, 'cd', args[0].toUpperCase()); // a bare drive switches to it
      const cmd = args[0].toLowerCase();
      const rest = args.slice(1).join(' ');
      if (['cd', 'chdir'].includes(cmd) || (ps && ['sl', 'set-location'].includes(cmd))) {
        if (!rest) { if (!ps) write(sess, `${sess.cwd}\n`); return; }
        const t = termResolve(sess.cwd, rest);
        if (t) Object.assign(sess, { cwd: t.path, cwdId: t.id });
        else if (ps) {
          const full = /^[a-z]:/i.test(rest) ? rest : `${sess.cwd.replace(/\\$/, '')}\\${rest}`;
          err(psError(args[0], line.trim(), `Cannot find path '${full}' because it does not exist.`,
            `ObjectNotFound: (${full}:String) [Set-Location], ItemNotFoundException\n    + FullyQualifiedErrorId : PathNotFound,Microsoft.PowerShell.Commands.SetLocationCommand\n \n`));
        } else err('The system cannot find the path specified.\n');
      } else if (cmd === 'dir' || (ps && ['ls', 'gci', 'get-childitem'].includes(cmd))) {
        const t = rest ? termResolve(sess.cwd, rest) : { id: sess.cwdId, path: sess.cwd };
        if (t) write(sess, termDir(sess, t));
        else if (ps) {
          err(psError(args[0], line.trim(), `Cannot find path '${rest}' because it does not exist.`,
            `ObjectNotFound: (${rest}:String) [Get-ChildItem], ItemNotFoundException\n    + FullyQualifiedErrorId : PathNotFound,Microsoft.PowerShell.Commands.GetChildItemCommand\n \n`));
        } else err('File Not Found\n');
      } else if (cmd === 'echo' || (ps && ['write-output', 'write-host'].includes(cmd))) {
        if (ps) write(sess, args.slice(1).map((a) => `${a}\n`).join('')); // Write-Output emits each argument on its own line
        else write(sess, `${line.replace(/^\s*echo\s?/i, '') || 'ECHO is on.'}\n`);
      } else if (cmd === 'cls' || (ps && cmd === 'clear')) {
        sess.out.replaceChildren(sess.live);
      } else if (cmd === 'ver') {
        write(sess, `\n${WIN_VERSION}\n`);
      } else if (cmd === 'help' || (ps && ['get-help', '?'].includes(cmd))) {
        write(sess, TERM_HELP);
      } else if (cmd === 'start' || (ps && ['start-process', 'saps'].includes(cmd))) {
        if (!rest) Shell.apps.launch('terminal');
        else if (!start(sess, rest)) {
          if (ps) {
            err(psError(args[0], line.trim(), 'This command cannot be run due to the error: The system cannot find the file specified.',
              'InvalidOperation: (:) [Start-Process], InvalidOperationException\n    + FullyQualifiedErrorId : InvalidOperationException,Microsoft.PowerShell.Commands.StartProcessCommand\n \n'));
          } else if (Shell.dialogs?.messageBox) {
            Shell.dialogs.messageBox({ title: rest, text: `Windows cannot find '${rest}'. Make sure you typed the name correctly, and then try again.`, icon: 'error', owner: self.win });
          } else err(`The system cannot find the file ${rest}.\n`);
        }
      } else if (TERM_EXES.includes(cmd.replace(/\.exe$/, '')) && termApp(cmd)) {
        Shell.apps.launch(termApp(cmd).id);
      } else if (cmd === 'hang') {
        const secs = args[1] === undefined ? 0 : Number(args[1]);
        if (secs >= 0 && secs < Infinity) hang(secs); else err('Usage: hang [seconds]\n');
      } else if (cmd === 'notify') {
        if (!rest) err('Usage: notify <text>\n');
        else if (Shell.notify) Shell.notify({ app: 'Terminal', icon: 'object/app-terminal', title: rest });
        else err('notify: notifications are not available\n');
      } else if (cmd === 'exit') {
        closeSession(sess.id);
      } else if (ps) {
        err(psError(args[0], line.trim(), `The term '${args[0]}' is not recognized as the name of a cmdlet, function, script file, or operable program. Check the spelling of the name, or if a path was included, verify that the path is correct and try again.`,
          `ObjectNotFound: (${args[0]}:String) [], CommandNotFoundException\n    + FullyQualifiedErrorId : CommandNotFoundException\n \n`, args[0].length));
      } else err(`'${args[0]}' is not recognized as an internal or external command,\noperable program or batch file.\n`);
    }
    /* start: a folder opens in Explorer, a file in the current folder opens as Explorer would,
       anything else is an app by id, name or program name. -> false when nothing matched. */
    function start(sess, name) {
      const dir = termResolve(sess.cwd, name);
      if (dir) { Shell.apps.launch('explorer', { folder: dir.id === 'users' ? 'c' : dir.id }); return true; }
      const q = name.toLowerCase();
      const file = termList(sess.cwdId).find((i) => !isDir(i) && [termName(i), i.name].some((n) => n.toLowerCase() === q));
      if (file) { Shell.fs.open(file.id); return true; }
      const app = termApp(name);
      if (app) Shell.apps.launch(app.id);
      return !!app;
    }
    /* A hung process takes no input and paints nothing new; the wm turns its window into a
       "Not Responding" ghost. Ends after `secs` (0: only when the window is ended). */
    function hang(secs) {
      hung = true;
      root.classList.add('hung');
      root.inert = strip.el.inert = true;
      self.win?.setNotResponding?.(true);
      if (secs) hangTimer = setTimeout(endHang, secs * 1000);
    }
    function endHang() {
      if (!hung) return;
      hung = false;
      clearTimeout(hangTimer);
      root.classList.remove('hung');
      root.inert = strip.el.inert = false;
      self.win?.setNotResponding?.(false);
      renderLive();
      if (self.win?.isFocused()) input.focus({ preventScroll: true });
    }
    function enter(sess) {
      const line = input.value;
      input.value = '';
      sess.draft = '';
      freeze(sess, line);
      if (line.trim()) sess.history.push(line);
      sess.hIndex = sess.history.length;
      run(sess, line);
      if (!sessions.includes(sess)) return; // exit closed it
      if (sess.profile === 'cmd') write(sess, '\n');
      renderLive();
    }
    function cancelLine(sess) {
      freeze(sess, input.value, sess.profile === 'cmd' ? '^C' : '');
      input.value = '';
      renderLive();
    }

    /* ---- input ---- */
    const screenSelection = () => {
      const sel = window.getSelection();
      return !sel.isCollapsed && cur?.screen.contains(sel.anchorNode) ? sel.toString() : '';
    };
    input.addEventListener('keydown', (e) => {
      const sess = cur;
      if (!sess) return;
      const combo = Shell.keys.comboOf(e);
      if (e.key === 'Enter') { e.preventDefault(); enter(sess); return; }
      if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
        e.preventDefault();
        sess.hIndex = Math.max(0, Math.min(sess.history.length, sess.hIndex + (e.key === 'ArrowUp' ? -1 : 1)));
        input.value = sess.history[sess.hIndex] ?? '';
        input.setSelectionRange(input.value.length, input.value.length);
      } else if (e.key === 'Escape') { e.preventDefault(); input.value = ''; }
      else if (e.key === 'Tab' && !e.ctrlKey) e.preventDefault(); // no completion in this shell
      else if (combo === 'Ctrl+C') {
        const sel = screenSelection();
        e.preventDefault();
        if (sel) { navigator.clipboard?.writeText?.(sel).catch(() => {}); window.getSelection().removeAllRanges(); } // copy, as Terminal does with a selection
        else cancelLine(sess);
      } else if (combo === 'Ctrl+L' && sess.profile === 'powershell') { e.preventDefault(); sess.out.replaceChildren(sess.live); }
      requestAnimationFrame(renderLive);
    });
    input.addEventListener('input', renderLive);
    input.addEventListener('focus', () => root.classList.add('focused'));
    input.addEventListener('blur', () => root.classList.remove('focused'));
    const KEYS = {
      'Ctrl+Shift+T': () => addSession('powershell'), 'Ctrl+Shift+1': () => addSession('powershell'), 'Ctrl+Shift+2': () => addSession('cmd'),
      'Ctrl+Shift+W': () => cur && closeSession(cur.id),
      'Ctrl+Tab': () => cycle(1), 'Ctrl+Shift+Tab': () => cycle(-1),
    };
    const cycle = (d) => { const i = sessions.indexOf(cur); select(sessions[(i + d + sessions.length) % sessions.length].id); };
    function onKey(e) {
      let combo = Shell.keys.comboOf(e);
      if (e.ctrlKey && e.shiftKey && /^Digit[12]$/.test(e.code)) combo = `Ctrl+Shift+${e.code.slice(5)}`; // Shift turns 1 into !
      const fn = KEYS[combo];
      if (fn) { e.preventDefault(); e.stopPropagation(); fn(); }
    }
    root.addEventListener('keydown', onKey, true);
    strip.el.addEventListener('keydown', onKey);
    const offFocus = onFocus(() => self.win, () => { if (window.getSelection().isCollapsed) input.focus({ preventScroll: true }); });

    self.win = openWindow({
      appId: 'terminal', title: 'Windows PowerShell', icon: 'object/app-terminal',
      width: 980, height: 560, minWidth: 320, minHeight: 200,
      titlebar: 'tall', titlebarContent: strip.el, background: 'mica', content: root,
      onClose: () => { offFocus(); clearTimeout(hangTimer); },
    });
    if (!self.win) { offFocus(); return null; }
    self.win.el.dataset.theme = 'dark'; // Windows Terminal's default theme is dark whatever the system theme
    const sess = addSession('powershell');
    if (command) { input.value = command; enter(sess); }
    return self.win;
  }

  /* ======================= Game launcher ======================= */
  /* Games run on the PS5: the shell announces the launch and shows a short splash. */
  function openGame({ title = 'Game' } = {}) {
    Shell.notify?.({ app: 'Game launcher', icon: 'object/app-game', title: `Starting ${title}`, body: 'Your game opens in a moment.' });
    const content = h('div', 'game-splash', null, [
      Shell.icon('object/app-game', { size: 64 }),
      h('div', 'game-title', { text: title }),
      h('div', 'game-status', null, [h('div', 'progress-ring', { role: 'progressbar', 'aria-label': `Starting ${title}` }), h('span', null, { text: 'Starting…' })]),
    ]);
    let timer = 0;
    const win = openWindow({
      appId: 'game', title, icon: 'object/app-game', width: 420, height: 260,
      titlebar: 'none', resizable: false, maximizable: false, minimizable: false, background: 'mica', content,
      onClose: () => clearTimeout(timer),
    });
    if (win) timer = setTimeout(() => win.close(), 2500);
    return win;
  }

  /* ---------- registration ---------- */
  Shell.apps.register({ id: 'notepad', name: 'Notepad', icon: 'object/app-notepad', pinned: true, startPinned: true, description: 'Plain text editor', open: openNotepad });
  Shell.apps.register({ id: 'calculator', name: 'Calculator', icon: 'object/app-calculator', startPinned: true, description: 'Standard calculator with history and memory', open: openCalculator });
  Shell.apps.register({ id: 'terminal', name: 'Terminal', icon: 'object/app-terminal', pinned: true, startPinned: true, description: 'Windows Terminal with PowerShell and Command Prompt', open: () => openTerminal() });
  Shell.apps.register({ id: 'game', name: 'Game launcher', icon: 'object/app-game', description: 'Starts a game on the PS5', open: openGame });

  Shell.on('boot', () => setTimeout(() => {
    const want = (n) => Shell.debugOpen.includes(n);
    if (want('notepad')) {
      const np = createNotepad(Shell.fs.get('f-readme'));
      np?.addTab(null, { focus: false });
    }
    if (want('calculator')) { calcSelfCheck(); openCalculator(); }
    if (want('terminal')) openTerminal({ command: 'dir' });
    if (want('terminal-help')) openTerminal({ command: 'help' });
    if (want('terminal-hang')) openTerminal({ command: 'hang' });
    if (want('notepad-save-prompt')) {
      const np = createNotepad(null);
      const ta = np?.win.body.querySelector('.np-text');
      if (ta) { ta.value = 'Shopping list\n- rice\n- coffee'; ta.dispatchEvent(new Event('input')); np.win.close(); }
    }
  }));
})();
