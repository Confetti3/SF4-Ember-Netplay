// The public rooms page: lists the rooms the Ember bridge reports
// (/rooms.json, nginx's proxy of the bridge's GET /v1/rooms/public) as cards
// like Ember's own Public rooms screen, and refreshes while the tab is shown.
// Join in Ember is a plain ember://room/open link the player clicks; the page
// never opens Ember by itself (see open.js). Room and host names are the
// players' own words, so they only ever go in as text.
(function () {
  'use strict';
  var site = window.EmberSite;
  var REFRESH_MS = 15000;
  var FACE = 32;
  // Native fighter IDs, as src/common/FighterMetadata.inc orders them and
  // assets/faces.webp lays them out.
  var FIGHTERS = ['Ryu', 'Ken', 'Chun-Li', 'E. Honda', 'Blanka', 'Zangief', 'Guile', 'Dhalsim', 'Balrog', 'Vega',
    'Sagat', 'M. Bison', 'C. Viper', 'Rufus', 'El Fuerte', 'Abel', 'Seth', 'Akuma', 'Gouken', 'T. Hawk', 'Cammy',
    'Fei Long', 'Dee Jay', 'Sakura', 'Rose', 'Gen', 'Dan', 'Guy', 'Cody', 'Ibuki', 'Makoto', 'Dudley', 'Adon',
    'Hakan', 'Juri', 'Yun', 'Yang', 'Evil Ryu', 'Oni', 'Rolento', 'Elena', 'Poison', 'Hugo', 'Decapre'];
  // Where a room's creator plays from, by the code a room carries as `area`, in
  // the order the filter lists them: Ember's own list (src/common/Area.hxx,
  // ember_protocol::area::Area, whose tests check this one against them).
  var AREAS = { naw: 'NA West', nac: 'NA Central', nae: 'NA East', cam: 'Central America & Caribbean', sam: 'South America', euw: 'Europe West', eue: 'Europe East', mea: 'Middle East & Africa', sas: 'South Asia', eas: 'East Asia', sea: 'South-East Asia', oce: 'Oceania' };
  // The relay a room's creator connects through, by the code every room carries
  // as `region`: how they connect, never where they are. A room without an area
  // (its creator hid it, or Ember before 1.6.0 made it) names only its relay.
  var RELAYS = { use1: 'Ember US (Kansas)', usw1: 'n0 US West', euc1: 'Ember Europe', aps1: 'n0 Asia' };
  var ROTATIONS = ['Winner stays', 'Loser stays', 'Both rotate'];
  // Room types as the room model numbers them (room::RoomType), in the order
  // the page lists them. A room whose host does not say is Classic: room hosts
  // before 1.5.0 have no other kind.
  var TYPES = [
    { code: 0, key: 'classic', label: 'Classic', title: 'Classic rooms', about: 'Tables with a queue. Take a seat and wait your turn.' },
    { code: 1, key: 'arcade', label: 'Arcade', title: 'Arcade', about: 'Sixteen cabinets. Sit down and play whoever has waited longest.' },
    { code: 2, key: 'king', label: 'King of the hill', title: 'King of the hill', about: 'Arcade cabinets, and the winner stays on for the next challenger.' }
  ];
  var VERSIONS = site.VERSIONS;
  var BRIDGE = /^brg_[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  var ROOM = /^[0-9a-f]{32}$/;

  var list = document.getElementById('rooms');
  var summary = document.getElementById('summary');
  var status = document.getElementById('status');
  var areas = document.getElementById('areas');
  var types = document.getElementById('types');
  var openOnly = document.getElementById('open-only');
  var refreshButton = document.getElementById('refresh');
  var state = { rooms: null, bridge: '', listedAt: 0, skew: 0, key: null, areas: {}, type: '', failed: false };
  // What the cards on screen were drawn from: the body without listed_at, and
  // whether the "could not refresh" note was showing. A refresh that changes
  // neither only updates the ages, so the cards, and the focus on them, stay.
  var drawn = { key: null, failed: null };
  var timer = null;
  var loading = false;

  var el = site.el;
  // The room's area when it gave one Ember knows, else none.
  function areaOf(room) { return Object.prototype.hasOwnProperty.call(AREAS, room.area) ? room.area : ''; }
  // What the card says of where the room is: its area, or else its relay, as a relay.
  function placeLabel(room) {
    var area = areaOf(room);
    if (area) return AREAS[area];
    if (typeof room.region !== 'string' || !room.region) return '';
    return 'Relay: ' + (Object.prototype.hasOwnProperty.call(RELAYS, room.region) ? RELAYS[room.region] : room.region.toUpperCase());
  }
  function versionOf(build) {
    for (var i = 0; i < VERSIONS.length; i++) if (VERSIONS[i][0] === build) return { label: VERSIONS[i][1], rank: i };
    return { label: null, rank: VERSIONS.length };
  }
  function typeOf(room) {
    var code = room.room_type;
    return typeof code === 'number' && TYPES[code] ? TYPES[code] : TYPES[0];
  }
  function full(room) { return room.capacity > 0 && room.members >= room.capacity; }
  function joinable(room) { return !room.locked && !full(room); }
  // Seconds on the bridge's clock now: the page's clock moved by how far it
  // was from listed_at when the list arrived, so a wrong local clock does not
  // change the ages.
  function serverNow() { return Math.floor(Date.now() / 1000) + state.skew; }
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
    site.copy(status, text, done, 'Copy failed. The room link is ' + text);
  }

  function card(room) {
    var isFull = full(room), open = joinable(room);
    var version = versionOf(room.build_id);
    var type = typeOf(room);
    var node = el('article', 'room-card ' + type.key + (open ? '' : ' dimmed'));
    node.setAttribute('aria-label', room.name);

    var head = el('div', 'room-head');
    var titles = el('div', 'room-titles');
    titles.appendChild(el('h2', 'room-name', room.name));
    var meta = el('p', 'room-meta');
    var where = placeLabel(room);
    meta.textContent = room.host_name && where ? room.host_name + ' · ' + where : room.host_name || where;
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
    if (type.code !== 0) rules.appendChild(chip(type.label, 'type ' + type.key));
    // An Arcade or King of the hill room with no set length plays sets of one
    // game (room::SetGoal), and nobody rotates: the cabinets pair players.
    if (typeof room.set_format === 'number') {
      var setLength = type.code !== 0 ? Math.max(room.set_format, 1) : room.set_format;
      if (setLength > 0) rules.appendChild(chip('First to ' + setLength, 'set'));
      if (setLength > 0 && type.code === 0 && typeof room.rotation === 'number' && ROTATIONS[room.rotation]) {
        rules.appendChild(chip(ROTATIONS[room.rotation]));
      }
    }
    if (version.rank > 0) rules.appendChild(chip(version.label ? 'Ember ' + version.label : 'Other version', 'version'));
    line.appendChild(rules);
    node.appendChild(line);

    var foot = el('div', 'room-foot');
    var standing = el('div', 'room-standing');
    var opened = state.listedAt && room.created_at ? serverNow() - room.created_at : -1;
    if (opened >= 0) {
      var openFor = el('span', 'room-age', 'Open for ' + age(opened));
      openFor.setAttribute('data-created', String(room.created_at));
      standing.appendChild(openFor);
    }
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

  // The heading over one type's rooms when the page lists more than one type.
  function groupHead(type, rooms) {
    var players = rooms.reduce(function (sum, room) { return sum + room.members; }, 0);
    var head = el('div', 'room-group ' + type.key);
    var title = el('h2', 'room-group-title', type.title);
    title.appendChild(el('span', 'room-group-count',
      rooms.length + (rooms.length === 1 ? ' room, ' : ' rooms, ') + players + (players === 1 ? ' player' : ' players')));
    head.appendChild(title);
    head.appendChild(el('p', 'room-group-about', type.about));
    return head;
  }

  function message(text, detail) {
    var node = el('div', 'room-card room-message');
    node.appendChild(el('p', 'room-message-title', text));
    if (detail) node.appendChild(el('p', 'note', detail));
    return node;
  }

  // Whether any area is ticked.
  function areasTicked() { return Object.keys(state.areas).length > 0; }

  // The area buttons: All areas, then each area that has a room, in AREAS
  // order; each area button ticks or unticks that area, and the rooms of the
  // ticked areas are listed (none ticked is All areas, which a tap on it
  // returns to). A room without an area is listed under All areas only.
  function areaButtons(rooms) {
    var seen = {};
    rooms.forEach(function (room) { seen[areaOf(room)] = true; });
    Object.keys(state.areas).forEach(function (code) { seen[code] = true; });
    while (areas.children.length > 1) areas.removeChild(areas.lastChild);
    Object.keys(AREAS).forEach(function (code) {
      if (!seen[code]) return;
      var button = el('button', 'room-filter', AREAS[code]);
      button.type = 'button';
      button.setAttribute('data-area', code);
      areas.appendChild(button);
    });
    for (var i = 0; i < areas.children.length; i++) {
      var child = areas.children[i];
      var code = child.getAttribute('data-area');
      child.setAttribute('aria-pressed', (code ? state.areas[code] === true : !areasTicked()) ? 'true' : 'false');
    }
  }

  // The type buttons: All types, then each type that has a room, in TYPES
  // order. Hidden while every room is Classic and no type is chosen.
  function typeButtons(rooms) {
    var seen = {};
    rooms.forEach(function (room) { seen[typeOf(room).code] = true; });
    if (state.type !== '') seen[state.type] = true;
    while (types.children.length > 1) types.removeChild(types.lastChild);
    TYPES.forEach(function (type) {
      if (!seen[type.code]) return;
      var button = el('button', 'room-filter', type.label);
      button.type = 'button';
      button.setAttribute('data-type', String(type.code));
      types.appendChild(button);
    });
    types.hidden = types.children.length <= 2 && state.type === '';
    for (var i = 0; i < types.children.length; i++) {
      var child = types.children[i];
      child.setAttribute('aria-pressed', child.getAttribute('data-type') === state.type ? 'true' : 'false');
    }
  }

  // Rewrites the "Open for" times in place from the clock.
  function updateAges() {
    var nodes = list.querySelectorAll('.room-age');
    for (var i = 0; i < nodes.length; i++) {
      var created = Number(nodes[i].getAttribute('data-created'));
      var text = 'Open for ' + age(Math.max(0, serverNow() - created));
      if (nodes[i].textContent !== text) nodes[i].textContent = text;
    }
  }

  function render() {
    drawn.failed = state.failed;
    list.setAttribute('aria-busy', 'false');
    while (list.firstChild) list.removeChild(list.firstChild);
    if (!state.rooms) {
      list.appendChild(message('The room list is not available right now.', 'Try again in a minute, or look in Ember under Online play, Public rooms.'));
      summary.textContent = '';
      return;
    }
    var rooms = state.rooms.slice();
    areaButtons(rooms);
    typeButtons(rooms);
    var players = rooms.reduce(function (sum, room) { return sum + room.members; }, 0);
    // The bridge's order (unlocked first, most free seats first), newest Ember first.
    rooms = rooms.map(function (room, index) { return { room: room, index: index, rank: versionOf(room.build_id).rank }; })
      .sort(function (a, b) { return a.rank - b.rank || a.index - b.index; })
      .map(function (entry) { return entry.room; });
    var shown = rooms.filter(function (room) {
      return (!areasTicked() || state.areas[areaOf(room)] === true) && (!openOnly.checked || joinable(room)) &&
        (state.type === '' || String(typeOf(room).code) === state.type);
    });
    summary.textContent = rooms.length === 0 ? 'No rooms open' :
      rooms.length + (rooms.length === 1 ? ' room, ' : ' rooms, ') + players + (players === 1 ? ' player' : ' players') +
      (state.failed ? '. Could not refresh; showing the last list.' : '');
    if (rooms.length === 0) {
      list.appendChild(message('No public rooms are open right now.',
        'Open one in Ember: Online play, Public rooms, Create room. This page shows it as soon as you are in it.'));
    } else if (shown.length === 0) {
      list.appendChild(message('No rooms match these filters.', 'Tick more areas or choose All areas, choose All types, or turn off Open seats only.'));
    } else {
      // Each type's rooms together under its own heading, once there is more
      // than one type to tell apart.
      var groups = TYPES.map(function (type) {
        return { type: type, rooms: shown.filter(function (room) { return typeOf(room) === type; }) };
      }).filter(function (group) { return group.rooms.length > 0; });
      groups.forEach(function (group) {
        if (groups.length > 1) list.appendChild(groupHead(group.type, group.rooms));
        group.rooms.forEach(function (room) { list.appendChild(card(room)); });
      });
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
        var key = JSON.stringify(body, function (name, value) {
          return this === body && name === 'listed_at' ? undefined : value;
        });
        var now = Math.floor(Date.now() / 1000);
        state.rooms = rooms;
        state.bridge = body.bridge_id;
        state.listedAt = typeof body.listed_at === 'number' ? body.listed_at : now;
        state.skew = state.listedAt - now;
        state.failed = false;
        state.key = key;
      })
      .catch(function () { state.failed = true; })
      .then(function () {
        loading = false;
        refreshButton.disabled = false;
        if (state.key !== drawn.key || state.failed !== drawn.failed) {
          drawn.key = state.key;
          render();
        } else {
          updateAges();
        }
        schedule();
      });
  }

  function schedule() {
    clearTimeout(timer);
    timer = document.hidden ? null : setTimeout(load, REFRESH_MS);
  }

  areas.addEventListener('click', function (event) {
    var button = event.target.closest ? event.target.closest('button') : null;
    if (!button || !areas.contains(button)) return;
    var code = button.getAttribute('data-area') || '';
    if (!code) state.areas = {};
    else if (state.areas[code]) delete state.areas[code];
    else state.areas[code] = true;
    render();
  });
  types.addEventListener('click', function (event) {
    var button = event.target.closest ? event.target.closest('button') : null;
    if (!button || !types.contains(button)) return;
    state.type = button.getAttribute('data-type') || '';
    render();
  });
  openOnly.addEventListener('change', render);
  refreshButton.addEventListener('click', load);
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) { clearTimeout(timer); timer = null; } else { load(); }
  });
  load();
}());
