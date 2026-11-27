// needs: site account answers
// The visitor's session on the pages that read it (/me, /players, /points;
// docs/design/SITE_PLAYER_CARDS.md, "Signing in"). session() asks
// /account/me.json once a page load and every caller shares the answer, read
// by answers.js. It settles on { state } with state one of signed_out,
// no_account, off, limited or failed, or on answers.session's signed-in model.
// show() is the one place the parts that stand for a state (parts/session.html:
// the card on its way, Connect Discord, the account Ember has not seen, not
// available) are shown and hidden.
//
// A visitor who follows Connect Discord from a player's card comes back to
// it: the callback always ends at /me, which goes on to the card once
// (returnTo). Only /players# addresses are kept, so it never leaves the site.
(function () {
  'use strict';
  var account = window.EmberAccount;
  var answers = window.EmberAnswers;
  var ME = '/account/me.json';
  var SIGN_OUT = '/account/sign-out';
  var SIGN_IN = '/account/sign-in';
  var RETURN = 'ember-return';
  var CARDS = '/players#';
  // What the unavailable panel says for the states that mean the same on every page.
  var SAID = {
    off: ['No player cards here yet', 'This service does not offer player cards yet.'],
    limited: ['Too many requests', 'Too many requests. Try again in a minute.']
  };
  var asked = null;

  function read(answer) {
    if (answer.kind !== 'ok') return { state: answer.kind };
    var model = answers.session(answer.body);
    if (!model) return { state: 'failed' };
    if (model.state === 'signed_in') account.remember({ name: model.card.name, main: model.card.main });
    else account.forget();
    return model;
  }

  function session() {
    if (!asked) asked = account.ask(ME, { cache: 'no-store', credentials: 'same-origin' }).then(read);
    return asked;
  }
  // The session asked again, as Check again does.
  function again() {
    asked = null;
    return session();
  }

  // Ends the session in this browser; settles true once the bridge cleared
  // it. The header forgets the player first. The bridge takes a sign-out only
  // from this site's own pages, which a browser without Sec-Fetch-Site shows
  // by the request's Origin; the site's no-referrer policy would send
  // "null", so this request keeps its origin.
  function signOut() {
    account.forget();
    return account.ask(SIGN_OUT, { method: 'POST', cache: 'no-store', credentials: 'same-origin', referrerPolicy: 'same-origin' })
      .then(function (answer) {
        if (answer.kind !== 'ok') return false;
        asked = Promise.resolve({ state: 'signed_out' });
        return true;
      });
  }

  function keepReturn() {
    var here = location.pathname + location.hash;
    if (here.indexOf(CARDS) !== 0) return;
    try { sessionStorage.setItem(RETURN, here); } catch (error) { /* storage off: the visitor stays on /me */ }
  }
  // The card a visitor was on when they went to sign in, once; null for none.
  function returnTo() {
    var kept = null;
    try {
      kept = sessionStorage.getItem(RETURN);
      sessionStorage.removeItem(RETURN);
    } catch (error) { kept = null; }
    return kept && kept.indexOf(CARDS) === 0 ? kept : null;
  }

  // Use another Discord account: out of this one, then Discord's sign-in. A
  // finished sign-in replaces the session cookie, so a sign-out that did not
  // go through does not stop it.
  function useAnother() {
    keepReturn();
    signOut().then(function () { location.href = SIGN_IN; });
  }

  // The state's parts shown and the rest hidden. words: connect, the heading
  // of Connect Discord on this page; failed, [title, note] of the unavailable
  // panel when the page could not read or reach what it asked for.
  function show(state, words) {
    var part = function (id) { return document.getElementById(id); };
    part('loading').hidden = state !== 'loading';
    part('connect').hidden = state !== 'signed_out' && state !== 'no_account';
    part('signed-out').hidden = state !== 'signed_out';
    part('unknown-account').hidden = state !== 'no_account';
    part('connect-title').textContent = words.connect;
    var said = state === 'failed' ? words.failed : SAID[state];
    part('unavailable').hidden = !said;
    if (said) {
      part('unavailable-title').textContent = said[0];
      part('unavailable-note').textContent = said[1];
    }
  }

  // Every Connect Discord link on the page keeps the card it was followed from.
  Array.prototype.forEach.call(document.querySelectorAll('a[href="' + SIGN_IN + '"]'), function (link) {
    link.addEventListener('click', keepReturn);
  });

  window.EmberSession = {
    SIGN_IN: SIGN_IN,
    session: session, again: again, signOut: signOut, useAnother: useAnother, returnTo: returnTo, show: show
  };
}());
