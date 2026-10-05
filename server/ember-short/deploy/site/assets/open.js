// Opens an Ember link: a room invitation (/j#CODE), a tournament match
// (/m#BRIDGE/MATCH), a public room of a service (/r#BRIDGE/ROOM), or, at
// /start, tells a player new to Ember how to get it
// and connect Discord for tournament sites. A site that names its service
// (/start#BRIDGE) gets a button that opens Ember's Connect Discord screen for
// it. It reads the link from the URL
// fragment, which is never sent to the server, makes no requests of its own,
// and offers the ember: link only on the player's click, never by itself: a
// browser without Ember's handler would otherwise replace this page with an error.
(function () {
  'use strict';
  var ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
  // brg_ or emt_ and a lowercase version 4 UUID, as the bridge writes them.
  var ID = /^[a-z]{3}_[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  // A public room's ID: 16 bytes in lowercase hex, as the bridge writes it.
  var ROOM = /^[0-9a-f]{32}$/;

  // The canonical 12-symbol room code, or null. Same rules as the game:
  // dashes and spaces ignored, any case, O read as 0 and I or L as 1.
  function parseCode(text) {
    var raw;
    try { raw = decodeURIComponent(String(text || '')); } catch (error) { return null; }
    if (raw.length > 64) return null;
    var code = '';
    for (var i = 0; i < raw.length; i++) {
      var symbol = raw.charAt(i).toUpperCase();
      if (symbol === '-' || symbol === ' ') continue;
      if (symbol === 'O') symbol = '0';
      else if (symbol === 'I' || symbol === 'L') symbol = '1';
      if (ALPHABET.indexOf(symbol) < 0 || code.length === 12) return null;
      code += symbol;
    }
    return code.length === 12 ? code.slice(0, 4) + '-' + code.slice(4, 8) + '-' + code.slice(8) : null;
  }

  // A room link: the code from the fragment, or from a path someone typed.
  function room(path, fragment) {
    var typed = path.replace(/^\/j\/?/, '');
    var code = parseCode(fragment) || parseCode(typed);
    if (!code) return null;
    // A code typed into the path moves into the fragment, out of later requests.
    if (typed && history.replaceState) history.replaceState(null, '', '/j#' + code);
    return {
      title: 'Join an Ember room',
      lead: 'Someone invited you to an SF4 Ember Netplay room.',
      code: code,
      ember: 'ember://join/' + code,
      link: 'https://embernetplay.link/j#' + code,
      after: 'Ember joins the room. Your browser may ask first whether to open Ember.',
      steps: ['Start SF4 Ember Netplay.', 'Choose <strong>Online play</strong>, then <strong>Join room</strong>.',
        'Choose <strong>Paste invitation</strong>, then <strong>Join room</strong>.'],
      lasts: 'The link works while the room is open. If it stops working, ask the host for a new one.'
    };
  }

  // A tournament match: the bridge and match the site's link names.
  function match(path, fragment) {
    var parts = fragment.replace(/\/$/, '').split('/');
    if (path.replace(/\/$/, '') !== '/m' || parts.length !== 2 || !ID.test(parts[0]) || !ID.test(parts[1]) ||
      parts[0].slice(0, 4) !== 'brg_' || parts[1].slice(0, 4) !== 'emt_') return null;
    return {
      title: 'Play your tournament match',
      lead: 'Your tournament match is ready to play in SF4 Ember Netplay.',
      code: null,
      ember: 'ember://tournament/open?bridge=' + parts[0] + '&match=' + parts[1],
      link: 'https://embernetplay.link/m#' + parts[0] + '/' + parts[1],
      after: 'Ember opens the match on its Tournament matches screen; press Play there. Your browser may ask first whether to open Ember.',
      steps: ['Start SF4 Ember Netplay.', 'Choose <strong>Ember ID</strong>, then <strong>Tournament matches</strong>.',
        'Choose <strong>Paste match link</strong>, then <strong>Play</strong>.'],
      lasts: 'Only the two players in this match can play it, so it is safe to share this link with them.'
    };
  }

  // A public room: the service and room a bot or site's link names.
  function publicRoom(path, fragment) {
    var parts = fragment.replace(/\/$/, '').split('/');
    if (path.replace(/\/$/, '') !== '/r' || parts.length !== 2 || !ID.test(parts[0]) || parts[0].slice(0, 4) !== 'brg_' ||
      !ROOM.test(parts[1])) return null;
    return {
      title: 'Join a public room',
      lead: 'Someone invited you to a public room in SF4 Ember Netplay.',
      code: null,
      ember: 'ember://room/open?bridge=' + parts[0] + '&room=' + parts[1],
      link: 'https://embernetplay.link/r#' + parts[0] + '/' + parts[1],
      after: 'Ember opens Public rooms and asks to join this room. Your browser may ask first whether to open Ember.',
      steps: ['Start SF4 Ember Netplay.', 'Choose <strong>Online play</strong>, then <strong>Public rooms</strong>.',
        'Choose <strong>Paste room link</strong>.'],
      lasts: 'You need an Ember ID, and the room must still be open. Anyone with an Ember ID can join a public room, so it is safe to share this link.'
    };
  }

  function copy(text, done) {
    var status = document.getElementById('status');
    function fallback() {
      var area = document.createElement('textarea');
      area.value = text;
      area.setAttribute('readonly', '');
      area.style.position = 'fixed';
      area.style.opacity = '0';
      document.body.appendChild(area);
      area.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (error) { ok = false; }
      document.body.removeChild(area);
      status.textContent = ok ? done : 'Select the address in the address bar and copy it.';
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { status.textContent = done; }, fallback);
    } else {
      fallback();
    }
  }

  // Where a tournament site sends a player it could not find.
  var GETTING_STARTED = {
    title: 'Play tournaments with Ember',
    lead: 'SF4 Ember Netplay plays Ultra Street Fighter IV tournament matches online. Get it, then connect the Discord account your tournament site knows.'
  };

  // The same, from a site that names its service: Ember opens Connect Discord
  // for it and walks the player through each step.
  function connect(fragment) {
    var bridge = fragment.replace(/\/$/, '');
    if (!ID.test(bridge) || bridge.slice(0, 4) !== 'brg_') return null;
    return {
      title: 'Connect Ember',
      lead: 'Your tournament site finds players by their Discord account. Ember connects yours in a few steps.',
      code: null,
      ember: 'ember://discord/connect?bridge=' + bridge,
      link: 'https://embernetplay.link/start#' + bridge,
      after: 'Ember sets itself up and opens Discord in your browser. Approve there with the Discord account this site knows, then come back here. Your browser may ask first whether to open Ember.',
      fallback: 'That happens when Ember has not been started on this PC yet, on Linux or Steam Deck, or when the browser blocks it. Copy the link and paste it into Ember instead, so it connects Discord for this site:',
      steps: ['Start SF4 Ember Netplay.', 'Choose <strong>Ember ID</strong>, then <strong>Discord</strong>.',
        'Choose <strong>Paste link</strong>, then approve in Discord.'],
      lasts: 'Discord is optional in Ember. Tournament sites that find players by Discord account, such as BluMint, need it.'
    };
  }

  // Every element a variant may change, put back as the HTML has it, so a
  // link changed in an open tab draws from a clean page.
  var current = null;
  function reset() {
    var byId = function (id) { return document.getElementById(id); };
    ['page', 'missing', 'link', 'fallback', 'tournaments', 'code', 'copy-code', 'version'].forEach(function (id) {
      byId(id).hidden = true;
    });
    byId('come-back').hidden = false;
    byId('get-title').textContent = "Don't have Ember yet?";
    byId('fallback-lead').textContent = FALLBACK_LEAD;
    byId('title').textContent = 'Open in Ember';
    byId('status').textContent = '';
    byId('open').href = '#';
    ['lead', 'after', 'lasts', 'code', 'steps'].forEach(function (id) { byId(id).textContent = ''; });
    document.title = 'Open in Ember';
    current = null;
  }

  function render() {
    reset();
    var fragment = location.hash.replace(/^#/, '');
    var path = location.pathname.replace(/\/$/, '');
    var found = path === '/start' ? connect(fragment) || GETTING_STARTED :
      path.indexOf('/m') === 0 ? match(path, fragment) :
      path.indexOf('/r') === 0 ? publicRoom(path, fragment) : room(path, fragment);
    if (!found) {
      document.getElementById('missing').hidden = false;
      return;
    }
    current = found;
    document.title = found.title;
    document.getElementById('title').textContent = found.title;
    document.getElementById('lead').textContent = found.lead;
    document.getElementById('page').hidden = false;
    // Public rooms, matches and Connect Discord open only in Ember 1.1.0 or newer.
    document.getElementById('version').hidden = path.indexOf('/j') === 0;
    if (!found.ember) {
      document.getElementById('get-title').textContent = 'Get Ember';
      document.getElementById('come-back').hidden = true;
      document.getElementById('tournaments').hidden = false;
      return;
    }
    document.getElementById('after').textContent = found.after;
    document.getElementById('lasts').textContent = found.lasts;
    document.getElementById('open').href = found.ember;
    // The steps are this page's own fixed text.
    document.getElementById('steps').innerHTML = found.steps.map(function (step) { return '<li>' + step + '</li>'; }).join('');
    if (found.fallback) document.getElementById('fallback-lead').textContent = found.fallback;
    if (found.code) {
      var code = document.getElementById('code');
      code.textContent = found.code;
      code.hidden = false;
      document.getElementById('copy-code').hidden = false;
    }
    document.getElementById('link').hidden = false;
    document.getElementById('fallback').hidden = false;
  }

  var FALLBACK_LEAD;
  function start() {
    FALLBACK_LEAD = document.getElementById('fallback-lead').textContent;
    // Bound once; each copies whatever link the page shows now.
    document.getElementById('copy-link').addEventListener('click', function () {
      if (current && current.link) copy(current.link, 'Link copied. Paste it into Ember.');
    });
    document.getElementById('copy-code').addEventListener('click', function () {
      if (current && current.code) copy(current.code, 'Code copied. Paste it into Ember.');
    });
    // Someone may paste another link into this tab: only the fragment changes,
    // so the browser does not load the page again.
    window.addEventListener('hashchange', render);
    render();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
