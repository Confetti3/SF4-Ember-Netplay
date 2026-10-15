// The landing page: shows how many public rooms are open on the Browse public
// rooms button, from the same /rooms.json the rooms page reads. Says nothing
// when the list cannot be had.
(function () {
  'use strict';
  var live = document.getElementById('live');
  if (!live || !window.fetch) return;
  fetch('/rooms.json', { cache: 'no-store', credentials: 'omit' })
    .then(function (response) { return response.ok ? response.json() : null; })
    .then(function (body) {
      if (!body || !Array.isArray(body.rooms)) return;
      var rooms = body.rooms.length;
      var players = body.rooms.reduce(function (sum, room) { return sum + (typeof room.members === 'number' ? room.members : 0); }, 0);
      live.textContent = rooms === 0 ? 'None open' : rooms + ' open, ' + players + (players === 1 ? ' player' : ' players');
      live.hidden = false;
    })
    .catch(function () {});
}());
