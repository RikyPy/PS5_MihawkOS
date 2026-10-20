/* Popups: Windows 11's hardware-key indicator and the emoji / clipboard panel.
   Shell.osd.show('volume'|'brightness'|'mute', value): the 22H2 flyout (acrylic pill centred above
   the taskbar: glyph, bar, value), shown by AudioVolumeUp/Down/Mute and BrightnessUp/Down and
   hidden 2 s after the last change.
   Shell.clipboardPanel.open('clipboard'|'emoji'|'kaomoji'|'symbols') (Win+V, Win+., Win+;): the
   combined panel with icon tabs (Emoji, GIF, Kaomoji, Symbols, Clipboard history), a search box,
   the content and a category strip, placed under the text caret of the focused field (else at
   the pointer). Choosing an emoji, kaomoji or symbol inserts it and keeps the panel open; choosing
   a clip pastes it and closes the panel, as Windows does.
   Owns the clipboard history: text copies (copy/cut events, navigator.clipboard.writeText), at
   most 25, with pin, delete and Clear all; pinned clips survive a reload, the rest do not
   (Windows clears them on restart). File copies are not recorded: Windows keeps only text, HTML
   and bitmaps in the history (CF_HDROP never shows in Win+V), although CONTRACT.md asks for them.
   Settings key read: clipboardHistory (default on). Store keys owned: clipboard-pins, emoji-recent.
   Contract: CONTRACT.md, "popups". */
(() => {
  'use strict';

  const Shell = window.Shell;
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(v, hi));

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
  /* Object glyphs are drawn black; css/popups.css inverts them in dark theme (they load as <img>). */
  const glyph = (name, size = 16) => Shell.icon(name, { size, className: name.startsWith('object/') ? 'cp-glyph' : '' });

  /* ======================================================================================
     Hardware-key indicator (OSD)
     ====================================================================================== */
  const OSD_MS = 2000;
  let osd = null, osdTimer = 0, osdPinned = false;

  const volumeGlyph = (v, muted) =>
    muted || v === 0 ? 'fluent/speaker-mute' : v < 34 ? 'fluent/speaker-1' : 'fluent/speaker-2'; // Volume1 / Volume2-3; no three-wave Fluent glyph here

  function showOsd(kind = 'volume', value) {
    const s = Shell.settings, bright = kind === 'brightness';
    const v = Math.round(clamp(Number(value ?? (bright ? s.brightness : s.volume)) || 0, 0, 100));
    const muted = !bright && (kind === 'mute' || !!s.muted);
    if (!osd) {
      osd = h('div', { class: 'osd acrylic surface-flyout', role: 'status' },
        h('span', { class: 'osd-glyph' }),
        h('span', { class: 'osd-bar' }, h('span', { class: 'osd-fill' })),
        h('span', { class: 'osd-value' }));
      Shell.layer('flyouts').append(osd);
      Shell.animate(osd, [{ opacity: 0, transform: 'translateY(12px)' }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    } else if (osd.classList.contains('leaving')) {
      osd.classList.remove('leaving');
      osd.getAnimations().forEach((a) => a.cancel());
    }
    const name = bright ? 'fluent/brightness' : volumeGlyph(v, muted);
    const slot = osd.querySelector('.osd-glyph');
    if (slot.dataset.icon !== name) { slot.dataset.icon = name; slot.replaceChildren(Shell.icon(name, { size: 16 })); }
    osd.classList.toggle('muted', muted);
    osd.style.setProperty('--osd-value', `${v}%`);
    osd.querySelector('.osd-value').textContent = v;
    osd.setAttribute('aria-label', `${bright ? 'Brightness' : muted ? 'Volume, muted' : 'Volume'} ${v}`);
    clearTimeout(osdTimer);
    if (!osdPinned) osdTimer = setTimeout(hideOsd, OSD_MS);
  }

  function hideOsd() {
    clearTimeout(osdTimer);
    const node = osd;
    if (!node || node.classList.contains('leaving')) return;
    node.classList.add('leaving');
    Shell.animate(node, [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateY(12px)' }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' })
      .then(() => { if (!node.classList.contains('leaving')) return; node.remove(); if (osd === node) osd = null; });
  }

  Shell.osd = { show: showOsd, hide: hideOsd };

  /* Windows steps the volume by 2 and unmutes on any volume key; brightness keys step by 10. */
  function stepSetting(key, delta) {
    const s = Shell.settings;
    if (key === 'volume' && s.muted) Shell.set('muted', false);
    const v = clamp(Math.round((s[key] ?? 0) + delta), 0, 100);
    if (v !== s[key]) Shell.set(key, v);
    showOsd(key, v);
  }
  function toggleMute() {
    const muted = !Shell.settings.muted;
    Shell.set('muted', muted);
    showOsd(muted ? 'mute' : 'volume', Shell.settings.volume);
  }
  // 'Volume*' are the older KeyboardEvent.key names some browsers still report
  ['AudioVolumeUp', 'VolumeUp'].forEach((k) => Shell.keys.on(k, () => stepSetting('volume', 2)));
  ['AudioVolumeDown', 'VolumeDown'].forEach((k) => Shell.keys.on(k, () => stepSetting('volume', -2)));
  ['AudioVolumeMute', 'VolumeMute'].forEach((k) => Shell.keys.on(k, toggleMute));
  Shell.keys.on('BrightnessUp', () => stepSetting('brightness', 10));
  Shell.keys.on('BrightnessDown', () => stepSetting('brightness', -10));

  /* ======================================================================================
     Panel content: emoji (Fluent Emoji art, see emojiArt), kaomoji and symbols (text)
     ====================================================================================== */
  const parseNamed = (str) => str.split('|').map((e) => { const i = e.indexOf(' '); return { text: e.slice(0, i), name: e.slice(i + 1) }; });
  const EMOJI = [
    ['smileys', 'Smiley faces and animals', 'object/glyph-emoji', '😀 grinning face|😃 grinning face with big eyes|😄 grinning face with smiling eyes|😁 beaming face|😆 laughing|😅 grinning face with sweat|🤣 rolling on the floor laughing|😂 face with tears of joy|🙂 slightly smiling face|🙃 upside-down face|😉 winking face|😊 smiling face with smiling eyes|😇 smiling face with halo|🥰 smiling face with hearts|😍 heart eyes|🤩 star-struck|😘 face blowing a kiss|😋 face savoring food|😛 face with tongue|😜 winking face with tongue|🤪 zany face|🤔 thinking face|🤨 raised eyebrow|😐 neutral face|😑 expressionless face|😶 face without mouth|😏 smirking face|😒 unamused face|🙄 face with rolling eyes|😬 grimacing face|😌 relieved face|😔 pensive face|😪 sleepy face|😴 sleeping face|😷 face with medical mask|🤒 face with thermometer|🤯 exploding head|🥳 partying face|😎 smiling face with sunglasses|🤓 nerd face|😕 confused face|😟 worried face|😮 face with open mouth|😲 astonished face|😳 flushed face|🥺 pleading face|😢 crying face|😭 loudly crying face|😱 face screaming in fear|😤 face with steam from nose|😡 pouting face|🤬 face with symbols on mouth|😈 smiling face with horns|💀 skull|💩 pile of poo|🤡 clown face|👻 ghost|👽 alien|🤖 robot|😺 grinning cat|🐶 dog face|🐱 cat face|🐭 mouse face|🐹 hamster|🐰 rabbit face|🦊 fox|🐻 bear|🐼 panda|🐨 koala|🐯 tiger face|🦁 lion|🐮 cow face|🐷 pig face|🐸 frog|🐵 monkey face|🐔 chicken|🐧 penguin|🐦 bird|🦉 owl|🦋 butterfly|🐝 honeybee|🐢 turtle|🐍 snake|🐙 octopus|🐬 dolphin|🐳 spouting whale|🦈 shark'],
    ['people', 'People', 'fluent/person', '👋 waving hand|🤚 raised back of hand|✋ raised hand|🖖 vulcan salute|👌 OK hand|🤌 pinched fingers|✌️ victory hand|🤞 crossed fingers|🤟 love-you gesture|🤘 sign of the horns|🤙 call me hand|👈 backhand index pointing left|👉 backhand index pointing right|👆 backhand index pointing up|👇 backhand index pointing down|👍 thumbs up|👎 thumbs down|✊ raised fist|👊 oncoming fist|👏 clapping hands|🙌 raising hands|👐 open hands|🤝 handshake|🙏 folded hands|💪 flexed biceps|👀 eyes|🧠 brain|👶 baby|🧒 child|👦 boy|👧 girl|🧑 person|👨 man|👩 woman|🧓 older person|👴 old man|👵 old woman|🙋 person raising hand|🤷 person shrugging|🤦 person facepalming|🙇 person bowing|💁 person tipping hand|🧑‍💻 technologist|🧑‍🎨 artist|🧑‍🚀 astronaut|🧑‍🍳 cook|🏃 person running|💃 woman dancing|🕺 man dancing|🧘 person in lotus position|🏄 person surfing|🚴 person biking'],
    ['celebrations', 'Celebrations and objects', 'object/glyph-celebration', '🎉 party popper|🎊 confetti ball|🎈 balloon|🎁 wrapped gift|🎂 birthday cake|🎄 Christmas tree|🎃 jack-o-lantern|🎆 fireworks|🎇 sparkler|✨ sparkles|🎀 ribbon|🏆 trophy|🥇 1st place medal|🎮 video game|🕹️ joystick|🎲 game die|🧩 puzzle piece|🎧 headphone|🎵 musical note|🎸 guitar|🎹 musical keyboard|📷 camera|📱 mobile phone|💻 laptop|🖥️ desktop computer|⌨️ keyboard|🖱️ computer mouse|💾 floppy disk|💡 light bulb|🔦 flashlight|📚 books|📝 memo|✏️ pencil|📌 pushpin|📎 paperclip|🔑 key|🔒 locked|🔧 wrench|🔨 hammer|⚙️ gear|🧲 magnet|💰 money bag|💎 gem stone|⏰ alarm clock|⌛ hourglass done|📦 package|✉️ envelope|🔔 bell'],
    ['food', 'Food and plants', 'object/glyph-food', '🍎 red apple|🍐 pear|🍊 tangerine|🍋 lemon|🍌 banana|🍉 watermelon|🍇 grapes|🍓 strawberry|🍒 cherries|🍑 peach|🥭 mango|🍍 pineapple|🥥 coconut|🥝 kiwi fruit|🍅 tomato|🥑 avocado|🥦 broccoli|🥕 carrot|🌽 ear of corn|🌶️ hot pepper|🍞 bread|🧀 cheese wedge|🍳 cooking|🥞 pancakes|🍔 hamburger|🍟 french fries|🍕 pizza|🌭 hot dog|🌮 taco|🍣 sushi|🍜 steaming bowl|🍦 soft ice cream|🍩 doughnut|🍪 cookie|🍫 chocolate bar|🍿 popcorn|☕ hot beverage|🍵 teacup without handle|🧃 beverage box|🍺 beer mug|🍷 wine glass|🌸 cherry blossom|🌹 rose|🌻 sunflower|🌷 tulip|🌱 seedling|🌲 evergreen tree|🌴 palm tree|🌵 cactus|🍀 four leaf clover|🍁 maple leaf|🍄 mushroom'],
    ['travel', 'Transportation and places', 'object/glyph-vehicle', '🚗 automobile|🚕 taxi|🚙 sport utility vehicle|🚌 bus|🚎 trolleybus|🏎️ racing car|🚓 police car|🚑 ambulance|🚒 fire engine|🚚 delivery truck|🚲 bicycle|🛵 motor scooter|🏍️ motorcycle|🚂 locomotive|🚄 high-speed train|🚇 metro|✈️ airplane|🛫 airplane departure|🚁 helicopter|🚀 rocket|🛸 flying saucer|⛵ sailboat|🚢 ship|⚓ anchor|🗺️ world map|🧭 compass|🏔️ snow-capped mountain|🌋 volcano|🏕️ camping|🏖️ beach with umbrella|🏝️ desert island|🏠 house|🏡 house with garden|🏢 office building|🏰 castle|🗼 Tokyo tower|🗽 Statue of Liberty|🌉 bridge at night|🌃 night with stars|🌅 sunrise|🌇 sunset|🌍 globe showing Europe-Africa|🌙 crescent moon|☀️ sun|⭐ star|☁️ cloud|🌈 rainbow|⚡ high voltage|❄️ snowflake|🔥 fire|💧 droplet|🌊 water wave'],
    ['symbols', 'Symbols', 'object/glyph-heart', '❤️ red heart|🧡 orange heart|💛 yellow heart|💚 green heart|💙 blue heart|💜 purple heart|🖤 black heart|🤍 white heart|🤎 brown heart|💔 broken heart|💕 two hearts|💞 revolving hearts|💖 sparkling heart|💯 hundred points|💢 anger symbol|💬 speech balloon|💭 thought balloon|💤 zzz|✅ check mark button|☑️ check box with check|✔️ check mark|❌ cross mark|❎ cross mark button|➕ plus|➖ minus|➗ divide|❓ red question mark|❗ red exclamation mark|‼️ double exclamation mark|⚠️ warning|🚫 prohibited|⛔ no entry|♻️ recycling symbol|🔴 red circle|🟠 orange circle|🟡 yellow circle|🟢 green circle|🔵 blue circle|🟣 purple circle|⚫ black circle|⚪ white circle|🔺 red triangle pointed up|🔻 red triangle pointed down|▶️ play button|⏸️ pause button|⏹️ stop button|🔁 repeat button|🔀 shuffle tracks button|🔊 speaker high volume|🔇 muted speaker|🆗 OK button|🆕 NEW button|🆒 COOL button|©️ copyright|®️ registered|™️ trade mark'],
  ].map(([id, label, icon, data]) => ({ id, label, icon, items: parseNamed(data) }));
  const emojiByText = new Map(EMOJI.flatMap((s) => s.items.map((it) => [it.text, it])));

  const KAOMOJI = [
    ['classic', 'Classic ASCII emoticons', [':-)', ':-(', ';-)', ':-D', ':-P', ':-O', ':-|', ':-/', ":'(", 'XD', '<3', '^_^', '^^;', '>_<', 'o_O', 'T_T', '-_-', '(^_^)v']],
    ['greeting', 'Greeting', ['ヾ(•ω•`)o', '(＾▽＾)ﾉ', '( ´ ▽ ` )ﾉ', '(*・ω・)ﾉ', '(^_^)/', '＼(^o^)／', 'ヽ(・∀・)ﾉ', '(✿◠‿◠)', '(・ω・)ノ', '(o・ω・)ﾉ']],
    ['happy', 'Happy', ['(◕‿◕)', '(＾▽＾)', '(*^▽^*)', '(≧▽≦)', '(´▽`)', '(*≧ω≦)', '٩(◕‿◕)۶', '(ﾉ◕ヮ◕)ﾉ*:･ﾟ✧', '(￣▽￣)', '(^人^)', 'ヽ(´▽`)/', '(☆▽☆)']],
    ['sad', 'Sad', ['(╥﹏╥)', '(T_T)', '(;_;)', '(ಥ﹏ಥ)', '(´；ω；`)', '( ; ω ; )', '(｡•́︿•̀｡)', '(◞‸◟)', '(._.)']],
    ['angry', 'Angry', ['(╬ Ò﹏Ó)', '(＃`Д´)', '(╯°□°）╯︵ ┻━┻', 'ヽ(`Д´)ﾉ', '(ノಠ益ಠ)ノ', '(눈_눈)', '(¬_¬)', '(｀ε´)']],
    ['surprised', 'Surprised', ['(⊙_⊙)', '(°o°)', '(O_O)', '(ﾟДﾟ)', '(・□・;)', 'Σ(°△°|||)', '(◎_◎;)', '(゜゜)']],
    ['other', 'Other', ['¯\\_(ツ)_/¯', '┬─┬ノ( º _ ºノ)', '( ͡° ͜ʖ ͡°)', '(ง •̀_•́)ง', 'ʕ•ᴥ•ʔ', '(=^･ω･^=)', '(￣ー￣)ｂ', '(•̀ᴗ•́)و']],
  ].map(([id, label, list]) => ({ id, label, icon: list[0], items: list.map((text) => ({ text, name: label.toLowerCase() })) }));

  const SYMBOLS = [
    ['punctuation', 'General punctuation', '… ‚ „ “ ” ‘ ’ « » ‹ › ¡ ¿ ‐ – — · • † ‡ § ¶ ‰ ′ ″ ※ ‽ ⁂'],
    ['currency', 'Currency symbols', '$ € £ ¥ ¢ ₹ ₽ ₩ ₪ ₫ ₺ ₱ ₦ ₴ ₿ ฿ ₡ ₲ ₵ ₸ ¤ ƒ'],
    ['latin', 'Latin symbols', 'À Á Â Ã Ä Å Æ Ç È É Ê Ë Ì Í Î Ï Ñ Ò Ó Ô Õ Ö Ø Œ Ù Ú Û Ü Ý ß à á â ã ä å æ ç è é ê ë ñ ö ø œ ü ÿ'],
    ['geometric', 'Geometric symbols', '← ↑ → ↓ ↔ ↕ ↖ ↗ ↘ ↙ ⇐ ⇒ ⇔ ■ □ ▪ ▫ ▲ △ ▼ ▽ ◆ ◇ ○ ● ◎ ★ ☆ ♠ ♣ ♥ ♦ ♪ ♫'],
    ['math', 'Math symbols', '+ − × ÷ = ≠ ≈ ≡ < > ≤ ≥ ± ∓ ∞ √ ∛ ∑ ∏ ∫ ∂ ∆ ∇ ∈ ∉ ∩ ∪ ⊂ ⊃ ⊆ ⊇ ∧ ∨ ¬ ∀ ∃ ∅ ° ² ³ ½ ¼ ¾ π'],
    ['supplemental', 'Supplemental symbols', '© ® ™ ℃ ℉ № ℗ ✓ ✗ ✕ ✦ ✧ ☐ ☑ ☒ ☼ ☾ ☯ ☮ ♀ ♂ ⚐ ⚑'],
    ['language', 'Language symbols', 'α β γ δ ε ζ η θ ι κ λ μ ν ξ ο π ρ σ τ υ φ χ ψ ω Γ Δ Θ Λ Ξ Π Σ Φ Ψ Ω'],
  ].map(([id, label, list]) => {
    const items = list.split(' ').map((text) => ({ text, name: label.toLowerCase() }));
    return { id, label, icon: items[0].text, items };
  });

  const DEFAULT_RECENT = ['😂', '❤️', '😍', '🤣', '😊', '🙏', '💕', '😭', '😘', '👍', '😅', '👏', '😁', '🔥', '🥰', '💔'];
  let recents = Shell.store.get('emoji-recent', DEFAULT_RECENT).filter((t) => emojiByText.has(t));
  function addRecent(text) {
    if (!emojiByText.has(text)) return;
    recents = [text, ...recents.filter((t) => t !== text)].slice(0, 24);
    Shell.store.set('emoji-recent', recents);
  }

  function setsOf(kind) {
    if (kind === 'emoji') return [{ id: 'recent', label: 'Most recently used', icon: 'fluent/history', items: recents.map((t) => emojiByText.get(t)) }, ...EMOJI];
    return kind === 'kaomoji' ? KAOMOJI : kind === 'symbols' ? SYMBOLS : [];
  }

  /* ======================================================================================
     Clipboard history
     ====================================================================================== */
  const MAX_CLIPS = 25;
  const historyOn = () => Shell.settings.clipboardHistory !== false;
  let clips = (Shell.store.get('clipboard-pins', []) || []).filter((c) => c && c.pinned && c.id && c.kind === 'text'); // drops file pins saved by earlier builds
  let clipSeq = 0;
  const savePins = () => Shell.store.set('clipboard-pins', clips.filter((c) => c.pinned));

  function addClip(clip) {
    if (!historyOn()) return;
    const same = (c) => c.text === clip.text;
    const old = clips.find(same);
    clips = [old ? { ...old, time: Date.now() } : { id: `clip-${Date.now().toString(36)}-${clipSeq++}`, time: Date.now(), pinned: false, ...clip }, ...clips.filter((c) => c !== old)];
    /* Windows keeps 25 entries; the oldest unpinned one goes first */
    while (clips.length > MAX_CLIPS) {
      const i = clips.findLastIndex((c) => !c.pinned);
      if (i < 0) break;
      clips.splice(i, 1);
    }
    if (old?.pinned) savePins();
    if (panel && tab === 'clipboard') render();
  }

  const TEXT_TYPES = /^(text|search|url|email|tel)$/i; // never password fields
  const isField = (el) => !!el && (el.tagName === 'TEXTAREA' || (el.tagName === 'INPUT' && TEXT_TYPES.test(el.type)));
  const editable = (el) => !!el && ((isField(el) && !el.disabled && !el.readOnly) || el.isContentEditable);

  function onCopy(e) {
    const el = document.activeElement;
    let text = e.clipboardData?.getData('text/plain') || '';
    if (!text && !e.defaultPrevented) {
      text = isField(el) ? el.value.slice(el.selectionStart ?? 0, el.selectionEnd ?? 0) : String(window.getSelection() || '');
    }
    if (text) addClip({ kind: 'text', text });
  }
  document.addEventListener('copy', onCopy);
  document.addEventListener('cut', onCopy);

  // shortcut: wraps navigator.clipboard.writeText so programmatic copies (Copy as path, Terminal) reach the history; a module that writes the clipboard another way is not recorded.
  const nativeClipboard = navigator.clipboard;
  const writeClipboard = nativeClipboard?.writeText ? nativeClipboard.writeText.bind(nativeClipboard) : null;
  if (writeClipboard) {
    try {
      nativeClipboard.writeText = (text) => { if (text) addClip({ kind: 'text', text: String(text) }); return writeClipboard(text); };
    } catch { /* read-only: programmatic copies stay out of the history */ }
  }
  const copyText = (text) => { navigator.clipboard?.writeText?.(text)?.catch?.(() => {}); };

  /* ======================================================================================
     The panel
     ====================================================================================== */
  const ID = 'clipboard-panel';
  const TABS = [
    { id: 'emoji', label: 'Emoji', icon: 'object/glyph-emoji' },
    { id: 'gif', label: 'GIF', icon: 'object/glyph-gif' },
    { id: 'kaomoji', label: 'Kaomoji', icon: 'object/glyph-kaomoji' },
    { id: 'symbols', label: 'Symbols', icon: 'object/glyph-symbols' },
    { id: 'clipboard', label: 'Clipboard history', icon: 'fluent/clipboard-paste' },
  ];
  let panel = null, tabsBar = null, searchRow = null, search = null, body = null, strip = null;
  let tab = 'emoji', target = null, savedRange = null, returnFocus = null, anchor = null, expanded = null, spyMuteUntil = 0;
  let pointer = null;
  document.addEventListener('pointermove', (e) => { pointer = { x: e.clientX, y: e.clientY }; }, { passive: true });

  /* The caret of an input or textarea, measured on a hidden copy of the field's text. */
  const MIRROR = ['boxSizing', 'width', 'height', 'paddingTop', 'paddingRight', 'paddingBottom', 'paddingLeft', 'borderTopWidth',
    'borderRightWidth', 'borderBottomWidth', 'borderLeftWidth', 'borderStyle', 'fontFamily', 'fontSize', 'fontWeight', 'fontStyle',
    'letterSpacing', 'lineHeight', 'textTransform', 'textIndent', 'tabSize', 'wordSpacing'];
  function caretRect(el) {
    const r = el.getBoundingClientRect();
    try {
      if (isField(el)) {
        const cs = getComputedStyle(el), area = el.tagName === 'TEXTAREA';
        const m = document.createElement('div');
        MIRROR.forEach((p) => { m.style[p] = cs[p]; });
        Object.assign(m.style, { position: 'fixed', left: `${r.left}px`, top: `${r.top}px`, visibility: 'hidden', overflow: 'hidden',
          whiteSpace: area ? 'pre-wrap' : 'pre', overflowWrap: area ? 'break-word' : 'normal' });
        m.textContent = el.value.slice(0, el.selectionEnd ?? 0);
        const mark = m.appendChild(document.createElement('span'));
        mark.textContent = '\u200b';
        document.body.append(m);
        const k = mark.getBoundingClientRect();
        m.remove();
        const x = clamp(k.left - el.scrollLeft, r.left, r.right), y = clamp(k.top - el.scrollTop, r.top, r.bottom);
        return { left: x, right: x, top: y, bottom: clamp(y + k.height, y, r.bottom) };
      }
      const sel = window.getSelection();
      if (sel.rangeCount && el.contains(sel.anchorNode)) {
        const range = sel.getRangeAt(0), k = range.getClientRects()[0] || range.getBoundingClientRect();
        if (k && (k.width || k.height)) return k;
      }
    } catch { /* fall back to the field */ }
    return { left: r.left, right: r.left, top: r.top, bottom: r.bottom };
  }

  function saveRange() {
    const sel = window.getSelection();
    savedRange = target && !isField(target) && sel.rangeCount && target.contains(sel.anchorNode) ? sel.getRangeAt(0).cloneRange() : null;
  }

  /* Types into the field the panel was opened for, through the editing command so the field's own
     undo stack and input events see it. Fields keep their selection while they are blurred. */
  function insertText(text) {
    if (!target?.isConnected || !editable(target)) return false;
    target.focus({ preventScroll: true });
    if (savedRange) { const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(savedRange); }
    if (!document.execCommand('insertText', false, text) && isField(target)) {
      target.setRangeText(text, target.selectionStart, target.selectionEnd, 'end');
      target.dispatchEvent(new Event('input', { bubbles: true }));
    }
    return true;
  }

  function markSelected(bar, pick) {
    [...bar.children].forEach((b) => {
      if (b.classList.contains('ui-pill')) return;
      const on = pick(b);
      b.classList.toggle('selected', on);
      b.setAttribute('aria-selected', String(on));
      b.tabIndex = on ? 0 : -1;
    });
  }

  function build() {
    tabsBar = h('div', { class: 'cp-tabs segmented', role: 'tablist', 'aria-label': 'Panel sections' },
      TABS.map((t) => h('button', { type: 'button', role: 'tab', class: 'cp-tab', 'data-tab': t.id, 'data-nav': 'tab', tabindex: '-1', 'aria-label': t.label, 'data-tooltip': t.label }, glyph(t.icon))));
    tabsBar.addEventListener('select', (e) => selectTab(e.detail.item.dataset.tab));
    search = h('input', { class: 'searchbox cp-search', type: 'text', placeholder: 'Search', 'aria-label': 'Search', spellcheck: 'false', autocomplete: 'off' });
    Shell.ui?.textbox?.(search, { clear: true });
    search.addEventListener('input', () => { render(); body.scrollTop = 0; });
    searchRow = h('div', { class: 'cp-search-row' }, search);
    body = h('div', { class: 'cp-body', role: 'tabpanel' });
    body.addEventListener('click', onBodyClick);
    body.addEventListener('scroll', spy, { passive: true });
    strip = h('div', { class: 'cp-strip segmented', role: 'tablist', 'aria-label': 'Categories' });
    strip.addEventListener('select', (e) => {
      markSelected(strip, (b) => b === e.detail.item);
      const sec = body.querySelector(`.cp-section[data-set="${e.detail.item.dataset.set}"]`);
      if (sec) { spyMuteUntil = performance.now() + 300; body.scrollTop = sec.offsetTop - 4; }
    });
    panel = h('section', { class: 'cp acrylic surface-flyout', role: 'dialog', 'aria-label': 'Emoji and clipboard history', tabindex: '-1' },
      tabsBar, searchRow, body, strip);
    panel.addEventListener('focusin', (e) => {
      const n = e.target.closest?.('[data-nav]');
      const region = n?.closest('.cp-tabs, .cp-strip, .cp-body');
      if (region) region.querySelectorAll('[data-nav]').forEach((x) => { x.tabIndex = x === n ? 0 : -1; });
    });
  }

  function selectTab(id) {
    const from = TABS.findIndex((t) => t.id === tab), to = TABS.findIndex((t) => t.id === id);
    tab = id;
    expanded = null;
    search.value = '';
    markSelected(tabsBar, (b) => b.dataset.tab === id);
    render();
    body.scrollTop = 0;
    if (from !== to && from >= 0) {
      const dx = Math.sign(to - from) * 24;
      Shell.animate(body, [{ opacity: 0, transform: `translateX(${dx}px)` }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    }
  }

  const note = (title, text, ...extra) =>
    h('div', { class: 'cp-note' }, h('p', { class: 'cp-note-title', text: title }), h('p', { class: 'cp-note-text', text }), ...extra);

  function render() {
    const hadFocus = body.contains(document.activeElement);
    body.replaceChildren();
    strip.replaceChildren();
    strip.hidden = true;
    searchRow.hidden = tab === 'clipboard';
    if (tab === 'clipboard') renderClipboard();
    else if (tab === 'gif') body.append(note("Couldn't load GIFs", 'Check your internet connection and try again.'));
    else renderSets(setsOf(tab));
    const first = body.querySelector('[data-nav]');
    if (first) first.tabIndex = 0;
    if (hadFocus && !body.contains(document.activeElement)) focusFirst();
  }

  function grid(items) {
    return h('div', { class: `cp-grid cp-grid-${tab}`, role: 'group' }, items.map((it) =>
      h('button', { type: 'button', class: 'cp-item', 'data-nav': 'item', 'data-text': it.text, tabindex: '-1', 'aria-label': tab === 'emoji' ? it.name : it.text,
        'data-tooltip': tab === 'emoji' ? it.name : null }, tab === 'emoji' ? emojiArt(it.text) : it.text)));
  }

  /* Emoji cells show Fluent Emoji art (assets/emoji, MIT, the Color style Windows 11 draws); choosing one
     still inserts the character. Files are named by code points without U+FE0F. Missing art falls back
     to the text so the cell never goes blank. */
  function emojiArt(text) {
    const name = [...text].filter((c) => c !== '\ufe0f').map((c) => c.codePointAt(0).toString(16)).join('-');
    const img = h('img', { class: 'cp-emoji', src: `assets/emoji/${name}.svg`, alt: '', width: 24, height: 24, loading: 'lazy', decoding: 'async', draggable: 'false' });
    img.addEventListener('error', () => img.replaceWith(text), { once: true });
    return img;
  }

  function renderSets(sets) {
    const q = search.value.trim().toLowerCase();
    if (q) {
      const seen = new Set();
      const hits = sets.filter((s) => s.id !== 'recent').flatMap((s) => s.items.filter((it) =>
        (it.name.includes(q) || s.label.toLowerCase().includes(q) || it.text === q) && !seen.has(it.text) && seen.add(it.text)));
      body.append(hits.length ? grid(hits) : note('No results found', 'Try a different word.'));
      return;
    }
    for (const s of sets) {
      if (!s.items.length) continue;
      body.append(h('div', { class: 'cp-section', 'data-set': s.id }, h('h3', { class: 'cp-heading', text: s.label }), grid(s.items)));
      strip.append(h('button', { type: 'button', role: 'tab', class: 'cp-cat', 'data-set': s.id, 'data-nav': 'cat', tabindex: '-1', 'aria-label': s.label, 'data-tooltip': s.label },
        /^(fluent|object)\//.test(s.icon) ? glyph(s.icon) : h('span', { class: 'cp-cat-text', text: s.icon })));
    }
    strip.hidden = false;
    const first = strip.firstElementChild;
    markSelected(strip, (b) => b === first);
  }

  /* Category strip follows the scroll: the last section whose top has passed the top edge. */
  function spy() {
    if (strip.hidden || performance.now() < spyMuteUntil) return;
    const secs = [...body.querySelectorAll('.cp-section')];
    let cur = secs[0];
    for (const s of secs) if (s.offsetTop - body.scrollTop <= 8) cur = s;
    if (body.scrollTop + body.clientHeight >= body.scrollHeight - 2) cur = secs[secs.length - 1];
    if (cur) markSelected(strip, (b) => b.dataset.set === cur.dataset.set);
  }

  function renderClipboard() {
    if (!historyOn()) {
      body.append(note('Clipboard history is off', 'To see your clipboard history, turn it on.',
        h('button', { type: 'button', class: 'btn accent', text: 'Turn on', onclick: () => Shell.set('clipboardHistory', true) })));
      return;
    }
    body.append(h('div', { class: 'cp-clip-head' },
      h('span', { class: 'cp-clip-title', text: 'Clipboard history' }),
      h('button', { type: 'button', class: 'btn cp-clear', text: 'Clear all', disabled: !clips.some((c) => !c.pinned), onclick: clearAll })));
    if (!clips.length) {
      body.append(note("You haven't copied anything yet", 'When you copy something, it shows up here.'));
      return;
    }
    body.append(h('div', { class: 'cp-clips', role: 'list' }, clips.map(card)));
  }

  const actionBtn = (cls, icon, label, onclick) =>
    h('button', { type: 'button', class: `btn subtle icon-only ${cls}`, 'data-nav': 'action', tabindex: '-1', 'aria-label': label, 'data-tooltip': label,
      onclick: (e) => { e.stopPropagation(); onclick(); } }, Shell.icon(icon, { size: 16 }));

  function card(c) {
    const open = expanded === c.id;
    const content = h('div', { class: 'cp-clip-text', text: c.text });
    const more = actionBtn('cp-more', 'fluent/more-horizontal', 'See more', () => { expanded = open ? null : c.id; render(); focusClip(c.id, '.cp-more'); });
    more.setAttribute('aria-expanded', String(open));
    const el = h('div', { class: `cp-card${c.pinned ? ' pinned' : ''}`, role: 'listitem', tabindex: '-1', 'data-nav': 'card', 'data-clip': c.id,
      'aria-label': c.text.slice(0, 200) },
    content,
    h('div', { class: 'cp-actions' }, more,
      open && actionBtn('cp-pin', c.pinned ? 'fluent/pin-off' : 'fluent/pin', c.pinned ? 'Unpin' : 'Pin', () => togglePin(c)),
      open && actionBtn('cp-delete', 'fluent/delete', 'Delete', () => removeClip(c)),
      !open && c.pinned && h('span', { class: 'cp-pinned', 'aria-hidden': 'true' }, Shell.icon('fluent/pin', { size: 16 }))));
    return el;
  }

  function focusClip(id, sel) {
    const el = id && body.querySelector(`.cp-card[data-clip="${id}"]`);
    const t = el && sel ? el.querySelector(sel) : el;
    t?.focus({ preventScroll: false });
    return !!t;
  }

  function togglePin(c) {
    c.pinned = !c.pinned;
    savePins();
    render();
    focusClip(c.id, '.cp-pin');
  }

  function removeClip(c) {
    const el = body.querySelector(`.cp-card[data-clip="${c.id}"]`);
    const cards = [...body.querySelectorAll('.cp-card')], i = cards.indexOf(el);
    const nextId = (cards[i + 1] || cards[i - 1])?.dataset.clip;
    clips = clips.filter((x) => x !== c);
    if (c.pinned) savePins();
    expanded = null;
    const done = () => { if (!panel) return; render(); if (!focusClip(nextId)) focusFirst(); };
    if (!el) { done(); return; }
    el.style.overflow = 'hidden';
    Shell.animate(el, [{ opacity: 1, height: `${el.offsetHeight}px` },
      { opacity: 0, height: '0px', minHeight: '0px', paddingTop: '0px', paddingBottom: '0px', borderWidth: '0px', marginBottom: '-4px' }],
    { duration: 167, easing: Shell.ease.out, fill: 'forwards' }).then(done);
  }

  function clearAll() {
    clips = clips.filter((c) => c.pinned);
    expanded = null;
    render();
    focusFirst();
  }

  function chooseClip(c) {
    insertText(c.text);
    copyText(c.text); // the chosen clip becomes the clipboard's content, as Windows does
    close();
  }

  function onBodyClick(e) {
    const item = e.target.closest('.cp-item');
    if (item) {
      const text = item.dataset.text;
      if (tab === 'emoji') addRecent(text);
      if (!insertText(text)) copyText(text); // no field to type into: the panel copies it instead
      if (panel) item.focus({ preventScroll: true }); // the panel stays open for the next one
      return;
    }
    const cardEl = e.target.closest('.cp-card');
    if (cardEl && !e.target.closest('button')) {
      const c = clips.find((x) => x.id === cardEl.dataset.clip);
      if (c) chooseClip(c);
    }
  }

  function focusFirst() {
    if (!panel) return;
    const first = body.querySelector('[data-nav]') || body.querySelector('button:not(:disabled)')
      || (searchRow.hidden ? tabsBar.querySelector('.selected') : search);
    (first || panel).focus({ preventScroll: true });
  }

  /* Up/Down: the nearest element of the same kind on the next row above or below. */
  function vertical(items, el, down) {
    const r = el.getBoundingClientRect(), cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    let best = null, bestD = Infinity;
    for (const n of items) {
      if (n.dataset.nav !== el.dataset.nav) continue;
      const b = n.getBoundingClientRect(), dy = b.top + b.height / 2 - cy;
      if (down ? dy < 4 : dy > -4) continue;
      const d = Math.abs(dy) * 1000 + Math.abs(b.left + b.width / 2 - cx);
      if (d < bestD) { bestD = d; best = n; }
    }
    return best;
  }

  function trapTab(e) {
    const stops = [...panel.querySelectorAll('button, input, [tabindex]')]
      .filter((n) => n.tabIndex >= 0 && !n.disabled && n.getClientRects().length);
    if (!stops.length) return;
    const i = stops.indexOf(document.activeElement);
    const next = e.shiftKey ? (i <= 0 ? stops[stops.length - 1] : null) : (i === stops.length - 1 ? stops[0] : null);
    if (next) { e.preventDefault(); next.focus(); }
  }

  function onKey(e) {
    if (!panel) return;
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); return; }
    const el = document.activeElement;
    if (!panel.contains(el)) return;
    if (e.key === 'Tab') { trapTab(e); return; }
    if (el === search) {
      if (e.key === 'ArrowDown') { e.preventDefault(); body.querySelector('[data-nav]')?.focus(); }
      if (e.key === 'Enter') { e.preventDefault(); body.querySelector('.cp-item')?.click(); }
      return;
    }
    const region = el.closest?.('.cp-tabs, .cp-strip, .cp-body');
    if (!region || !el.dataset.nav) return;
    const items = [...region.querySelectorAll('[data-nav]')].filter((n) => n.getClientRects().length);
    const i = items.indexOf(el), inBody = region === body;
    let next = null;
    if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') next = items[i + (e.key === 'ArrowRight' ? 1 : -1)];
    else if (e.key === 'Home') next = items[0];
    else if (e.key === 'End') next = items[items.length - 1];
    else if (inBody && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) {
      next = vertical(items, el, e.key === 'ArrowDown');
      if (!next && e.key === 'ArrowUp' && !searchRow.hidden) next = search;
    } else if (inBody && el.dataset.nav === 'card') {
      const c = clips.find((x) => x.id === el.dataset.clip);
      if (c && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); chooseClip(c); return; }
      if (c && e.key === 'Delete') { e.preventDefault(); removeClip(c); return; }
    } else if (inBody && !searchRow.hidden && !e.ctrlKey && !e.altKey && !e.metaKey && ((e.key.length === 1 && e.key !== ' ') || e.key === 'Backspace')) {
      search.focus(); // typing searches, as in Windows; the key lands in the box
      return;
    }
    if (!next) return;
    e.preventDefault();
    next.focus();
    if (!inBody && next !== el) next.click(); // tabs and categories follow the focus
  }

  function place() {
    if (!panel) return null;
    if (anchor && Shell.ui?.position) return Shell.ui.position(panel, anchor, 'bottom-start', { gap: 8, margin: 8 }).side;
    const work = innerHeight - (Shell.layer('taskbar')?.offsetHeight || 0);
    panel.style.left = `${Math.round((innerWidth - panel.offsetWidth) / 2)}px`;
    panel.style.top = `${Math.round(Math.max(8, (work - panel.offsetHeight) / 2))}px`;
    return 'bottom';
  }

  function open(which = 'emoji') {
    if (!TABS.some((t) => t.id === which)) which = 'emoji';
    if (panel) { if (which !== tab) selectTab(which); focusFirst(); return; }
    const el = document.activeElement;
    returnFocus = el;
    target = editable(el) ? el : null;
    saveRange();
    anchor = target ? caretRect(target)
      : pointer ? { left: pointer.x, right: pointer.x, top: pointer.y, bottom: pointer.y } : null;
    build();
    tab = which;
    expanded = null;
    markSelected(tabsBar, (b) => b.dataset.tab === which);
    Shell.layer('flyouts').append(panel);
    render();
    const side = place();
    Shell.transient.open(ID, { group: 'input-panel', close: hide, elements: [panel, ...(target ? [target] : [])] });
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('resize', place);
    target?.addEventListener('blur', saveRange);
    Shell.animate(panel, [{ opacity: 0, transform: `translateY(${side === 'top' ? 16 : -16}px)` }, { opacity: 1, transform: 'none' }], { duration: 250, easing: Shell.ease.in });
    focusFirst();
  }

  /* Called by Shell.transient (light dismiss, Esc, a choice) or close(). */
  function hide() {
    const p = panel;
    if (!p) return;
    panel = null;
    window.removeEventListener('keydown', onKey, true);
    window.removeEventListener('resize', place);
    target?.removeEventListener('blur', saveRange);
    if (p.contains(document.activeElement)) {
      const back = target?.isConnected ? target : returnFocus?.isConnected ? returnFocus : null;
      back?.focus({ preventScroll: true });
      if (back === target && savedRange) { const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(savedRange); }
    }
    p.inert = true;
    Shell.tooltip?.hide?.();
    Shell.animate(p, [{ opacity: 1 }, { opacity: 0 }], { duration: 167, easing: Shell.ease.out, fill: 'forwards' }).then(() => p.remove());
  }

  function close() { if (Shell.transient.isOpen(ID)) Shell.transient.close(ID); else hide(); }
  const toggle = (which) => { if (panel && tab === which) close(); else open(which); };

  Shell.clipboardPanel = { open, close, toggle, isOpen: () => !!panel,
    /* Settings > Clipboard > Clear clipboard data: everything but the pinned clips. */
    clear() { if (panel) clearAll(); else { clips = clips.filter((c) => c.pinned); expanded = null; } } };

  Shell.keys.on('Win+V', () => toggle('clipboard'));
  Shell.keys.on('Win+.', () => toggle('emoji'));
  Shell.keys.on('Win+;', () => toggle('emoji'));

  Shell.on('setting', ({ key } = {}) => {
    if (key !== 'clipboardHistory') return;
    if (!historyOn()) clips = clips.filter((c) => c.pinned); // turning history off clears it; pins stay
    if (panel && tab === 'clipboard') { render(); focusFirst(); }
  });

  /* ?open=osd | osd-brightness | emoji | gif | kaomoji | symbols | clipboard (seeds clips) */
  Shell.on('boot', () => setTimeout(() => {
    const want = (n) => Shell.debugOpen.includes(n);
    if (want('osd') || want('osd-brightness')) { osdPinned = true; showOsd(want('osd') ? 'volume' : 'brightness'); }
    if (want('clipboard')) {
      addClip({ kind: 'text', text: 'C:\\Users\\Mihawk\\Documents\\Notes.txt' });
      addClip({ kind: 'text', text: 'Welcome to Mihawk OS.\n\nThis is a prototype of the Mihawk 11 desktop for the PS5 Wine runtime.' });
      if (clips[1]) clips[1].pinned = true;
    }
    const which = ['clipboard', 'emoji', 'gif', 'kaomoji', 'symbols'].find(want);
    if (which) open(which);
  }));
})();
