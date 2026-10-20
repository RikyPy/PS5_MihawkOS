/* Search: the Windows 11 (23H2) Search flyout, opened by Win+S, the taskbar's Search button
   or typing in Start. Owns: Shell.search (open, close, toggle, isOpen), its flyout (search box,
   filter tabs, home with Top apps / Recent / Quick searches, grouped results under a Best
   match, and a preview card with the selected item's actions). Sources: Shell.apps, Shell.fs
   and Shell.settingsIndex (from the settings app). Recent launches persist under
   `search-recent`. Debug captures: ?open=search&q=no. Shares Start's footprint and helpers. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const kit = Shell.start._kit;
  const { h } = kit;
  const TABS = [['all', 'All'], ['app', 'Apps'], ['file', 'Documents'], ['setting', 'Settings'], ['folder', 'Folders']];
  const GROUPS = [['app', 'Apps'], ['setting', 'Settings'], ['file', 'Documents'], ['folder', 'Folders']];
  const WEIGHT = { app: 0.4, setting: 0.3, folder: 0.2, file: 0.1 };
  const QUICK = ['Screenshots', 'Display', 'Downloads', 'Notepad'];
  const TYPES = { txt: 'Text Document', png: 'PNG File', jpg: 'JPG File', pdf: 'PDF File', zip: 'Compressed (zipped) Folder', exe: 'Application' };

  let sr = null; // the open flyout: {host, panel, input, tabs, pill, body, tab, flat, rows, sel, cleanup}
  let recent = Shell.store.get('search-recent', []);

  /* ---------- matching ---------- */
  /* Windows ranks a name that starts with the query above one with a word that does, and
     that above a match inside a word. */
  function score(name, q) {
    const n = (name || '').toLowerCase();
    if (!q || !n.includes(q)) return 0;
    if (n.startsWith(q)) return 3;
    return n.split(/[\s\-_.()]+/).some((w) => w.startsWith(q)) ? 2 : 1;
  }
  const inRecycle = (it) => Shell.fs.path(it.id)[0]?.id === 'recycle';

  function entries(q) {
    const out = [];
    const push = (e, s) => { if (s) out.push({ ...e, score: s }); };
    for (const a of Shell.apps.list()) {
      if (a.id === 'game') continue;
      push({ kind: 'app', id: a.id, name: a.name, icon: a.icon || 'object/app-generic', app: a, open: () => Shell.apps.launch(a.id) }, score(a.name, q));
    }
    for (const it of Shell.fs.search(q)) {
      if (it.id === 'home' || inRecycle(it)) continue;
      if (it.kind === 'shortcut') {
        if (it.target !== 'game') continue; // a shortcut to an app finds the app itself
        push({ kind: 'app', id: it.id, name: it.name, icon: it.icon, item: it, open: () => Shell.fs.open(it.id) }, score(it.name, q));
      } else {
        push({ kind: it.kind === 'file' ? 'file' : 'folder', id: it.id, name: it.name, icon: it.icon, item: it, open: () => Shell.fs.open(it.id) }, score(it.name, q));
      }
    }
    for (const st of Shell.settingsIndex || []) {
      const kw = [].concat(st.keywords || []).join(' ');
      const s = score(st.title, q) || (score(kw, q) ? 1 : 0);
      push({ kind: 'setting', id: `set:${st.title}`, name: st.title, icon: st.icon || 'fluent/settings', setting: st,
        open: () => Shell.apps.launch('settings', { page: st.page }) }, s);
    }
    return out.sort((a, b) => (b.score + WEIGHT[b.kind]) - (a.score + WEIGHT[a.kind]) || a.name.localeCompare(b.name));
  }

  /* ---------- descriptions for the preview ---------- */
  function kindLabel(e) {
    if (e.kind === 'app') return 'App';
    if (e.kind === 'setting') return 'System settings';
    const it = e.item;
    if (it.kind === 'drive') return 'Local Disk';
    if (it.kind === 'system') return 'System folder';
    if (it.kind === 'folder') return 'File folder';
    return TYPES[it.ext] || `${(it.ext || '').toUpperCase()} File`.trim();
  }
  /* The Windows path of an item: known folders live under C:\Users\Mihawk. */
  function winPath(it) {
    let out = '';
    for (const p of Shell.fs.path(it.id).slice(0, -1)) {
      if (p.id === 'root') continue;
      if (p.kind === 'drive') out = `${p.id.toUpperCase()}:`;
      else if (p.known) out = `C:\\Users\\Mihawk\\${p.name}`;
      else out += `\\${p.name}`;
    }
    return out || 'This PC';
  }
  const fullDate = (t) => new Date(t).toLocaleString('en-US', { month: 'numeric', day: 'numeric', year: 'numeric', hour: 'numeric', minute: '2-digit' }).replace(',', '');

  /* ---------- open / close ---------- */
  function open({ query = '', from } = {}) {
    if (sr) { sr.input.value = query; render(); sr.input.focus({ preventScroll: true }); return; }
    const panel = h('section', 'sr-panel acrylic-base surface-flyout', { role: 'dialog', 'aria-label': 'Search' });
    const s = sr = { panel, host: kit.mountHost(panel), tab: 'all', flat: [], rows: [], sel: 0, cleanup: [], prev: document.activeElement };

    const box = h('label', 'st-search sr-box');
    s.input = h('input', 'sr-input', { type: 'text', role: 'searchbox', placeholder: 'Type here to search', 'aria-label': 'Search', spellcheck: 'false', autocomplete: 'off' });
    box.append(Shell.icon('fluent/search', { size: 16 }), s.input);

    s.tabs = h('div', 'sr-tabs', { role: 'tablist' });
    for (const [id, label] of TABS) {
      const t = h('button', 'sr-tab', { role: 'tab', 'data-tab': id, 'aria-selected': id === 'all' ? 'true' : 'false', text: label });
      t.addEventListener('click', () => setTab(id));
      s.tabs.append(t);
    }
    s.pill = h('span', 'sr-tab-pill', { 'aria-hidden': 'true' });
    s.tabs.append(s.pill);
    s.body = h('div', 'sr-body');
    panel.append(box, s.tabs, s.body);
    kit.place(panel);

    s.input.addEventListener('input', () => { s.sel = 0; render(); });
    s.input.addEventListener('keydown', onInputKey);
    const onKey = (e) => {
      if (e.defaultPrevented || Shell.layer('menus')?.childElementCount) return;
      if (e.key === 'Escape') { e.preventDefault(); close(); return; }
      if (kit.cycleTab(e, kit.tabStops(panel))) return;
      if (e.target === s.input) return;
      if (e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) { s.input.focus({ preventScroll: true }); return; } // the key lands in the box
      if (e.key === 'ArrowUp' && document.activeElement === s.body.querySelector('[data-nav]')) { e.preventDefault(); s.input.focus({ preventScroll: true }); return; }
      kit.spatial(e, [s.body]);
    };
    const onResize = () => { kit.place(panel); movePill(false); };
    window.addEventListener('keydown', onKey);
    window.addEventListener('resize', onResize);
    s.cleanup.push(() => window.removeEventListener('keydown', onKey), () => window.removeEventListener('resize', onResize));

    Shell.transient.open('search', { close: teardown, elements: [panel, kit.inMenus, kit.onTaskbarButton('search')], group: 'flyout' });
    s.input.value = query;
    render();
    movePill(false);
    s.input.focus({ preventScroll: true });
    s.input.setSelectionRange(query.length, query.length);
    if (from === 'start') {
      /* Start turns into Search in place: the box stays, the content below crossfades. */
      Shell.animate(s.tabs, [{ opacity: 0 }, { opacity: 1 }], { duration: kit.dur('fast'), easing: 'linear' });
      Shell.animate(s.body, [{ opacity: 0, transform: 'translateY(8px)' }, { opacity: 1, transform: 'none' }], { duration: kit.dur('normal'), easing: Shell.ease.in });
    } else {
      kit.rise(panel);
    }
  }

  function close() { Shell.transient.close('search'); }

  function teardown() {
    const s = sr;
    sr = null;
    if (!s) return;
    s.cleanup.forEach((fn) => fn());
    const hadFocus = s.panel.contains(document.activeElement);
    kit.sink(s.panel).then(() => s.host.remove());
    if (hadFocus && s.prev && s.prev !== document.body && s.prev.isConnected) s.prev.focus({ preventScroll: true });
  }

  function launch(e) {
    close();
    e.open();
  }

  function onInputKey(e) {
    const s = sr;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      if (s.flat.length) select(Math.max(0, Math.min(s.flat.length - 1, s.sel + (e.key === 'ArrowDown' ? 1 : -1))));
      else if (e.key === 'ArrowDown') s.body.querySelector('[data-nav]')?.focus({ preventScroll: true });
    } else if (e.key === 'Enter') {
      e.preventDefault();
      const target = s.flat[s.sel];
      if (target) launch(target);
    }
  }

  /* ---------- tabs ---------- */
  function setTab(id) {
    sr.tab = id;
    sr.sel = 0;
    sr.tabs.querySelectorAll('.sr-tab').forEach((t) => t.setAttribute('aria-selected', t.dataset.tab === id ? 'true' : 'false'));
    movePill(true);
    render();
    sr.input.focus({ preventScroll: true });
  }
  /* The 16 x 3 accent pill under the selected tab slides to the new one. */
  function movePill(animate) {
    const t = sr.tabs.querySelector(`[data-tab="${sr.tab}"]`);
    if (!t) return;
    sr.pill.classList.toggle('is-instant', !animate);
    sr.pill.style.transform = `translateX(${t.offsetLeft + (t.offsetWidth - 16) / 2}px)`;
  }

  /* ---------- body ---------- */
  function render() {
    const s = sr;
    const q = s.input.value.trim().toLowerCase();
    s.body.textContent = '';
    s.flat = [];
    s.rows = [];
    if (!q) { renderHome(); return; }
    const found = entries(q).filter((e) => s.tab === 'all' || e.kind === s.tab);
    if (!found.length) {
      const empty = h('div', 'sr-empty');
      empty.append(Shell.icon('fluent/search', { size: 32 }), h('p', '', { text: `No results found for "${s.input.value.trim()}"` }));
      s.body.append(empty);
      return;
    }
    const results = h('div', 'sr-results');
    const list = h('div', 'sr-list', { role: 'listbox', 'aria-label': 'Results' });
    s.preview = h('div', 'sr-preview');
    const best = found[0];
    list.append(groupTitle('Best match'), row(best, true));
    for (const [kind, title] of GROUPS) {
      const items = found.filter((e) => e !== best && e.kind === kind).slice(0, s.tab === 'all' ? 4 : 50);
      if (!items.length) continue;
      list.append(groupTitle(title));
      items.forEach((e) => list.append(row(e, false)));
    }
    results.append(list, s.preview);
    s.body.append(results);
    select(Math.min(s.sel, s.flat.length - 1));
  }

  const groupTitle = (text) => h('h3', 'sr-group', { text });

  function row(e, isBest) {
    const s = sr;
    const index = s.flat.length;
    const b = h('button', `sr-row st-plate${isBest ? ' is-best' : ''}`, { role: 'option', tabindex: '-1' });
    const text = h('span', 'sr-row-text');
    text.append(h('span', 'sr-row-name', { text: e.name }));
    if (isBest) text.append(h('span', 'sr-row-kind', { text: kindLabel(e) }));
    else if (e.item && e.kind !== 'app') text.append(h('span', 'sr-row-kind', { text: winPath(e.item) }));
    const chev = h('span', 'sr-chev', { 'aria-hidden': 'true' });
    chev.append(Shell.icon('fluent/chevron-right', { size: 12 }));
    b.append(Shell.icon(e.icon, { size: isBest ? 32 : 24, className: 'sr-row-icon' }), text, chev);
    /* The row opens the item; its chevron only shows it in the preview, as in Windows. */
    b.addEventListener('click', (ev) => { if (ev.target.closest('.sr-chev')) select(index); else launch(e); });
    b.addEventListener('contextmenu', (ev) => { select(index); kit.showMenu(ev, actions(e).map(({ label, icon, run }) => ({ label, icon, action: run }))); });
    s.flat.push(e);
    s.rows.push(b);
    return b;
  }

  function select(i) {
    const s = sr;
    if (i < 0 || !s.flat[i]) return;
    s.sel = i;
    s.rows.forEach((r, k) => { r.classList.toggle('is-selected', k === i); r.setAttribute('aria-selected', k === i ? 'true' : 'false'); });
    kit.reveal(s.rows[i]);
    renderPreview(s.flat[i]);
  }

  function actions(e) {
    const list = [{ label: 'Open', icon: 'fluent/open', run: () => launch(e) }];
    if (e.kind === 'setting') return list;
    const refHolder = e.app ? e.app.id : e.item.id;
    const pinned = Shell.start.isPinned(refHolder);
    if (e.item) list.push({ label: 'Open file location', icon: 'fluent/folder-open', run: () => { close(); kit.openLocation(e.item); } });
    if (e.kind !== 'file') {
      list.push({ label: pinned ? 'Unpin from Start' : 'Pin to Start', icon: pinned ? 'fluent/pin-off' : 'fluent/pin',
        run: () => { if (pinned) Shell.start.unpin(refHolder); else Shell.start.pin(refHolder); if (sr) renderPreview(e); } });
    }
    if (e.app) {
      const t = kit.taskbarPinItem(e.app);
      list.push({ label: t.label, icon: t.icon, run: () => { t.action(); if (sr) renderPreview(e); } });
    }
    if (e.kind === 'file') {
      list.push({ label: 'Copy full path', icon: 'fluent/copy', run: () => navigator.clipboard?.writeText(`${winPath(e.item)}\\${e.item.name}`).catch(() => {}) });
    }
    return list;
  }

  function renderPreview(e) {
    const card = sr.preview;
    card.textContent = '';
    const head = h('div', 'sr-pv-head');
    head.append(Shell.icon(e.icon, { size: e.icon.startsWith('fluent/') ? 48 : 64, className: 'sr-pv-icon' }),
      h('div', 'sr-pv-name', { text: e.name }), h('div', 'sr-pv-kind', { text: kindLabel(e) }));
    card.append(head);
    if (e.item && e.kind !== 'app') {
      const meta = h('dl', 'sr-pv-meta');
      meta.append(h('dt', '', { text: 'Location' }), h('dd', '', { text: winPath(e.item) }));
      if (e.item.modified) meta.append(h('dt', '', { text: 'Last modified' }), h('dd', '', { text: fullDate(e.item.modified) }));
      card.append(meta);
    }
    const acts = h('div', 'sr-actions');
    for (const a of actions(e)) {
      const b = h('button', 'sr-action st-plate', { 'data-nav': true });
      b.append(Shell.icon(a.icon, { size: 16 }), h('span', '', { text: a.label }));
      b.addEventListener('click', a.run);
      acts.append(b);
    }
    card.append(acts);
  }

  /* Home with Search highlights off: Top apps, then Recent and Quick searches. */
  function renderHome() {
    const home = h('div', 'sr-home');
    const top = (Shell.start.pins?.() || []).slice(0, 6);
    if (top.length) {
      const tiles = h('div', 'sr-tiles');
      for (const e of top) {
        const t = h('button', 'sr-tile', { 'data-nav': true, 'aria-label': e.name });
        t.append(Shell.icon(e.icon, { size: 32 }), h('span', 'sr-tile-name', { text: e.name }));
        t.addEventListener('click', () => launch(e));
        tiles.append(t);
      }
      home.append(groupTitle('Top apps'), tiles);
    }

    const cols = h('div', 'sr-cols');
    const recentCol = h('div', 'sr-col');
    const seen = new Set();
    const items = [];
    for (const ref of recent) {
      const e = kit.resolve(ref);
      if (e && !seen.has(e.ref)) { seen.add(e.ref); items.push(e); }
    }
    for (const it of kit.recentFiles()) {
      if (items.length >= 5) break;
      items.push({ ref: `fs:${it.id}`, name: it.name, icon: it.icon, item: it, open: () => Shell.fs.open(it.id) });
    }
    if (items.length) {
      recentCol.append(groupTitle('Recent'));
      items.slice(0, 5).forEach((e) => {
        const b = h('button', 'sr-row st-plate', { 'data-nav': true });
        const text = h('span', 'sr-row-text');
        text.append(h('span', 'sr-row-name', { text: e.name }));
        b.append(Shell.icon(e.icon, { size: 24, className: 'sr-row-icon' }), text);
        b.addEventListener('click', () => launch(e));
        recentCol.append(b);
      });
    }
    const quickCol = h('div', 'sr-col');
    quickCol.append(groupTitle('Quick searches'));
    for (const q of QUICK) {
      const b = h('button', 'sr-row st-plate', { 'data-nav': true });
      b.append(Shell.icon('fluent/search', { size: 16, className: 'sr-row-icon sr-quick-icon' }), h('span', 'sr-row-name', { text: q }));
      b.addEventListener('click', () => { sr.input.value = q; sr.sel = 0; render(); sr.input.focus({ preventScroll: true }); });
      quickCol.append(b);
    }
    cols.append(recentCol, quickCol);
    home.append(cols);
    sr.body.append(home);
  }

  /* ---------- public API ---------- */
  Shell.search = {
    open,
    close,
    toggle: (opts) => (Shell.transient.isOpen('search') ? close() : open(opts)),
    isOpen: () => Shell.transient.isOpen('search'),
  };

  Shell.keys.on('Win+S', () => Shell.search.toggle());

  /* Recent launches for the home view: the app, or the shortcut that launched it. */
  Shell.on('app-launch', ({ id, args }) => {
    const ref = args && args.from ? `fs:${args.from}` : id;
    if (ref === 'game') return;
    recent = [ref, ...recent.filter((r) => r !== ref)].slice(0, 8);
    Shell.store.set('search-recent', recent);
  });

  Shell.on('boot', () => {
    setTimeout(() => {
      if (Shell.debugOpen.includes('search')) open({ query: new URLSearchParams(location.search).get('q') || '' });
    }, 0);
  });
})();
