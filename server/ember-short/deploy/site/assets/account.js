// needs: site marks
// What the site's own JSON answers mean, and the header's account link. Every
// page that runs scripts loads this (scripts/site/shell.py).
//
// ask(path, init) is the one place an HTTP answer is read. It settles on
//   { kind: 'ok', body }   2xx, the body parsed (null for an empty one)
//   { kind: 'off' }        404: this service does not offer the route yet
//   { kind: 'signed_out' } 401: the browser is not signed in
//   { kind: 'no_account' } 403: its Discord account has no Ember ID
//   { kind: 'limited' }    429: too many requests for now
//   { kind: 'failed' }     anything else: another status, no answer at all,
//                          or a body that is not JSON
// and each page says what each kind means for it.
//
// The link reads "Your card" for anyone. Once this tab has seen a signed-in
// answer (session.js) it shows that player's main fighter and name, kept in
// sessionStorage so the tab's other pages show them without asking; any
// answer that says the browser is signed out, or has no Ember ID, and signing
// out forget them.
(function () {
  'use strict';
  var el = window.EmberSite.el;
  var marks = window.EmberMarks;
  var KEPT = 'ember-account';
  var KINDS = { 401: 'signed_out', 403: 'no_account', 404: 'off', 429: 'limited' };

  function ask(path, init) {
    return fetch(path, init)
      .then(function (response) {
        if (!response.ok) return { kind: KINDS[response.status] || 'failed' };
        if (response.status === 204) return { kind: 'ok', body: null };
        return response.json().then(function (body) { return { kind: 'ok', body: body }; });
      })
      .catch(function () { return { kind: 'failed' }; })
      .then(function (answer) {
        if (answer.kind === 'signed_out' || answer.kind === 'no_account') forget();
        return answer;
      });
  }

  // What the header shows of the player: { name, main }, each null when the
  // card had none, or null when no signed-in answer is kept.
  function remembered() {
    var kept = null;
    try { kept = JSON.parse(sessionStorage.getItem(KEPT) || 'null'); } catch (error) { kept = null; }
    if (!kept || typeof kept !== 'object') return null;
    return {
      name: typeof kept.name === 'string' && kept.name ? kept.name : null,
      main: marks.isFighter(kept.main) ? kept.main : null
    };
  }

  function drawLink() {
    var link = document.querySelector('.site-account');
    if (!link) return;
    var who = remembered();
    while (link.firstChild) link.removeChild(link.firstChild);
    if (who && who.main !== null) link.appendChild(marks.face(who.main, 'tiny'));
    link.appendChild(el('span', 'site-account-name', who && who.name ? who.name : 'Your card'));
    if (who && who.name) link.setAttribute('aria-label', 'Your card, ' + who.name);
    else link.removeAttribute('aria-label');
  }

  // A signed-in answer's player, for the header: { name, main }.
  function remember(player) {
    try { sessionStorage.setItem(KEPT, JSON.stringify({ name: player.name, main: player.main })); } catch (error) { /* storage off: the link stays plain */ }
    drawLink();
  }
  function forget() {
    try { sessionStorage.removeItem(KEPT); } catch (error) { /* storage off: nothing was kept */ }
    drawLink();
  }

  drawLink();
  window.EmberAccount = { ask: ask, remember: remember, forget: forget };
}());
