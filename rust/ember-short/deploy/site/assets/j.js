// Reads the room code from the link and shows it with copy buttons. The code
// stays in this browser: it is in the URL fragment, which is never sent to
// the server, and this page makes no requests of its own.
(function () {
  'use strict';
  var ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
  var LINK = 'https://embernetplay.link/j#';

  // The canonical 12-symbol code, or null. Same rules as the game:
  // dashes and spaces ignored, any case, O read as 0 and I or L as 1.
  function parse(text) {
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
    return code.length === 12 ? code : null;
  }

  function display(code) {
    return code.slice(0, 4) + '-' + code.slice(4, 8) + '-' + code.slice(8);
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
      status.textContent = ok ? done : 'Select the code above and copy it.';
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { status.textContent = done; }, fallback);
    } else {
      fallback();
    }
  }

  function start() {
    var fromPath = location.pathname.replace(/^\/j\/?/, '');
    var code = parse(location.hash.replace(/^#/, '')) || parse(fromPath);
    if (!code) {
      document.getElementById('missing').hidden = false;
      return;
    }
    var shown = display(code);
    var link = LINK + shown;
    // A code typed into the path moves into the fragment, out of later requests.
    if (fromPath && history.replaceState) history.replaceState(null, '', '/j#' + shown);
    document.getElementById('code').textContent = shown;
    document.getElementById('copy-link').addEventListener('click', function () { copy(link, 'Link copied. Paste it into Ember.'); });
    document.getElementById('copy-code').addEventListener('click', function () { copy(shown, 'Code copied. Paste it into Ember.'); });
    document.getElementById('room').hidden = false;
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
