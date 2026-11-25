// The home page: names the Stable release on the download button, and shows
// who is playing now from the same /rooms.json the rooms page reads, the
// busiest rooms first. The section stays hidden when the list cannot be had.
// Room names are the players' own words, so they only ever go in as text.
(function () {
  'use strict';
  var site = window.EmberSite;
  var el = site.el;
  var SHOWN = 4;
  // Faces a room card has room for, and how many faces.webp holds.
  var FACES = 6;
  var FIGHTERS = 44;

  document.getElementById('get').textContent = 'Download Ember ' + site.STABLE_VERSION;

  var live = document.getElementById('live');
  if (!window.fetch) return;

  function card(room) {
    var link = el('a', 'live-room');
    link.href = '/rooms';
    link.appendChild(el('span', 'live-room-name', room.name));
    var faces = el('span', 'live-room-faces');
    (Array.isArray(room.fighters) ? room.fighters : []).slice(0, FACES).forEach(function (id) {
      if (Number.isInteger(id) && id >= 0 && id < FIGHTERS) faces.appendChild(el('span', 'face small f' + id));
    });
    faces.setAttribute('aria-hidden', 'true');
    link.appendChild(faces);
    var meta = el('span', 'live-room-meta');
    meta.appendChild(el('span', '', room.tables_playing > 0 ? 'In a match' : 'Waiting for players'));
    meta.appendChild(el('span', '', room.members + ' / ' + room.capacity));
    link.appendChild(meta);
    return link;
  }

  fetch('/rooms.json', { cache: 'no-store', credentials: 'omit' })
    .then(function (response) { return response.ok ? response.json() : null; })
    .then(function (body) {
      if (!body || !Array.isArray(body.rooms)) return;
      var rooms = body.rooms.filter(function (room) {
        return room && typeof room.name === 'string' && typeof room.members === 'number' && typeof room.capacity === 'number';
      });
      var players = rooms.reduce(function (sum, room) { return sum + room.members; }, 0);
      document.getElementById('live-count').textContent = rooms.length === 0
        ? 'No public room is open right now. Make one in Ember and it is listed here.'
        : rooms.length + (rooms.length === 1 ? ' room open, ' : ' rooms open, ') + players + (players === 1 ? ' player' : ' players');
      rooms.sort(function (a, b) { return b.members - a.members; });
      var list = document.getElementById('live-rooms');
      rooms.slice(0, SHOWN).forEach(function (room) { list.appendChild(card(room)); });
      live.hidden = false;
    })
    .catch(function () {});
}());
