/* File Explorer (Windows 11 23H2).
   Owns: the 'explorer' app and Win+E; per window: tabs in the title bar, the address bar
   (Back/Forward/Up/Refresh, breadcrumbs with folder menus, path editing, search), the command
   bar, the navigation pane (Home, Gallery, Quick access pins, This PC tree, Recycle Bin), the
   content views (Home, Gallery, This PC, folders as details/list/icons/tiles, search results),
   the details pane and the status bar; selection, rubber band, keyboard, inline rename, item
   drag and drop (a Shell.drag source and target), Cut/Copy/Paste through Shell.clipboard,
   copies and moves through Shell.dialogs.fileOperation (conflicts, progress), Properties and
   Open with through Shell.dialogs, and the Recycle Bin's commands. Store keys it owns: explorer-pins (Quick access ids), explorer-views ({folder: {view, sort,
   dir}}), explorer-layout ({navWidth, nav, details, compact, ext}). Contract: CONTRACT.md. */
(() => {
  'use strict';

  const Shell = window.Shell;
  if (!Shell) return;

  /* ---------- small DOM helpers ---------- */
  function h(tag, cls, props) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (v == null || v === false) continue;
        if (k === 'text') e.textContent = v;
        else if (k === 'children') v.forEach((c) => c && e.append(c));
        else e.setAttribute(k, v === true ? '' : v);
      }
    }
    return e;
  }
  const ico = (name, size = 16, className = '') => Shell.icon(name, { size, className });
  const menu = (opts) => Shell.menu?.show?.(opts);
  const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;

  /* ---------- shared state (every Explorer window) ---------- */
  const store = Shell.store;
  const DEFAULT_PINS = ['desktop', 'downloads', 'documents', 'pictures', 'music', 'videos'];
  let pins = store.get('explorer-pins', DEFAULT_PINS);
  const folderViews = store.get('explorer-views', {});
  const layout = { navWidth: 0, nav: true, details: false, compact: false, ext: false, ...store.get('explorer-layout', {}) };
  const collapsedSections = new Set();
  /* The last `clipboard` event: Shell.clipboard.get() leaves out Recycle Bin items, so a Cut made
     in the Recycle Bin is read from here (Explorer pastes it; the desktop cannot). */
  let lastClip = { ids: [], cut: false };
  const clipNow = () => Shell.clipboard.get() ||
    (lastClip.cut && lastClip.ids.some((id) => get(id)?.parent === 'recycle') ? { ids: lastClip.ids.filter(get), cut: true } : null);
  const isCut = (id) => lastClip.cut && lastClip.ids.includes(id);
  const instances = new Set();
  const saveLayout = () => store.set('explorer-layout', layout);
  const refreshAll = (what) => instances.forEach((x) => x.refresh(what));

  /* ---------- file-system vocabulary ---------- */
  const get = (id) => (id ? Shell.fs.get(id) : null);
  const isContainer = (it) => !!it && (it.kind === 'folder' || it.kind === 'drive' || it.kind === 'system');
  const IMAGE_EXT = ['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp'];
  const isImage = (it) => !!it && it.kind === 'file' && IMAGE_EXT.includes(it.ext);
  function isInside(id, ancestor) {
    let it = get(id);
    for (let guard = 0; it && it.parent && guard < 64; guard++) {
      if (it.parent === ancestor) return true;
      it = get(it.parent);
    }
    return false;
  }
  const inRecycle = (id) => isInside(id, 'recycle');
  const canCreate = (loc) => { const it = get(loc); return !!it && (it.kind === 'folder' || it.kind === 'drive') && !inRecycle(loc); };
  const dropOk = (loc) => loc === 'recycle' || canCreate(loc);
  const movable = (it) => !!it && (it.kind === 'file' || it.kind === 'shortcut' || (it.kind === 'folder' && !it.known));
  const renamable = (it) => movable(it) && it.parent !== 'recycle';
  /* Locations that are views, not file-system folders: [name, icon]. */
  const PSEUDO = { gallery: ['Gallery', 'fluent/image-multiple'], network: ['Network', 'object/network-wired'] };
  const SPECIAL = ['home', 'root', 'gallery', 'network'];
  const nameOf = (loc) => PSEUDO[loc]?.[0] || get(loc)?.name || 'Home';
  const recycleIcon = () => (Shell.fs.children('recycle').length ? 'object/recycle-full' : 'object/recycle-empty');
  function iconOf(it) {
    if (!it) return 'object/folder';
    if (it.id === 'recycle') return recycleIcon();
    return it.icon || (it.kind === 'folder' ? 'object/folder' : 'object/document');
  }
  const locIcon = (loc) => PSEUDO[loc]?.[1] || iconOf(get(loc));
  const subfolders = (id) => Shell.fs.children(id).filter(isContainer)
    .sort((a, b) => (a.kind === 'drive') - (b.kind === 'drive') || collator.compare(a.name, b.name));
  const hasSubfolders = (id) => id === 'root' || Shell.fs.children(id).some(isContainer);
  function walk(id, out = []) {
    Shell.fs.children(id).forEach((c) => { out.push(c); if (isContainer(c)) walk(c.id, out); });
    return out;
  }

  /* Windows hides known extensions by default (View > Show > File name extensions). */
  function displayName(it) {
    if (!layout.ext && it.kind === 'file' && it.ext && it.name.length > it.ext.length + 1 &&
        it.name.toLowerCase().endsWith(`.${it.ext}`)) return it.name.slice(0, -(it.ext.length + 1));
    return it.name;
  }
  const TYPES = { txt: 'Text Document', png: 'PNG File', jpg: 'JPG File', pdf: 'PDF File', xlsx: 'XLSX File',
    zip: 'Compressed (zipped) Folder', gz: 'GZ File', exe: 'Application' };
  function typeOf(it) {
    if (it.kind === 'folder') return 'File folder';
    if (it.kind === 'drive') return 'Local Disk';
    if (it.kind === 'system') return 'System Folder';
    if (it.kind === 'shortcut') return 'Shortcut';
    return TYPES[it.ext] || (it.ext ? `${it.ext.toUpperCase()} File` : 'File');
  }
  const dateFmt = new Intl.DateTimeFormat('en-US', { month: 'numeric', day: 'numeric', year: 'numeric', hour: 'numeric', minute: '2-digit' });
  const fmtDate = (ms) => (ms ? dateFmt.format(ms).replace(',', '') : '');
  /* Details view sizes are whole kilobytes with separators, as Explorer prints them. */
  const fmtKB = (b) => `${Math.ceil((b || 0) / 1024).toLocaleString('en-US')} KB`;
  const collator = new Intl.Collator('en', { numeric: true, sensitivity: 'base' });

  /* Paths as Windows prints them; known folders live under the user's profile. */
  function osPath(id) {
    const special = { home: 'Home', gallery: 'Gallery', root: 'This PC', network: 'Network', recycle: 'Recycle Bin' };
    if (!id) return '';
    if (special[id]) return special[id];
    const parts = [];
    for (const it of Shell.fs.path(id)) {
      if (it.id === 'root') continue;
      if (it.kind === 'drive') parts.push(it.name.match(/\(([A-Z]:)\)/)?.[1] || it.name);
      else if (it.known) parts.push('C:', 'Users', 'Mihawk', it.name);
      else parts.push(it.name);
    }
    const p = parts.join('\\');
    return /^[A-Z]:$/.test(p) ? `${p}\\` : p;
  }
  function resolvePath(text) {
    const want = text.trim().replace(/^"|"$/g, '').replace(/[\\/]+$/, '').toLowerCase();
    if (!want) return null;
    const special = { home: 'home', gallery: 'gallery', 'this pc': 'root', network: 'network', 'recycle bin': 'recycle', 'shell:recyclebinfolder': 'recycle' };
    if (special[want]) return special[want];
    const all = walk('root').filter(isContainer);
    const hit = all.find((c) => osPath(c.id).toLowerCase().replace(/\\$/, '') === want.replace(/\\$/, ''));
    return (hit || all.find((c) => c.name.toLowerCase() === want) || {}).id || null;
  }
  const locationText = (parent) => { const p = get(parent); return p ? (p.known || p.kind === 'drive' ? p.name : osPath(p.id)) : ''; };

  /* Fake photo thumbnails: the gradients behind the shell's own wallpapers. */
  function thumbCss(it) {
    const pool = Shell.wallpapers.filter((w) => !w.solid);
    let hsh = 0;
    for (const ch of it.id) hsh = (hsh * 31 + ch.charCodeAt(0)) | 0;
    return pool[Math.abs(hsh) % pool.length].css;
  }

  /* An item's picture: its object icon, a thumbnail for photos from 48 px, the shortcut badge. */
  function itemIcon(it, size) {
    const w = h('span', 'ex-ico');
    w.style.width = w.style.height = `${size}px`;
    if (size >= 48 && isImage(it)) {
      const th = h('span', 'ex-thumb');
      th.style.background = thumbCss(it);
      w.append(th);
    } else w.append(ico(iconOf(it), size));
    if (it.kind === 'shortcut') w.append(ico('object/shortcut-arrow', Math.max(10, Math.round(size * 0.375)), 'ex-badge'));
    return w;
  }

  /* ---------- columns, views, sorting ---------- */
  const COLS = {
    name: { label: 'Name', width: 280, text: displayName, sort: (it) => it.name },
    modified: { label: 'Date modified', width: 170, text: (it) => fmtDate(it.modified), sort: (it) => it.modified || 0 },
    type: { label: 'Type', width: 150, text: typeOf, sort: typeOf },
    size: { label: 'Size', width: 100, right: true, text: (it) => (it.kind === 'file' ? fmtKB(it.size) : ''), sort: (it) => (it.kind === 'file' ? it.size || 0 : -1) },
    from: { label: 'Original location', width: 220, text: (it) => osPath(it.deletedFrom), sort: (it) => osPath(it.deletedFrom) },
    deleted: { label: 'Date deleted', width: 170, text: (it) => fmtDate(it.modified), sort: (it) => it.modified || 0 },
    folder: { label: 'Folder', width: 240, text: (it) => osPath(it.parent), sort: (it) => osPath(it.parent) },
    location: { label: 'Location', width: 220, text: (it) => locationText(it.parent), sort: (it) => locationText(it.parent) },
  };
  const COLSETS = { folder: ['name', 'modified', 'type', 'size'], recycle: ['name', 'from', 'deleted', 'size', 'type'],
    search: ['name', 'modified', 'type', 'size', 'folder'], recent: ['name', 'modified', 'location'] };
  const VIEWS = [ /* [id, label, icon, icon size, shortcut] */
    ['large', 'Large icons', 'fluent/grid', 96, 'Ctrl+Shift+2'], ['medium', 'Medium icons', 'fluent/apps', 48, 'Ctrl+Shift+3'],
    ['small', 'Small icons', 'fluent/apps-list', 16, 'Ctrl+Shift+4'], ['list', 'List', 'fluent/navigation', 16, 'Ctrl+Shift+5'],
    ['details', 'Details', 'fluent/text-bullet-list', 16, 'Ctrl+Shift+6'], ['tiles', 'Tiles', 'fluent/square-multiple', 48, 'Ctrl+Shift+7'],
  ];
  const viewInfo = (v) => VIEWS.find((x) => x[0] === v) || VIEWS[4];
  const PICTURE_FOLDERS = ['pictures', 'f-shots', 'f-wall'];
  const viewOf = (loc) => folderViews[loc]?.view || (PICTURE_FOLDERS.includes(loc) ? 'large' : 'details');
  function sortOf(loc, cols) {
    const v = folderViews[loc] || {};
    return { key: cols.includes(v.sort) ? v.sort : 'name', dir: v.dir === -1 ? -1 : 1 };
  }
  function setFolderView(loc, patch) {
    folderViews[loc] = { ...folderViews[loc], ...patch };
    store.set('explorer-views', folderViews);
    refreshAll('view');
  }
  function sortItems(items, key, dir) {
    const group = (it) => (isContainer(it) ? 0 : 1);
    return [...items].sort((a, b) => {
      let c = group(a) - group(b);
      if (!c) {
        const va = COLS[key].sort(a), vb = COLS[key].sort(b);
        c = typeof va === 'string' ? collator.compare(va, vb) : va - vb;
        if (!c && key !== 'name') c = collator.compare(a.name, b.name);
      }
      return c * dir;
    });
  }
  const FILTERS = {
    folders: { label: 'Folders', icon: 'fluent/folder', test: isContainer },
    documents: { label: 'Documents', icon: 'fluent/document', test: (it) => ['txt', 'pdf', 'xlsx', 'docx', 'md', 'rtf'].includes(it.ext) },
    pictures: { label: 'Pictures', icon: 'fluent/image', test: isImage },
    music: { label: 'Music', icon: 'fluent/music-note', test: (it) => ['mp3', 'flac', 'wav', 'm4a'].includes(it.ext) },
    videos: { label: 'Videos', icon: 'fluent/video', test: (it) => ['mp4', 'mkv', 'avi', 'mov'].includes(it.ext) },
    apps: { label: 'Applications', icon: 'fluent/apps', test: (it) => it.ext === 'exe' || it.kind === 'shortcut' },
  };

  /* ---------- file operations ---------- */
  function moveItem(id, parent) {
    const it = get(id);
    if (!movable(it) || it.parent === parent || id === parent || isInside(parent, id)) return;
    if (it.parent === 'recycle') Shell.fs.restore(id);
    if (Shell.fs.children(parent).some((c) => c.id !== id && c.name.toLowerCase() === it.name.toLowerCase())) {
      const base = it.ext && it.name.toLowerCase().endsWith(`.${it.ext}`) ? it.name.slice(0, -(it.ext.length + 1)) : it.name;
      Shell.fs.rename(id, Shell.fs.uniqueName(parent, base, base !== it.name ? it.ext : undefined));
    }
    Shell.fs.move(id, parent);
  }
  /* Copies and moves (Paste, drops) -> Promise of the ids that landed in `to`. Windows' "Replace or
     Skip Files" and progress dialogs come from Shell.dialogs.fileOperation; a copy into its own
     folder is named " - Copy" without asking, and a moved Recycle Bin item is restored first. */
  async function transfer(ids, to, op) {
    ids = ids.filter((id) => movable(get(id)) && id !== to && !isInside(to, id));
    const before = new Set(Shell.fs.children(to).map((c) => c.id));
    if (op === 'move') {
      ids.forEach((id) => get(id).parent === 'recycle' && Shell.fs.restore(id));
      ids = ids.filter((id) => get(id).parent !== to);
    }
    const same = op === 'copy' ? ids.filter((id) => get(id).parent === to) : [];
    same.forEach((id) => Shell.fs.copy(id, to));
    const rest = ids.filter((id) => !same.includes(id));
    const fileOp = Shell.dialogs?.fileOperation;
    if (rest.length && fileOp) await Promise.resolve(fileOp({ op, ids: rest, to })).catch((err) => console.warn('[explorer] file operation failed', err));
    else rest.forEach((id) => (op === 'move' ? moveItem(id, to) : Shell.fs.copy(id, to)));
    return Shell.fs.children(to).filter((c) => !before.has(c.id)).map((c) => c.id);
  }
  function dropInto(ids, target, copy) {
    if (target === 'recycle') { ids.forEach((id) => movable(get(id)) && get(id).parent !== 'recycle' && Shell.fs.remove(id)); return Promise.resolve([]); }
    return transfer(ids, target, copy ? 'copy' : 'move');
  }
  /* Permanent delete (Delete in the Recycle Bin, Shift+Delete, Empty Recycle Bin) after shell32's
     confirmation: "Delete File" / "Delete Folder" / "Delete Shortcut" show the item's picture beside
     its name, type, size and date, "Delete Multiple Items" the count; no warning icon. Yes is the
     default; No, Esc and the close button keep the items. Modal to its Explorer window (a press on it
     flashes the dialog), drawn with the classic classes of css/dialogs.css. Resolves true on Yes. */
  function confirmPurge(items, owner) {
    if (!items.length) return Promise.resolve(false);
    if (!Shell.wm?.open) return Promise.resolve(true);
    const one = items.length === 1 ? items[0] : null;
    const noun = one && (isContainer(one) ? 'folder' : one.kind === 'shortcut' ? 'shortcut' : 'file');
    const root = h('div', 'dlg ex-purge');
    root.dataset.theme = 'light';
    const main = h('div', 'ex-purge-main');
    main.append(h('div', null, { text: one ? `Are you sure you want to permanently delete this ${noun}?`
      : `Are you sure you want to permanently delete these ${items.length} items?` }));
    if (one) {
      const facts = noun === 'folder' ? [`Date created: ${fmtDate(one.modified)}`]
        : [`Type: ${typeOf(one)}`, `Size: ${Shell.fs.formatSize(one.size || 0)}`, `Date modified: ${fmtDate(one.modified)}`];
      main.append(h('div', 'ex-purge-item', { children: [itemIcon(one, 64),
        h('div', 'ex-purge-facts', { children: [displayName(one), ...facts].map((t) => h('div', null, { text: t })) })] }));
    }
    const button = (label) => h('button', 'dlg-btn', { type: 'button', children: [h('span', null, { children: [h('u', null, { text: label[0] }), label.slice(1)] })] });
    const yes = button('Yes'), no = button('No');
    root.append(main, h('div', 'dlg-foot ex-purge-foot', { children: [yes, no] }));

    const wa = Shell.wm.workArea?.() || { x: 0, y: 0, width: innerWidth, height: innerHeight - 48 };
    const width = Math.min(one ? 436 : 400, wa.width);
    const host = h('div', null);
    Object.assign(host.style, { position: 'fixed', left: '-10000px', top: '0', visibility: 'hidden', width: `${width - 2}px` });
    host.append(root);
    document.body.append(host);
    const height = Math.min(Math.ceil(root.getBoundingClientRect().height) + 34, wa.height); /* 32 px caption + frame */
    host.remove();
    const r = owner?.el?.isConnected ? owner.el.getBoundingClientRect() : { left: wa.x, top: wa.y, width: wa.width, height: wa.height };
    const place = (v, lo, span) => Math.round(Math.max(lo, Math.min(v, lo + span)));
    const x = place(r.left + (r.width - width) / 2, wa.x || 0, wa.width - width);
    const y = place(r.top + (r.height - height) / 2, wa.y || 0, wa.height - height);

    return new Promise((resolve) => {
      let answer = false, closed = false, flashT = 0;
      const frame = owner?.el?.querySelector(':scope > .window-frame');
      const modal = !!frame && !frame.inert;
      const blocker = h('div', 'ex-blocker', { 'aria-hidden': 'true' });
      const back = document.activeElement;
      const win = Shell.wm.open({
        appId: owner?.appId || 'explorer', title: one ? `Delete ${noun[0].toUpperCase()}${noun.slice(1)}` : 'Delete Multiple Items',
        icon: 'object/app-explorer', width, height, x, y, minWidth: width, minHeight: height, content: root,
        resizable: false, maximizable: false, minimizable: false, background: 'solid',
        onClose: () => { done(); resolve(answer); },
      });
      win.el.classList.add('dlg-window', 'dlg-noicon');
      const ownFrame = win.el.querySelector(':scope > .window-frame');
      if (ownFrame) { ownFrame.dataset.theme = 'light'; ownFrame.classList.add('dlg-frame'); }
      /* FlashWindow: the dialog's frame blinks inactive/active four times. */
      const flash = () => {
        clearInterval(flashT);
        let n = 0;
        win.el.classList.add('dlg-flashing');
        flashT = setInterval(() => {
          n++;
          win.el.classList.toggle('dlg-flash', n % 2 === 1 && n < 8);
          if (n >= 8) { clearInterval(flashT); win.el.classList.remove('dlg-flashing'); }
        }, 70);
      };
      const onDown = (e) => {
        if (e.target !== blocker) return;
        e.preventDefault();
        e.stopPropagation();
        win.focus();
        flash();
      };
      const off = Shell.on('window', ({ type, win: w }) => {
        if (w !== owner || closed) return;
        if (type === 'close') win.close();
        else if (type === 'minimize') win.minimize();
        else if (type === 'restore' && win.isMinimized()) win.restore();
        else if (type === 'focus') setTimeout(() => { if (!closed) win.focus(); });
      });
      function done() {
        closed = true;
        clearInterval(flashT);
        off();
        window.removeEventListener('pointerdown', onDown, true);
        if (modal) { blocker.remove(); frame.inert = false; }
        /* Activation goes back to the owner, to the control that had focus. */
        setTimeout(() => {
          if (!Shell.wm.all?.().includes(owner)) return;
          owner.focus?.();
          if (back?.isConnected && owner.el.contains(back)) back.focus({ preventScroll: true });
        });
      }
      if (modal) { frame.inert = true; owner.el.append(blocker); }
      window.addEventListener('pointerdown', onDown, true);

      yes.addEventListener('click', () => { answer = true; win.close(); });
      no.addEventListener('click', () => win.close());
      /* The default ring follows focus between the push buttons (DM_SETDEFID). */
      const mark = () => [yes, no].forEach((b) => b.classList.toggle('is-default', document.activeElement === no ? b === no : b === yes));
      root.addEventListener('focusin', mark);
      root.addEventListener('focusout', () => setTimeout(mark));
      win.el.addEventListener('keydown', (e) => {
        if (e.key === 'Alt' || e.key === 'Tab' || e.key.startsWith('Arrow')) root.classList.add('dlg-cues');
        if (e.key === 'Escape') { e.preventDefault(); win.close(); return; }
        if (e.key === 'Enter') { e.preventDefault(); (document.activeElement === no ? no : yes).click(); return; }
        if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') { e.preventDefault(); (document.activeElement === yes ? no : yes).focus(); return; }
        const b = !e.ctrlKey && !e.metaKey && { y: yes, n: no }[e.key.toLowerCase()];
        if (b) { e.preventDefault(); root.classList.add('dlg-cues'); b.click(); }
      });
      yes.focus();
      mark();
    });
  }
  /* Shell.fs only purges the whole Recycle Bin, so a single purge flags the item (and its
     contents) `deleted`, which Shell.fs.children/search already skip; the next Empty drops it.
     shortcut: writes the live item from Shell.fs.get, replace with Shell.fs.purge(id) once core has one. */
  async function purge(ids, owner) {
    const items = ids.map(get).filter((it) => movable(it));
    if (!(await confirmPurge(items, owner))) return;
    items.forEach((it) => {
      walk(it.id).forEach((c) => { c.deleted = true; });
      it.deleted = true;
      Shell.fs.move(it.id, 'recycle'); /* saves and emits `fs` */
    });
  }
  async function emptyRecycleBinAsk(owner) {
    if (await confirmPurge(Shell.fs.children('recycle'), owner)) Shell.fs.emptyRecycleBin();
  }
  function validDrop(target, ids, copy) {
    if (!target || !dropOk(target)) return false;
    if (ids.some((id) => id === target || isInside(target, id))) return false;
    return copy || !ids.every((id) => get(id)?.parent === target);
  }
  const dropVerb = (target, copy) => (target === 'recycle' ? 'Move to Recycle Bin' : `${copy ? 'Copy' : 'Move'} to ${nameOf(target)}`);
  function togglePin(id) {
    pins = pins.includes(id) ? pins.filter((p) => p !== id) : [...pins, id];
    store.set('explorer-pins', pins);
    refreshAll('pins');
  }

  /* ---------- one Explorer window ---------- */
  function createExplorer(args = {}) {
    const S = {
      tabs: [], active: 0, expanded: new Set(), kb: false, pending: null, renaming: null,
      typeBuf: '', typeTimer: 0, renameTimer: 0, searchTimer: 0, raf: 0, pointerCleanup: null,
      winEl: null, win: null, closed: false,
    };
    const colW = Object.fromEntries(Object.entries(COLS).map(([k, c]) => [k, c.width]));
    const cleanups = [];
    const tab = () => S.tabs[S.active];
    const makeTab = (loc) => ({
      history: [loc], index: 0, sel: new Set(), anchor: null, focusId: null, search: '', filter: null, scroll: 0, items: [],
      get loc() { return this.history[this.index]; },
    });

    /* ----- DOM: title bar tabs ----- */
    const tabsEl = h('div', 'ex-tabs');
    const tabList = h('div', 'ex-tablist', { role: 'tablist', 'aria-label': 'Tabs' });
    const addTabBtn = h('button', 'ex-tab-add', { 'aria-label': 'Add a new tab', 'data-tooltip': 'Add a new tab (Ctrl+T)', 'data-no-drag': '' });
    addTabBtn.append(ico('fluent/add', 16));
    tabsEl.append(tabList, addTabBtn);

    /* ----- DOM: header (address bar row + command bar) ----- */
    const root = h('div', 'ex', { 'aria-label': 'File Explorer' });
    const header = h('div', 'ex-header');
    const addrRow = h('div', 'ex-addrrow');
    const iconBtn = (icon, label) => { const b = h('button', 'ex-iconbtn', { 'aria-label': label }); b.append(ico(icon, 16)); return b; };
    const backBtn = iconBtn('fluent/arrow-left', 'Back');
    const fwdBtn = iconBtn('fluent/arrow-right', 'Forward');
    const upBtn = iconBtn('fluent/arrow-up', 'Up');
    const refreshBtn = iconBtn('fluent/arrow-clockwise', 'Refresh');
    const address = h('div', 'ex-address');
    const crumbs = h('div', 'ex-crumbs');
    const pathInput = h('input', 'ex-path', { type: 'text', spellcheck: 'false', autocomplete: 'off', 'aria-label': 'Address', hidden: true });
    address.append(crumbs, pathInput);
    const searchBox = h('div', 'ex-search');
    const searchInput = h('input', 'ex-search-input', { type: 'text', spellcheck: 'false', autocomplete: 'off', 'aria-label': 'Search' });
    const searchClear = h('button', 'ex-search-clear', { 'aria-label': 'Clear', tabindex: '-1', hidden: true });
    searchClear.append(ico('fluent/dismiss', 12));
    const searchGlyph = h('span', 'ex-search-glyph');
    searchGlyph.append(ico('fluent/search', 16));
    searchBox.append(searchInput, searchClear, searchGlyph);
    addrRow.append(backBtn, fwdBtn, upBtn, refreshBtn, address, searchBox);

    const cmdbar = h('div', 'ex-cmdbar', { role: 'toolbar', 'aria-label': 'Command bar' });
    const C = {};
    function cmd(key, { icon, label, tip, chevron, iconOnly, cls = '', run, submenu }) {
      const b = h('button', `ex-cmd ex-cmd-${key} ${cls}`.trim(), { 'aria-label': label, 'data-tooltip': iconOnly ? tip || label : null });
      b.append(ico(icon, 16, 'ex-cmd-icon'));
      if (!iconOnly) b.append(h('span', 'ex-cmd-label', { text: label }));
      if (chevron) b.append(ico('fluent/chevron-down', 12, 'ex-cmd-chev'));
      b.addEventListener('click', () => (submenu ? openMenuAt(b, submenu(), key === 'more' ? 'bottom-end' : 'bottom-start') : run()));
      C[key] = { el: b, icon, label, run, submenu };
      return b;
    }
    const sep = (cls = '') => h('div', `ex-cmd-sep ${cls}`.trim(), { role: 'separator' });
    cmdbar.append(
      cmd('new', { icon: 'fluent/add', label: 'New', chevron: true, submenu: newMenuItems }),
      sep(),
      cmd('cut', { icon: 'fluent/cut', label: 'Cut', tip: 'Cut (Ctrl+X)', iconOnly: true, run: () => clip(true) }),
      cmd('copy', { icon: 'fluent/copy', label: 'Copy', tip: 'Copy (Ctrl+C)', iconOnly: true, run: () => clip(false) }),
      cmd('paste', { icon: 'fluent/clipboard-paste', label: 'Paste', tip: 'Paste (Ctrl+V)', iconOnly: true, run: paste }),
      cmd('rename', { icon: 'fluent/rename', label: 'Rename', tip: 'Rename (F2)', iconOnly: true, run: () => renameSelected() }),
      cmd('share', { icon: 'fluent/share', label: 'Share', iconOnly: true, run() {} }),
      cmd('delete', { icon: 'fluent/delete', label: 'Delete', tip: 'Delete (Delete)', iconOnly: true, run: deleteSelected }),
      sep(),
      cmd('sort', { icon: 'fluent/arrow-sort', label: 'Sort', chevron: true, submenu: sortMenuItems }),
      cmd('view', { icon: 'fluent/text-bullet-list', label: 'View', chevron: true, submenu: viewMenuItems }),
      cmd('filter', { icon: 'fluent/filter', label: 'Filter', chevron: true, submenu: filterMenuItems }),
      sep('ex-recycle-only'),
      cmd('empty', { icon: 'fluent/delete', label: 'Empty Recycle Bin', cls: 'ex-recycle-only', run: emptyRecycleBin }),
      cmd('restore', { icon: 'fluent/history', label: 'Restore all items', cls: 'ex-recycle-only', run: restoreItems }),
      sep('ex-sep-more'),
      cmd('more', { icon: 'fluent/more-horizontal', label: 'See more', iconOnly: true, submenu: moreMenuItems }),
      h('div', 'ex-cmd-spacer'),
      cmd('details', { icon: 'fluent/info', label: 'Details', run: () => setLayout('details', !layout.details) }),
    );
    header.append(addrRow, cmdbar);

    /* ----- DOM: body (navigation pane, content, details pane) and status bar ----- */
    const body = h('div', 'ex-body');
    const nav = h('div', 'ex-nav', { role: 'tree', 'aria-label': 'Navigation pane' });
    const splitter = h('div', 'ex-splitter', { 'aria-hidden': 'true' });
    const content = h('div', 'ex-content', { tabindex: '0', role: 'listbox', 'aria-multiselectable': 'true', 'aria-label': 'Items view' });
    const dpane = h('aside', 'ex-dpane', { 'aria-label': 'Details pane' });
    body.append(nav, splitter, content, dpane);
    const status = h('div', 'ex-status');
    const statusText = h('div', 'ex-status-text', { 'aria-live': 'polite' });
    const statusDetails = h('button', 'ex-status-btn', { 'aria-label': 'Details', 'data-tooltip': 'Display information about each item in the window. (Ctrl+Shift+6)' });
    statusDetails.append(ico('fluent/text-bullet-list', 16));
    const statusLarge = h('button', 'ex-status-btn', { 'aria-label': 'Large icons', 'data-tooltip': 'Display items by using large thumbnails. (Ctrl+Shift+2)' });
    statusLarge.append(ico('fluent/grid', 16));
    status.append(statusText, h('div', 'ex-status-fill'), statusDetails, statusLarge);
    root.append(header, body, status);

    /* ----- layout state ----- */
    function applyLayout() {
      root.classList.toggle('ex-compact', layout.compact);
      root.classList.toggle('ex-no-nav', !layout.nav);
      if (layout.navWidth) root.style.setProperty('--ex-nav-w', `${layout.navWidth}px`);
      C.details.el.classList.toggle('checked', layout.details);
      C.details.el.setAttribute('aria-pressed', String(layout.details));
    }
    function setLayout(key, value) {
      layout[key] = value;
      saveLayout();
      refreshAll('layout');
    }

    /* ----- navigation ----- */
    function navigate(loc, { select = null, push = true } = {}) {
      if (!loc || (!PSEUDO[loc] && !get(loc))) return;
      const t = tab();
      endRename(false);
      if (push && (t.loc !== loc || t.search)) {
        t.history = t.history.slice(0, t.index + 1);
        t.history.push(loc);
        t.index = t.history.length - 1;
      }
      t.search = '';
      t.filter = null;
      t.sel = new Set(select ? [select] : []);
      t.anchor = t.focusId = select;
      t.scroll = 0;
      S.pending = null;
      searchInput.value = '';
      renderAll(true);
      if (select) scrollToFocus();
    }
    function go(delta) {
      const t = tab();
      const i = t.index + delta;
      if (i < 0 || i >= t.history.length) return;
      const from = t.loc;
      t.index = i;
      t.search = '';
      t.filter = null;
      const back = delta < 0 && get(from)?.parent === t.loc;
      t.sel = new Set(back ? [from] : []);
      t.anchor = t.focusId = back ? from : null;
      searchInput.value = '';
      renderAll(true);
    }
    function goUp() {
      const from = tab().loc;
      const parent = get(from)?.parent;
      if (parent) navigate(parent, { select: from });
    }
    function refresh() { endRename(true); renderAll(true); }

    /* ----- tabs ----- */
    function tabTitle(t) { return t.search ? `Search Results in ${nameOf(t.loc)}` : nameOf(t.loc); }
    function renderTabs() {
      tabList.replaceChildren(...S.tabs.map((t, i) => {
        const sel = i === S.active;
        const el = h('div', `ex-tab${sel ? ' selected' : ''}`, {
          role: 'tab', 'aria-selected': String(sel), tabindex: sel ? '0' : '-1', 'data-no-drag': '',
          'data-drop': dropOk(t.loc) ? t.loc : null, title: tabTitle(t),
        });
        const x = h('button', 'ex-tab-close', { 'aria-label': 'Close tab', 'data-tooltip': 'Close tab (Ctrl+W)', tabindex: '-1' });
        x.append(ico('fluent/dismiss', 12));
        el.append(ico(locIcon(t.loc), 16, 'ex-tab-icon'), h('span', 'ex-tab-title', { text: tabTitle(t) }), x);
        el.addEventListener('pointerdown', (e) => { if (e.button === 0 && !x.contains(e.target) && i !== S.active) selectTab(i); });
        el.addEventListener('auxclick', (e) => { if (e.button === 1) { e.preventDefault(); closeTab(i); } });
        el.addEventListener('mousedown', (e) => { if (e.button === 1) e.preventDefault(); }); /* no autoscroll */
        x.addEventListener('click', (e) => { e.stopPropagation(); closeTab(i); });
        el.addEventListener('contextmenu', (e) => { e.preventDefault(); tabMenu(e, i); });
        el.addEventListener('keydown', (e) => {
          if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') {
            e.preventDefault();
            selectTab((S.active + (e.key === 'ArrowRight' ? 1 : -1) + S.tabs.length) % S.tabs.length);
            tabList.children[S.active]?.focus();
          }
        });
        return el;
      }));
    }
    function selectTab(i) {
      if (i === S.active || !S.tabs[i]) return;
      endRename(true);
      tab().scroll = content.scrollTop;
      S.active = i;
      searchInput.value = tab().search;
      renderAll(false);
      content.scrollTop = tab().scroll;
    }
    function addTab(loc = 'home', activate = true) {
      endRename(true);
      tab() && (tab().scroll = content.scrollTop);
      S.tabs.push(makeTab(loc));
      if (activate) { S.active = S.tabs.length - 1; searchInput.value = ''; renderAll(true); } else renderTabs();
    }
    function closeTab(i) {
      if (S.tabs.length === 1) { S.win?.close?.(); return; }
      endRename(true);
      S.tabs.splice(i, 1);
      if (i < S.active) S.active--;
      S.active = Math.min(S.active, S.tabs.length - 1);
      searchInput.value = tab().search;
      renderAll(false);
    }
    function tabMenu(e, i) {
      const n = S.tabs.length;
      menu({ x: e.clientX, y: e.clientY, items: [
        { label: 'Duplicate tab', icon: 'fluent/tab-desktop', action: () => { S.tabs.splice(i + 1, 0, makeTab(S.tabs[i].loc)); S.active = i + 1; renderAll(true); } },
        { separator: true },
        { label: 'Close tab', icon: 'fluent/dismiss', shortcut: 'Ctrl+W', action: () => closeTab(i) },
        { label: 'Close other tabs', disabled: n === 1, action: () => { S.tabs = [S.tabs[i]]; S.active = 0; renderAll(false); } },
        { label: 'Close tabs to the right', disabled: i === n - 1, action: () => { S.tabs.length = i + 1; S.active = Math.min(S.active, i); renderAll(false); } },
      ] });
    }
    addTabBtn.addEventListener('click', () => addTab('home'));

    /* ----- address bar ----- */
    const crumbChain = (loc) => (PSEUDO[loc] ? [loc] : Shell.fs.path(loc).map((i) => i.id));
    function renderAddress() {
      const t = tab();
      const loc = t.loc;
      const parent = get(loc)?.parent;
      backBtn.disabled = t.index <= 0;
      fwdBtn.disabled = t.index >= t.history.length - 1;
      upBtn.disabled = !parent;
      backBtn.dataset.tooltip = t.index > 0 ? `Back to ${nameOf(t.history[t.index - 1])} (Alt + Left Arrow)` : 'Back (Alt + Left Arrow)';
      fwdBtn.dataset.tooltip = t.index < t.history.length - 1 ? `Forward to ${nameOf(t.history[t.index + 1])} (Alt + Right Arrow)` : 'Forward (Alt + Right Arrow)';
      upBtn.dataset.tooltip = parent ? `Up to "${nameOf(parent)}" (Alt + Up Arrow)` : 'Up (Alt + Up Arrow)';
      refreshBtn.dataset.tooltip = `Refresh "${nameOf(loc)}" (F5)`;
      const lead = h('span', 'ex-crumb-icon');
      lead.append(ico(locIcon(loc), 16));
      crumbs.replaceChildren(lead, crumbChevron(null));
      const chain = crumbChain(loc);
      chain.forEach((id, i) => {
        crumbs.append(crumbSeg(id));
        if (i < chain.length - 1 || hasSubfolders(id) || t.search) crumbs.append(crumbChevron(id));
      });
      if (t.search) crumbs.append(h('span', 'ex-crumb-text', { text: `Search Results in ${nameOf(loc)}` }));
      searchInput.placeholder = `Search ${nameOf(loc)}`;
      if (document.activeElement !== searchInput) searchInput.value = t.search;
      searchClear.hidden = !searchInput.value;
      fitCrumbs();
    }
    function crumbSeg(id) {
      const b = h('button', 'ex-crumb', { 'data-loc': id, 'data-drop': dropOk(id) ? id : null, tabindex: '-1' });
      b.textContent = nameOf(id);
      b.addEventListener('click', () => navigate(id));
      b.addEventListener('auxclick', (e) => { if (e.button === 1) addTab(id, false); });
      return b;
    }
    function crumbChevron(id) {
      const b = h('button', 'ex-crumb-chev', { 'aria-label': id ? `Folders in ${nameOf(id)}` : 'Locations', tabindex: '-1' });
      b.append(ico('fluent/chevron-right', 12));
      b.addEventListener('click', () => {
        const list = id ? subfolders(id).map((c) => c.id) : ['home', 'gallery', 'desktop', 'root', 'network', 'recycle'];
        const items = list.map((c) => ({ label: nameOf(c), icon: locIcon(c), action: () => navigate(c) }));
        openMenuAt(b, items.length ? items : [{ label: 'No subfolders', disabled: true }], 'bottom-start');
      });
      return b;
    }
    /* Long paths: leading segments fold into a "<" button that lists them, as Windows does. */
    function fitCrumbs() {
      crumbs.querySelector('.ex-crumb-more')?.remove();
      [...crumbs.children].forEach((c) => { c.hidden = false; });
      if (crumbs.scrollWidth <= crumbs.clientWidth + 1) return;
      const segs = [...crumbs.querySelectorAll('.ex-crumb')];
      const more = h('button', 'ex-crumb-more', { 'aria-label': 'More locations', tabindex: '-1' });
      more.append(ico('fluent/chevron-left', 12));
      crumbs.children[1].hidden = true;
      crumbs.insertBefore(more, crumbs.children[1]);
      const hiddenIds = [];
      for (const s of segs.slice(0, -1)) {
        if (crumbs.scrollWidth <= crumbs.clientWidth + 1) break;
        s.hidden = true;
        if (s.nextElementSibling?.classList.contains('ex-crumb-chev')) s.nextElementSibling.hidden = true;
        hiddenIds.push(s.dataset.loc);
      }
      more.addEventListener('click', () => openMenuAt(more, hiddenIds.reverse().map((c) => ({ label: nameOf(c), icon: locIcon(c), action: () => navigate(c) })), 'bottom-start'));
    }
    function enterEdit() {
      endRename(true);
      address.classList.add('editing');
      crumbs.hidden = true;
      pathInput.hidden = false;
      pathInput.value = osPath(tab().loc);
      pathInput.focus();
      pathInput.select();
    }
    function exitEdit() {
      if (pathInput.hidden) return;
      address.classList.remove('editing');
      pathInput.hidden = true;
      crumbs.hidden = false;
      fitCrumbs();
    }
    address.addEventListener('pointerdown', (e) => {
      if (e.button === 0 && (e.target === crumbs || e.target === address)) { e.preventDefault(); enterEdit(); }
    });
    pathInput.addEventListener('keydown', (e) => {
      e.stopPropagation();
      if (e.key === 'Escape') { exitEdit(); content.focus({ preventScroll: true }); }
      if (e.key === 'Enter') {
        const id = resolvePath(pathInput.value);
        if (id) { exitEdit(); navigate(id); content.focus({ preventScroll: true }); } else pathInput.select();
      }
    });
    pathInput.addEventListener('blur', exitEdit);
    backBtn.addEventListener('click', () => go(-1));
    fwdBtn.addEventListener('click', () => go(1));
    upBtn.addEventListener('click', goUp);
    refreshBtn.addEventListener('click', refresh);

    /* ----- search ----- */
    function runSearch(q) {
      const t = tab();
      q = q.trim();
      if (q === t.search) return;
      t.search = q;
      t.sel = new Set();
      t.focusId = t.anchor = null;
      renderAll(true);
    }
    searchInput.addEventListener('input', () => {
      searchClear.hidden = !searchInput.value;
      clearTimeout(S.searchTimer);
      S.searchTimer = setTimeout(() => runSearch(searchInput.value), 250);
    });
    searchInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { clearTimeout(S.searchTimer); runSearch(searchInput.value); e.stopPropagation(); }
      if (e.key === 'Escape') { searchInput.value = ''; searchClear.hidden = true; runSearch(''); content.focus({ preventScroll: true }); e.stopPropagation(); }
      if (e.key === 'ArrowDown' && tab().items.length) { e.preventDefault(); content.focus({ preventScroll: true }); moveFocus('Home', e); }
    });
    searchClear.addEventListener('click', () => { searchInput.value = ''; searchClear.hidden = true; runSearch(''); searchInput.focus(); });
    const focusSearch = () => { searchInput.focus(); searchInput.select(); };
    function searchResults(loc, q) {
      return Shell.fs.search(q).filter((it) => it.kind !== 'system' && it.id !== loc &&
        (loc === 'recycle' ? it.parent === 'recycle' : !inRecycle(it.id) && it.parent !== 'recycle' &&
          (['home', 'root', 'gallery'].includes(loc) || isInside(it.id, loc))));
    }

    /* ----- command bar state and menus ----- */
    const selItems = () => [...tab().sel].map(get).filter(Boolean);
    function updateCommands() {
      const t = tab();
      const loc = t.loc;
      const sel = selItems();
      const any = sel.length > 0;
      const allMovable = any && sel.every(movable);
      const recycle = loc === 'recycle' && !t.search;
      root.classList.toggle('ex-in-recycle', recycle);
      C.new.el.disabled = !canCreate(loc) || !!t.search;
      C.cut.el.disabled = !allMovable;
      C.copy.el.disabled = !allMovable || recycle;
      C.paste.el.disabled = !clipNow() || !canCreate(loc) || !!t.search;
      C.rename.el.disabled = !(sel.length === 1 && renamable(sel[0]));
      C.share.el.disabled = true; /* nothing to share to on this device */
      C.delete.el.disabled = !allMovable; /* in the Recycle Bin it deletes permanently */
      C.filter.el.hidden = SPECIAL.includes(loc);
      C.filter.el.classList.toggle('checked', !!t.filter);
      C.empty.el.disabled = !Shell.fs.children('recycle').length;
      C.restore.el.disabled = !Shell.fs.children('recycle').length;
      const restoreLabel = any ? 'Restore the selected items' : 'Restore all items';
      C.restore.el.querySelector('.ex-cmd-label').textContent = restoreLabel;
      C.restore.el.setAttribute('aria-label', restoreLabel);
      const special = SPECIAL.includes(loc);
      const v = viewInfo(special ? 'details' : viewOf(loc));
      if (C.view.icon !== v[2]) {
        C.view.icon = v[2];
        C.view.el.querySelector('.ex-cmd-icon').replaceWith(ico(v[2], 16, 'ex-cmd-icon'));
      }
      const view = viewOf(loc);
      statusDetails.classList.toggle('checked', !special && view === 'details');
      statusLarge.classList.toggle('checked', !special && view === 'large');
      statusDetails.disabled = statusLarge.disabled = special;
    }
    function openMenuAt(btn, items, placement) {
      btn.classList.add('open');
      const r = btn.getBoundingClientRect();
      const m = menu({ x: r.left, y: r.bottom, anchorRect: r, placement, items, onClose: () => btn.classList.remove('open') });
      if (!m) btn.classList.remove('open');
    }
    function newMenuItems() {
      const loc = tab().loc;
      const ok = canCreate(loc);
      return [
        { label: 'Folder', icon: 'object/folder', disabled: !ok, action: () => newItem('folder') },
        { label: 'Shortcut', icon: 'object/shortcut-arrow', disabled: true },
        { separator: true },
        { label: 'Text Document', icon: 'object/text', disabled: !ok, action: () => newItem('txt') },
        { label: 'Compressed (zipped) Folder', icon: 'object/folder', disabled: !ok, action: () => newItem('zip') },
      ];
    }
    function sortMenuItems() {
      const t = tab();
      const cols = t.loc === 'recycle' ? COLSETS.recycle : COLSETS.folder;
      const s = sortOf(t.loc, cols);
      const special = SPECIAL.includes(t.loc);
      return [
        ...cols.map((k) => ({ label: COLS[k].label, radio: true, checked: s.key === k, disabled: special, action: () => setFolderView(t.loc, { sort: k }) })),
        { separator: true },
        { label: 'Ascending', radio: true, checked: s.dir === 1, disabled: special, action: () => setFolderView(t.loc, { dir: 1 }) },
        { label: 'Descending', radio: true, checked: s.dir === -1, disabled: special, action: () => setFolderView(t.loc, { dir: -1 }) },
      ];
    }
    function viewMenuItems() {
      const loc = tab().loc;
      const special = SPECIAL.includes(loc);
      const cur = viewOf(loc);
      return [
        ...VIEWS.map(([v, label, icon, , shortcut]) => ({ label, icon, shortcut, radio: true, checked: !special && cur === v, disabled: special, action: () => setFolderView(loc, { view: v }) })),
        { separator: true },
        { label: 'Compact view', icon: 'fluent/text-bullet-list', checked: layout.compact, action: () => setLayout('compact', !layout.compact) },
        { label: 'Show', icon: 'fluent/select-all', submenu: [
          { label: 'Navigation pane', checked: layout.nav, action: () => setLayout('nav', !layout.nav) },
          { label: 'Details pane', checked: layout.details, shortcut: 'Alt+Shift+P', action: () => setLayout('details', !layout.details) },
          { label: 'File name extensions', checked: layout.ext, action: () => setLayout('ext', !layout.ext) },
        ] },
      ];
    }
    function filterMenuItems() {
      const t = tab();
      return [
        ...Object.entries(FILTERS).map(([k, f]) => ({ label: f.label, icon: f.icon, radio: true, checked: t.filter === k,
          action: () => { t.filter = t.filter === k ? null : k; renderContent(true); } })),
        { separator: true },
        { label: 'Clear filter', icon: 'fluent/dismiss', disabled: !t.filter, action: () => { t.filter = null; renderContent(true); } },
      ];
    }
    function moreMenuItems() {
      const t = tab();
      const recycleShown = root.classList.contains('ex-in-recycle');
      const folded = Object.entries(C).filter(([k, c]) => k !== 'more' && !c.el.hidden &&
        (recycleShown || !c.el.classList.contains('ex-recycle-only')) && getComputedStyle(c.el).display === 'none');
      const items = folded.map(([, c]) => (c.submenu ? { label: c.label, icon: c.icon, submenu: c.submenu() }
        : { label: c.el.getAttribute('aria-label'), icon: c.icon, disabled: c.el.disabled, action: c.run }));
      if (items.length) items.push({ separator: true });
      const sel = selItems();
      const pinTarget = sel.length === 1 && isContainer(sel[0]) ? sel[0].id : (!sel.length && canCreate(t.loc) ? t.loc : null);
      items.push(
        { label: 'Undo', icon: 'fluent/arrow-clockwise', shortcut: 'Ctrl+Z', disabled: true },
        { separator: true },
        { label: pinTarget && pins.includes(pinTarget) ? 'Unpin from Quick access' : 'Pin to Quick access', icon: 'fluent/pin', disabled: !pinTarget, action: () => togglePin(pinTarget) },
        { separator: true },
        { label: 'Select all', icon: 'fluent/select-all', shortcut: 'Ctrl+A', action: selectAll },
        { label: 'Select none', icon: 'fluent/dismiss', action: () => { t.sel.clear(); applySelection(); } },
        { label: 'Invert selection', icon: 'fluent/arrow-sync', action: invertSelection },
        { separator: true },
        { label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter', action: showProperties },
      );
      return items;
    }

    /* ----- actions ----- */
    function clip(cut) {
      const sel = selItems().filter(movable);
      if (sel.length) Shell.clipboard.set(sel.map((i) => i.id), { cut });
    }
    function paste() {
      const t = tab();
      const loc = t.loc;
      const c = clipNow();
      if (!c || !canCreate(loc) || t.search) return;
      if (c.cut) Shell.clipboard.clear(); /* cut items land once; the clipboard empties */
      transfer(c.ids, loc, c.cut ? 'move' : 'copy').then((made) => {
        if (S.closed || !made.length) return;
        S.pending = { select: made };
        instance.refresh();
      });
    }
    /* Delete sends to the Recycle Bin without asking (Windows 11's default); in the Recycle Bin,
       and with Shift, it deletes permanently after confirmation. */
    function deleteSelected(permanent = false) {
      const sel = selItems().filter(movable);
      if (!sel.length) return;
      if (permanent === true || sel.every((it) => it.parent === 'recycle')) purge(sel.map((it) => it.id), S.win);
      else sel.forEach((it) => it.parent !== 'recycle' && Shell.fs.remove(it.id));
    }
    function restoreItems() {
      const sel = selItems();
      (sel.length ? sel : Shell.fs.children('recycle')).forEach((it) => Shell.fs.restore(it.id));
    }
    function emptyRecycleBin() { emptyRecycleBinAsk(S.win); }
    function newItem(kind) {
      const loc = tab().loc;
      if (!canCreate(loc) || tab().search) return;
      let item;
      if (kind === 'folder') item = Shell.fs.create(loc, { name: Shell.fs.uniqueName(loc, 'New folder'), kind: 'folder', icon: 'object/folder' });
      else if (kind === 'txt') item = Shell.fs.create(loc, { name: Shell.fs.uniqueName(loc, 'New Text Document', 'txt'), kind: 'file', ext: 'txt', icon: 'object/text', content: '' });
      else item = Shell.fs.create(loc, { name: Shell.fs.uniqueName(loc, 'New Compressed (zipped) Folder', 'zip'), kind: 'file', ext: 'zip', icon: 'object/folder', size: 22 });
      S.pending = { select: [item.id], rename: item.id };
    }
    function compressSelected() {
      const sel = selItems().filter(movable);
      const loc = tab().loc;
      if (!sel.length || !canCreate(loc)) return;
      const first = sel[0];
      const base = first.kind === 'file' && first.ext && first.name.toLowerCase().endsWith(`.${first.ext}`) ? first.name.slice(0, -(first.ext.length + 1)) : first.name;
      const size = sel.reduce((a, it) => a + (it.size || 0), 0);
      const item = Shell.fs.create(loc, { name: Shell.fs.uniqueName(loc, base, 'zip'), kind: 'file', ext: 'zip', icon: 'object/folder', size: Math.round(size * 0.6) + 22 });
      S.pending = { select: [item.id], rename: item.id };
    }
    function copyPath() {
      const sel = selItems();
      const text = (sel.length ? sel.map((it) => `"${osPath(it.id)}"`) : [`"${osPath(tab().loc)}"`]).join('\r\n');
      navigator.clipboard?.writeText(text).catch(() => {});
    }
    /* The property sheet of the focused item, else of the folder shown; This PC opens Settings >
       About as Windows 11 does. Home, Gallery, Network (and no dialogs module) show the details pane. */
    function showProperties(id) {
      const t = tab();
      if (typeof id !== 'string') id = t.sel.size ? (t.sel.has(t.focusId) ? t.focusId : [...t.sel][0]) : t.loc;
      if (id === 'root') { Shell.apps.launch('settings', { page: 'system/about' }); return; }
      if (Shell.dialogs?.properties && get(id) && !SPECIAL.includes(id)) { Shell.dialogs.properties(id); return; }
      if (!layout.details) setLayout('details', true);
    }
    const canOpenWith = (it) => !!Shell.dialogs?.openWith && it?.kind === 'file' && it.ext !== 'exe' && it.parent !== 'recycle';
    /* "Select an app to open this .<ext> file"; the chosen app gets {file}. */
    function openWith(it) {
      if (!canOpenWith(it)) return false;
      Promise.resolve(Shell.dialogs.openWith(it.id))
        .then((app) => { if (app && Shell.apps.get(app)) Shell.apps.launch(app, { file: it.id }); })
        .catch((err) => console.warn('[explorer] open with failed', err));
      return true;
    }
    function openItem(it) {
      if (!it) return;
      if (it.parent === 'recycle') { showProperties(it.id); return; }
      if (isContainer(it)) { navigate(it.id); return; }
      if (it.kind === 'shortcut' || it.ext === 'txt') { Shell.fs.open(it.id); return; }
      if (it.ext === 'exe') { Shell.apps.launch('game', { title: displayName(it) }); return; }
      if (openWith(it)) return; /* no app on this PC is associated with the type */
      if (it.parent !== tab().loc || tab().search) navigate(it.parent, { select: it.id });
    }
    function openSelected() {
      const sel = selItems();
      const folders = sel.filter(isContainer);
      if (folders.length) { folders.slice(1).forEach((f) => addTab(f.id, false)); navigate(folders[0].id); }
      sel.filter((it) => !isContainer(it)).forEach(openItem);
    }
    function selectAll() {
      const t = tab();
      t.sel = new Set(t.items);
      applySelection();
    }
    function invertSelection() {
      const t = tab();
      t.sel = new Set(t.items.filter((id) => !t.sel.has(id)));
      applySelection();
    }
    function renameSelected() {
      const t = tab();
      const id = t.focusId && t.sel.has(t.focusId) ? t.focusId : [...t.sel][0];
      if (id) startRename(id);
    }

    /* ----- navigation pane ----- */
    function navItem(id, depth, { pinned = false, expandable = false } = {}) {
      const open = S.expanded.has(id);
      const row = h('div', 'ex-nav-item', {
        role: 'treeitem', tabindex: '-1', 'data-loc': id, 'data-drop': dropOk(id) ? id : null,
        'aria-expanded': expandable ? String(open) : null, title: nameOf(id),
      });
      row.style.setProperty('--depth', depth);
      const chev = h('span', `ex-nav-chev${open ? ' open' : ''}`);
      if (expandable) chev.append(ico('fluent/chevron-right', 12));
      row.append(chev, ico(locIcon(id), 16, 'ex-nav-icon'), h('span', 'ex-nav-label', { text: nameOf(id) }));
      if (pinned) row.append(ico('fluent/pin', 16, 'ex-nav-pin'));
      return row;
    }
    function renderNav() {
      const focusedLoc = nav.contains(document.activeElement) ? document.activeElement.dataset.loc : null;
      const scroll = nav.scrollTop;
      nav.replaceChildren(navItem('home', 0), navItem('gallery', 0), h('div', 'ex-nav-sep', { role: 'separator' }));
      pins.filter((id) => get(id) && !inRecycle(id) && get(id).parent !== 'recycle').forEach((id) => nav.append(navItem(id, 0, { pinned: true })));
      nav.append(h('div', 'ex-nav-sep', { role: 'separator' }));
      const tree = (id, depth) => {
        nav.append(navItem(id, depth, { expandable: hasSubfolders(id) }));
        if (S.expanded.has(id)) subfolders(id).forEach((c) => tree(c.id, depth + 1));
      };
      tree('root', 0);
      nav.append(navItem('network', 0)); /* Recycle Bin shows here only with "Show all folders" */
      const sel = nav.querySelector(`.ex-nav-item[data-loc="${CSS.escape(tab().loc)}"]`);
      if (sel) { sel.classList.add('selected'); sel.setAttribute('aria-selected', 'true'); }
      (sel || nav.querySelector('.ex-nav-item')).tabIndex = 0;
      nav.scrollTop = scroll;
      if (focusedLoc) nav.querySelector(`.ex-nav-item[data-loc="${CSS.escape(focusedLoc)}"]`)?.focus({ preventScroll: true });
    }
    function toggleExpand(id) {
      if (S.expanded.has(id)) S.expanded.delete(id); else S.expanded.add(id);
      renderNav();
    }
    function navMenu(e, id) {
      const it = get(id);
      const pinned = pins.includes(id);
      const items = [
        { label: 'Open in new tab', icon: 'fluent/tab-desktop', action: () => addTab(id, false) },
        { label: 'Open in new window', icon: 'fluent/window-new', action: () => Shell.apps.launch('explorer', { folder: id }) },
      ];
      if (id === 'recycle') items.push({ label: 'Empty Recycle Bin', icon: 'fluent/delete', disabled: !Shell.fs.children('recycle').length, action: emptyRecycleBin });
      else if (it && it.kind !== 'system') items.push({ label: pinned ? 'Unpin from Quick access' : 'Pin to Quick access', icon: pinned ? 'fluent/pin-off' : 'fluent/pin', action: () => togglePin(id) });
      if (it && it.kind !== 'system') items.push({ label: 'Copy as path', icon: 'fluent/document', action: () => navigator.clipboard?.writeText(`"${osPath(id)}"`).catch(() => {}) });
      items.push({ label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter', action: () => {
        if (id !== 'root' && (SPECIAL.includes(id) || !Shell.dialogs?.properties)) navigate(id); /* its details pane */
        showProperties(id);
      } });
      if (canCreate(id)) items.push({ separator: true }, { label: 'Open in Terminal', icon: 'object/app-terminal', action: () => Shell.apps.launch('terminal', { cwd: osPath(id) }) });
      menu({ x: e.clientX, y: e.clientY, items });
    }
    nav.addEventListener('click', (e) => {
      const row = e.target.closest('.ex-nav-item');
      if (!row) return;
      if (e.target.closest('.ex-nav-chev') && row.hasAttribute('aria-expanded')) { toggleExpand(row.dataset.loc); return; }
      if (e.ctrlKey) { addTab(row.dataset.loc, false); return; }
      navigate(row.dataset.loc);
    });
    nav.addEventListener('dblclick', (e) => {
      const row = e.target.closest('.ex-nav-item');
      if (row?.hasAttribute('aria-expanded') && !e.target.closest('.ex-nav-chev')) toggleExpand(row.dataset.loc);
    });
    nav.addEventListener('auxclick', (e) => { const row = e.target.closest('.ex-nav-item'); if (row && e.button === 1) addTab(row.dataset.loc, false); });
    nav.addEventListener('mousedown', (e) => { if (e.button === 1) e.preventDefault(); });
    nav.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      const row = e.target.closest('.ex-nav-item');
      if (row) navMenu(e, row.dataset.loc);
    });
    function navKey(e) {
      const rows = [...nav.querySelectorAll('.ex-nav-item')];
      const row = e.target.closest('.ex-nav-item');
      const i = rows.indexOf(row);
      if (i < 0) return false;
      const focusRow = (r) => { if (!r) return; rows.forEach((x) => { x.tabIndex = -1; }); r.tabIndex = 0; r.focus(); };
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { focusRow(rows[i + (e.key === 'ArrowDown' ? 1 : -1)]); return true; }
      if (e.key === 'Home' || e.key === 'End') { focusRow(e.key === 'Home' ? rows[0] : rows[rows.length - 1]); return true; }
      if (e.key === 'Enter' || e.key === ' ') { navigate(row.dataset.loc); return true; }
      const exp = row.getAttribute('aria-expanded');
      if ((e.key === 'ArrowRight' && exp === 'false') || (e.key === 'ArrowLeft' && exp === 'true')) { toggleExpand(row.dataset.loc); return true; }
      return false;
    }

    /* Splitter between the navigation pane and the content. */
    splitter.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      const startX = e.clientX;
      const startW = nav.getBoundingClientRect().width;
      splitter.setPointerCapture(e.pointerId);
      splitter.classList.add('active');
      const move = (ev) => {
        const w = Math.round(Math.max(120, Math.min(root.clientWidth * 0.5, startW + ev.clientX - startX)));
        root.style.setProperty('--ex-nav-w', `${w}px`);
        layout.navWidth = w;
      };
      const up = () => {
        splitter.removeEventListener('pointermove', move);
        splitter.removeEventListener('pointerup', up);
        splitter.removeEventListener('pointercancel', up);
        splitter.classList.remove('active');
        saveLayout();
      };
      splitter.addEventListener('pointermove', move);
      splitter.addEventListener('pointerup', up);
      splitter.addEventListener('pointercancel', up);
    });

    /* ----- content views ----- */
    function itemShell(it, cls) {
      const t = tab();
      const el = h('div', `ex-item ${cls}`, {
        role: 'option', 'data-id': it.id, 'aria-selected': String(t.sel.has(it.id)),
        'data-drop': isContainer(it) && dropOk(it.id) ? it.id : null,
      });
      if (isCut(it.id)) el.classList.add('cut');
      return el;
    }
    function note(text) { return h('div', 'ex-note', { text }); }

    function detailsView(items, cols, { sortable = true, sort = null, emptyText = '' } = {}) {
      const wrap = h('div', 'ex-dv', { role: 'grid' });
      wrap.style.setProperty('--cols', cols.map((k) => `${colW[k]}px`).join(' '));
      const head = h('div', 'ex-dv-head', { role: 'row' });
      cols.forEach((k) => {
        const sorted = sortable && sort && sort.key === k;
        const c = h('div', `ex-dv-hcell${COLS[k].right ? ' right' : ''}${sorted ? ' sorted' : ''}${sortable ? '' : ' static'}`, {
          role: 'columnheader', 'data-col': k, 'aria-sort': sorted ? (sort.dir === 1 ? 'ascending' : 'descending') : null,
        });
        c.append(h('span', 'ex-dv-htext', { text: COLS[k].label }));
        if (sorted) c.append(ico(sort.dir === 1 ? 'fluent/chevron-up' : 'fluent/chevron-down', 12, 'ex-dv-sort'));
        c.append(h('div', 'ex-dv-grip', { 'data-grip': k }));
        head.append(c);
      });
      wrap.append(head);
      items.forEach((it) => {
        const row = itemShell(it, 'ex-row');
        cols.forEach((k) => {
          const cell = h('div', `ex-cell ex-cell-${k}${COLS[k].right ? ' right' : ''}`, { role: 'gridcell' });
          if (k === 'name') cell.append(itemIcon(it, 16), h('span', 'ex-name', { text: displayName(it) }));
          else cell.textContent = COLS[k].text(it);
          row.append(cell);
        });
        wrap.append(row);
      });
      if (!items.length && emptyText) wrap.append(note(emptyText));
      return wrap;
    }
    function iconsView(items, view, emptyText) {
      if (!items.length) return note(emptyText);
      const size = viewInfo(view)[3];
      const wrap = h('div', `ex-grid ex-grid-${view}`);
      items.forEach((it) => {
        const tile = itemShell(it, `ex-tile ex-tile-${view}`);
        tile.append(itemIcon(it, size));
        if (view === 'tiles') {
          const text = h('div', 'ex-tile-text');
          text.append(h('span', 'ex-name', { text: displayName(it) }), h('span', 'ex-meta', { text: typeOf(it) }));
          if (it.kind === 'file') text.append(h('span', 'ex-meta', { text: Shell.fs.formatSize(it.size || 0) }));
          tile.append(text);
        } else tile.append(h('span', 'ex-name', { text: displayName(it) }));
        tile.title = view === 'large' || view === 'medium' ? displayName(it) : '';
        wrap.append(tile);
      });
      return wrap;
    }
    function section(key, title, node) {
      const closed = collapsedSections.has(key);
      const s = h('section', `ex-section${closed ? ' collapsed' : ''}`);
      const head = h('button', 'ex-section-head', { 'aria-expanded': String(!closed) });
      head.append(ico('fluent/chevron-down', 12, 'ex-section-chev'), h('span', null, { text: title }));
      const bodyEl = h('div', 'ex-section-body');
      bodyEl.append(node);
      head.addEventListener('click', () => {
        const opening = collapsedSections.has(key);
        if (opening) collapsedSections.delete(key); else collapsedSections.add(key);
        head.setAttribute('aria-expanded', String(opening));
        const full = bodyEl.scrollHeight;
        const dur = Shell.reducedMotion() ? 0 : (opening ? 250 : 167);
        bodyEl.style.overflow = 'hidden';
        if (opening) s.classList.remove('collapsed');
        const a = bodyEl.animate(opening
          ? [{ height: '0px', opacity: 0 }, { height: `${bodyEl.scrollHeight || full}px`, opacity: 1 }]
          : [{ height: `${full}px`, opacity: 1 }, { height: '0px', opacity: 0 }],
        { duration: dur, easing: opening ? Shell.ease.in : Shell.ease.out, fill: 'forwards' });
        a.finished.catch(() => {}).then(() => {
          if (!opening) s.classList.add('collapsed');
          bodyEl.style.overflow = '';
          a.cancel();
        });
      });
      s.append(head, bodyEl);
      return s;
    }
    function homeView() {
      const wrap = h('div', 'ex-page ex-home');
      const qa = h('div', 'ex-qa-grid');
      pins.map(get).filter((it) => it && it.parent !== 'recycle' && !inRecycle(it.id)).forEach((it) => {
        const tile = itemShell(it, 'ex-qa');
        const text = h('div', 'ex-qa-text');
        const sub = h('div', 'ex-qa-sub');
        sub.append(h('span', null, { text: 'Stored locally' }), ico('fluent/pin', 12, 'ex-qa-pin'));
        text.append(h('span', 'ex-name', { text: it.name }), sub);
        tile.append(itemIcon(it, 48), text);
        qa.append(tile);
      });
      const recent = walk('root').filter((it) => it.kind === 'file').sort((a, b) => (b.modified || 0) - (a.modified || 0)).slice(0, 12);
      wrap.append(
        section('qa', 'Quick access', qa.children.length ? qa : note('Pin folders to Quick access and they will show here.')),
        section('fav', 'Favorites', note("After you've favorited some files, we'll show them here.")),
        section('recent', 'Recent', detailsView(recent, COLSETS.recent, { sortable: false, emptyText: "After you've opened some files, we'll show the most recent ones here." })),
      );
      return wrap;
    }
    function thisPcView() {
      const wrap = h('div', 'ex-page ex-thispc');
      const grid = h('div', 'ex-drive-grid');
      Shell.fs.children('root').filter((it) => it.kind === 'drive').forEach((it) => {
        const used = it.size ? (it.size - (it.free || 0)) / it.size : 0;
        const tile = itemShell(it, 'ex-drive');
        const text = h('div', 'ex-drive-text');
        const bar = h('div', `ex-bar${used > 0.9 ? ' full' : ''}`, { role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': String(Math.round(used * 100)) });
        const fill = h('div', 'ex-bar-fill');
        fill.style.width = `${(used * 100).toFixed(1)}%`;
        bar.append(fill);
        text.append(h('span', 'ex-name', { text: it.name }), bar,
          h('span', 'ex-meta', { text: `${Shell.fs.formatSize(it.free || 0)} free of ${Shell.fs.formatSize(it.size || 0)}` }));
        tile.append(itemIcon(it, 48), text);
        grid.append(tile);
      });
      wrap.append(section('drives', 'Devices and drives', grid));
      return wrap;
    }
    function galleryView() {
      const wrap = h('div', 'ex-page ex-gallery');
      const items = walk('root').filter(isImage).sort((a, b) => (b.modified || 0) - (a.modified || 0));
      if (!items.length) { wrap.append(note('This folder is empty.')); return wrap; }
      const month = new Intl.DateTimeFormat('en-US', { month: 'long', year: 'numeric' });
      let grid = null;
      let last = '';
      items.forEach((it) => {
        const m = month.format(it.modified || Date.now());
        if (m !== last) {
          last = m;
          grid = h('div', 'ex-photos');
          wrap.append(h('div', 'ex-gallery-head', { text: m }), grid);
        }
        const tile = itemShell(it, 'ex-photo');
        tile.title = displayName(it);
        const th = h('span', 'ex-photo-img');
        th.style.background = thumbCss(it);
        tile.append(th);
        grid.append(tile);
      });
      return wrap;
    }
    /* Network discovery is off on this PC, so Explorer shows its info bar and no devices. */
    function networkView() {
      const wrap = h('div', 'ex-page ex-network');
      const bar = h('div', 'ex-infobar', { role: 'status' });
      bar.append(ico('fluent/info', 16, 'ex-infobar-icon'),
        h('span', null, { text: 'Network discovery is turned off. Network computers and devices are not visible.' }));
      wrap.append(bar);
      return wrap;
    }
    function folderView(loc) {
      const t = tab();
      const recycle = loc === 'recycle';
      let items = Shell.fs.children(loc);
      if (t.filter) items = items.filter(FILTERS[t.filter].test);
      const cols = recycle ? COLSETS.recycle : COLSETS.folder;
      const sort = sortOf(loc, cols);
      items = sortItems(items, sort.key, sort.dir);
      const emptyText = t.filter ? 'No items match your filter.' : 'This folder is empty.';
      const view = viewOf(loc);
      return view === 'details' ? detailsView(items, cols, { sort, emptyText }) : iconsView(items, view, emptyText);
    }
    function resultsView() {
      const t = tab();
      let items = searchResults(t.loc, t.search);
      if (t.filter) items = items.filter(FILTERS[t.filter].test);
      const sort = sortOf(t.loc, COLSETS.search);
      return detailsView(sortItems(items, sort.key, sort.dir), COLSETS.search, { sort, emptyText: 'No items match your search.' });
    }
    function renderContent(animate) {
      const t = tab();
      const loc = t.loc;
      const scroll = content.scrollTop;
      let node;
      let view = 'page';
      if (t.search) { node = resultsView(); view = 'details'; } else if (loc === 'home') node = homeView();
      else if (loc === 'gallery') node = galleryView();
      else if (loc === 'root') node = thisPcView();
      else if (loc === 'network') node = networkView();
      else { node = folderView(loc); view = viewOf(loc); }
      content.dataset.view = view;
      content.replaceChildren(node);
      t.items = itemEls().map((e) => e.dataset.id);
      t.sel = new Set([...t.sel].filter((id) => t.items.includes(id)));
      if (t.focusId && !t.items.includes(t.focusId)) t.focusId = null;
      if (S.pending) {
        const ids = S.pending.select.filter((id) => t.items.includes(id));
        if (ids.length) {
          const p = S.pending;
          S.pending = null;
          t.sel = new Set(ids);
          t.anchor = t.focusId = ids[0];
          applySelection();
          scrollToFocus();
          if (p.rename) startRename(p.rename);
          return;
        }
      }
      if (!animate) content.scrollTop = scroll;
      applySelection();
      if (animate && !Shell.reducedMotion()) {
        node.animate([{ opacity: 0, transform: 'translateY(6px)' }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
      }
    }

    /* ----- selection ----- */
    const itemEls = () => [...content.querySelectorAll('.ex-item')];
    function applySelection() {
      const t = tab();
      itemEls().forEach((el) => {
        const on = t.sel.has(el.dataset.id);
        el.classList.toggle('selected', on);
        el.setAttribute('aria-selected', String(on));
        el.classList.toggle('focused', el.dataset.id === t.focusId);
      });
      content.classList.toggle('ex-kb', S.kb);
      renderStatus();
      updateCommands();
      renderDetailsPane();
    }
    function selectOnly(id) {
      const t = tab();
      t.sel = new Set([id]);
      t.anchor = t.focusId = id;
    }
    function selectRange(id, add) {
      const t = tab();
      const ids = itemEls().map((e) => e.dataset.id);
      const a = ids.indexOf(t.anchor);
      const b = ids.indexOf(id);
      if (!add) t.sel = new Set();
      const lo = a < 0 ? b : Math.min(a, b);
      const hi = a < 0 ? b : Math.max(a, b);
      for (let i = lo; i <= hi; i++) t.sel.add(ids[i]);
      t.focusId = id;
    }
    function scrollToFocus() {
      const el = content.querySelector(`.ex-item[data-id="${CSS.escape(tab().focusId || '')}"]`);
      if (!el) return;
      const head = content.querySelector('.ex-dv-head');
      const top = el.offsetTop - (head ? head.offsetHeight : 0);
      const cr = content.getBoundingClientRect();
      const r = el.getBoundingClientRect();
      if (r.top < cr.top + (head ? head.offsetHeight : 0)) content.scrollTop = Math.max(0, top - 4);
      else if (r.bottom > cr.bottom) content.scrollTop += r.bottom - cr.bottom + 4;
    }
    function moveFocus(key, e) {
      const els = itemEls().filter((x) => x.offsetParent !== null);
      if (!els.length) return;
      const t = tab();
      const cur = els.find((x) => x.dataset.id === t.focusId);
      let next = null;
      if (!cur || key === 'Home') next = els[0];
      else if (key === 'End') next = els[els.length - 1];
      else if (key === 'PageDown' || key === 'PageUp') {
        const r = cur.getBoundingClientRect();
        const dy = (content.clientHeight - 40) * (key === 'PageDown' ? 1 : -1);
        next = spatialAt(els, r.left + r.width / 2, r.top + dy) || (key === 'PageDown' ? els[els.length - 1] : els[0]);
      } else {
        const i = els.indexOf(cur);
        const rowLike = cur.classList.contains('ex-row');
        const list = content.dataset.view === 'list';
        const vertical = key === 'ArrowDown' || key === 'ArrowUp';
        const fwd = key === 'ArrowDown' || key === 'ArrowRight';
        if (vertical) next = rowLike || list ? els[i + (fwd ? 1 : -1)] : spatial(cur, els, key);
        else if (rowLike) return;
        else next = list ? spatial(cur, els, key) : els[i + (fwd ? 1 : -1)];
      }
      if (!next) return;
      const id = next.dataset.id;
      if (e.shiftKey) selectRange(id, e.ctrlKey);
      else if (e.ctrlKey) t.focusId = id;
      else selectOnly(id);
      S.kb = true;
      applySelection();
      scrollToFocus();
    }
    function spatial(cur, els, key) {
      const r = cur.getBoundingClientRect();
      const vertical = key === 'ArrowDown' || key === 'ArrowUp';
      const sign = key === 'ArrowDown' || key === 'ArrowRight' ? 1 : -1;
      let best = null;
      let bp = Infinity;
      let bs = Infinity;
      for (const el of els) {
        if (el === cur) continue;
        const q = el.getBoundingClientRect();
        const p = (vertical ? q.top - r.top : q.left - r.left) * sign;
        if (p < 4) continue;
        const s = vertical ? Math.abs(q.left - r.left) : Math.abs(q.top - r.top);
        if (p < bp - 4 || (Math.abs(p - bp) <= 4 && s < bs)) { best = el; bp = p; bs = s; }
      }
      return best;
    }
    function spatialAt(els, x, y) {
      let best = null;
      let bd = Infinity;
      els.forEach((el) => {
        const q = el.getBoundingClientRect();
        const d = Math.abs(q.top + q.height / 2 - y) * 4 + Math.abs(q.left + q.width / 2 - x);
        if (d < bd) { bd = d; best = el; }
      });
      return best;
    }
    function typeAhead(ch) {
      clearTimeout(S.typeTimer);
      S.typeBuf += ch.toLowerCase();
      S.typeTimer = setTimeout(() => { S.typeBuf = ''; }, 1000);
      const t = tab();
      const ids = itemEls().filter((x) => x.offsetParent !== null).map((x) => x.dataset.id);
      if (!ids.length) return;
      const repeat = [...S.typeBuf].every((c) => c === S.typeBuf[0]);
      const q = repeat ? S.typeBuf[0] : S.typeBuf;
      const start = Math.max(0, ids.indexOf(t.focusId));
      const offset = repeat ? 1 : 0;
      for (let k = 0; k < ids.length; k++) {
        const id = ids[(start + offset + k) % ids.length];
        const it = get(id);
        if (it && displayName(it).toLowerCase().startsWith(q)) {
          selectOnly(id);
          S.kb = true;
          applySelection();
          scrollToFocus();
          return;
        }
      }
    }

    /* ----- status bar and details pane ----- */
    function renderStatus() {
      const t = tab();
      const sel = selItems();
      const sep = () => h('span', 'ex-status-sep', { 'aria-hidden': 'true' });
      statusText.replaceChildren(h('span', null, { text: plural(t.items.length, 'item') }), sep());
      if (sel.length) {
        statusText.append(h('span', null, { text: `${plural(sel.length, 'item')} selected` }));
        if (sel.every((it) => it.kind === 'file')) statusText.append(h('span', 'ex-status-size', { text: Shell.fs.formatSize(sel.reduce((a, it) => a + (it.size || 0), 0)) }));
        statusText.append(sep());
      }
    }
    function renderDetailsPane() {
      dpane.hidden = !layout.details;
      if (!layout.details) return;
      const t = tab();
      const sel = selItems();
      const headEl = h('div', 'ex-dp-head');
      const rows = [];
      if (sel.length === 1) {
        const it = sel[0];
        if (isImage(it)) {
          const th = h('span', 'ex-dp-thumb');
          th.style.background = thumbCss(it);
          headEl.append(th);
        } else headEl.append(itemIcon(it, 96));
        headEl.append(h('div', 'ex-dp-name', { text: displayName(it) }), h('div', 'ex-dp-type', { text: typeOf(it) }));
        if (it.kind === 'drive') rows.push(['Total size', Shell.fs.formatSize(it.size)], ['Free space', Shell.fs.formatSize(it.free)]);
        if (it.kind === 'file') rows.push(['Size', Shell.fs.formatSize(it.size || 0)]);
        if (it.kind === 'folder') rows.push(['Contains', plural(Shell.fs.children(it.id).length, 'item')]);
        if (it.parent === 'recycle') rows.push(['Original location', osPath(it.deletedFrom)], ['Date deleted', fmtDate(it.modified)]);
        else rows.push(['Date modified', fmtDate(it.modified)], ['Path', osPath(it.id)]);
      } else if (sel.length > 1) {
        headEl.append(itemIcon(sel[0], 96), h('div', 'ex-dp-name', { text: `${sel.length} items selected` }));
        const size = sel.reduce((a, it) => a + (it.kind === 'file' ? it.size || 0 : 0), 0);
        if (size) rows.push(['Total file size', Shell.fs.formatSize(size)]);
      } else {
        headEl.append(ico(locIcon(t.loc), 96, 'ex-dp-loc'), h('div', 'ex-dp-name', { text: tabTitle(t) }));
        headEl.append(h('div', 'ex-dp-hint', { text: 'Select a single file to get more information and share your cloud content.' }));
      }
      const list = h('dl', 'ex-dp-props');
      rows.forEach(([k, v]) => list.append(h('dt', null, { text: k }), h('dd', null, { text: v })));
      dpane.replaceChildren(headEl);
      if (rows.length) dpane.append(h('div', 'ex-dp-title', { text: 'Details' }), list);
    }
    statusDetails.addEventListener('click', () => setFolderView(tab().loc, { view: 'details' }));
    statusLarge.addEventListener('click', () => setFolderView(tab().loc, { view: 'large' }));

    /* ----- whole-window render ----- */
    function updateTitle() {
      const t = tab();
      root.dataset.loc = t.loc;
      S.win?.setTitle?.(`${tabTitle(t)} - File Explorer`);
    }
    function renderAll(animate) {
      applyLayout();
      renderTabs();
      renderAddress();
      renderNav();
      renderContent(animate);
      updateTitle();
    }

    /* ----- inline rename ----- */
    const BAD_CHARS = /[\\/:*?"<>|]/g;
    function startRename(id) {
      endRename(true);
      const it = get(id);
      const el = content.querySelector(`.ex-item[data-id="${CSS.escape(id)}"]`);
      if (!renamable(it) || !el) return;
      const nameEl = el.querySelector('.ex-name');
      if (!nameEl) return;
      const shown = displayName(it);
      const hiddenExt = it.name.slice(shown.length);
      const input = h('input', 'ex-rename', { type: 'text', spellcheck: 'false', autocomplete: 'off', 'aria-label': 'Name' });
      input.value = shown;
      nameEl.hidden = true;
      nameEl.after(input);
      el.classList.add('renaming');
      input.focus();
      const dot = shown.lastIndexOf('.');
      input.setSelectionRange(0, it.kind === 'file' && dot > 0 ? dot : shown.length);
      const finish = (commit) => {
        if (S.renaming !== state) return;
        S.renaming = null;
        Shell.tooltip?.hide?.();
        const v = input.value.trim();
        input.remove();
        nameEl.hidden = false;
        el.classList.remove('renaming');
        if (commit && v && v + hiddenExt !== it.name) Shell.fs.rename(id, v + hiddenExt);
      };
      const state = { finish };
      S.renaming = state;
      input.addEventListener('keydown', (e) => {
        e.stopPropagation();
        if (e.key === 'Enter') { e.preventDefault(); finish(true); content.focus({ preventScroll: true }); }
        if (e.key === 'Escape') { e.preventDefault(); finish(false); content.focus({ preventScroll: true }); }
      });
      input.addEventListener('input', () => {
        if (!BAD_CHARS.test(input.value)) return;
        const pos = input.selectionStart - (input.value.slice(0, input.selectionStart).match(BAD_CHARS) || []).length;
        input.value = input.value.replace(BAD_CHARS, '');
        input.setSelectionRange(pos, pos);
        Shell.tooltip?.show?.(input, 'A file name can\'t contain any of the following characters:\n\\ / : * ? " < > |', { placement: 'bottom' });
      });
      input.addEventListener('pointerdown', (e) => e.stopPropagation());
      input.addEventListener('dblclick', (e) => e.stopPropagation());
      input.addEventListener('blur', () => finish(true));
    }
    function endRename(commit) { S.renaming?.finish(commit); }

    /* ----- pointer: selection, rubber band, drag and drop ----- */
    content.addEventListener('pointerdown', (e) => {
      clearTimeout(S.renameTimer);
      if (e.button !== 0 || e.target.closest('.ex-rename, .ex-section-head')) return;
      if (e.target.closest('.ex-dv-head')) { headerPointer(e); return; }
      if (e.target === content && (e.offsetX >= content.clientWidth || e.offsetY >= content.clientHeight)) return; /* scroll bar */
      content.focus({ preventScroll: true });
      S.kb = false;
      const t = tab();
      const itemEl = e.target.closest('.ex-item');
      if (!itemEl) { startMarquee(e); return; }
      const id = itemEl.dataset.id;
      const info = { id, wasSel: t.sel.has(id), wasOnly: t.sel.has(id) && t.sel.size === 1, ctrl: e.ctrlKey, shift: e.shiftKey };
      if (e.shiftKey) selectRange(id, e.ctrlKey);
      else if (!e.ctrlKey && !info.wasSel) selectOnly(id);
      else t.focusId = id;
      if (!e.shiftKey) t.anchor = e.ctrlKey ? t.anchor ?? id : id;
      applySelection();
      trackItemPointer(e, info);
    });
    function trackItemPointer(e, info) {
      const sx = e.clientX;
      const sy = e.clientY;
      const pid = e.pointerId;
      let drag = null;
      const move = (ev) => {
        if (ev.pointerId !== pid) return;
        if (!drag) {
          if (Math.hypot(ev.clientX - sx, ev.clientY - sy) < 4) return; /* SM_CXDRAG, as the marquee */
          drag = startDrag(info.id);
          if (!drag) { done(); return; }
        }
        dragMove(drag, ev);
      };
      const up = (ev) => {
        if (ev.pointerId !== pid) return;
        done();
        if (drag) { dragEnd(drag, ev); return; }
        const t = tab();
        if (info.ctrl && !info.shift) {
          if (t.sel.has(info.id) && info.wasSel) t.sel.delete(info.id); else t.sel.add(info.id);
          applySelection();
        } else if (!info.shift && info.wasSel) {
          if (t.sel.size > 1) { selectOnly(info.id); applySelection(); } else if (info.wasOnly && ev.target.closest?.('.ex-name')) {
            S.renameTimer = setTimeout(() => startRename(info.id), 500); /* slow second click */
          }
        }
      };
      const key = (ev) => {
        if (!drag) return;
        if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); cancelDrag(drag); drag = null; done(); } else if (ev.key === 'Control' && drag.last) dragMove(drag, { clientX: drag.last.clientX, clientY: drag.last.clientY, ctrlKey: ev.type === 'keydown' });
      };
      const done = () => {
        window.removeEventListener('pointermove', move, true);
        window.removeEventListener('pointerup', up, true);
        window.removeEventListener('pointercancel', up, true);
        window.removeEventListener('keydown', key, true);
        window.removeEventListener('keyup', key, true);
        S.pointerCleanup = null;
      };
      window.addEventListener('pointermove', move, true);
      window.addEventListener('pointerup', up, true);
      window.addEventListener('pointercancel', up, true);
      window.addEventListener('keydown', key, true);
      window.addEventListener('keyup', key, true);
      S.pointerCleanup = () => { if (drag) cancelDrag(drag); done(); };
    }
    function internalTarget(node) {
      const zone = node.closest('[data-drop]');
      if (zone && (root.contains(zone) || tabsEl.contains(zone))) return { loc: zone.dataset.drop, zone };
      if (content.contains(node) && !tab().search) return { loc: tab().loc, zone: null };
      return null;
    }
    function startDrag(id) {
      const t = tab();
      if (!t.sel.has(id)) { t.sel.add(id); applySelection(); }
      const ids = [...t.sel].filter((x) => movable(get(x)));
      if (!ids.length) return null;
      clearTimeout(S.renameTimer);
      const ghost = h('div', 'ex-ghost');
      const plate = h('div', 'ex-ghost-plate');
      plate.append(itemIcon(get(ids[0]), 48));
      if (ids.length > 1) plate.append(h('span', 'ex-ghost-count', { text: String(ids.length) }));
      const tip = h('div', 'ex-ghost-tip drop-desc', { hidden: true }); // drop description style: css/cursors.css
      ghost.append(plate, tip);
      (Shell.layer('tooltips') || document.body).append(ghost);
      Shell.tooltip?.hide?.();
      root.classList.add('ex-dragging');
      return { ids, ghost, tip, zone: null, target: null, ext: false, last: null };
    }
    function setDropZone(d, zone) {
      if (d.zone === zone) return;
      d.zone?.classList.remove('drop-target');
      d.zone = zone;
      zone?.classList.add('drop-target');
    }
    function externalLabel(top, copy) {
      const verb = copy ? 'Copy' : 'Move';
      const other = top.closest('.ex');
      if (other) {
        const zone = top.closest('[data-drop]');
        const loc = zone && other.contains(zone) ? zone.dataset.drop : other.dataset.loc;
        return dropVerb(loc, copy);
      }
      if (Shell.layer('desktop')?.contains(top)) {
        const id = top.closest('[data-id]')?.dataset.id;
        if (id === 'recycle') return 'Move to Recycle Bin';
        if (id && isContainer(get(id))) return `${verb} to ${nameOf(id)}`;
        return `${verb} to Desktop`;
      }
      return verb;
    }
    function dragMove(d, ev) {
      d.last = { clientX: ev.clientX, clientY: ev.clientY };
      Shell.tooltip?.hide?.(); /* hover tooltips stay quiet while dragging */
      const copy = !!ev.ctrlKey;
      d.ghost.style.transform = `translate(${ev.clientX}px, ${ev.clientY}px)`;
      const top = document.elementFromPoint(ev.clientX, ev.clientY);
      let label = null;
      setDropZone(d, null);
      d.target = null;
      if (top && S.winEl.contains(top)) {
        if (d.ext) { Shell.drag.hover(-1e5, -1e5, d.ids); d.ext = false; }
        const tgt = internalTarget(top);
        if (tgt && validDrop(tgt.loc, d.ids, copy)) {
          d.target = tgt.loc;
          label = dropVerb(tgt.loc, copy);
          if (tgt.zone) setDropZone(d, tgt.zone);
        }
      } else if (top) {
        d.ext = !!Shell.drag.hover(ev.clientX, ev.clientY, d.ids, S.winEl);
        if (d.ext) label = externalLabel(top, copy);
      }
      d.tip.hidden = !label;
      d.ghost.classList.toggle('no-drop', !label);
      document.documentElement.classList.toggle('drag-nodrop', !label && !(top && S.winEl.contains(top))); // the unavailable cursor outside any target
      if (label) {
        const verbIcon = copy && !label.includes('Recycle') ? 'fluent/add' : 'fluent/arrow-right';
        if (d.tip.dataset.label !== label) {
          d.tip.dataset.label = label;
          const to = / to (.+)$/.exec(label); // the destination is the drop description's accent insert
          d.tip.replaceChildren(ico(verbIcon, 16, 'ex-ghost-verb drop-desc-icon'), h('span', null, { text: to ? `${label.slice(0, to.index)} to ` : label }),
            to && h('span', 'drop-desc-insert', { text: to[1] }) || '');
        }
      }
    }
    function endDragVisuals(d) {
      setDropZone(d, null);
      d.ghost.remove();
      root.classList.remove('ex-dragging');
      document.documentElement.classList.remove('drag-nodrop');
    }
    function cancelDrag(d) {
      if (d.ext) Shell.drag.hover(-1e5, -1e5, d.ids);
      endDragVisuals(d);
    }
    function dragEnd(d, ev) {
      endDragVisuals(d);
      const copy = !!ev.ctrlKey;
      if (d.target && validDrop(d.target, d.ids, copy)) dropInto(d.ids, d.target, copy);
      else if (d.ext) Shell.drag.drop(ev.clientX, ev.clientY, d.ids, { exclude: S.winEl, copy });
    }

    /* Rubber-band selection from empty space. */
    function startMarquee(e) {
      const t = tab();
      const base = e.ctrlKey ? new Set(t.sel) : new Set();
      if (!e.ctrlKey && !e.shiftKey) { t.sel = new Set(); t.focusId = null; applySelection(); }
      const origin = () => content.getBoundingClientRect();
      let cr = origin();
      const sx = e.clientX - cr.left + content.scrollLeft;
      const sy = e.clientY - cr.top + content.scrollTop;
      const rects = itemEls().filter((x) => x.offsetParent !== null).map((el) => {
        const r = el.getBoundingClientRect();
        return { id: el.dataset.id, l: r.left - cr.left + content.scrollLeft, t: r.top - cr.top + content.scrollTop, r: r.right - cr.left + content.scrollLeft, b: r.bottom - cr.top + content.scrollTop };
      });
      let box = null;
      const pid = e.pointerId;
      let last = { x: e.clientX, y: e.clientY };
      const update = () => {
        cr = origin();
        const x = Math.max(0, Math.min(content.scrollWidth, last.x - cr.left + content.scrollLeft));
        const y = Math.max(0, Math.min(content.scrollHeight, last.y - cr.top + content.scrollTop));
        const l = Math.min(sx, x), tp = Math.min(sy, y), w = Math.abs(x - sx), hh = Math.abs(y - sy);
        Object.assign(box.style, { left: `${l}px`, top: `${tp}px`, width: `${w}px`, height: `${hh}px` });
        const sel = new Set(base);
        rects.forEach((q) => {
          if (q.r > l && q.l < l + w && q.b > tp && q.t < tp + hh) { if (base.has(q.id)) sel.delete(q.id); else sel.add(q.id); }
        });
        t.sel = sel;
        applySelection();
      };
      const move = (ev) => {
        if (ev.pointerId !== pid) return;
        last = { x: ev.clientX, y: ev.clientY };
        if (!box) {
          if (Math.hypot(ev.clientX - e.clientX, ev.clientY - e.clientY) < 4) return;
          box = h('div', 'ex-marquee');
          content.append(box);
          root.classList.add('ex-marqueeing');
        }
        /* auto-scroll while the band leaves the view */
        const r = content.getBoundingClientRect();
        if (ev.clientY > r.bottom - 4) content.scrollTop += Math.min(24, ev.clientY - r.bottom + 8);
        else if (ev.clientY < r.top + 4) content.scrollTop -= Math.min(24, r.top - ev.clientY + 8);
        update();
      };
      const up = (ev) => {
        if (ev.pointerId !== pid) return;
        window.removeEventListener('pointermove', move, true);
        window.removeEventListener('pointerup', up, true);
        window.removeEventListener('pointercancel', up, true);
        S.pointerCleanup = null;
        box?.remove();
        root.classList.remove('ex-marqueeing');
      };
      window.addEventListener('pointermove', move, true);
      window.addEventListener('pointerup', up, true);
      window.addEventListener('pointercancel', up, true);
      S.pointerCleanup = () => up({ pointerId: pid });
    }

    /* Column headers: click sorts, the grip between headers resizes. */
    function headerPointer(e) {
      const grip = e.target.closest('.ex-dv-grip');
      const cell = e.target.closest('.ex-dv-hcell');
      if (!cell) return;
      const wrap = cell.closest('.ex-dv');
      if (grip) {
        e.preventDefault();
        const k = grip.dataset.grip;
        const start = colW[k];
        const sx = e.clientX;
        grip.setPointerCapture(e.pointerId);
        const cols = [...wrap.querySelectorAll('.ex-dv-hcell')].map((c) => c.dataset.col);
        const move = (ev) => {
          colW[k] = Math.max(50, Math.round(start + ev.clientX - sx));
          wrap.style.setProperty('--cols', cols.map((c) => `${colW[c]}px`).join(' '));
        };
        const up = () => { grip.removeEventListener('pointermove', move); grip.removeEventListener('pointerup', up); grip.removeEventListener('pointercancel', up); };
        grip.addEventListener('pointermove', move);
        grip.addEventListener('pointerup', up);
        grip.addEventListener('pointercancel', up);
        return;
      }
      if (cell.classList.contains('static')) return;
      cell.classList.add('pressed');
      const release = (ev) => {
        cell.classList.remove('pressed');
        window.removeEventListener('pointerup', release, true);
        if (!cell.contains(document.elementFromPoint(ev.clientX, ev.clientY))) return;
        const t = tab();
        const k = cell.dataset.col;
        const cols = t.search ? COLSETS.search : t.loc === 'recycle' ? COLSETS.recycle : COLSETS.folder;
        const s = sortOf(t.loc, cols);
        setFolderView(t.loc, s.key === k ? { sort: k, dir: -s.dir } : { sort: k, dir: k === 'modified' || k === 'deleted' ? -1 : 1 });
      };
      window.addEventListener('pointerup', release, true);
    }

    content.addEventListener('dblclick', (e) => {
      const el = e.target.closest('.ex-item');
      if (!el || e.target.closest('.ex-rename')) return;
      clearTimeout(S.renameTimer);
      openItem(get(el.dataset.id));
    });
    content.addEventListener('auxclick', (e) => {
      const it = get(e.target.closest('.ex-item')?.dataset.id);
      if (e.button === 1 && isContainer(it)) addTab(it.id, false);
    });
    content.addEventListener('mousedown', (e) => { if (e.button === 1) e.preventDefault(); });
    content.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      if (e.target.closest('.ex-rename, .ex-dv-head')) return;
      const el = e.target.closest('.ex-item');
      const t = tab();
      content.focus({ preventScroll: true });
      if (el) {
        if (!t.sel.has(el.dataset.id)) selectOnly(el.dataset.id); else t.focusId = el.dataset.id;
        applySelection();
        itemMenu(e.clientX, e.clientY);
      } else {
        t.sel = new Set();
        applySelection();
        backgroundMenu(e.clientX, e.clientY);
      }
    });
    function itemMenu(x, y, anchorRect) {
      const sel = selItems();
      if (!sel.length) return;
      const one = sel.length === 1 ? sel[0] : null;
      const allMovable = sel.every(movable);
      const items = [];
      if (sel.every((it) => it.parent === 'recycle')) {
        items.push(
          { label: 'Restore', icon: 'fluent/history', action: restoreItems },
          { separator: true },
          { label: 'Cut', icon: 'fluent/cut', shortcut: 'Ctrl+X', action: () => clip(true) },
          { label: 'Delete', icon: 'fluent/delete', shortcut: 'Delete', action: deleteSelected },
          { separator: true },
          { label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter', action: showProperties },
        );
      } else {
        items.push({ commandBar: [
          { icon: 'fluent/cut', label: 'Cut', disabled: !allMovable, action: () => clip(true) },
          { icon: 'fluent/copy', label: 'Copy', disabled: !allMovable, action: () => clip(false) },
          { icon: 'fluent/rename', label: 'Rename', disabled: !(one && renamable(one)), action: () => startRename(one.id) },
          { icon: 'fluent/share', label: 'Share', disabled: true, action() {} },
          { icon: 'fluent/delete', label: 'Delete', disabled: !allMovable, action: deleteSelected },
        ] });
        items.push({ label: 'Open', icon: one && !isContainer(one) ? iconOf(one) : 'fluent/folder-open', shortcut: 'Enter', action: openSelected });
        if (sel.every(isContainer)) {
          items.push(
            { label: 'Open in new tab', icon: 'fluent/tab-desktop', action: () => sel.forEach((it) => addTab(it.id, false)) },
            { label: 'Open in new window', icon: 'fluent/window-new', action: () => sel.forEach((it) => Shell.apps.launch('explorer', { folder: it.id })) },
          );
        }
        const another = { label: 'Choose another app', disabled: !canOpenWith(one), action: () => openWith(one) };
        if (one && one.ext === 'txt') items.push({ label: 'Open with', icon: 'fluent/apps', submenu: [{ label: 'Notepad', icon: 'object/app-notepad', action: () => Shell.apps.launch('notepad', { file: one.id }) }, { separator: true }, another] });
        else if (canOpenWith(one)) items.push({ label: 'Open with...', icon: 'fluent/apps', action: () => openWith(one) }); /* no associated app */
        if (one && isContainer(one) && one.kind !== 'system') {
          const pinned = pins.includes(one.id);
          items.push({ label: pinned ? 'Unpin from Quick access' : 'Pin to Quick access', icon: pinned ? 'fluent/pin-off' : 'fluent/pin', action: () => togglePin(one.id) });
        }
        if (allMovable && canCreate(tab().loc)) items.push({ label: 'Compress to ZIP file', icon: 'fluent/folder', action: compressSelected });
        items.push(
          { label: 'Copy as path', icon: 'fluent/document', shortcut: 'Ctrl+Shift+C', action: copyPath },
          { label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter', action: showProperties },
        );
        if (one && canCreate(one.id)) items.push({ separator: true }, { label: 'Open in Terminal', icon: 'object/app-terminal', action: () => Shell.apps.launch('terminal', { cwd: osPath(one.id) }) });
        items.push({ separator: true }, { label: 'Show more options', icon: 'fluent/open', shortcut: 'Shift+F10', action() {} });
      }
      menu({ x, y, items, anchorRect });
    }
    function backgroundMenu(x, y, anchorRect) {
      const t = tab();
      const loc = t.loc;
      const items = [];
      if (clipNow() && canCreate(loc) && !t.search) items.push({ commandBar: [{ icon: 'fluent/clipboard-paste', label: 'Paste', action: paste }] });
      const special = SPECIAL.includes(loc);
      items.push(
        { label: 'View', icon: viewInfo(special ? 'details' : viewOf(loc))[2], submenu: viewMenuItems() },
        { label: 'Sort by', icon: 'fluent/arrow-sort', submenu: sortMenuItems() },
      );
      if (loc === 'recycle') items.push({ separator: true }, { label: 'Empty Recycle Bin', icon: 'fluent/delete', disabled: !Shell.fs.children('recycle').length, action: emptyRecycleBin });
      if (canCreate(loc) && !t.search) items.push({ separator: true }, { label: 'New', icon: 'fluent/add', submenu: newMenuItems() });
      items.push({ separator: true }, { label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter', action: showProperties });
      if (canCreate(loc)) items.push({ separator: true }, { label: 'Open in Terminal', icon: 'object/app-terminal', action: () => Shell.apps.launch('terminal', { cwd: osPath(loc) }) });
      items.push({ separator: true }, { label: 'Show more options', icon: 'fluent/open', shortcut: 'Shift+F10', action() {} });
      menu({ x, y, items, anchorRect });
    }

    /* ----- keyboard ----- */
    const WINDOW_KEYS = {
      'Ctrl+T': () => addTab('home'),
      'Ctrl+W': () => closeTab(S.active),
      'Ctrl+Tab': () => selectTab((S.active + 1) % S.tabs.length),
      'Ctrl+Shift+Tab': () => selectTab((S.active + S.tabs.length - 1) % S.tabs.length),
      'Ctrl+N': () => Shell.apps.launch('explorer', { folder: tab().loc }),
      'Alt+ArrowLeft': () => go(-1),
      'Alt+ArrowRight': () => go(1),
      'Alt+ArrowUp': goUp,
      F5: refresh,
      'Ctrl+R': refresh,
      'Ctrl+L': enterEdit,
      'Alt+D': enterEdit,
      F4: enterEdit,
      'Ctrl+F': focusSearch,
      'Ctrl+E': focusSearch,
      F3: focusSearch,
      'Ctrl+Shift+N': () => newItem('folder'),
      'Alt+Shift+P': () => setLayout('details', !layout.details),
    };
    /* Ctrl+Shift+1..6 pick a view by physical key, since Shift changes the character. */
    const VIEW_DIGITS = Object.fromEntries(VIEWS.map((v) => [`Digit${v[4].slice(-1)}`, v[0]]));
    function contentKey(e, combo) {
      const k = e.key;
      if (['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown'].includes(k) && !e.altKey) { moveFocus(k, e); return true; }
      const t = tab();
      switch (combo) {
        case 'Ctrl+A': selectAll(); return true;
        case 'Ctrl+C': clip(false); return true;
        case 'Ctrl+X': clip(true); return true;
        case 'Ctrl+V': paste(); return true;
        case 'Ctrl+Shift+C': copyPath(); return true;
        case 'Delete': deleteSelected(); return true;
        case 'Shift+Delete': deleteSelected(true); return true;
        case 'F2': renameSelected(); return true;
        case 'Enter': openSelected(); return true;
        case 'Alt+Enter': showProperties(); return true;
        case 'Backspace': go(-1); return true;
        case 'Escape': /* Esc after Cut cancels it */
          if (!clipNow()?.cut) return false;
          Shell.clipboard.clear();
          return true;
        case 'Shift+F10':
        case 'ContextMenu': {
          const el = content.querySelector(`.ex-item[data-id="${CSS.escape(t.focusId || '')}"]`);
          const r = (el || content).getBoundingClientRect();
          if (el && t.sel.size) itemMenu(r.left + 24, r.bottom, r); else backgroundMenu(r.left + 24, r.top + 24);
          return true;
        }
        default: break;
      }
      if (k === ' ') {
        if (t.focusId) { if (e.ctrlKey && t.sel.has(t.focusId)) t.sel.delete(t.focusId); else t.sel.add(t.focusId); S.kb = true; applySelection(); }
        return true;
      }
      if (k.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) { typeAhead(k); return true; }
      return false;
    }
    root.addEventListener('keydown', onKeyDown);
    tabsEl.addEventListener('keydown', onKeyDown);
    function onKeyDown(e) {
      const combo = Shell.keys.comboOf(e);
      const act = WINDOW_KEYS[combo];
      if (act) { e.preventDefault(); e.stopPropagation(); act(e); return; }
      if (e.ctrlKey && e.shiftKey && !e.altKey && VIEW_DIGITS[e.code]) {
        e.preventDefault();
        if (!SPECIAL.includes(tab().loc)) setFolderView(tab().loc, { view: VIEW_DIGITS[e.code] });
        return;
      }
      if (e.target.matches?.('input, textarea')) return;
      if (nav.contains(e.target)) { if (navKey(e)) e.preventDefault(); return; }
      if (e.target !== content) return;
      clearTimeout(S.renameTimer);
      if (contentKey(e, combo)) e.preventDefault();
    }

    /* ----- drop target for other surfaces (desktop icons, other Explorer windows) ----- */
    let extZone = null;
    const setExtZone = (zone) => {
      if (extZone === zone) return;
      extZone?.classList.remove('drop-target');
      extZone = zone;
      zone?.classList.add('drop-target');
    };
    const nodeAt = (x, y) => document.elementsFromPoint(x, y).find((n) => root.contains(n)) || null;
    const extMove = (ev) => {
      const node = nodeAt(ev.clientX, ev.clientY);
      const zone = node?.closest('[data-drop]');
      setExtZone(zone && root.contains(zone) && zone.dataset.drop !== tab().loc ? zone : null);
    };
    const endExt = () => {
      window.removeEventListener('pointermove', extMove, true);
      setExtZone(null);
      root.classList.remove('ex-drop-hover');
    };
    cleanups.push(endExt);
    cleanups.push(Shell.drag.registerTarget(root, {
      accepts: () => true,
      onEnter: () => { root.classList.add('ex-drop-hover'); window.addEventListener('pointermove', extMove, true); },
      onLeave: endExt,
      onDrop: (ids, { x, y, copy }) => {
        endExt();
        const node = nodeAt(x, y);
        const tgt = node && internalTarget(node);
        if (tgt && validDrop(tgt.loc, ids, copy)) dropInto(ids, tgt.loc, copy);
      },
    }));

    /* ----- keep in sync with the file system, pins, layout and other windows ----- */
    /* A tab whose folder was deleted or moved to the Recycle Bin falls back to where it was. */
    function fixLocations() {
      S.tabs.forEach((t) => {
        let loc = t.loc;
        for (let guard = 0; guard < 32 && !PSEUDO[loc] && loc !== 'recycle'; guard++) {
          const it = get(loc);
          if (!it) { loc = 'home'; break; }
          if (it.parent === 'recycle') loc = it.deletedFrom || 'home';
          else if (inRecycle(loc)) loc = it.parent;
          else break;
        }
        if (loc !== t.loc) t.history[t.index] = loc;
      });
    }
    const instance = {
      root,
      refresh() {
        if (S.raf || S.closed) return;
        S.raf = requestAnimationFrame(() => {
          S.raf = 0;
          if (S.closed) return;
          fixLocations();
          renderAll(false);
        });
      },
    };
    instances.add(instance);

    const ro = new ResizeObserver(() => { if (!pathInput.hidden) return; fitCrumbs(); });
    ro.observe(address);

    function cleanup() {
      if (S.closed) return;
      S.closed = true;
      instances.delete(instance);
      endRename(false);
      S.pointerCleanup?.();
      cancelAnimationFrame(S.raf);
      clearTimeout(S.typeTimer);
      clearTimeout(S.renameTimer);
      clearTimeout(S.searchTimer);
      ro.disconnect();
      cleanups.forEach((fn) => fn());
    }

    /* ----- open the window ----- */
    const startLoc = args.folder && (PSEUDO[args.folder] || get(args.folder)) ? args.folder : 'home';
    S.tabs.push(makeTab(startLoc));
    if (args.select) { tab().sel.add(args.select); tab().anchor = tab().focusId = args.select; }

    const wa = Shell.wm?.workArea?.() || { x: 0, y: 0, width: window.innerWidth, height: window.innerHeight - 48 };
    const narrow = wa.width < 720 || wa.height < 520;
    const geo = narrow
      ? { x: wa.x, y: wa.y, width: wa.width, height: wa.height }
      : { width: Math.min(1100, wa.width - 64), height: Math.min(700, wa.height - 48) };
    if (Shell.wm?.open) {
      try {
        S.win = Shell.wm.open({
          appId: 'explorer', title: `${nameOf(startLoc)} - File Explorer`, icon: 'object/app-explorer',
          ...geo, minWidth: Math.min(320, wa.width), minHeight: Math.min(300, wa.height),
          content: root, titlebar: 'tall', titlebarContent: tabsEl, background: 'mica-alt',
          onClose: () => { cleanup(); },
        });
      } catch (err) { console.warn('[explorer] window manager failed, using a plain frame', err); }
    }
    if (!S.win) S.win = fallbackWindow(geo, wa, root, tabsEl, cleanup);
    S.winEl = S.win.el || root;
    renderAll(false);
    if (args.select) scrollToFocus();
    /* Only while still the foreground window: a window opened after this one keeps activation. */
    requestAnimationFrame(() => { if (S.win.isFocused?.() ?? true) content.focus({ preventScroll: true }); });
    return S.win;
  }

  /* Without the window manager (it is written in parallel), show the window as a plain frame. */
  function fallbackWindow(geo, wa, root, tabsEl, onClose) {
    const frame = h('div', 'ex-fallback mica-alt');
    const w = geo.width;
    const ht = geo.height;
    Object.assign(frame.style, {
      left: `${geo.x ?? wa.x + Math.round((wa.width - w) / 2)}px`,
      top: `${geo.y ?? wa.y + Math.round((wa.height - ht) / 2)}px`,
      width: `${w}px`, height: `${ht}px`,
    });
    const bar = h('div', 'ex-fallback-bar');
    bar.append(tabsEl);
    frame.append(bar, root);
    (Shell.layer('windows') || document.body).append(frame);
    return {
      el: frame, body: frame,
      setTitle() {}, setIcon() {}, focus() {},
      close() { frame.remove(); onClose(); },
    };
  }

  /* ---------- registration ---------- */
  Shell.apps.register({
    id: 'explorer', name: 'File Explorer', icon: 'object/app-explorer', pinned: true, startPinned: true,
    description: 'Browse the files and folders on this PC',
    open: (args) => createExplorer(args),
  });
  Shell.keys.on('Win+E', () => { Shell.apps.launch('explorer'); });
  Shell.on('fs', () => refreshAll('fs'));
  Shell.on('clipboard', (c) => { lastClip = { ids: [...(c?.ids || [])], cut: !!c?.cut }; refreshAll('clipboard'); });
  Shell.on('boot', () => {
    const want = Shell.debugOpen.find((n) => n === 'explorer' || n.startsWith('explorer:'));
    if (want) setTimeout(() => Shell.apps.launch('explorer', { folder: want.split(':')[1] || 'home' }), 0);
    /* explorer-recycle: a Recycle Bin with a few seed items in it (captures use ?fresh);
       explorer-purge / explorer-purge-many: the same, asking to delete its first item for good / to empty it. */
    const bin = ['explorer-recycle', 'explorer-purge', 'explorer-purge-many'].find((n) => Shell.debugOpen.includes(n));
    if (bin) {
      if (!Shell.fs.children('recycle').length) ['f-doc1', 'f-dl1', 'f-shot2', 'f-doc3'].forEach((id) => get(id) && Shell.fs.remove(id));
      setTimeout(() => {
        const win = Shell.apps.launch('explorer', { folder: 'recycle' });
        if (bin === 'explorer-purge') purge([Shell.fs.children('recycle')[0]?.id], win);
        else if (bin === 'explorer-purge-many') emptyRecycleBinAsk(win);
      }, 0);
    }
    /* explorer-cut: Documents with two items cut, dimmed until pasted or Esc. */
    if (Shell.debugOpen.includes('explorer-cut')) {
      Shell.clipboard.set(['f-doc0', 'f-doc2'].filter(get), { cut: true });
      setTimeout(() => Shell.apps.launch('explorer', { folder: 'documents' }), 0);
    }
  });
})();
