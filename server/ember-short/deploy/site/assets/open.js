// Opens an Ember link: a room invitation (/j#CODE), a tournament match
// (/m#BRIDGE/MATCH), a public room of a service (/r#BRIDGE/ROOM), or, at
// /start, tells a player new to Ember how to get it
// and connect Discord for tournament sites. A site that names its service
// (/start#BRIDGE) gets a button that opens Ember's Connect Discord screen for
// it, and one that also gives its own name (/start#BRIDGE/NAME) is named to
// the player: a name of 1 to 40 letters, digits, spaces and . ' & -, starting
// with a letter or digit (SITE), is shown as text, and any other is dropped.
// It reads the link from the URL
// fragment, which is never sent to the server, and makes no requests of its own.
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
    var staging = parts[0] === STAGING_BRIDGE;
    return {
      bridge: parts[0],
      need: staging ? 'nightly' : STABLE_VERSION,
      needText: staging ? 'This match needs the newest Ember Nightly on both players\' PCs.' :
        'This match needs Ember ' + STABLE_VERSION + ' on both players\' PCs. Ember shows its version in the corner of Home.',
      update: staging ? 'Get the newest Nightly' : 'Update to Ember ' + STABLE_VERSION,
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

  // A public room: the service and room a bot or site's link names.
  function publicRoom(path, fragment) {
    var parts = fragment.replace(/\/$/, '').split('/');
    if (path.replace(/\/$/, '') !== '/r' || parts.length !== 2 || !ID.test(parts[0]) || parts[0].slice(0, 4) !== 'brg_' ||
      !ROOM.test(parts[1])) return null;
    return {
      bridge: parts[0],
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
    document.getElementById('lead').textContent = found.lead;
    document.getElementById('page').hidden = false;
    // Public rooms, matches and Connect Discord open only in Ember 1.1.0 or
    // newer, and those of the staging bridge only in Nightly.
    document.getElementById('version').hidden = path.indexOf('/j') === 0;
    if (found.bridge === STAGING_BRIDGE) {
      document.getElementById('version').textContent = NIGHTLY_NOTE;
      document.getElementById('download').href = NIGHTLY_DOWNLOAD;
      document.getElementById('not-opened-download').href = NIGHTLY_DOWNLOAD;
    }
    // A phone can neither open Ember nor install it.
    if (PHONE) document.getElementById('get').hidden = true;
    if (!found.ember) {
      document.getElementById('get-title').textContent = 'Get Ember';
      document.getElementById('come-back').hidden = true;
      document.getElementById('tournaments').hidden = false;
      if (PHONE) document.getElementById('tournaments-title').textContent = 'Connect Discord on your PC';
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
    document.getElementById('fallback').hidden = false;
    if (PHONE) {
      document.getElementById('fallback-title').textContent = PHONE_FALLBACK_TITLE;
      document.getElementById('fallback-lead').textContent = (found.needText ? found.needText + ' ' : '') + PHONE_FALLBACK_LEAD;
      return;
    }
    document.getElementById('link').hidden = false;
    var known = found.need && rememberedOpen() === found.need;
    if (found.need) showNeed(found, known);
    // A match opens by itself once per link while this page is open, and only
    // where Ember opened before from a match page asking for the same version.
    if (path === '/m' && found.link !== autoOpened && known) {
      autoOpened = found.link;
      document.getElementById('after').textContent = AUTO_AFTER;
      location.href = found.ember;
    }
  }

  // A match plays only when both fighters run the same build, and Ember cannot
  // tell this page which one it is. Until Ember has opened from a match page
  // asking for this version, Update leads and Open follows it.
  function showNeed(found, known) {
    var byId = function (id) { return document.getElementById(id); };
    byId('need').textContent = found.needText;
    byId('update').textContent = found.update;
    byId('update').href = found.need === 'nightly' ? NIGHTLY_DOWNLOAD : STABLE_DOWNLOAD;
    byId('update-how').textContent = found.updateHow;
    byId('not-opened-download').textContent = found.update;
    if (found.need !== 'nightly') byId('version').textContent = 'Tournament matches need Ember ' + STABLE_VERSION + '.';
    ['need', 'update', 'update-how'].forEach(function (id) { byId(id).hidden = false; });
    if (!known) {
      byId('update').classList.add('primary');
      byId('open').classList.remove('primary');
    }
  }

  // After Open in Ember, a page that still has focus a moment later most
  // likely had no Ember to hand the link to. Browsers cannot tell for sure,
  // so this only reveals help and never hides any.
  var openTimer = 0;
  var recordTimer = 0;
  var leftPage = false;
  var recording = false;
  function opened() {
    leftPage = false;
    recording = true;
    clearTimeout(openTimer);
    clearTimeout(recordTimer);
    openTimer = setTimeout(function () {
      if (!leftPage && !document.hidden && document.hasFocus()) document.getElementById('not-opened').hidden = false;
    }, OPEN_WAIT_MS);
    recordTimer = setTimeout(function () {
      recording = false;
      if (!leftPage) forgetOpen();
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

  var FALLBACK_LEAD, FALLBACK_TITLE, TOURNAMENTS_TITLE, NOT_OPENED_DOWNLOAD, PHONE;
  function start() {
    FALLBACK_LEAD = document.getElementById('fallback-lead').textContent;
    NOT_OPENED_DOWNLOAD = document.getElementById('not-opened-download').textContent;
    FALLBACK_TITLE = document.getElementById('fallback-title').textContent;
    TOURNAMENTS_TITLE = document.getElementById('tournaments-title').textContent;
    PHONE = onPhone();
    document.getElementById('phone').hidden = !PHONE;
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
