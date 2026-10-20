/* Desktop: the icons on the wallpaper.
   Owns: the desktop items (This PC, Recycle Bin and Shell.fs 'desktop'), their grid positions,
   selection (click, Ctrl/Shift, rubber band, keyboard, type-ahead), icon drag and drop (within
   the desktop, onto folders and the Recycle Bin, and to other surfaces through Shell.drag),
   inline rename, the desktop and item context menus and the Create Shortcut wizard. Cut, Copy and
   Paste go through Shell.clipboard; Properties, Open with and name collisions on paste or drop go
   to Shell.dialogs. API: Shell.desktop.refresh(), Shell.desktop.select(ids).
   Store key: desktop-positions, {id: [column, row]} in cell units (fractions when "Align icons
   to grid" is off), so icon sizes and a smaller screen keep the user's layout. */
(() => {
  'use strict';

  const Shell = window.Shell;

  /* Geometry. --desktop-grid (75 px, Tier B IconSpacing) is the column pitch at small and medium.
     IconVerticalSpacing is counted for the 32 px base icon, so rows grow by the extra icon height
     and two label lines never reach the next row; large icons widen the column to fit. */
  const ICON = { small: 32, medium: 48, large: 96 };
  const ORIGIN = 1;           // px from the work area edge to the first cell; plates sit 1 px inside a cell
  const DRAG_START = 4;       // SM_CXDRAG / SM_CYDRAG
  const SLOW_CLICK = 500;     // GetDoubleClickTime(): a later click on a selected label renames it
  const SYSTEM = ['root', 'recycle'];
  const BAD_CHAR = /[\\/:*?"<>|]/;
  const BAD_CHARS = /[\\/:*?"<>|\r\n]/g;
  const DESKTOP_PATH = 'C:\\Users\\Mihawk\\Desktop';
  const TYPES = { txt: 'Text Document', png: 'PNG File', jpg: 'JPG File', pdf: 'PDF File', zip: 'Compressed (zipped) Folder', exe: 'Application' };

  let root = null, band = null, grid = 75;
  const els = new Map();                // id -> item element
  let cells = new Map();                // id -> [column, row] as laid out now
  const positions = Shell.store.get('desktop-positions', {});
  const sel = new Set();
  let focusId = null, anchorId = null;
  let cutIds = new Set();                // dimmed while Shell.clipboard holds them as a cut
  let renaming = null, renderPending = false;
  let drag = null;
  let menuPoint = null, kbdMenu = false; // kbdMenu: a key opened the menu; swallow its contextmenu event
  let slowTimer = 0, typeTimer = 0, typed = '', wheelAt = 0;

  /* ---------- items ---------- */
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const key = (c, r) => `${Math.round(c)},${Math.round(r)}`;
  const isSystem = (id) => SYSTEM.includes(id);
  const items = () => [...SYSTEM.map((id) => Shell.fs.get(id)).filter(Boolean), ...Shell.fs.children('desktop')];
  const savePositions = () => Shell.store.set('desktop-positions', positions);
  /* Windows hides known extensions by default: "Read me.txt" shows and renames as "Read me". */
  const hidesExt = (it) => it.kind === 'file' && !!it.ext && it.name.toLowerCase().endsWith(`.${it.ext.toLowerCase()}`);
  const displayName = (it) => (hidesExt(it) ? it.name.slice(0, -(it.ext.length + 1)) : it.name);
  const iconOf = (it) => (it.id === 'recycle'
    ? (Shell.fs.children('recycle').length ? 'object/recycle-full' : 'object/recycle-empty')
    : it.icon && it.icon.startsWith('object/') ? it.icon : 'object/document');
  const typeName = (it) => ({ folder: 'File folder', shortcut: 'Shortcut', system: 'System Folder', drive: 'Local Disk' }[it.kind]
    || TYPES[it.ext] || `${(it.ext || '').toUpperCase()} File`.trim());
  const fmtDate = (ms) => new Date(ms).toLocaleString('en-US', { month: 'numeric', day: 'numeric', year: 'numeric', hour: 'numeric', minute: '2-digit' }).replace(',', '');
  const accepts = (it) => !!it && (it.id === 'recycle' || it.kind === 'folder' || it.kind === 'drive');
  const h = (tag, className, text) => {
    const n = document.createElement(tag);
    if (className) n.className = className;
    if (text != null) n.textContent = text;
    return n;
  };

  function infotip(it) {
    if (it.id === 'recycle') return 'Contains the files and folders that you have deleted.';
    if (it.kind === 'file') return `Type: ${typeName(it)}\nSize: ${Shell.fs.formatSize(it.size || 0)}\nDate modified: ${fmtDate(it.modified)}`;
    if (it.kind === 'folder') return `Date created: ${fmtDate(it.modified)}${Shell.fs.children(it.id).length ? '' : '\nEmpty folder'}`;
    return '';
  }

  /* ---------- grid ---------- */
  function metrics() {
    const icon = ICON[Shell.settings.iconSize] || ICON.medium;
    const w = Math.max(grid, icon + 27), h = grid + icon - 32;
    const W = root.clientWidth, H = root.clientHeight;
    return {
      icon, w, h, W, H,
      cols: Math.max(1, Math.floor((W - 2 * ORIGIN) / w)),
      rows: Math.max(1, Math.floor((H - 2 * ORIGIN) / h)),
    };
  }

  /* Nearest free cell to `near` (column-major order breaks ties), or the first free one. */
  function freeCell(m, taken, near) {
    let best = null, bestD = Infinity;
    for (let c = 0; c < m.cols; c++) {
      for (let r = 0; r < m.rows; r++) {
        if (taken.has(key(c, r))) continue;
        if (!near) return [c, r];
        const d = ((c - near[0]) * m.w) ** 2 + ((r - near[1]) * m.h) ** 2;
        if (d < bestD) { bestD = d; best = [c, r]; }
      }
    }
    return best || [m.cols - 1, m.rows - 1]; // shortcut: a full screen stacks the rest on the last cell
  }

  /* Items in column-major order of their stored cells; items without one keep list order last. */
  function byPosition(list) {
    const at = (it, i) => positions[it.id] || [Infinity, i];
    return list.map((it, i) => [it, at(it, i)]).sort(([, a], [, b]) => a[0] - b[0] || a[1] - b[1]).map(([it]) => it);
  }

  /* Where every item sits now. Stored cells win in list order; a collision takes the nearest free
     cell and a new item the first free one, and both are remembered. A cell the screen no longer
     has goes to the first free cell but is not remembered, so a smaller window keeps the larger
     one's layout. */
  function resolve(m) {
    const list = items(), out = new Map(), taken = new Set(), pending = [];
    if (Shell.settings.autoArrange) {
      byPosition(list).forEach((it, i) => out.set(it.id, [Math.floor(i / m.rows), i % m.rows]));
      return out;
    }
    const free = !Shell.settings.alignToGrid;
    const maxC = Math.max(0, (m.W - 2 * ORIGIN - m.w) / m.w), maxR = Math.max(0, (m.H - 2 * ORIGIN - m.h) / m.h);
    for (const it of list) {
      const p = positions[it.id];
      if (!p) { pending.push([it, null, true]); continue; }
      if (free) {
        const cell = [clamp(p[0], 0, maxC), clamp(p[1], 0, maxR)];
        out.set(it.id, cell);
        taken.add(key(...cell));
        continue;
      }
      const c = Math.round(p[0]), r = Math.round(p[1]), fits = c < m.cols && r < m.rows;
      if (fits && !taken.has(key(c, r))) { out.set(it.id, [c, r]); taken.add(key(c, r)); }
      else pending.push([it, fits ? [c, r] : null, fits]); // off-screen icons fill the first free cells, column by column
    }
    let changed = false;
    for (const [it, near, remember] of pending) {
      const cell = freeCell(m, taken, near);
      out.set(it.id, cell);
      taken.add(key(...cell));
      if (remember) { positions[it.id] = cell; changed = true; }
    }
    if (changed) savePositions();
    return out;
  }

  function layout() {
    if (!root) return;
    const m = metrics();
    root.style.setProperty('--desk-cell-w', `${m.w}px`);
    root.style.setProperty('--desk-icon', `${m.icon}px`);
    cells = resolve(m);
    for (const [id, [c, r]] of cells) {
      const el = els.get(id);
      if (!el) continue;
      el.style.left = `${ORIGIN + 1 + c * m.w}px`;
      el.style.top = `${ORIGIN + 1 + r * m.h}px`;
    }
  }

  /* Lays the given ids out in this order, column by column, and remembers it. */
  function arrange(ids) {
    const m = metrics();
    ids.forEach((id, i) => { positions[id] = [Math.floor(i / m.rows), i % m.rows]; });
    savePositions();
    layout();
  }

  const colMajor = () => [...cells].sort(([, a], [, b]) => a[0] - b[0] || a[1] - b[1]).map(([id]) => id);
  const takenExcept = (ids) => new Set([...cells].filter(([id]) => !ids.includes(id)).map(([, cell]) => key(...cell)));

  /* Puts ids in the free cells nearest a client point (new items, drops from other surfaces). */
  function placeNear(ids, x, y) {
    if (!ids.length || Shell.settings.autoArrange) return;
    const m = metrics(), box = root.getBoundingClientRect();
    const near = [clamp(Math.floor((x - box.left - ORIGIN) / m.w), 0, m.cols - 1), clamp(Math.floor((y - box.top - ORIGIN) / m.h), 0, m.rows - 1)];
    const taken = takenExcept(ids);
    for (const id of ids) {
      const cell = freeCell(m, taken, near);
      taken.add(key(...cell));
      positions[id] = cell;
    }
    savePositions();
    layout();
  }

  /* ---------- rendering ---------- */
  function render() {
    if (!root) return;
    if (renaming) { renderPending = true; return; }
    const m = metrics(), seen = new Set();
    for (const it of items()) {
      seen.add(it.id);
      let el = els.get(it.id);
      if (!el) {
        el = h('div', 'desk-item');
        el.id = `desk-item-${it.id}`;
        el.dataset.id = it.id;
        el.setAttribute('role', 'option');
        el.append(h('span'), h('div', 'desk-label'));
        els.set(it.id, el);
        root.append(el);
      }
      const icon = iconOf(it), iconKey = `${icon}@${m.icon}@${it.kind}`;
      if (el.dataset.icon !== iconKey) {
        el.dataset.icon = iconKey;
        const box = h('span', 'desk-icon');
        box.append(Shell.icon(icon, { size: m.icon }));
        if (it.kind === 'shortcut') box.append(Shell.icon('object/shortcut-arrow', { size: Math.round(m.icon / 3), className: 'desk-badge' }));
        el.firstChild.replaceWith(box);
      }
      el.lastChild.textContent = displayName(it);
      const tip = infotip(it);
      if (tip) el.dataset.tooltip = tip; else delete el.dataset.tooltip;
    }
    for (const [id, el] of els) {
      if (seen.has(id)) continue;
      el.remove();
      els.delete(id);
      sel.delete(id);
      cutIds.delete(id);
    }
    if (!seen.has(focusId)) focusId = null;
    if (!seen.has(anchorId)) anchorId = null;
    root.classList.toggle('icons-hidden', !Shell.settings.showDesktopIcons);
    if (!Shell.settings.showDesktopIcons) { sel.clear(); focusId = null; }
    layout();
    paint();
  }

  function paint() {
    for (const [id, el] of els) {
      el.classList.toggle('sel', sel.has(id));
      el.classList.toggle('focus', id === focusId);
      el.classList.toggle('cut', cutIds.has(id));
      el.setAttribute('aria-selected', String(sel.has(id)));
    }
    if (focusId) root.setAttribute('aria-activedescendant', `desk-item-${focusId}`);
    else root.removeAttribute('aria-activedescendant');
  }

  /* F5 / Refresh: Windows redraws the icons, which blink once. */
  function refresh() {
    render();
    if (Shell.reducedMotion()) return;
    root.classList.add('refreshing');
    setTimeout(() => root.classList.remove('refreshing'), 120);
  }

  /* ---------- selection ---------- */
  function selectOnly(id) {
    sel.clear();
    if (id) sel.add(id);
    focusId = anchorId = id;
  }

  /* Shift range in an icon view is the rectangle spanned by the anchor and the target. */
  function selectRange(from, to) {
    const a = cells.get(from) || cells.get(to), b = cells.get(to);
    if (!b) return;
    sel.clear();
    for (const [id, [c, r]] of cells) {
      if (c >= Math.min(a[0], b[0]) - 0.01 && c <= Math.max(a[0], b[0]) + 0.01 && r >= Math.min(a[1], b[1]) - 0.01 && r <= Math.max(a[1], b[1]) + 0.01) sel.add(id);
    }
  }

  const selected = () => colMajor().filter((id) => sel.has(id));
  const movableSelected = () => selected().filter((id) => !isSystem(id));
  const focusRoot = () => root && root.focus({ preventScroll: true });

  /* ---------- pointer ---------- */
  /* Follows one pressed pointer until release; Esc or losing the window cancels. capture() keeps
     the pointer once the gesture becomes a drag, which also stops hover on the icons under it. */
  function track(e, { move, up, cancel }) {
    const pid = e.pointerId, x0 = e.clientX, y0 = e.clientY;
    const onMove = (ev) => { if (ev.pointerId === pid) move(ev, ev.clientX - x0, ev.clientY - y0); };
    const onUp = (ev) => { if (ev.pointerId === pid) { stop(); up(ev, ev.clientX - x0, ev.clientY - y0); } };
    const onCancel = (ev) => { if (!ev.pointerId || ev.pointerId === pid) { stop(); cancel(); } };
    const onKey = (ev) => {
      if (ev.key !== 'Escape') return;
      ev.preventDefault();
      ev.stopPropagation();
      stop();
      cancel();
    };
    function stop() {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onCancel);
      window.removeEventListener('keydown', onKey, true);
      window.removeEventListener('blur', onCancel);
      try { root.releasePointerCapture(pid); } catch { /* already released */ }
    }
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    window.addEventListener('pointercancel', onCancel);
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('blur', onCancel);
    return { capture: () => { try { root.setPointerCapture(pid); } catch { /* pointer gone */ } } };
  }

  function onPointerDown(e) {
    clearTimeout(slowTimer);
    kbdMenu = false;
    if (renaming) {
      if (e.target === renaming.ta) return;
      commitRename();
    }
    if (drag) return;
    const wasActive = root.contains(document.activeElement);
    const id = e.target.closest('.desk-item')?.dataset.id || null;
    root.classList.remove('kbd');
    focusRoot();
    if (e.button === 2) {
      if (id && !sel.has(id)) selectOnly(id);
      if (!id && !e.ctrlKey) sel.clear();
      paint();
      return;
    }
    if (e.button !== 0) return;
    if (id) pressItem(e, id, wasActive);
    else pressEmpty(e);
  }

  function pressItem(e, id, wasActive) {
    const wasOnlySelection = wasActive && sel.size === 1 && sel.has(id);
    let onClick = null;
    if (e.ctrlKey) {
      if (sel.has(id)) onClick = () => sel.delete(id);
      else sel.add(id);
      focusId = anchorId = id;
    } else if (e.shiftKey) {
      selectRange(anchorId || id, id);
      focusId = id;
    } else if (sel.has(id)) {
      focusId = id;
      onClick = () => selectOnly(id); // a drag keeps the group; a click narrows it
    } else {
      selectOnly(id);
    }
    paint();
    let dragging = false;
    const t = track(e, {
      move(ev, dx, dy) {
        if (!dragging) {
          if (Math.hypot(dx, dy) < DRAG_START) return;
          dragging = true;
          t.capture();
          startDrag(id);
        }
        dragMove(ev, dx, dy);
      },
      up(ev, dx, dy) {
        if (dragging) { finishDrag(ev, dx, dy); return; }
        if (onClick) { onClick(); paint(); }
        const onLabel = ev.target instanceof Element && ev.target.closest('.desk-label') && els.get(id)?.contains(ev.target);
        if (wasOnlySelection && onLabel && !e.ctrlKey && !e.shiftKey) slowTimer = setTimeout(() => startRename(id), SLOW_CLICK);
      },
      cancel() { if (dragging) cancelDrag(); },
    });
  }

  /* Rubber band from empty space: Windows' Highlight-blue rectangle; Ctrl toggles against the selection
     it started with, Shift adds to it. */
  function pressEmpty(e) {
    const toggle = e.ctrlKey, base = e.ctrlKey || e.shiftKey ? new Set(sel) : new Set();
    if (!base.size) { sel.clear(); focusId = null; paint(); }
    const box = root.getBoundingClientRect();
    const x0 = e.clientX - box.left, y0 = e.clientY - box.top;
    let rects = null;
    const t = track(e, {
      move(ev) {
        const x = clamp(ev.clientX - box.left, 0, box.width), y = clamp(ev.clientY - box.top, 0, box.height);
        if (!rects) {
          if (Math.hypot(x - x0, y - y0) < DRAG_START) return;
          t.capture();
          band.hidden = false;
          rects = [...els].filter(([, el]) => el.offsetParent).map(([id, el]) => [id, el.offsetLeft, el.offsetTop, el.offsetWidth, el.offsetHeight]);
        }
        const L = Math.min(x, x0), T = Math.min(y, y0), R = Math.max(x, x0), B = Math.max(y, y0);
        Object.assign(band.style, { left: `${L}px`, top: `${T}px`, width: `${R - L}px`, height: `${B - T}px` });
        sel.clear();
        base.forEach((id) => sel.add(id));
        for (const [id, l, tp, w, hgt] of rects) {
          if (l < R && l + w > L && tp < B && tp + hgt > T) {
            if (toggle && base.has(id)) sel.delete(id); else sel.add(id);
          }
        }
        paint();
      },
      up() {
        band.hidden = true;
        if (!rects) return;
        focusId = selected()[0] || null;
        if (focusId) anchorId = focusId;
        paint();
      },
      cancel() { band.hidden = true; },
    });
  }

  /* ---------- drag and drop ---------- */
  /* The originals stay put and selected; a translucent copy of them follows the pointer with the
     grab offset. Over a folder or the Recycle Bin the target lights up and Windows' drop
     description ("Move to <name>") appears under the pointer. */
  function startDrag(grabbed) {
    Shell.tooltip?.hide?.();
    const ids = [grabbed, ...selected().filter((id) => id !== grabbed)];
    const ghost = h('div', 'desk-ghost');
    ghost.style.pointerEvents = 'none'; // #overlays gives its children pointer events
    ['--desk-cell-w', '--desk-icon'].forEach((v) => ghost.style.setProperty(v, root.style.getPropertyValue(v)));
    const box = root.getBoundingClientRect();
    for (const id of ids) {
      const src = els.get(id);
      if (!src || !src.offsetParent) continue;
      const copy = src.cloneNode(true);
      copy.removeAttribute('id');
      delete copy.dataset.tooltip;
      copy.classList.add('sel');
      copy.style.left = `${box.left + src.offsetLeft}px`;
      copy.style.top = `${box.top + src.offsetTop}px`;
      ghost.append(copy);
    }
    const dd = h('div', 'desk-dd drop-desc'); // drop description style: css/cursors.css
    dd.style.pointerEvents = 'none';
    dd.hidden = true;
    (Shell.layer('overlays') || document.body).append(ghost, dd);
    drag = { ids, ghost, dd, overId: null, into: null };
    document.documentElement.classList.add('desk-dragging');
  }

  function dropTarget(x, y) {
    const movable = drag.ids.filter((id) => !isSystem(id));
    const hit = document.elementFromPoint(x, y);
    if (hit && root.contains(hit)) {
      if (movable.length) Shell.drag.hover(x, y, movable, root); // leaves any surface we were over
      const id = hit.closest('.desk-item')?.dataset.id;
      if (id && !drag.ids.includes(id) && movable.length && accepts(Shell.fs.get(id))) return { id };
      return { desk: true };
    }
    const ext = movable.length ? Shell.drag.hover(x, y, movable, root) : null;
    return ext ? { ext } : null;
  }

  /* The folder an Explorer window would put the items in: the folder row, nav item, breadcrumb
     or tab under the pointer ([data-drop]), else the window's own folder. Reads Explorer's DOM,
     as Explorer reads the desktop's for its own drop description. */
  function explorerFolder(x, y, ids) {
    const hit = document.elementFromPoint(x, y), win = hit?.closest('.ex');
    if (!win) return null;
    const loc = hit.closest('[data-drop]')?.dataset.drop || win.dataset.loc;
    const it = Shell.fs.get(loc);
    return it && accepts(it) && !ids.includes(loc) && !ids.every((id) => Shell.fs.get(id)?.parent === loc) ? loc : null;
  }

  function dragMove(ev, dx, dy) {
    drag.ghost.style.transform = `translate(${dx}px, ${dy}px)`;
    const t = dropTarget(ev.clientX, ev.clientY), overId = t?.id || null;
    if (overId !== drag.overId) {
      els.get(drag.overId)?.classList.remove('drop');
      els.get(overId)?.classList.add('drop');
      drag.overId = overId;
    }
    const into = overId || (t?.ext && explorerFolder(ev.clientX, ev.clientY, drag.ids.filter((id) => !isSystem(id)))) || null;
    if (into !== drag.into) {
      drag.into = into;
      drag.dd.hidden = !into;
      if (into) {
        const name = h('span', 'desk-dd-insert drop-desc-insert', displayName(Shell.fs.get(into)));
        drag.dd.replaceChildren(Shell.icon('fluent/arrow-right', { size: 16, className: 'desk-dd-icon drop-desc-icon' }), h('span', '', 'Move to '), name);
      }
    }
    if (into) drag.dd.style.transform = `translate(${ev.clientX + 12}px, ${ev.clientY + 20}px)`;
    document.documentElement.classList.toggle('desk-nodrop', !t);
  }

  function endDrag() {
    if (!drag) return;
    drag.ghost.remove();
    drag.dd.remove();
    els.get(drag.overId)?.classList.remove('drop');
    document.documentElement.classList.remove('desk-dragging', 'desk-nodrop');
    drag = null;
  }

  function cancelDrag() {
    endDrag();
    Shell.drag.hover(-1, -1, [], root); // clears another surface's drop highlight
  }

  function finishDrag(ev, dx, dy) {
    const t = dropTarget(ev.clientX, ev.clientY);
    const ids = drag.ids, movable = ids.filter((id) => !isSystem(id));
    endDrag();
    if (t?.id) return void dropInto(t.id, movable);
    if (t?.desk) { Shell.drag.hover(-1, -1, [], root); return reposition(ids, dx, dy); }
    if (movable.length) Shell.drag.drop(ev.clientX, ev.clientY, movable, { exclude: root });
    else Shell.drag.hover(-1, -1, [], root);
  }

  function dropInto(target, ids, copy = false) {
    if (target === 'recycle') ids.forEach((id) => Shell.fs.remove(id));
    else transfer(ids, target, copy);
  }

  /* A drop on the desktop. The grabbed icon goes to the cell under it and the others keep their
     offsets; an occupied cell sends the newcomer to the nearest free one. Auto arrange inserts at
     that point instead; with "Align icons to grid" off the icons land where they were let go. */
  function reposition(ids, dx, dy) {
    ids = ids.filter((id) => cells.has(id)); // an item may have gone while it was dragged
    if (!ids.length) return;
    const m = metrics(), [gc, gr] = cells.get(ids[0]);
    if (Shell.settings.autoArrange) {
      const c = clamp(Math.round(gc + dx / m.w), 0, m.cols - 1), r = clamp(Math.round(gr + dy / m.h), 0, m.rows - 1);
      const rest = colMajor().filter((id) => !ids.includes(id));
      rest.splice(Math.min(c * m.rows + r, rest.length), 0, ...ids);
      return arrange(rest);
    }
    if (!Shell.settings.alignToGrid) {
      const maxC = Math.max(0, (m.W - 2 * ORIGIN - m.w) / m.w), maxR = Math.max(0, (m.H - 2 * ORIGIN - m.h) / m.h);
      ids.forEach((id) => { const [c, r] = cells.get(id); positions[id] = [clamp(c + dx / m.w, 0, maxC), clamp(r + dy / m.h, 0, maxR)]; });
      savePositions();
      return layout();
    }
    const dc = Math.round(gc + dx / m.w) - gc, dr = Math.round(gr + dy / m.h) - gr;
    if (!dc && !dr) return;
    const taken = takenExcept(ids);
    for (const id of ids) {
      const [c, r] = cells.get(id);
      let cell = [clamp(c + dc, 0, m.cols - 1), clamp(r + dr, 0, m.rows - 1)];
      if (taken.has(key(...cell))) cell = freeCell(m, taken, cell);
      taken.add(key(...cell));
      positions[id] = cell;
    }
    savePositions();
    layout();
  }

  /* An Explorer drag over the desktop lights up the folder or Recycle Bin under the pointer. */
  let extIds = [], extOver = null;
  function setExtOver(id) {
    if (id === extOver) return;
    els.get(extOver)?.classList.remove('drop');
    extOver = id;
    els.get(id)?.classList.add('drop');
  }
  function extMove(ev) {
    const node = document.elementsFromPoint(ev.clientX, ev.clientY).find((n) => root.contains(n));
    const id = node?.closest('.desk-item')?.dataset.id;
    setExtOver(id && !extIds.includes(id) && accepts(Shell.fs.get(id)) ? id : null);
  }
  function extEnter(ids) {
    extIds = ids;
    window.addEventListener('pointermove', extMove, true);
  }
  function extLeave() {
    window.removeEventListener('pointermove', extMove, true);
    setExtOver(null);
  }

  /* Items dropped on the desktop from another surface (Explorer; Ctrl there copies). */
  async function acceptDrop(ids, { x, y, copy }) {
    const hit = document.elementFromPoint(x, y)?.closest?.('.desk-item');
    const target = hit && Shell.fs.get(hit.dataset.id);
    const movable = ids.filter((id) => { const it = Shell.fs.get(id); return it && it.kind !== 'drive' && it.kind !== 'system'; });
    if (target && accepts(target) && !ids.includes(target.id)) return dropInto(target.id, movable, copy);
    placeNear(await transfer(movable, 'desktop', copy), x, y);
  }

  /* ---------- file operations ---------- */
  /* Copies or moves items into folder `to` as Windows' file operation does: a copy beside its
     source becomes "<name> - Copy" (Shell.fs.copy) and a move into its own folder does nothing.
     A name another item already has there goes to the Replace or Skip Files dialog
     (Shell.dialogs.fileOperation) or, without it, keeps both as "<name> (2)".
     Resolves with the ids in `to` the operation made (copy) or carried (move). */
  async function transfer(ids, to, copy = false) {
    const inside = new Set(Shell.fs.path(to).map((i) => i.id));
    ids = ids.filter((id) => { const it = Shell.fs.get(id); return it && it.kind !== 'system' && it.kind !== 'drive' && !inside.has(id); });
    const before = new Set(Shell.fs.children(to).map((i) => i.id));
    const clash = (it) => Shell.fs.children(to).some((o) => o.id !== it.id && o.name.toLowerCase() === it.name.toLowerCase());
    const away = ids.filter((id) => Shell.fs.get(id).parent !== to);
    if (copy) ids.filter((id) => !away.includes(id)).forEach((id) => Shell.fs.copy(id, to));
    if (to === 'desktop') away.forEach((id) => delete positions[id]); // arrivals take free cells, not a cell they once had
    if (away.some((id) => clash(Shell.fs.get(id))) && Shell.dialogs?.fileOperation) {
      try { await Shell.dialogs.fileOperation({ op: copy ? 'copy' : 'move', ids: away, to }); } catch { /* the dialog reports its own failures */ }
    } else {
      for (const id of away) {
        const it = Shell.fs.get(id), name = clash(it) && Shell.fs.uniqueName(to, displayName(it), hidesExt(it) ? it.ext : null);
        if (copy) { const c = Shell.fs.copy(id, to); if (c && name) Shell.fs.rename(c.id, name); }
        else { if (name) Shell.fs.rename(id, name); Shell.fs.move(id, to); }
      }
    }
    return Shell.fs.children(to).filter((i) => (copy ? !before.has(i.id) : ids.includes(i.id))).map((i) => i.id);
  }

  /* Paste: the shared clipboard's items into `to`. On the desktop they land at the menu's point
     (from the keyboard, in the first free cells) and become the selection. A paste uses up a cut. */
  async function paste(to = 'desktop', point = null) {
    const c = Shell.clipboard.get();
    if (!c || !Shell.clipboard.canPaste(to)) return;
    if (c.cut) Shell.clipboard.clear();
    const ids = await transfer(c.ids, to, !c.cut);
    if (to !== 'desktop' || !root) return;
    if (point) placeNear(ids, ...point);
    Shell.desktop.select(ids);
  }

  /* Paste shortcut: "<name> - Shortcut" to each copied item (Windows offers it after Copy, not Cut). */
  function pasteShortcut(point) {
    const c = Shell.clipboard.get();
    if (!c || c.cut) return;
    const ids = c.ids.map((id) => {
      const it = Shell.fs.get(id), t = targetOf(it);
      return Shell.fs.create('desktop', { name: Shell.fs.uniqueName('desktop', `${it.name} - Shortcut`), kind: 'shortcut', icon: t.icon, target: t.app, args: t.args }).id;
    });
    if (point) placeNear(ids, ...point);
    Shell.desktop.select(ids);
  }

  /* ---------- actions ---------- */
  const openItems = (ids = selected()) => ids.forEach((id) => Shell.fs.open(id));
  const remove = (ids = movableSelected()) => ids.forEach((id) => Shell.fs.remove(id));

  /* Permanent delete (Shift+Delete, Empty Recycle Bin) asks first with Explorer's wording; without
     the dialogs module nothing is deleted, since Windows never purges unasked. */
  async function confirmPurge(items) {
    if (!items.length || !Shell.dialogs?.messageBox) return false;
    const one = items.length === 1 ? items[0] : null;
    const what = one && ({ folder: 'Folder', shortcut: 'Shortcut' }[one.kind] || 'File');
    const answer = await Promise.resolve(Shell.dialogs.messageBox({
      title: one ? `Delete ${what}` : 'Delete Multiple Items',
      text: one ? `Are you sure you want to permanently delete this ${what.toLowerCase()}?` : `Are you sure you want to permanently delete these ${items.length} items?`,
      icon: 'warning', buttons: ['Yes', 'No'], defaultButton: 'Yes',
    })).catch(() => null);
    if (!Shell.wm.focused?.()) focusRoot(); // no window took over: the desktop, not the closing dialog, keeps the keys
    return answer === 'Yes';
  }
  /* Shell.fs only purges the whole Recycle Bin, so Shift+Delete flags the item and its contents
     `deleted` (children/search skip them) and parks it in the bin for the next Empty, as Explorer does.
     shortcut: writes the live items from Shell.fs, replace with Shell.fs.purge(id) once core has one. */
  async function purge(ids = movableSelected()) {
    const items = ids.map((id) => Shell.fs.get(id)).filter(Boolean);
    if (!(await confirmPurge(items))) return;
    const flag = (it) => { Shell.fs.children(it.id).forEach(flag); it.deleted = true; };
    items.forEach((it) => { if (!Shell.fs.get(it.id)) return; flag(it); Shell.fs.move(it.id, 'recycle'); });
  }
  async function emptyRecycleBin() {
    if (await confirmPurge(Shell.fs.children('recycle'))) Shell.fs.emptyRecycleBin();
  }

  /* Cut and Copy fill the shared clipboard; its `clipboard` event dims what was cut. */
  const clip = (cut, ids = movableSelected()) => ids.length && Shell.clipboard.set(ids, { cut });

  /* Share hands the files to the browser's share sheet (in Edge or Chrome on Windows, Windows' own
     Share), so it is offered only for files whose text this POC holds, where files can be shared. */
  function shareable(ids) {
    const list = ids.map((id) => Shell.fs.get(id));
    if (!list.length || !list.every((it) => it?.kind === 'file' && typeof it.content === 'string')) return null;
    const files = list.map((it) => new File([it.content], it.name, { type: 'text/plain' }));
    try { return navigator.canShare?.({ files }) ? files : null; } catch { return null; }
  }

  /* Open with > Choose another app: the shared picker; the chosen app opens the file. */
  async function chooseApp(it) {
    const app = await Shell.dialogs?.openWith?.(it.id);
    if (app && Shell.fs.get(it.id)) Shell.apps.launch(app, app === 'explorer' ? { folder: it.parent, select: it.id } : { file: it.id });
  }

  function copyAsPath(ids = movableSelected()) {
    const paths = ids.map((id) => { const it = Shell.fs.get(id); return `"${DESKTOP_PATH}\\${it.name}${it.kind === 'shortcut' ? '.lnk' : ''}"`; });
    navigator.clipboard?.writeText(paths.join('\r\n')).catch(() => { /* clipboard denied */ });
  }

  function openLocation(it) {
    const title = it.args?.title;
    const folder = title && Shell.fs.search(title).find((f) => f.kind === 'folder' && f.name === title);
    Shell.apps.launch('explorer', folder ? { folder: folder.id } : { folder: 'desktop', select: it.id });
  }

  /* Compress to ZIP file: "<first name>.zip" beside the items, selected and in rename. */
  function compress(ids) {
    const first = Shell.fs.get(ids[0]), size = ids.reduce((a, id) => a + (Shell.fs.get(id)?.size || 0), 0);
    if (!first) return;
    const it = Shell.fs.create('desktop', { name: Shell.fs.uniqueName('desktop', displayName(first), 'zip'), kind: 'file', ext: 'zip', icon: 'object/folder', size: Math.round(size * 0.6) + 22 });
    if (menuPoint) placeNear([it.id], ...menuPoint);
    selectOnly(it.id);
    paint();
    startRename(it.id);
  }

  function sortBy(by) {
    const name = (a, b) => displayName(a).localeCompare(displayName(b), undefined, { numeric: true, sensitivity: 'base' });
    const cmp = {
      Name: name,
      Size: (a, b) => (a.size || 0) - (b.size || 0) || name(a, b),
      'Item type': (a, b) => typeName(a).localeCompare(typeName(b)) || name(a, b),
      'Date modified': (a, b) => (b.modified || 0) - (a.modified || 0) || name(a, b),
    }[by];
    const rank = (it) => (isSystem(it.id) ? SYSTEM.indexOf(it.id) : it.kind === 'folder' ? 2 : 3);
    arrange(items().sort((a, b) => rank(a) - rank(b) || (rank(a) < 2 ? 0 : cmp(a, b))).map((it) => it.id));
  }

  function newItem(kind) {
    const it = Shell.fs.create('desktop', kind === 'folder'
      ? { name: Shell.fs.uniqueName('desktop', 'New folder'), kind: 'folder', icon: 'object/folder' }
      : { name: Shell.fs.uniqueName('desktop', 'New Text Document', 'txt'), kind: 'file', ext: 'txt', icon: 'object/text', content: '' });
    if (menuPoint) placeNear([it.id], ...menuPoint);
    selectOnly(it.id);
    paint();
    startRename(it.id);
  }

  /* ---------- inline rename ---------- */
  function startRename(id) {
    const it = Shell.fs.get(id), el = els.get(id);
    if (!it || !el || renaming || !el.offsetParent) return;
    clearTimeout(slowTimer);
    selectOnly(id);
    paint();
    const label = el.querySelector('.desk-label');
    const ta = h('textarea', 'desk-rename');
    ta.value = displayName(it);
    ta.spellcheck = false;
    ta.rows = 1;
    ta.setAttribute('aria-label', 'Name');
    label.hidden = true;
    el.append(ta);
    el.classList.add('renaming');
    renaming = { id, ta, label, el };
    const fit = () => { ta.style.height = '0'; ta.style.height = `${ta.scrollHeight}px`; };
    ta.addEventListener('input', () => {
      if (/[\\/:*?"<>|\r\n]/.test(ta.value)) {
        const caret = ta.selectionStart - (ta.value.slice(0, ta.selectionStart).match(BAD_CHARS) || []).length;
        const shown = BAD_CHAR.test(ta.value);
        ta.value = ta.value.replace(BAD_CHARS, '');
        ta.setSelectionRange(caret, caret);
        if (shown) Shell.tooltip?.show?.(ta, 'A file name can\u2019t contain any of the following characters:\n\\ / : * ? " < > |', { placement: 'bottom' });
      } else {
        Shell.tooltip?.hide?.();
      }
      fit();
    });
    ta.addEventListener('keydown', (e) => {
      e.stopPropagation();
      if (e.key === 'Enter') { e.preventDefault(); commitRename(); focusRoot(); }
      else if (e.key === 'Escape') { e.preventDefault(); endRename(); focusRoot(); }
    });
    ta.addEventListener('blur', () => commitRename());
    fit();
    ta.focus({ preventScroll: true });
    ta.select();
  }

  function endRename() {
    const r = renaming;
    if (!r) return;
    renaming = null;
    r.ta.remove();
    r.label.hidden = false;
    r.el.classList.remove('renaming');
    Shell.tooltip?.hide?.();
    if (renderPending) { renderPending = false; render(); }
  }

  function commitRename() {
    if (!renaming) return;
    const { id, ta } = renaming;
    const it = Shell.fs.get(id), value = ta.value.replace(BAD_CHARS, '').trim();
    endRename();
    if (it && value && value !== displayName(it)) Shell.fs.rename(id, hidesExt(it) ? `${value}.${it.ext}` : value);
  }

  /* ---------- keyboard ---------- */
  function moveFocus([dx, dy], e) {
    root.classList.add('kbd');
    const order = colMajor();
    if (!order.length) return;
    let next = order[0];
    const from = focusId && cells.get(focusId);
    if (from) {
      let best = Infinity;
      next = focusId;
      for (const id of order) {
        const [c, r] = cells.get(id);
        const along = dx ? (c - from[0]) * dx : (r - from[1]) * dy;
        if (id === focusId || along <= 0.01) continue;
        const score = along + 2 * (dx ? Math.abs(r - from[1]) : Math.abs(c - from[0]));
        if (score < best) { best = score; next = id; }
      }
    }
    if (e.shiftKey) { selectRange(anchorId || next, next); focusId = next; }
    else if (e.ctrlKey) focusId = next;
    else selectOnly(next);
    paint();
  }

  /* Type-ahead: letters typed within a second find the next name with that prefix; the same
     letter repeated cycles through the names that start with it. */
  function typeAhead(ch) {
    clearTimeout(typeTimer);
    typeTimer = setTimeout(() => { typed = ''; }, 1000);
    typed += ch.toLowerCase();
    const cycle = [...typed].every((c) => c === typed[0]), prefix = cycle ? typed[0] : typed;
    const order = colMajor(), start = Math.max(0, order.indexOf(focusId) + (cycle ? 1 : 0));
    for (let i = 0; i < order.length; i++) {
      const id = order[(start + i) % order.length];
      if (displayName(Shell.fs.get(id)).toLowerCase().startsWith(prefix)) {
        root.classList.add('kbd');
        selectOnly(id);
        paint();
        return;
      }
    }
  }

  function keyboardMenu(classic) {
    kbdMenu = true;
    const el = focusId && sel.has(focusId) ? els.get(focusId) : null;
    if (el) {
      const r = el.getBoundingClientRect();
      return openMenu(r.left + r.width / 2, r.top + r.height / 2, true, classic);
    }
    sel.clear();
    paint();
    const box = root.getBoundingClientRect();
    openMenu(box.left + 8, box.top + 8, false, classic);
  }

  /* Tab and F6 leave the desktop for the taskbar (Shift: its far end), never for a window. */
  function focusTaskbar(back) {
    const list = [...(Shell.layer('taskbar')?.querySelectorAll('button, [tabindex]') || [])]
      .filter((n) => n.tabIndex >= 0 && !n.disabled && n.getClientRects().length);
    (back ? list[list.length - 1] : list[0])?.focus();
  }

  /* Keyboard focus arriving (Tab from the taskbar, Win+D, a menu closed with Esc) shows the focus
     rectangle on the last focused icon, or the first, without changing the selection. */
  function onFocus() {
    if (renaming || !root.matches(':focus-visible') || !Shell.settings.showDesktopIcons) return;
    if (!els.has(focusId)) focusId = colMajor()[0] || null;
    root.classList.add('kbd');
    paint();
  }

  function onKeyDown(e) {
    if (renaming || drag || e.target !== root) return;
    clearTimeout(slowTimer);
    if ((e.key === 'Tab' || e.key === 'F6') && !e.ctrlKey && !e.altKey && !e.metaKey) { e.preventDefault(); focusTaskbar(e.shiftKey); return; }
    const combo = Shell.keys.comboOf(e);
    if (!Shell.settings.showDesktopIcons && !['F5', 'Shift+F10', 'ContextMenu'].includes(combo)) return; // hidden icons take no keys
    const dir = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[e.key];
    if (dir) { e.preventDefault(); moveFocus(dir, e); return; }
    if (e.ctrlKey && e.shiftKey && /^Digit[1-4]$/.test(e.code)) { // Ctrl+Shift+1..4: extra large/large/medium/small
      e.preventDefault();
      Shell.set('iconSize', ['large', 'large', 'medium', 'small'][Number(e.code.slice(-1)) - 1]);
      return;
    }
    const order = colMajor();
    const act = {
      Enter: () => openItems(),
      'Alt+Enter': () => focusId && properties(focusId),
      F2: () => focusId && sel.has(focusId) && startRename(focusId),
      Delete: () => remove(),
      'Shift+Delete': () => purge(),
      'Ctrl+A': () => { order.forEach((id) => sel.add(id)); paint(); },
      'Ctrl+X': () => clip(true),
      'Ctrl+C': () => clip(false),
      'Ctrl+Insert': () => clip(false),
      'Ctrl+V': () => paste(),
      'Shift+Insert': () => paste(),
      'Ctrl+Shift+C': () => copyAsPath(),
      'Ctrl+ ': () => { if (focusId) { if (sel.has(focusId)) sel.delete(focusId); else sel.add(focusId); paint(); } },
      Home: () => { root.classList.add('kbd'); selectOnly(order[0]); paint(); },
      End: () => { root.classList.add('kbd'); selectOnly(order[order.length - 1]); paint(); },
      Escape: () => Shell.clipboard.get()?.cut && Shell.clipboard.clear(),
      F5: refresh,
      'Shift+F10': () => keyboardMenu(true),
      ContextMenu: () => keyboardMenu(false),
    }[combo];
    if (act) { e.preventDefault(); act(); return; }
    if (e.key.length === 1 && (e.key !== ' ' || typed) && !e.ctrlKey && !e.altKey && !e.metaKey) { e.preventDefault(); typeAhead(e.key); }
  }

  /* Ctrl+wheel steps the icon size, as on Windows (which has more steps between). */
  function onWheel(e) {
    if (!e.ctrlKey) return;
    e.preventDefault();
    if (performance.now() - wheelAt < 150) return;
    wheelAt = performance.now();
    const order = ['small', 'medium', 'large'], i = order.indexOf(Shell.settings.iconSize);
    const next = order[clamp(i + (e.deltaY < 0 ? 1 : -1), 0, 2)];
    if (next !== Shell.settings.iconSize) Shell.set('iconSize', next);
  }

  /* ---------- context menus ---------- */
  /* Drops leading, trailing and doubled separators left by conditional items. */
  const tidy = (list) => list.filter((it, i, a) => !it.separator || (i > 0 && i < a.length - 1 && !a[i - 1].separator));

  function openMenu(x, y, onItem, classic = false) {
    if (!Shell.menu?.show) return;
    menuPoint = [x, y];
    Shell.menu.show({
      x, y,
      items: onItem && sel.size ? itemMenu(classic) : desktopMenu(classic),
      onClose: () => setTimeout(() => {
        const a = document.activeElement;
        if (!a || a === document.body) focusRoot();
      }, 0),
    });
  }

  /* "Show more options": the same commands in the classic menu, at the same point. */
  const moreOptions = (onItem) => () => setTimeout(() => menuPoint && openMenu(menuPoint[0], menuPoint[1], onItem, true), 0);

  function desktopMenu(classic) {
    const s = Shell.settings;
    const toggle = (k) => () => Shell.set(k, !Shell.settings[k]);
    const size = (label, value, shortcut) => ({ label, radio: true, checked: s.iconSize === value, shortcut, action: () => Shell.set('iconSize', value) });
    const settings = (page) => () => Shell.apps.launch('settings', { page });
    const at = menuPoint, c = Shell.clipboard.get(), canPaste = Shell.clipboard.canPaste('desktop');
    const view = [
      size('Large icons', 'large', 'Ctrl+Shift+2'),
      size('Medium icons', 'medium', 'Ctrl+Shift+3'),
      size('Small icons', 'small', 'Ctrl+Shift+4'),
      { separator: true },
      { label: 'Auto arrange icons', checked: s.autoArrange, action: toggle('autoArrange') },
      { label: 'Align icons to grid', checked: s.alignToGrid, action: toggle('alignToGrid') },
      { separator: true },
      { label: 'Show desktop icons', checked: s.showDesktopIcons, action: toggle('showDesktopIcons') },
    ];
    const sort = ['Name', 'Size', 'Item type', 'Date modified'].map((label) => ({ label, action: () => sortBy(label) }));
    const create = [
      { label: 'Folder', icon: 'object/folder', action: () => newItem('folder') },
      { label: 'Shortcut', icon: 'object/shortcut-arrow', action: shortcutWizard },
      { separator: true },
      { label: 'Text Document', icon: 'object/text', action: () => newItem('text') },
    ];
    if (classic) {
      return [
        { label: 'View', submenu: view },
        { label: 'Sort by', submenu: sort },
        { label: 'Refresh', action: refresh },
        { separator: true },
        { label: 'Paste', disabled: !canPaste, action: () => paste('desktop', at) },
        { label: 'Paste shortcut', disabled: !canPaste || c.cut, action: () => pasteShortcut(at) },
        { separator: true },
        { label: 'New', submenu: create },
        { separator: true },
        { label: 'Display settings', icon: 'fluent/desktop', action: settings('display') },
        { label: 'Personalize', icon: 'fluent/paint-brush', action: settings('personalization') },
      ];
    }
    const terminal = Shell.apps.get('terminal');
    return tidy([
      canPaste && { commandBar: [{ icon: 'fluent/clipboard-paste', label: 'Paste', action: () => paste('desktop', at) }] },
      { label: 'View', icon: 'fluent/grid', submenu: view },
      { label: 'Sort by', icon: 'fluent/arrow-sort', submenu: sort },
      { label: 'Refresh', icon: 'fluent/arrow-clockwise', action: refresh },
      { separator: true },
      { label: 'New', icon: 'fluent/add', submenu: create },
      { separator: true },
      { label: 'Display settings', icon: 'fluent/desktop', action: settings('display') },
      { label: 'Personalize', icon: 'fluent/paint-brush', action: settings('personalization') },
      { separator: true },
      ...(terminal ? [{ label: 'Open in Terminal', icon: 'object/app-terminal', action: () => Shell.apps.launch('terminal', { cwd: DESKTOP_PATH }) }] : []),
      { separator: true },
      { label: 'Show more options', icon: 'fluent/open', shortcut: 'Shift+F10', action: moreOptions(false) },
    ].filter(Boolean));
  }

  function itemMenu(classic) {
    const ids = selected(), movable = ids.filter((id) => !isSystem(id));
    const one = ids.length === 1 ? Shell.fs.get(ids[0]) : null;
    const pinned = !!one && !!Shell.start?.isPinned?.(one.id);
    const open = { label: 'Open', shortcut: 'Enter', action: () => openItems(ids) };
    const location = one?.kind === 'shortcut' && { label: 'Open file location', icon: 'fluent/folder-open', action: () => openLocation(one) };
    const empty = one?.id === 'recycle' && { label: 'Empty Recycle Bin', icon: 'fluent/delete', disabled: !Shell.fs.children('recycle').length, action: emptyRecycleBin };
    const pinItem = one && one.kind !== 'file' && {
      label: pinned ? 'Unpin from Start' : 'Pin to Start', icon: pinned ? 'fluent/pin-off' : 'fluent/pin', disabled: !Shell.start?.pin,
      action: () => (pinned ? Shell.start.unpin(one.id) : Shell.start.pin(one.id)),
    };
    const props = { label: 'Properties', icon: 'fluent/wrench', shortcut: 'Alt+Enter', disabled: !one || !hasProperties(one.id), action: () => properties(one.id) };
    const notepad = (label, icon) => ({ label, icon, action: () => Shell.apps.launch('notepad', { file: one.id }) });
    /* Open with: a submenu of the type's apps, or straight to the picker for a type with none. */
    const choose = { label: 'Choose another app', disabled: !Shell.dialogs?.openWith, action: () => chooseApp(one) };
    const openWith = one?.kind === 'file' && one.ext !== 'exe' && (one.ext === 'txt'
      ? { label: 'Open with', icon: 'fluent/apps', submenu: [notepad('Notepad', 'object/app-notepad'), { separator: true }, choose] }
      : { ...choose, label: 'Open with', icon: 'fluent/apps' });
    const pasteInto = one?.kind === 'folder' && { label: 'Paste', disabled: !Shell.clipboard.canPaste(one.id), action: () => paste(one.id) };
    const files = shareable(movable);
    if (classic) {
      return tidy([
        open, openWith, location,
        { separator: true },
        pinItem, empty,
        { separator: true },
        { label: 'Cut', disabled: !movable.length, action: () => clip(true, movable) },
        { label: 'Copy', disabled: !movable.length, action: () => clip(false, movable) },
        pasteInto,
        { separator: true },
        { label: 'Delete', disabled: !movable.length, action: () => remove(movable) },
        { label: 'Rename', disabled: !one, action: () => startRename(one.id) },
        { separator: true },
        props,
      ].filter(Boolean));
    }
    /* Windows 11 command row. Folders have no Share; files have it, enabled where shareable() is. */
    const commandBar = [
      movable.length && { icon: 'fluent/cut', label: 'Cut', action: () => clip(true, movable) },
      movable.length && { icon: 'fluent/copy', label: 'Copy', action: () => clip(false, movable) },
      one && { icon: 'fluent/rename', label: 'Rename', action: () => startRename(one.id) },
      movable.some((id) => Shell.fs.get(id).kind !== 'folder') && { icon: 'fluent/share', label: 'Share', disabled: !files, action: () => navigator.share({ files }).catch(() => { /* dismissed */ }) },
      movable.length && { icon: 'fluent/delete', label: 'Delete', action: () => remove(movable) },
    ].filter(Boolean);
    return tidy([
      { commandBar },
      open, openWith,
      one?.kind === 'folder' && { label: 'Open in new window', icon: 'fluent/window-new', action: () => Shell.apps.launch('explorer', { folder: one.id }) },
      location, empty, pinItem,
      movable.length && { label: 'Compress to ZIP file', icon: 'fluent/folder', action: () => compress(movable) },
      movable.length && { label: 'Copy as path', icon: 'fluent/document', shortcut: 'Ctrl+Shift+C', action: () => copyAsPath(movable) },
      one && props,
      { separator: true },
      one?.kind === 'file' && notepad('Edit in Notepad', 'object/app-notepad'), // Notepad's verb for every file, in the app-verb section
      { separator: true },
      { label: 'Show more options', icon: 'fluent/open', shortcut: 'Shift+F10', action: moreOptions(true) },
    ].filter(Boolean));
  }

  /* ---------- dialogs ---------- */
  function dialogButtons(...specs) {
    const foot = h('div', 'desk-dialog-foot');
    const buttons = specs.map(([label, accent]) => h('button', accent ? 'btn accent' : 'btn', label));
    foot.append(...buttons);
    return [foot, ...buttons];
  }

  function openDialog(opts) {
    return Shell.wm.open({ resizable: false, maximizable: false, minimizable: false, background: 'solid', ...opts });
  }

  /* Properties: the shared property sheet. This PC opens Settings > System > About, as Windows 11 does. */
  const hasProperties = (id) => id === 'root' || !!Shell.dialogs?.properties;
  function properties(id) {
    if (id === 'root') Shell.apps.launch('settings', { page: 'about' });
    else Shell.dialogs?.properties?.(id);
  }

  /* The location typed in Create Shortcut: an app (id, name or .exe name) or a file-system item. */
  function resolveTarget(text) {
    const q = text.trim().replace(/^"|"$/g, '').toLowerCase();
    if (!q) return null;
    const leaf = q.replace(/^.*\\/, ''), exe = leaf.replace(/\.exe$/, '');
    const alias = { calc: 'calculator', wt: 'terminal', cmd: 'terminal', powershell: 'terminal', 'ms-settings:': 'settings' }[exe] || exe;
    const app = Shell.apps.list().find((a) => a.id === alias || (a.name || '').toLowerCase() === alias);
    if (app) return { name: app.name, icon: app.icon || 'object/app-generic', app: app.id, args: {} };
    const item = Shell.fs.search(leaf).find((i) => i.name.toLowerCase() === leaf || displayName(i).toLowerCase() === leaf);
    return item ? targetOf(item) : null;
  }

  /* What a shortcut to a file-system item launches. */
  function targetOf(item) {
    if (item.kind === 'shortcut') return { name: displayName(item), icon: iconOf(item), app: item.target, args: { ...(item.args || {}) } };
    const folder = ['folder', 'drive', 'system'].includes(item.kind), isText = item.ext === 'txt';
    return {
      name: displayName(item),
      icon: item.icon && item.icon.startsWith('object/') ? item.icon : 'object/folder',
      app: isText ? 'notepad' : 'explorer',
      args: folder ? { folder: item.id } : isText ? { file: item.id } : { folder: item.parent, select: item.id },
    };
  }

  /* New > Shortcut: Windows' two-page Create Shortcut wizard. "Windows can't find" shows inline
     instead of in a message box. */
  function shortcutWizard() {
    if (!Shell.wm?.open) return;
    const point = menuPoint;
    const body = h('div', 'desk-dialog desk-wizard');
    const main = h('div', 'desk-wizard-main');
    const field = h('input', 'textbox');
    field.id = `desk-wizard-field-${Date.now()}`;
    field.spellcheck = false;
    const error = h('p', 'desk-wizard-error');
    error.setAttribute('role', 'alert');
    error.hidden = true;
    const [foot, next, cancel] = dialogButtons(['Next', true], ['Cancel']);
    next.disabled = true;
    body.append(main, foot);
    const page = (title, intro, label, outro) => {
      const lab = h('label', 'desk-wizard-label', label);
      lab.htmlFor = field.id;
      main.replaceChildren(h('h2', 'desk-wizard-title', title), ...(intro ? [h('p', '', intro)] : []), lab, field, error, ...(outro ? [h('p', '', outro)] : []));
    };
    page('What item would you like to create a shortcut for?',
      'This wizard helps you to create shortcuts to local or network programs, files, folders, computers, or Internet addresses.',
      'Type the location of the item:');
    let target = null;
    const win = openDialog({ appId: 'shortcut-wizard', title: 'Create Shortcut', icon: 'object/shortcut-arrow', width: 600, height: 440, content: body });
    field.addEventListener('input', () => { next.disabled = !field.value.trim(); error.hidden = true; });
    next.addEventListener('click', () => {
      if (!target) {
        target = resolveTarget(field.value);
        if (!target) {
          error.textContent = `Windows can\u2019t find '${field.value.trim()}'. Make sure you typed the name correctly, and then try again.`;
          error.hidden = false;
          return;
        }
        page('What would you like to name the shortcut?', '', 'Type a name for this shortcut:', 'Click Finish to create the shortcut.');
        field.value = target.name;
        next.textContent = 'Finish';
        field.focus();
        field.select();
        return;
      }
      const name = field.value.replace(BAD_CHARS, '').trim();
      if (!name) return;
      const it = Shell.fs.create('desktop', { name: Shell.fs.uniqueName('desktop', name), kind: 'shortcut', icon: target.icon, target: target.app, args: target.args });
      if (point) placeNear([it.id], ...point);
      win?.close?.();
    });
    cancel.addEventListener('click', () => win?.close?.());
    body.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && !next.disabled) { e.preventDefault(); next.click(); }
      else if (e.key === 'Escape') { e.preventDefault(); cancel.click(); }
    });
    setTimeout(() => field.focus(), 0);
  }

  /* ---------- public API ---------- */
  Shell.desktop = {
    refresh,
    select(ids) {
      sel.clear();
      (ids || []).forEach((id) => { if (els.has(id)) sel.add(id); });
      focusId = anchorId = [...sel][0] || null;
      paint();
    },
  };

  /* ---------- boot ---------- */
  Shell.on('boot', () => {
    const host = Shell.layer('desktop');
    if (!host) return;
    grid = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--desktop-grid')) || 75;
    root = h('div', 'desk');
    root.tabIndex = 0;
    root.setAttribute('role', 'listbox');
    root.setAttribute('aria-multiselectable', 'true');
    root.setAttribute('aria-label', 'Desktop');
    band = h('div', 'desk-band');
    band.hidden = true;
    root.append(band);
    host.append(root);

    root.addEventListener('pointerdown', onPointerDown);
    root.addEventListener('keydown', onKeyDown);
    root.addEventListener('wheel', onWheel, { passive: false });
    root.addEventListener('dblclick', (e) => {
      clearTimeout(slowTimer);
      const id = e.target.closest?.('.desk-item')?.dataset.id;
      if (id && !renaming && e.button === 0) openItems(sel.has(id) ? selected() : [id]);
    });
    root.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      if (renaming || drag || kbdMenu) { kbdMenu = false; return; }
      const onItem = !!e.target.closest?.('.desk-item');
      openMenu(e.clientX, e.clientY, onItem, e.shiftKey); // Shift+right-click opens the classic menu
    });
    new ResizeObserver(() => layout()).observe(root);
    Shell.drag.registerTarget(root, { accepts: () => true, onEnter: extEnter, onLeave: extLeave, onDrop: acceptDrop });

    Shell.on('fs', () => render());
    Shell.on('clipboard', ({ ids, cut }) => { cutIds = new Set(cut ? ids : []); paint(); });
    Shell.on('setting', ({ key: k }) => {
      if (k === 'autoArrange' && Shell.settings.autoArrange) arrange(byPosition(items()).map((it) => it.id));
      if (['iconSize', 'autoArrange', 'alignToGrid', 'showDesktopIcons'].includes(k)) render();
    });

    render();
    focusRoot();
    root.addEventListener('focus', onFocus); // after the boot focus, which is no keyboard arrival
    if (Shell.debugOpen.includes('menu-desktop')) requestAnimationFrame(() => openMenu(600, 400, false));
    /* menu-desktop-item: the first file's menu; menu-desktop-paste: the desktop menu with that file copied. */
    const file = colMajor().find((id) => Shell.fs.get(id)?.kind === 'file');
    if (file && Shell.debugOpen.includes('menu-desktop-item')) requestAnimationFrame(() => { selectOnly(file); paint(); keyboardMenu(false); });
    if (file && Shell.debugOpen.includes('menu-desktop-paste')) { Shell.clipboard.set([file]); requestAnimationFrame(() => openMenu(600, 400, false)); }
  });
})();
