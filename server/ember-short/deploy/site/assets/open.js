// Opens an Ember link: a room invitation (/j#CODE), a tournament match
// (/m#BRIDGE/MATCH), a public room of a service (/r#BRIDGE/ROOM), or, at
// /start, tells a player new to Ember how to get it
// and connect Discord for tournament sites. A site that names its service
// (/start#BRIDGE) gets a button that opens Ember's Connect Discord screen for
// it, and one that also gives its own name (/start#BRIDGE/NAME) is named to
// the player: a name of 1 to 40 letters, digits, spaces and . ' & -, starting
// with a letter or digit (SITE), is shown as text, and any other is dropped.
// It reads the link from the URL
// fragment, which is never sent to the server. Its one request asks whether
// a match is over (/link/BRIDGE/MATCH, which nginx hands to that bridge), and
// a match that is over shows that instead of opening Ember, so a link left
// from an earlier match sends nobody back to it. The answer also names the
// match's game: an SFV match opens SFV Ember (ember-sfv:), never by itself.
// A match page offers nothing to open until that answer comes or its wait
// ends. When the answer does not come, or names no game (a bridge from
// before games), the page works as it did before, for SF4 Ember.
// It offers the ember: link on the player's click, and opens it by itself only
// for a match (/m) in a browser where Ember has opened from a match page that
// named the same required version: a browser without Ember's handler would
// otherwise replace this page with an error, and an older Ember would open a
// match it cannot play. Until then a match page leads with Update Ember.
// That memory is one flag in this browser's storage, never a link.
// On a phone or tablet it offers neither Ember nor its download, only the link
// to take to the PC.
// A link naming the staging bridge offers Nightly, the only Ember that trusts it.
(function () {
  'use strict';
  var site = window.EmberSite;
  var ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
  // brg_ or emt_ and a lowercase version 4 UUID, as the bridge writes them.
  var ID = /^[a-z]{3}_[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  // A public room's ID: 16 bytes in lowercase hex, as the bridge writes it.
  var ROOM = /^[0-9a-f]{32}$/;
  // A site's name on its connect link: letters, digits, spaces and . ' & -.
  var SITE = /^[\p{L}\p{N}][\p{L}\p{N} .'&-]{0,39}$/u;
  var STAGING_BRIDGE = 'brg_1195478b-ac7e-4bc9-90af-c74eac5c747b';
  var PRODUCTION_BRIDGE = 'brg_0dbc0598-2312-4ce3-9df8-e160330565e6';
  // How long a match page waits to learn whether the match is over before it
  // opens Ember anyway. A later answer that it is over still shows.
  var OVER_WAIT_MS = 3000;
  var OVER_TITLE = 'This match is over';
  var OVER_LEAD = 'This match has ended, so its link no longer opens it. If you have another match to play, ' +
    'open the newest match link from your tournament site.';
  var STABLE_DOWNLOAD = 'https://github.com/Confetti3/SF4-Ember-Netplay/releases/latest';
  // Nightly builds are pre-releases, so the repository has no latest release.
  var NIGHTLY_DOWNLOAD = 'https://github.com/Confetti3/SF4-Ember-Netplay-Nightly/releases';
  var VERSION_NOTE = 'Public rooms and tournaments need Ember 1.1.0 or newer.';
  var NIGHTLY_NOTE = 'This link is for Ember\'s staging bridge, which only Ember Nightly 1.4.0 or newer opens. ' +
    'Download the newest Nightly at the top of the list.';
  var STABLE_VERSION = site.STABLE_VERSION;
  // How long the page waits after Open in Ember before it guesses Ember did not open.
  var OPEN_WAIT_MS = 2000;
  // How long after that click Ember taking focus still counts as Ember opening:
  // a first launch waits for the browser's prompt and the launcher to start.
  var RECORD_WAIT_MS = 15000;
  // Set once Ember took focus right after Open in Ember on this page: the
  // version a match page asked for, or 1 from any other page.
  var OPENED_KEY = 'ember.opened';
  var PHONE_FALLBACK_TITLE = 'Open it on your PC';
  var PHONE_FALLBACK_LEAD = 'Copy the link and send it to yourself, then open it on the PC you play on, or paste it into Ember there:';
  var AUTO_AFTER = 'Ember opened from this browser before, so this page opens it for you. If nothing happens, choose Open in Ember. ' +
    'If Ember says your version differs from your opponent\'s, choose Update.';

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
  // /j is an SF4 Ember room's link and /v an SFV Ember room's: the same code,
  // opened in the Ember that made it.
  function room(path, fragment) {
    var page = /^\/v(\/|$)/.test(path) ? '/v' : /^\/j(\/|$)/.test(path) ? '/j' : null;
    if (!page) return null;
    var typed = path.slice(page.length).replace(/^\//, '');
    var code = parseCode(fragment) || parseCode(typed);
    if (!code) return null;
    // A code typed into the path moves into the fragment, out of later requests.
    if (typed && history.replaceState) history.replaceState(null, '', page + '#' + code);
    if (page === '/v') return {
      title: 'Join an SFV Ember room',
      lead: 'Someone invited you to an SFV Ember room.',
      game: 'sfv',
      code: code,
      ember: 'ember-sfv://join/' + code,
      link: 'https://embernetplay.link/v#' + code,
      after: 'SFV Ember joins the room. Your browser may ask first whether to open SFV Ember.',
      steps: ['Start SFV Ember.', 'Choose <strong>Online play</strong>, then <strong>Join room</strong>.',
        'Choose <strong>Paste invitation</strong>, then <strong>Join room</strong>.'],
      lasts: 'The link works while the room is open. If it stops working, ask the host for a new one.'
    };
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
    var staging = parts[0] === STAGING_BRIDGE;
    return {
      bridge: parts[0],
      match: parts[1],
      // SF4's until the bridge names the match's game (lookUp).
      game: 'usf4',
      need: staging ? 'nightly' : STABLE_VERSION,
      needText: staging ? 'Both players need the same Ember Nightly. If Ember says yours differs from your opponent\'s, update it.' :
        'Both players need the same Ember version. If Ember says yours differs from your opponent\'s, update it. Ember shows its version in the corner of Home.',
      update: staging ? 'Get the newest Nightly' : 'Get the newest Ember',
      updateHow: staging ? 'In Ember Nightly: Help & about, then Check for updates.' :
        'Or update inside Ember: Help & about, then Check for updates, then Exit and open updater, then Download and install. Then come back and choose Open in Ember.',
      title: 'Play your tournament match',
      lead: 'Your tournament match is ready to play in SF4 Ember Netplay.',
      code: null,
      ember: 'ember://tournament/open?bridge=' + parts[0] + '&match=' + parts[1],
      link: 'https://embernetplay.link/m#' + parts[0] + '/' + parts[1],
      after: 'Ember opens the match on its Tournament matches screen; press Play there. Your browser may ask first whether to open Ember.',
      steps: ['Start SF4 Ember Netplay.',
        'Choose <strong>Online play</strong>, then <strong>Ember ID</strong>, then <strong>Tournament matches</strong>.',
        'Choose <strong>Paste match link</strong>, then <strong>Play</strong>.'],
      lasts: 'Only the two players in this match can play it, so it is safe to share this link with them.'
    };
  }

  // The same match once its bridge says it is SFV's: SFV Ember's own link
  // and words. SF4 Ember's version, update and download help does not
  // apply, and nothing here opens it by itself, since this browser's
  // remembered open is for SF4 Ember's ember: link. Only the match changes;
  // draw shows it.
  function asSfv(found) {
    found.game = 'sfv';
    found.need = null;
    found.needText = null;
    found.lead = 'Your tournament match is ready to play in SFV Ember.';
    found.phone = 'SFV Ember runs on the Windows PC you play Street Fighter V on, so open this link on that PC.';
    found.ember = 'ember-sfv://tournament/open?bridge=' + found.bridge + '&match=' + found.match;
    found.after = 'SFV Ember opens the match on its Tournament matches screen; press Play there. Your browser may ask first whether to open SFV Ember.';
    found.steps = ['Start SFV Ember.',
      'Choose <strong>Online play</strong>, then <strong>Ember ID</strong>, then <strong>Tournament matches</strong>.',
      'Choose <strong>Paste match link</strong>, then <strong>Play</strong>.'];
  }

  // A public room: the service and room a bot or site's link names.
  function publicRoom(path, fragment) {
    var parts = fragment.replace(/\/$/, '').split('/');
    if (path.replace(/\/$/, '') !== '/r' || parts.length !== 2 || !ID.test(parts[0]) || parts[0].slice(0, 4) !== 'brg_' ||
      !ROOM.test(parts[1])) return null;
    return {
      bridge: parts[0],
      room: parts[1].toLowerCase(),
      // SF4's until the bridge lists the room as SFV's (roomLookUp).
      game: 'usf4',
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

  // The same public room once the bridge lists it among SFV's rooms: SFV
  // Ember's link and words.
  function asSfvRoom(found) {
    found.game = 'sfv';
    found.lead = 'Someone invited you to a public room in SFV Ember.';
    found.ember = 'ember-sfv://room/open?bridge=' + found.bridge + '&room=' + found.room;
    found.after = 'SFV Ember opens Public rooms and asks to join this room. Your browser may ask first whether to open SFV Ember.';
    found.steps = ['Start SFV Ember.', 'Choose <strong>Online play</strong>, then <strong>Public rooms</strong>.',
      'Choose <strong>Paste room link</strong>.'];
  }

  function copy(text, done) {
    site.copy(document.getElementById('status'), text, done, 'Select the address in the address bar and copy it.', true);
  }

  // Where a tournament site sends a player it could not find.
  var GETTING_STARTED = {
    title: 'Play tournaments with Ember',
    lead: 'SF4 Ember Netplay plays Ultra Street Fighter IV tournament matches online. Get it, then connect the Discord account you signed up for the tournament with.'
  };

  // The site's name from its connect link, or null.
  function siteName(text) {
    var name;
    try { name = decodeURIComponent(text); } catch (error) { return null; }
    return SITE.test(name) ? name : null;
  }

  // The same, from a site that names its service: Ember opens Connect Discord
  // for it and walks the player through each step.
  function connect(fragment) {
    var parts = fragment.replace(/\/$/, '').split('/');
    var bridge = parts[0];
    if (!ID.test(bridge) || bridge.slice(0, 4) !== 'brg_' || parts.length > 2) return null;
    var site = parts.length === 2 ? siteName(parts[1]) : null;
    // An older Ember's Paste link takes a connect link only without the
    // name, so the address bar keeps the link a player may copy from it.
    if (parts.length === 2 && history.replaceState) history.replaceState(null, '', '/start#' + bridge);
    var finder = site || 'The tournament you signed up for';
    return {
      bridge: bridge,
      title: 'Connect Discord to Ember',
      lead: finder + ' finds you by your Discord account. Connect it to Ember in a few steps.',
      code: null,
      ember: 'ember://discord/connect?bridge=' + bridge,
      link: 'https://embernetplay.link/start#' + bridge,
      after: 'Ember sets itself up and opens Discord in your browser. Approve there with the Discord account you use on ' +
        (site || 'the tournament site') + ', then go back to ' + (site || 'it') + '. Your browser may ask first whether to open Ember.',
      fallback: 'That happens when Ember has not been started on this PC yet, on Linux or Steam Deck, or when your browser blocks it. Copy the link and paste it into Ember instead, so it connects Discord for ' +
        (site || 'your tournament') + ':',
      steps: ['Start SF4 Ember Netplay.',
        'Choose <strong>Online play</strong>, then <strong>Ember ID</strong>, then <strong>Discord</strong>.',
        'Choose <strong>Paste link</strong>, then approve in Discord.'],
      lasts: 'Discord is optional in Ember. ' + (site ? site + ' needs it to find you.' :
        'Tournament sites that find players by Discord account, such as BluMint, need it.')
    };
  }

  // Every element a variant may change, put back as the HTML has it, so a
  // link changed in an open tab draws from a clean page.
  var current = null;
  function reset() {
    var byId = function (id) { return document.getElementById(id); };
    ['page', 'missing', 'link', 'fallback', 'tournaments', 'code', 'copy-code', 'version', 'not-opened', 'need', 'update',
      'update-how'].forEach(function (id) {
      byId(id).hidden = true;
    });
    byId('phone').hidden = !PHONE;
    byId('phone').textContent = PHONE_NOTE;
    byId('not-opened-download').hidden = false;
    byId('open').classList.add('primary');
    byId('update').classList.remove('primary');
    byId('not-opened-download').textContent = NOT_OPENED_DOWNLOAD;
    byId('get').hidden = false;
    byId('come-back').hidden = false;
    byId('get-title').textContent = "Don't have Ember yet?";
    byId('tournaments-title').textContent = TOURNAMENTS_TITLE;
    byId('fallback-title').textContent = FALLBACK_TITLE;
    byId('fallback-lead').textContent = FALLBACK_LEAD;
    byId('version').textContent = VERSION_NOTE;
    byId('download').href = STABLE_DOWNLOAD;
    byId('not-opened-download').href = STABLE_DOWNLOAD;
    clearTimeout(openTimer);
    clearTimeout(recordTimer);
    recording = false;
    byId('title').textContent = 'Open in Ember';
    byId('status').textContent = '';
    byId('open').href = '#';
    ['lead', 'after', 'lasts', 'code', 'steps', 'need', 'update', 'update-how'].forEach(function (id) { byId(id).textContent = ''; });
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
    // A match's bridge names its game, which decides which Ember Open starts:
    // until it answers, or the wait ends, the page offers nothing to open.
    if (found.match && askable(found)) lookUp(found, path, '/link/' + found.bridge + '/' + found.match);
    else if (found.room && window.fetch && found.bridge === PRODUCTION_BRIDGE) roomLookUp(found, path);
    else if ((found.match || found.room) && window.fetch && found.bridge !== STAGING_BRIDGE) sfvBridgeLookUp(found, path);
    else draw(found, path)();
  }

  // Shows what found offers, and returns what to do once it shows: open a
  // match by itself, or nothing.
  function draw(found, path) {
    var sfv = found.game === 'sfv';
    document.getElementById('lead').textContent = found.lead;
    if (found.phone) document.getElementById('phone').textContent = found.phone;
    document.getElementById('page').hidden = false;
    // Public rooms, matches and Connect Discord open only in Ember 1.1.0 or
    // newer, and those of the staging bridge only in Nightly. None of that,
    // nor SF4 Ember's download, is SFV Ember's.
    document.getElementById('version').hidden = path.indexOf('/j') === 0 || path.indexOf('/v') === 0 || sfv;
    document.getElementById('not-opened-download').hidden = sfv;
    if (found.bridge === STAGING_BRIDGE) {
      document.getElementById('version').textContent = NIGHTLY_NOTE;
      document.getElementById('download').href = NIGHTLY_DOWNLOAD;
      document.getElementById('not-opened-download').href = NIGHTLY_DOWNLOAD;
    }
    // A phone can neither open Ember nor install it.
    if (PHONE || sfv) document.getElementById('get').hidden = true;
    if (!found.ember) {
      document.getElementById('get-title').textContent = 'Get Ember';
      document.getElementById('come-back').hidden = true;
      document.getElementById('tournaments').hidden = false;
      if (PHONE) document.getElementById('tournaments-title').textContent = 'Connect Discord on your PC';
      return function () {};
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
    document.getElementById('fallback').hidden = false;
    if (PHONE) {
      document.getElementById('fallback-title').textContent = PHONE_FALLBACK_TITLE;
      document.getElementById('fallback-lead').textContent = (found.needText ? found.needText + ' ' : '') + PHONE_FALLBACK_LEAD;
      return function () {};
    }
    document.getElementById('link').hidden = false;
    if (found.need) showNeed(found);
    // A match opens by itself once per link while this page is open, and only
    // where Ember opened before from a match page asking for the same version.
    // An SFV match never does: it needs no version (asSfv).
    if (path !== '/m' || !found.need || found.link === autoOpened || rememberedOpen() !== found.need) return function () {};
    return function () {
      if (current !== found) return;
      autoOpened = found.link;
      document.getElementById('after').textContent = AUTO_AFTER;
      location.href = found.ember;
    };
  }

  // Only SF4 Ember's two bridges are asked here (SFV Ember's through
  // sfvBridgeLookUp): nginx on this site reaches no other.
  function askable(found) {
    return !!window.fetch && (found.bridge === PRODUCTION_BRIDGE || found.bridge === STAGING_BRIDGE);
  }

  // Asks the match's bridge whether the match is over and which game it is,
  // then draws the page once, for that game. No answer within OVER_WAIT_MS,
  // any failure, or an answer naming no game (a bridge from before games)
  // draws it for SF4 Ember, as before games. An answer after the wait still
  // shows: a match that is over, or one that is SFV's, drawn again for SFV
  // Ember.
  // A public room's game: the production bridge's SFV list (/rooms-sfv.json,
  // the site's own cached proxy) names it, or it is SF4's. The page draws
  // once, for that game; no answer within OVER_WAIT_MS draws it as SF4's.
  function roomLookUp(found, path) {
    var drawn = false;
    function settle(sfv) {
      if (drawn || current !== found) return;
      drawn = true;
      clearTimeout(timer);
      if (sfv) asSfvRoom(found);
      draw(found, path)();
    }
    var timer = setTimeout(function () { settle(false); }, OVER_WAIT_MS);
    fetch('/rooms-sfv.json', { cache: 'no-store', credentials: 'omit' })
      .then(function (response) { return response.ok ? response.json() : null; })
      .then(function (body) {
        var rooms = body && Array.isArray(body.rooms) ? body.rooms : [];
        settle(rooms.some(function (room) { return room && typeof room.room_id === 'string' && room.room_id.toLowerCase() === found.room; }));
      }, function () { settle(false); });
  }

  // A room or match on another bridge than SF4 Ember's two: SFV Ember's own
  // bridge (docs/design/SFV_BRIDGE.md) has every room and match on it SFV's.
  // Its ID is not built into this page; /rooms-sfv.json, the site's cached
  // proxy of that bridge's listing, names it (bridge_id). A match on it then
  // asks /link-sfv/MATCH whether it is over. Any other bridge, no answer within
  // OVER_WAIT_MS, or any failure draws the page as before, for SF4 Ember.
  function sfvBridgeLookUp(found, path) {
    var drawn = false;
    function settle(sfv) {
      if (drawn || current !== found) return;
      drawn = true;
      clearTimeout(timer);
      if (!sfv) draw(found, path)();
      else if (found.room) { asSfvRoom(found); draw(found, path)(); }
      else { asSfv(found); lookUp(found, path, '/link-sfv/' + found.match); }
    }
    var timer = setTimeout(function () { settle(false); }, OVER_WAIT_MS);
    fetch('/rooms-sfv.json', { cache: 'no-store', credentials: 'omit' })
      .then(function (response) { return response.ok ? response.json() : null; })
      .then(function (body) {
        settle(!!body && typeof body.bridge_id === 'string' && body.bridge_id === found.bridge);
      }, function () { settle(false); });
  }

  function lookUp(found, path, url) {
    var drawn = false;
    function settle(body) {
      if (drawn || current !== found) return;
      drawn = true;
      clearTimeout(timer);
      if (body && body.game === 'sfv') asSfv(found);
      var proceed = draw(found, path);
      if (body && body.over === true) showOver();
      else proceed();
    }
    var timer = setTimeout(function () { settle(null); }, OVER_WAIT_MS);
    fetch(url, { cache: 'no-store', credentials: 'omit' })
      .then(function (response) { return response.ok ? response.json() : null; })
      .then(function (body) {
        if (current !== found) return;
        if (!drawn) {
          settle(body);
        } else if (body && body.over === true) {
          showOver();
        } else if (body && body.game === 'sfv' && found.game !== 'sfv') {
          reset();
          current = found;
          document.title = found.title;
          document.getElementById('title').textContent = found.title;
          asSfv(found);
          draw(found, path);
        }
      }, function () { settle(null); });
  }

  // The match a link names has ended: nothing to open, get or copy.
  function showOver() {
    ['link', 'fallback', 'get', 'phone'].forEach(function (id) { document.getElementById(id).hidden = true; });
    document.getElementById('title').textContent = OVER_TITLE;
    document.getElementById('lead').textContent = OVER_LEAD;
    document.title = OVER_TITLE;
  }

  // A match plays only when both fighters run the same build. This page cannot
  // see which Ember a PC has, so it offers update help and never claims the
  // player needs it: Open in Ember always leads. A real mismatch still fails
  // in Ember, which names it.
  function showNeed(found) {
    var byId = function (id) { return document.getElementById(id); };
    byId('need').textContent = found.needText;
    byId('update').textContent = found.update;
    byId('update').href = found.need === 'nightly' ? NIGHTLY_DOWNLOAD : STABLE_DOWNLOAD;
    byId('update-how').textContent = found.updateHow;
    byId('not-opened-download').textContent = found.update;
    if (found.need !== 'nightly') byId('version').textContent = 'The newest Ember is ' + STABLE_VERSION + '.';
    ['need', 'update', 'update-how'].forEach(function (id) { byId(id).hidden = false; });
  }

  // After Open in Ember, a page that still has focus a moment later most
  // likely had no Ember to hand the link to. Browsers cannot tell for sure,
  // so this only reveals help and never hides any.
  var openTimer = 0;
  var recordTimer = 0;
  var leftPage = false;
  var recording = false;
  function opened() {
    // SFV Ember's link says nothing of SF4 Ember's handler.
    var flagged = !current || current.game !== 'sfv';
    leftPage = false;
    recording = flagged;
    clearTimeout(openTimer);
    clearTimeout(recordTimer);
    openTimer = setTimeout(function () {
      if (!leftPage && !document.hidden && document.hasFocus()) document.getElementById('not-opened').hidden = false;
    }, OPEN_WAIT_MS);
    recordTimer = setTimeout(function () {
      recording = false;
      if (flagged && !leftPage) forgetOpen();
    }, RECORD_WAIT_MS);
  }
  function left() { leftPage = true; }
  // Ember taking focus right after the click is the sign it opened. Leaving
  // the page does not count: a browser with no handler for ember: may replace
  // the page with an error.
  function lostFocus() {
    if (recording && !leftPage) rememberOpen();
    left();
  }

  // The flag that lets a match open by itself. Storage may be off or full;
  // then the page simply waits for a click, as it did before.
  var autoOpened = null;
  function rememberedOpen() {
    try { return localStorage.getItem(OPENED_KEY); } catch (error) { return null; }
  }
  // A match page records the version it asked for. Any other page records 1,
  // and never over a version a match page recorded.
  function rememberOpen() {
    var value = current && current.need ? current.need : rememberedOpen() || '1';
    try { localStorage.setItem(OPENED_KEY, value); } catch (error) { /* stays manual */ }
  }
  // Only after a click that Ember did not answer. An automatic open that does
  // nothing proves less (Chrome opens an app without a click only once the
  // player chose to always allow it), so it keeps the flag.
  function forgetOpen() {
    try { localStorage.removeItem(OPENED_KEY); } catch (error) { /* nothing stored */ }
  }

  // Ember runs on the PC the player plays on, not a phone or tablet.
  function onPhone() {
    var agent = navigator.userAgent || '';
    if (navigator.userAgentData && navigator.userAgentData.mobile) return true;
    // iPadOS asks for desktop pages as a Mac with touch.
    return /Android|iPhone|iPad|iPod|Mobile/i.test(agent) || (/Macintosh/.test(agent) && navigator.maxTouchPoints > 1);
  }

  var FALLBACK_LEAD, FALLBACK_TITLE, TOURNAMENTS_TITLE, NOT_OPENED_DOWNLOAD, PHONE, PHONE_NOTE;
  function start() {
    PHONE_NOTE = document.getElementById('phone').textContent;
    FALLBACK_LEAD = document.getElementById('fallback-lead').textContent;
    NOT_OPENED_DOWNLOAD = document.getElementById('not-opened-download').textContent;
    FALLBACK_TITLE = document.getElementById('fallback-title').textContent;
    TOURNAMENTS_TITLE = document.getElementById('tournaments-title').textContent;
    PHONE = onPhone();
    document.getElementById('open').addEventListener('click', function () {
      if (current && current.ember) opened();
    });
    window.addEventListener('blur', lostFocus);
    window.addEventListener('pagehide', left);
    document.addEventListener('visibilitychange', function () { if (document.hidden) lostFocus(); });
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
