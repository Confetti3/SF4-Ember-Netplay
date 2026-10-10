// The public rooms page: lists the rooms the Ember bridge reports
// (/rooms.json, nginx's proxy of the bridge's GET /v1/rooms/public) as cards
// like Ember's own Public rooms screen, and refreshes while the tab is shown.
// Join in Ember is a plain ember://room/open link the player clicks; the page
// never opens Ember by itself (see open.js). Room and host names are the
// players' own words, so they only ever go in as text.
(function () {
  'use strict';
  var REFRESH_MS = 15000;
  var FACE = 32;
  // Native fighter IDs, as src/common/FighterMetadata.inc orders them and
  // assets/faces.webp lays them out.
  var FIGHTERS = ['Ryu', 'Ken', 'Chun-Li', 'E. Honda', 'Blanka', 'Zangief', 'Guile', 'Dhalsim', 'Balrog', 'Vega',
    'Sagat', 'M. Bison', 'C. Viper', 'Rufus', 'El Fuerte', 'Abel', 'Seth', 'Akuma', 'Gouken', 'T. Hawk', 'Cammy',
    'Fei Long', 'Dee Jay', 'Sakura', 'Rose', 'Gen', 'Dan', 'Guy', 'Cody', 'Ibuki', 'Makoto', 'Dudley', 'Adon',
    'Hakan', 'Juri', 'Yun', 'Yang', 'Evil Ryu', 'Oni', 'Rolento', 'Elena', 'Poison', 'Hugo', 'Decapre'];
  var REGIONS = { use1: 'US East', usw1: 'US West', euc1: 'Europe', aps1: 'Asia Pacific' };
  var ROTATIONS = ['Winner stays', 'Loser stays', 'Both rotate'];
  // Sidecar build IDs of the releases public rooms run, newest first. Add the
  // new one at each release; a room on a build not listed here says
  // "Other version".
  var VERSIONS = [
    ['400f36a4a4353c86e8f2c294b4490f287895f9849137daa284ce822e1596b2cd', '1.1.2'],
    ['3581000c7b42636e90b6c14dc378cad67c621f8bc82ae6f0900476323c5723d0', '1.1.1'],
    ['d3dafb0b0a96f8a0152ac6edf76d79272ed085e618cc4dcc656018a918f56166', '1.1.0'],
    ['1ba5ca463447b379cdd39d266942dc0f7171215dbf502454e281da4ccd86f8ea', '1.1.0-rc2'],
    ['cc4a0d6ce968fbaa99fe7653b02b80a083d18f1423c27f4f5376a07f01adc863', '1.1.0-rc1']
  ];
  var BRIDGE = /^brg_[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  var ROOM = /^[0-9a-f]{32}$/;

  var list = document.getElementById('rooms');
  var summary = document.getElementById('summary');
  var status = document.getElementById('status');
  var regions = document.getElementById('regions');
  var openOnly = document.getElementById('open-only');
  var refreshButton = document.getElementById('refresh');
  var state = { rooms: null, bridge: '', listedAt: 0, region: '', failed: false };
  var timer = null;
  var loading = false;

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function regionLabel(code) { return REGIONS[code] || String(code || '').toUpperCase(); }
  function versionOf(build) {
    for (var i = 0; i < VERSIONS.length; i++) if (VERSIONS[i][0] === build) return { label: VERSIONS[i][1], rank: i };
    return { label: null, rank: VERSIONS.length };
  }
  function full(room) { return room.capacity > 0 && room.members >= room.capacity; }
  function joinable(room) { return !room.locked && !full(room); }
  function age(seconds) {
    var minutes = Math.max(1, Math.floor(seconds / 60));
    return minutes < 60 ? minutes + ' min' : Math.floor(minutes / 60) + ' h';
  }

  function chip(text, kind) { return el('span', 'room-chip' + (kind ? ' ' + kind : ''), text); }

  function face(id) {
    var tile = el('span', 'room-face');
    if (typeof id === 'number' && id >= 0 && id < FIGHTERS.length) {
      tile.style.backgroundPosition = (-id * FACE) + 'px 0';
      tile.title = FIGHTERS[id];
      tile.setAttribute('role', 'img');
      tile.setAttribute('aria-label', FIGHTERS[id]);
    } else {
      tile.className += ' none';
      tile.textContent = '?';
      tile.title = 'No main yet';
    }
    return tile;
  }

  function copy(text, done) {
    function fallback() {
      var area = document.createElement('textarea');
      area.value = text;
      area.setAttribute('readonly', '');
      area.className = 'offscreen';
      document.body.appendChild(area);
      area.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (error) { ok = false; }
      document.body.removeChild(area);
      status.textContent = ok ? done : 'Copy failed. The room link is ' + text;
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { status.textContent = done; }, fallback);
    } else {
      fallback();
    }
  }

  function card(room) {
    var isFull = full(room), open = joinable(room);
    var version = versionOf(room.build_id);
    var node = el('article', 'room-card' + (open ? '' : ' dimmed'));
    node.setAttribute('aria-label', room.name);

    var head = el('div', 'room-head');
    var titles = el('div', 'room-titles');
    titles.appendChild(el('h2', 'room-name', room.name));
    var meta = el('p', 'room-meta');
    var where = regionLabel(room.region);
    meta.textContent = room.host_name ? room.host_name + ' · ' + where : where;
    titles.appendChild(meta);
    head.appendChild(titles);

    var seats = el('div', 'room-seats');
    seats.appendChild(el('span', 'room-count', room.members + '/' + room.capacity));
    var bar = el('span', 'room-bar');
    var fill = el('span', 'room-bar-fill');
    fill.style.width = (room.capacity > 0 ? Math.min(100, Math.round(100 * room.members / room.capacity)) : 0) + '%';
    bar.appendChild(fill);
    seats.appendChild(bar);
    seats.setAttribute('aria-label', room.members + ' of ' + room.capacity + ' seats taken');
    head.appendChild(seats);
    node.appendChild(head);

    var line = el('div', 'room-line');
    var faces = el('div', 'room-faces');
    var fighters = Array.isArray(room.fighters) ? room.fighters : [];
    var shown = Math.min(fighters.length, 6);
    for (var i = 0; i < shown; i++) faces.appendChild(face(fighters[i]));
    if (room.members > shown && shown > 0) faces.appendChild(el('span', 'room-more', '+' + (room.members - shown)));
    line.appendChild(faces);

    var rules = el('div', 'room-rules');
    if (typeof room.set_format === 'number' && room.set_format > 0) {
      rules.appendChild(chip('First to ' + room.set_format, 'set'));
      if (typeof room.rotation === 'number' && ROTATIONS[room.rotation]) rules.appendChild(chip(ROTATIONS[room.rotation]));
    }
    if (version.rank > 0) rules.appendChild(chip(version.label ? 'Ember ' + version.label : 'Other version', 'version'));
    line.appendChild(rules);
    node.appendChild(line);

    var foot = el('div', 'room-foot');
    var standing = el('div', 'room-standing');
    var opened = state.listedAt && room.created_at ? state.listedAt - room.created_at : -1;
    if (opened >= 0) standing.appendChild(el('span', 'room-age', 'Open for ' + age(opened)));
    if (room.tables_playing > 0) standing.appendChild(chip('In a match', 'match'));
    if (room.locked) standing.appendChild(chip('Locked', 'status'));
    else if (isFull) standing.appendChild(chip('Full', 'status'));
    foot.appendChild(standing);
    var actions = el('div', 'room-actions');
    var link = 'https://embernetplay.link/r#' + state.bridge + '/' + room.room_id;
    var share = el('button', 'room-action', 'Copy link');
    share.type = 'button';
    share.addEventListener('click', function () { copy(link, 'Room link copied. Paste it in Ember under Public rooms.'); });
    actions.appendChild(share);
    if (open) {
      var join = el('a', 'room-action primary', 'Join in Ember');
      join.href = 'ember://room/open?bridge=' + state.bridge + '&room=' + room.room_id;
      join.addEventListener('click', function () {
        status.textContent = 'Opening Ember for “' + room.name + '”. If nothing happens, start Ember once, or use Copy link.';
      });
      actions.appendChild(join);
    }
    foot.appendChild(actions);
    node.appendChild(foot);
    return node;
  }

  function message(text, detail) {
    var node = el('div', 'room-card room-message');
    node.appendChild(el('p', 'room-message-title', text));
    if (detail) node.appendChild(el('p', 'note', detail));
    return node;
  }

  function regionButtons(rooms) {
    var seen = {};
    rooms.forEach(function (room) { seen[room.region] = true; });
    if (state.region && !seen[state.region]) seen[state.region] = true;
    var codes = Object.keys(seen).sort(function (a, b) { return regionLabel(a).localeCompare(regionLabel(b)); });
    while (regions.children.length > 1) regions.removeChild(regions.lastChild);
    codes.forEach(function (code) {
      var button = el('button', 'room-filter', regionLabel(code));
      button.type = 'button';
      button.setAttribute('data-region', code);
      regions.appendChild(button);
    });
    for (var i = 0; i < regions.children.length; i++) {
      var child = regions.children[i];
      child.setAttribute('aria-pressed', child.getAttribute('data-region') === state.region ? 'true' : 'false');
    }
  }

  function render() {
    list.setAttribute('aria-busy', 'false');
    while (list.firstChild) list.removeChild(list.firstChild);
    if (!state.rooms) {
      list.appendChild(message('The room list is not available right now.', 'Try again in a minute, or look in Ember under Online play, Public rooms.'));
      summary.textContent = '';
      return;
    }
    var rooms = state.rooms.slice();
    regionButtons(rooms);
    var players = rooms.reduce(function (sum, room) { return sum + room.members; }, 0);
    // The bridge's order (unlocked first, most free seats first), newest Ember first.
    rooms = rooms.map(function (room, index) { return { room: room, index: index, rank: versionOf(room.build_id).rank }; })
      .sort(function (a, b) { return a.rank - b.rank || a.index - b.index; })
      .map(function (entry) { return entry.room; });
    var shown = rooms.filter(function (room) {
      return (!state.region || room.region === state.region) && (!openOnly.checked || joinable(room));
    });
    summary.textContent = rooms.length === 0 ? 'No rooms open' :
      rooms.length + (rooms.length === 1 ? ' room, ' : ' rooms, ') + players + (players === 1 ? ' player' : ' players') +
      (state.failed ? '. Could not refresh; showing the last list.' : '');
    if (rooms.length === 0) {
      list.appendChild(message('No public rooms are open right now.',
        'Open one in Ember: Online play, Public rooms, Create room. This page shows it as soon as you are in it.'));
    } else if (shown.length === 0) {
      list.appendChild(message('No rooms match these filters.', 'Choose All regions or turn off Open seats only.'));
    } else {
      shown.forEach(function (room) { list.appendChild(card(room)); });
    }
  }

  function valid(body) {
    if (!body || !BRIDGE.test(body.bridge_id) || !Array.isArray(body.rooms)) return null;
    return body.rooms.filter(function (room) {
      return room && ROOM.test(room.room_id) && typeof room.name === 'string' && typeof room.members === 'number' &&
        typeof room.capacity === 'number';
    });
  }

  function load() {
    if (loading) return;
    loading = true;
    refreshButton.disabled = true;
    fetch('/rooms.json', { cache: 'no-store', credentials: 'omit' })
      .then(function (response) {
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
      })
      .then(function (body) {
        var rooms = valid(body);
        if (!rooms) throw new Error('unexpected list');
        state.rooms = rooms;
        state.bridge = body.bridge_id;
        state.listedAt = typeof body.listed_at === 'number' ? body.listed_at : Math.floor(Date.now() / 1000);
        state.failed = false;
      })
      .catch(function () { state.failed = true; })
      .then(function () {
        loading = false;
        refreshButton.disabled = false;
        render();
        schedule();
      });
  }

  function schedule() {
    clearTimeout(timer);
    timer = document.hidden ? null : setTimeout(load, REFRESH_MS);
  }

  regions.addEventListener('click', function (event) {
    var button = event.target.closest ? event.target.closest('button') : null;
    if (!button || !regions.contains(button)) return;
    state.region = button.getAttribute('data-region') || '';
    render();
  });
  openOnly.addEventListener('change', render);
  refreshButton.addEventListener('click', load);
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) { clearTimeout(timer); timer = null; } else { load(); }
  });
  load();
}());
