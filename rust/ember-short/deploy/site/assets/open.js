// Opens an Ember link: a room invitation (/j#CODE) or a tournament match
// (/m#BRIDGE/MATCH). It reads the link from the URL fragment, which is never
// sent to the server, makes no requests of its own, and offers the ember:
// link only on the player's click, never by itself: a browser without
// Ember's handler would otherwise replace this page with an error.
(function () {
  'use strict';
  var ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
  // brg_ or emt_ and a lowercase version 4 UUID, as the bridge writes them.
  var ID = /^[a-z]{3}_[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

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
      steps: ['Start SF4 Ember Netplay.', 'Choose <strong>Settings</strong>, <strong>Ember ID</strong>, then <strong>Tournament matches</strong>.',
        'Choose <strong>Paste match link</strong>, then <strong>Play</strong>.'],
      lasts: 'Only the two players in this match can play it, so it is safe to share this link with them.'
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

  function start() {
    var fragment = location.hash.replace(/^#/, '');
    var path = location.pathname;
    var found = path.indexOf('/m') === 0 ? match(path, fragment) : room(path, fragment);
    if (!found) {
      document.getElementById('missing').hidden = false;
      return;
    }
    document.title = found.title;
    document.getElementById('title').textContent = found.title;
    document.getElementById('lead').textContent = found.lead;
    document.getElementById('after').textContent = found.after;
    document.getElementById('lasts').textContent = found.lasts;
    document.getElementById('open').href = found.ember;
    // The steps are this page's own fixed text.
    document.getElementById('steps').innerHTML = found.steps.map(function (step) { return '<li>' + step + '</li>'; }).join('');
    document.getElementById('copy-link').addEventListener('click', function () { copy(found.link, 'Link copied. Paste it into Ember.'); });
    if (found.code) {
      var code = document.getElementById('code');
      code.textContent = found.code;
      code.hidden = false;
      var copyCode = document.getElementById('copy-code');
      copyCode.hidden = false;
      copyCode.addEventListener('click', function () { copy(found.code, 'Code copied. Paste it into Ember.'); });
    }
    document.getElementById('link').hidden = false;
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
