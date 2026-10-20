/* Quick Settings (Win+A): the Windows 11 23H2 flyout. Owns the tile grid (split toggle +
   chevron tiles, edit mode with unpin / add / drag to reorder), the brightness and volume
   sliders, the subpages (Wi-Fi, Bluetooth, Sound output, Accessibility, Cast, Project) and
   the footer; plus the screen filter that makes brightness and night light visible on the
   whole screen. API: Shell.quickSettings.open/close/toggle/isOpen.
   Settings keys owned: muted (read by the taskbar), mobileHotspot, nearbyShare. Store key owned: qs-tiles.
   Network, device and output names are synthetic demonstration data. */
(() => {
  'use strict';

  const Shell = window.Shell;
  const ID = 'quicksettings';
  const S = () => Shell.settings;

  /* h('button', {class, text, onclick, 'aria-label': ...}, ...children) */
  function h(tag, props = {}, ...kids) {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(props)) {
      if (v == null || v === false) continue;
      if (k === 'class') n.className = v;
      else if (k === 'text') n.textContent = v;
      else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
      else n.setAttribute(k, v === true ? '' : v);
    }
    n.append(...kids.flat().filter((c) => c != null && c !== false));
    return n;
  }
  const icon = (name, size = 16) => Shell.icon(`fluent/${name}`, { size });
  const iconBtn = (name, label, onclick, cls = '') =>
    h('button', { class: `fly-icon-btn ${cls}`.trim(), type: 'button', 'aria-label': label, 'data-tooltip': label, onclick }, icon(name));

  /* Settings pages are found by title in the settings module's index; the app opens on its
     home page when the index is missing. */
  function openSettings(title) {
    const hit = (Shell.settingsIndex || []).find((e) => e.title.toLowerCase() === title.toLowerCase());
    close();
    Shell.apps.launch('settings', hit ? { page: hit.page } : {});
  }

  /* ---------- tiles ---------- */
  const TILES = [
    { id: 'wifi', label: 'Wi-Fi', icon: 'wifi', iconOff: 'wifi-off', key: 'wifi', page: 'wifi', more: 'Manage Wi-Fi connections' },
    { id: 'bluetooth', label: 'Bluetooth', icon: 'bluetooth', key: 'bluetooth', page: 'bluetooth', more: 'Manage Bluetooth devices' },
    { id: 'airplane', label: 'Airplane mode', icon: 'airplane', key: 'airplane' },
    { id: 'energySaver', label: 'Energy saver', icon: 'battery-saver', key: 'energySaver' },
    { id: 'nightLight', label: 'Night light', icon: 'weather-moon', key: 'nightLight' },
    { id: 'accessibility', label: 'Accessibility', icon: 'accessibility', page: 'accessibility' },
    { id: 'cast', label: 'Cast', icon: 'cast', page: 'cast' },
    { id: 'hotspot', label: 'Mobile hotspot', icon: 'phone', key: 'mobileHotspot' },
    { id: 'nearby', label: 'Nearby sharing', icon: 'share', key: 'nearbyShare' },
    { id: 'project', label: 'Project', icon: 'tab-desktop', page: 'project' },
  ];
  const tileDef = (id) => TILES.find((t) => t.id === id);
  let order = Shell.store.get('qs-tiles', TILES.slice(0, 8).map((t) => t.id)).filter(tileDef);
  const saveOrder = () => Shell.store.set('qs-tiles', order);

  /* Synthetic radio state. */
  const networks = [
    { name: 'Mihawk-5G', secured: true },
    { name: 'Mihawk-Guest', secured: true },
    { name: 'Studio-5G', secured: true },
    { name: 'Library', secured: false },
    { name: 'DIRECT-42-Printer', secured: true },
  ];
  let connectedNet = 'Mihawk-5G';
  const devices = [
    { name: 'DualSense Wireless Controller', connected: true },
    { name: 'Headphones', connected: false },
  ];
  const outputs = ['Speakers (High Definition Audio Device)', 'Headphones (DualSense Wireless Controller)', 'PS5 Display (HDMI)'];
  let output = 0;
  const projectModes = [['PC screen only', 'desktop'], ['Duplicate', 'copy'], ['Extend', 'square-multiple'], ['Second screen only', 'tab-desktop']];
  let projectMode = 0;
  const a11y = [
    { id: 'magnifier', label: 'Magnifier', icon: 'zoom-in' },
    { id: 'colorFilters', label: 'Color filters', icon: 'color' },
    { id: 'narrator', label: 'Narrator', icon: 'speaker-2' },
    { id: 'mono', label: 'Mono audio', icon: 'speaker-1' },
    { id: 'captions', label: 'Live captions', icon: 'text-font' },
    { id: 'sticky', label: 'Sticky keys', icon: 'keyboard' },
  ];
  const a11yOn = new Set();
  let airplaneSaved = null;

  function tileLabel(def) {
    if (def.id === 'wifi' && S().wifi && connectedNet) return connectedNet;
    return def.label;
  }

  function toggleKey(key) {
    const next = !S()[key];
    Shell.set(key, next);
    if (key !== 'airplane') return;
    /* Airplane mode turns the radios off and gives them back as they were. */
    if (next) {
      airplaneSaved = { wifi: S().wifi, bluetooth: S().bluetooth };
      Shell.set('wifi', false);
      Shell.set('bluetooth', false);
    } else if (airplaneSaved) {
      Shell.set('wifi', airplaneSaved.wifi);
      Shell.set('bluetooth', airplaneSaved.bluetooth);
      airplaneSaved = null;
    }
  }

  /* ---------- flyout state ---------- */
  let host = null, panel = null, pages = null, footer = null;
  let page = null;          // null = main page, else a PAGES key
  let editing = false;
  let returnFocus = null;
  let pageCleanup = null;   // timers of the current subpage
  const timers = new Set();
  const later = (fn, ms) => { const t = setTimeout(() => { timers.delete(t); fn(); }, ms); timers.add(t); return t; };

  function buildTile(def) {
    const tile = h('div', { class: 'qs-tile', 'data-id': def.id });
    const btns = h('div', { class: 'qs-tile-btns' });
    const main = h('button', {
      class: 'qs-tile-btn', type: 'button', 'aria-label': def.label,
      onclick: () => {
        if (editing) return;
        if (def.key) toggleKey(def.key); else showPage(def.page);
      },
    }, icon(def.icon));
    main.dataset.icon = def.icon;
    if (def.key) main.setAttribute('aria-pressed', 'false');
    if (!def.key && def.page) main.append(h('span', { class: 'qs-tile-chevron' }, icon('chevron-right', 16)));
    btns.append(main);
    if (def.key && def.page) {
      btns.append(h('button', {
        class: 'qs-tile-btn', type: 'button', 'aria-label': def.more, 'data-tooltip': def.more,
        onclick: () => { if (!editing) showPage(def.page); },
      }, icon('chevron-right', 16)));
    }
    btns.addEventListener('pointerdown', (e) => { if (editing && e.button === 0) startTileDrag(e, tile); });
    tile.append(
      btns,
      h('span', { class: 'qs-tile-label', text: tileLabel(def) }),
      h('button', { class: 'qs-unpin', type: 'button', 'aria-label': `Unpin ${def.label}`, onclick: () => unpin(def.id, tile) }, icon('pin-off', 12)),
    );
    return tile;
  }

  function syncTiles() {
    if (!panel) return;
    panel.querySelectorAll('.qs-tile').forEach((tile) => {
      const def = tileDef(tile.dataset.id);
      tile.querySelector('.qs-tile-label').textContent = tileLabel(def);
      if (!def.key) return;
      const on = !!S()[def.key];
      tile.classList.toggle('on', on);
      const main = tile.querySelector('.qs-tile-btn');
      main.setAttribute('aria-pressed', String(on));
      const want = !on && def.iconOff ? def.iconOff : def.icon;
      if (main.dataset.icon !== want) {
        main.dataset.icon = want;
        main.querySelector('.icon').replaceWith(icon(want));
      }
    });
  }

  function volumeIcon() {
    const v = S().volume;
    return S().muted || v === 0 ? 'speaker-mute' : v < 34 ? 'speaker-0' : v < 67 ? 'speaker-1' : 'speaker-2';
  }

  function slider(key, label) {
    const input = h('input', { type: 'range', class: 'slider', min: '0', max: '100', step: '1', 'aria-label': label });
    input.value = S()[key];
    const fill = () => input.style.setProperty('--fill', `${input.value}%`);
    if (Shell.ui?.slider) Shell.ui.slider(input); else fill();
    input.addEventListener('input', () => {
      if (!Shell.ui?.slider) fill();
      Shell.set(key, Number(input.value));
      if (key === 'volume' && S().muted) Shell.set('muted', false);
    });
    return input;
  }

  function syncSliders() {
    if (!panel) return;
    panel.querySelectorAll('.qs-sliders input.slider').forEach((input) => {
      const key = input.dataset.key;
      if (Number(input.value) !== S()[key] && document.activeElement !== input) {
        input.value = S()[key];
        input.style.setProperty('--fill', `${input.value}%`);
        input.dispatchEvent(new Event('change'));
      }
    });
    const mute = panel.querySelector('.qs-mute');
    if (mute && mute.dataset.icon !== volumeIcon()) {
      mute.dataset.icon = volumeIcon();
      mute.querySelector('.icon').replaceWith(icon(volumeIcon()));
      mute.setAttribute('aria-label', S().muted ? 'Unmute' : 'Mute');
    }
  }

  function buildMain() {
    const grid = h('div', { class: 'qs-tiles', role: 'group', 'aria-label': 'Quick settings' }, order.map((id) => buildTile(tileDef(id))));
    const bright = slider('brightness', 'Brightness');
    bright.dataset.key = 'brightness';
    const vol = slider('volume', 'Volume');
    vol.dataset.key = 'volume';
    const mute = iconBtn(volumeIcon(), S().muted ? 'Unmute' : 'Mute', () => Shell.set('muted', !S().muted), 'qs-mute');
    mute.dataset.icon = volumeIcon();
    const main = h('div', { class: 'qs-page qs-main' },
      grid,
      h('div', { class: 'qs-sliders' },
        h('div', { class: 'qs-slider-row' }, h('span', { class: 'qs-lead' }, icon('brightness')), bright, h('span')),
        h('div', { class: 'qs-slider-row' }, mute, vol, iconBtn('chevron-right', 'Select a sound output', () => showPage('sound'))),
      ),
    );
    return main;
  }

  /* ---------- footer ---------- */
  function renderFooter() {
    footer.replaceChildren();
    if (page) {
      const [text, title] = PAGES[page].link;
      footer.append(h('button', { class: 'fly-icon-btn qs-link', type: 'button', text, onclick: () => openSettings(title) }));
      return;
    }
    if (editing) {
      const hidden = TILES.filter((t) => !order.includes(t.id));
      const add = h('button', { class: 'btn', type: 'button', disabled: !hidden.length, onclick: (e) => addMenu(e.currentTarget) }, icon('add'), ' Add');
      footer.append(h('span', { class: 'spacer' }), add, h('button', { class: 'btn accent', type: 'button', text: 'Done', onclick: () => setEditing(false) }));
      return;
    }
    footer.append(
      h('button', { class: 'fly-icon-btn qs-battery', type: 'button', 'data-tooltip': BATTERY, 'aria-label': BATTERY, onclick: () => openSettings('Power & battery') },
        icon(S().energySaver ? 'battery-saver' : 'battery-charge'), h('span', { text: '100%' })),
      h('span', { class: 'spacer' }),
      iconBtn('edit', 'Edit quick settings', () => setEditing(true)),
      iconBtn('settings', 'All settings', () => openSettings('System')),
    );
  }

  const BATTERY = 'Battery status: 100% available (plugged in)'; // same reading as the taskbar tray

  /* ---------- edit mode: unpin, add, drag to reorder ---------- */
  function setEditing(on) {
    editing = on;
    panel.classList.toggle('editing', on);
    renderFooter();
    if (!on) syncTiles();
  }

  function unpin(id, tile) {
    order = order.filter((t) => t !== id);
    saveOrder();
    Shell.animate(tile, [{ opacity: 1, transform: 'scale(1)' }, { opacity: 0, transform: 'scale(0.85)' }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' })
      .then(() => flip(tile.parentElement, () => tile.remove()))
      .then(renderFooter);
  }

  function addTile(id) {
    if (!panel || order.includes(id)) return;
    order.push(id);
    saveOrder();
    const tile = buildTile(tileDef(id));
    panel.querySelector('.qs-tiles').append(tile);
    syncTiles();
    Shell.animate(tile, [{ opacity: 0, transform: 'scale(0.85)' }, { opacity: 1, transform: 'scale(1)' }], { duration: 250, easing: Shell.ease.in });
    renderFooter();
  }

  function addMenu(btn) {
    const hidden = TILES.filter((t) => !order.includes(t.id));
    if (!Shell.menu?.show) { if (hidden[0]) addTile(hidden[0].id); return; }
    const r = btn.getBoundingClientRect();
    Shell.menu.show({
      x: r.left, y: r.top, anchorRect: r, placement: 'top-start', minWidth: 200,
      items: hidden.map((t) => ({ label: t.label, icon: `fluent/${t.icon}`, action: () => addTile(t.id) })),
    });
  }

  /* Animate children (except `skip`) from where they were to where `change` puts them (FLIP). */
  function flip(container, change, skip = null) {
    const kids = [...container.children].filter((k) => k !== skip);
    const before = new Map(kids.map((k) => [k, k.getBoundingClientRect()]));
    change();
    const moves = [...container.children].map((k) => {
      const a = before.get(k); if (!a) return null;
      const b = k.getBoundingClientRect();
      if (a.left === b.left && a.top === b.top) return null;
      return Shell.animate(k, [{ transform: `translate(${a.left - b.left}px, ${a.top - b.top}px)` }, { transform: 'none' }], { duration: 250, easing: Shell.ease.standard });
    });
    return Promise.all(moves);
  }

  function startTileDrag(e, tile) {
    const grid = tile.parentElement;
    const btns = e.currentTarget;
    const start = tile.getBoundingClientRect();
    const grab = { x: e.clientX - start.left, y: e.clientY - start.top };
    let dragging = false;
    btns.setPointerCapture(e.pointerId);
    /* Layout rect (ignores running FLIP transforms), so a swap never flips back. */
    const slot = (t) => {
      const p = pages.getBoundingClientRect();
      const left = p.left + t.offsetLeft - pages.scrollLeft, top = p.top + t.offsetTop - pages.scrollTop;
      return { left, top, right: left + t.offsetWidth, bottom: top + t.offsetHeight };
    };
    const place = (x, y) => { const s = slot(tile); tile.style.translate = `${x - grab.x - s.left}px ${y - grab.y - s.top}px`; };
    const move = (ev) => {
      if (!dragging) {
        if (Math.hypot(ev.clientX - e.clientX, ev.clientY - e.clientY) < 4) return;
        dragging = true;
        tile.classList.add('dragging');
      }
      const target = [...grid.children].find((t) => {
        if (t === tile) return false;
        const r = slot(t);
        return ev.clientX >= r.left && ev.clientX <= r.right && ev.clientY >= r.top && ev.clientY <= r.bottom;
      });
      if (target) {
        const kids = [...grid.children];
        const after = kids.indexOf(tile) < kids.indexOf(target);
        flip(grid, () => grid.insertBefore(tile, after ? target.nextSibling : target), tile);
      }
      place(ev.clientX, ev.clientY);
    };
    const up = () => {
      btns.removeEventListener('pointermove', move);
      btns.removeEventListener('pointerup', up);
      btns.removeEventListener('pointercancel', up);
      if (!dragging) return;
      const from = tile.style.translate;
      tile.style.translate = '';
      Shell.animate(tile, [{ translate: from || '0 0' }, { translate: '0 0' }], { duration: 167, easing: Shell.ease.in })
        .then(() => tile.classList.remove('dragging'));
      order = [...grid.children].map((t) => t.dataset.id);
      saveOrder();
    };
    btns.addEventListener('pointermove', move);
    btns.addEventListener('pointerup', up);
    btns.addEventListener('pointercancel', up);
  }

  /* ---------- subpages ---------- */
  const note = (text, ...extra) => h('p', { class: 'qs-note' }, text, ...extra);

  /* An expandable row with a name, a status line and an action area (networks, devices). */
  function connItem({ name, iconName, status, expanded, onToggle, body }) {
    const item = h('div', { class: `qs-item${expanded ? ' expanded' : ''}`, role: 'listitem' });
    item.append(h('button', { class: 'qs-item-head', type: 'button', 'aria-expanded': String(!!expanded), onclick: onToggle },
      icon(iconName),
      h('span', { class: 'qs-item-text' }, h('span', { class: 'qs-item-name', text: name }), status ? h('span', { class: 'qs-item-status', text: status }) : null)));
    if (expanded) item.append(h('div', { class: 'qs-item-body' }, body));
    return item;
  }

  /* A selectable row with the ListView accent pill (outputs, projection modes). */
  function selectItem({ name, iconName, selected, onSelect }) {
    return h('div', { class: `qs-item${selected ? ' selected' : ''}`, role: 'listitem' },
      h('button', { class: 'qs-item-head', type: 'button', 'aria-pressed': String(selected), onclick: onSelect },
        icon(iconName), h('span', { class: 'qs-item-text' }, h('span', { class: 'qs-item-name', text: name }))));
  }

  const switchEl = (label, checked, onchange) => {
    const input = h('input', { type: 'checkbox', role: 'switch', 'aria-label': label, onchange: () => onchange(input.checked) });
    input.checked = checked;
    return h('label', { class: 'toggle' }, input, h('span', { class: 'toggle-track' }, h('span', { class: 'toggle-thumb' })));
  };

  let expandedNet = null, expandedDev = null, connecting = null;

  const PAGES = {
    wifi: {
      title: 'Wi-Fi', key: 'wifi', link: ['More Wi-Fi settings', 'Wi-Fi'],
      list() {
        if (!S().wifi) return note('Wi-Fi is turned off');
        const sorted = [...networks].sort((a, b) => (b.name === connectedNet) - (a.name === connectedNet));
        return sorted.map((n) => {
          const isOn = n.name === connectedNet;
          const sec = n.secured ? 'secured' : 'open';
          const status = connecting === n.name ? 'Connecting'
            : isOn ? `Connected, ${sec}` : expandedNet === n.name ? (n.secured ? 'Secured' : 'Open') : '';
          const auto = h('input', { type: 'checkbox', class: 'checkbox' });
          auto.checked = true;
          const body = isOn
            ? [h('button', { class: 'hyperlink', type: 'button', text: 'Properties', onclick: () => openSettings('Wi-Fi') }), h('span', { class: 'spacer' }),
              h('button', { class: 'btn', type: 'button', text: 'Disconnect', onclick: () => { connectedNet = null; renderPage(); syncTiles(); } })]
            : [h('label', { class: 'checkbox-row' }, auto, 'Connect automatically'), h('span', { class: 'spacer' }),
              h('button', { class: 'btn accent', type: 'button', text: 'Connect', disabled: !!connecting, onclick: () => connect(n.name) })];
          return connItem({
            name: n.name, iconName: 'wifi', status, expanded: expandedNet === n.name && connecting !== n.name, body,
            onToggle: () => { expandedNet = expandedNet === n.name ? null : n.name; renderPage(); },
          });
        });
      },
    },
    bluetooth: {
      title: 'Bluetooth', key: 'bluetooth', link: ['More Bluetooth settings', 'Bluetooth & devices'],
      list() {
        if (!S().bluetooth) return note('Bluetooth is turned off');
        return devices.map((d) => connItem({
          name: d.name, iconName: 'bluetooth', status: d.connected ? 'Connected' : 'Paired', expanded: expandedDev === d.name,
          body: [h('span', { class: 'spacer' }), h('button', {
            class: d.connected ? 'btn' : 'btn accent', type: 'button', text: d.connected ? 'Disconnect' : 'Connect',
            onclick: () => { d.connected = !d.connected; renderPage(); },
          })],
          onToggle: () => { expandedDev = expandedDev === d.name ? null : d.name; renderPage(); },
        }));
      },
    },
    sound: {
      title: 'Sound output', link: ['More volume settings', 'Sound'],
      list: () => outputs.map((name, i) => selectItem({ name, iconName: 'speaker-2', selected: i === output, onSelect: () => { output = i; renderPage(); } })),
    },
    project: {
      title: 'Project', link: ['More display settings', 'Display'],
      list: () => projectModes.map(([name, ic], i) => selectItem({ name, iconName: ic, selected: i === projectMode, onSelect: () => { projectMode = i; renderPage(); } })),
    },
    accessibility: {
      title: 'Accessibility', link: ['More accessibility settings', 'Accessibility'],
      list: () => a11y.map((a) => {
        const sw = switchEl(a.label, a11yOn.has(a.id), (on) => { on ? a11yOn.add(a.id) : a11yOn.delete(a.id); });
        const head = h('div', { class: 'qs-item-head', onclick: (e) => { if (!sw.contains(e.target)) sw.querySelector('input').click(); } },
          icon(a.icon), h('span', { class: 'qs-item-text' }, h('span', { class: 'qs-item-name', text: a.label })), sw);
        return h('div', { class: 'qs-item', role: 'listitem' }, head);
      }),
    },
    cast: {
      title: 'Cast', link: ['More display settings', 'Display'],
      list() {
        const msg = note('Searching for devices', h('progress', { class: 'progress-bar', 'aria-label': 'Searching' }));
        const t = later(() => msg.replaceChildren('No devices found. Make sure your wireless display or adapter is turned on and nearby.'), 4000);
        pageCleanup = () => clearTimeout(t);
        return msg;
      },
    },
  };

  function connect(name) {
    connecting = name;
    renderPage();
    later(() => {
      connecting = null;
      connectedNet = name;
      expandedNet = name;
      renderPage();
      syncTiles();
    }, 1200);
  }

  function buildSub(name) {
    const def = PAGES[name];
    const header = h('header', { class: 'qs-sub-header' },
      iconBtn('chevron-left', 'Back', () => showPage(null)),
      h('h2', { class: 'fly-title', text: def.title }),
      def.key ? switchEl(def.title, !!S()[def.key], (on) => Shell.set(def.key, on)) : null);
    return h('div', { class: 'qs-page qs-sub', 'data-page': name }, header, h('div', { class: 'qs-list', role: 'list', 'aria-label': def.title }));
  }

  /* Re-renders the list of the open subpage, keeping focus on the same row. */
  function renderPage() {
    if (!panel || !page) return;
    const list = pages.querySelector('.qs-sub .qs-list');
    if (!list) return;
    const focused = list.contains(document.activeElement) ? document.activeElement.closest('.qs-item')?.querySelector('.qs-item-name')?.textContent : null;
    pageCleanup?.(); pageCleanup = null;
    const items = PAGES[page].list();
    list.replaceChildren(...[items].flat());
    const sw = pages.querySelector('.qs-sub-header input[role="switch"]');
    if (sw && PAGES[page].key) sw.checked = !!S()[PAGES[page].key];
    if (focused) [...list.querySelectorAll('.qs-item-name')].find((n) => n.textContent === focused)?.closest('.qs-item-head')?.focus();
  }

  /* Swaps the main page and a subpage: the new page slides in from its side while the
     flyout's height animates to fit it, as Windows does. */
  function showPage(name, { instant = false } = {}) {
    if (!panel) return;
    if (editing) setEditing(false);
    const forward = !!name;
    const old = pages.firstElementChild;
    const from = pages.offsetHeight;
    pageCleanup?.(); pageCleanup = null;
    page = name;
    const next = name ? buildSub(name) : buildMain();
    if (old) {
      old.style.cssText = 'position:absolute;left:0;right:0;top:0';
      old.inert = true;
      Shell.animate(old, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: `translateX(${forward ? -40 : 40}px)` }],
        { duration: instant ? 0 : 167, easing: Shell.ease.out, fill: 'forwards' }).then(() => old.remove());
    }
    pages.scrollTop = 0;
    pages.append(next);
    if (name) renderPage();
    renderFooter();
    syncTiles();
    const to = next.offsetHeight;
    if (!instant) {
      Shell.animate(pages, [{ height: `${from}px` }, { height: `${to}px` }], { duration: 250, easing: Shell.ease.standard });
      Shell.animate(next, [{ opacity: 0, transform: `translateX(${forward ? 40 : -40}px)` }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    }
    (next.querySelector('.qs-sub-header .fly-icon-btn') || next.querySelector('.qs-tile-btn'))?.focus({ preventScroll: true });
  }

  /* Tab cycles inside the open flyout, as Windows does: the next (or previous) tabbable element
     in document order, wrapping at either end, so focus never reaches the desktop behind it. */
  function trapTab(e, root) {
    const list = [...root.querySelectorAll('button, input, select, textarea, a[href], [tabindex]')]
      .filter((el) => el.tabIndex >= 0 && !el.disabled && !el.closest('[inert]') && el.getClientRects().length);
    if (!list.length) return;
    e.preventDefault();
    const cur = document.activeElement;
    const F = Node.DOCUMENT_POSITION_FOLLOWING;
    const next = !root.contains(cur) ? null
      : e.shiftKey ? list.filter((el) => el.compareDocumentPosition(cur) & F).at(-1)
        : list.find((el) => cur.compareDocumentPosition(el) & F);
    (next || (e.shiftKey ? list.at(-1) : list[0])).focus();
  }

  /* ---------- open / close ---------- */
  function onKey(e) {
    if (!panel || e.defaultPrevented) return;
    if (e.key === 'Tab' && !e.altKey && !e.ctrlKey && !e.metaKey) { trapTab(e, panel); return; }
    if (e.key === 'Escape') {
      e.preventDefault();
      if (editing) setEditing(false);
      else if (page) showPage(null);
      else close();
      return;
    }
    if (e.key.startsWith('Arrow') && document.activeElement?.classList.contains('qs-tile-btn')) {
      e.preventDefault();
      moveTileFocus(e.key);
    }
  }

  /* Arrow keys walk the tile grid: left/right in reading order, up/down to the nearest
     button in the next row. */
  function moveTileFocus(key) {
    const btns = [...panel.querySelectorAll('.qs-tile-btn')];
    const cur = document.activeElement;
    const i = btns.indexOf(cur);
    if (key === 'ArrowLeft' || key === 'ArrowRight') { btns[i + (key === 'ArrowRight' ? 1 : -1)]?.focus(); return; }
    const c = cur.getBoundingClientRect();
    const cx = c.left + c.width / 2, cy = c.top + c.height / 2;
    let best = null, bestD = Infinity;
    for (const b of btns) {
      const r = b.getBoundingClientRect();
      const dy = r.top + r.height / 2 - cy;
      if (key === 'ArrowDown' ? dy < 8 : dy > -8) continue;
      const d = Math.abs(dy) * 1000 + Math.abs(r.left + r.width / 2 - cx);
      if (d < bestD) { bestD = d; best = b; }
    }
    best?.focus();
  }

  function open() {
    if (panel) return;
    returnFocus = document.activeElement;
    page = null;
    editing = false;
    pages = h('div', { class: 'qs-pages' });
    footer = h('footer', { class: 'qs-footer' });
    panel = h('section', { class: 'qs acrylic surface-flyout', role: 'dialog', 'aria-label': 'Quick settings', tabindex: '-1' }, pages, footer);
    host = h('div', { class: 'fly-host' }, panel);
    Shell.layer('flyouts').append(host);
    pages.append(buildMain());
    renderFooter();
    syncTiles();
    /* clicks inside a menu opened from the flyout (edit mode's Add) keep it open */
    Shell.transient.open(ID, { close: hide, elements: [panel, (e) => !!e.target.closest?.('#menus')], group: 'flyout' });
    window.addEventListener('keydown', onKey);
    Shell.animate(panel, [{ opacity: 0, transform: 'translateY(40px)' }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    (panel.querySelector('.qs-tile-btn') || panel).focus({ preventScroll: true });
  }

  /* Called by Shell.transient (light dismiss, another flyout opening, or close()). */
  function hide() {
    const p = panel, ph = host;
    if (!p) return;
    host = panel = pages = footer = null;
    window.removeEventListener('keydown', onKey);
    pageCleanup?.(); pageCleanup = null;
    timers.forEach(clearTimeout); timers.clear();
    connecting = null;
    if (editing) Shell.transient.closeAll('menu'); // the Add menu
    p.inert = true;
    if (p.contains(document.activeElement) && returnFocus?.isConnected) returnFocus.focus({ preventScroll: true });
    Shell.animate(p, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateY(40px)' }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' })
      .then(() => ph.remove());
  }

  function close() { if (Shell.transient.isOpen(ID)) Shell.transient.close(ID); else hide(); }

  Shell.quickSettings = {
    open,
    close,
    toggle() { if (panel) close(); else open(); },
    isOpen: () => !!panel,
  };

  /* ---------- screen filter: brightness dims, night light warms (in #overlays) ---------- */
  let warm = null, dim = null;
  function applyFilter() {
    if (!warm) return;
    warm.classList.toggle('on', !!S().nightLight);
    /* 100 % is the page's own colours; 0 % dims to 40 %. Quadratic, so the default 80 % only
       costs ~2 % and the low end darkens like a backlight. */
    dim.style.opacity = (0.6 * ((100 - S().brightness) / 100) ** 2).toFixed(3);
  }
  function mountFilter() {
    const f = h('div', { class: 'screen-filter', 'aria-hidden': 'true' });
    /* Night light at Windows' default 48 % strength: blue down to ~58 %, green to ~82 %. */
    f.innerHTML = '<svg width="0" height="0" style="position:absolute"><filter id="pw-night-light" color-interpolation-filters="sRGB">'
      + '<feColorMatrix type="matrix" values="1 0 0 0 0  0 0.82 0 0 0  0 0 0.58 0 0  0 0 0 1 0"/></filter></svg>'
      + '<div class="screen-filter-warm"></div><div class="screen-filter-dim"></div>';
    warm = f.querySelector('.screen-filter-warm');
    dim = f.querySelector('.screen-filter-dim');
    Shell.layer('overlays').append(f);
    applyFilter();
  }

  Shell.keys.on('Win+A', () => Shell.quickSettings.toggle());

  Shell.on('setting', ({ key }) => {
    if (key === 'brightness' || key === 'nightLight') applyFilter();
    if (!panel) return;
    syncTiles();
    syncSliders();
    if (key === 'energySaver' && !page && !editing) renderFooter();
    if (page && (PAGES[page].key === key)) renderPage();
  });

  Shell.on('boot', () => {
    mountFilter();
    if (Shell.debugOpen.includes('quicksettings') || Shell.debugOpen.includes('quicksettings-wifi')) open();
    if (Shell.debugOpen.includes('quicksettings-wifi')) { expandedNet = connectedNet; showPage('wifi', { instant: true }); }
  });
})();
