// needs: site marks account answers session blocks card
// /players: another player's card (docs/design/SITE_PLAYER_CARDS.md), by
// #card=<token> from a board or a shared link, or #name=<name>, which finds
// whom the game's own search finds. It asks POST /account/card.json, which
// needs a browser signed in with a Discord account an Ember ID has connected:
// signed out it shows Connect Discord, and with an account Ember has not seen
// how to connect one (session.js shows both). A name several players have
// lists them to choose from. When a card or a list comes, focus moves to its
// heading. Names are the players' own words, so they only ever go in as text.
(function () {
  'use strict';
  var site = window.EmberSite;
  var marks = window.EmberMarks;
  var account = window.EmberAccount;
  var answers = window.EmberAnswers;
  var sessions = window.EmberSession;
  var blocks = window.EmberBlocks;
  var cards = window.EmberCard;
  var el = site.el;
  var CARD = '/account/card.json';
  // The shortest name a search takes (ember_protocol::cards::MIN_SEARCH_NAME).
  var MIN_NAME = 2;
  var PAGE_TITLE = document.title;
  var WORDS = {
    connect: 'Connect Discord to open player cards',
    failed: ['Player cards are not available right now', 'Try again in a minute, or open the card in Ember from a room or your friends.']
  };
  var UNREAD = ['Player cards are not available right now', 'The answer could not be read. Try again in a minute.'];

  var crumb = document.getElementById('crumb');
  var status = document.getElementById('status');
  var cardView = document.getElementById('card-view');
  var playerCard = document.getElementById('player-card');
  var choose = document.getElementById('choose');
  var chooseTitle = document.getElementById('choose-title');
  var choices = document.getElementById('choices');
  var notFound = document.getElementById('not-found');
  var notFoundNote = document.getElementById('not-found-note');
  var asked = 0;

  // What the address asks for: { card } or { name }; { broken: true } for a
  // card that is no token, or null for neither.
  function wanted() {
    var params;
    try { params = new URLSearchParams(location.hash.replace(/^#/, '')); } catch (error) { return null; }
    var token = params.get('card');
    if (token !== null) return marks.TOKEN.test(token) ? { card: token } : { broken: true };
    var name = (params.get('name') || '').trim();
    return name ? { name: name } : null;
  }

  // The page for a state: session.js's (loading, signed_out, no_account,
  // off, limited, failed with what failed says) or this page's own (card,
  // choose, not_found).
  function showState(state, failed) {
    sessions.show(state, { connect: WORDS.connect, failed: failed || WORDS.failed });
    cardView.hidden = state !== 'card';
    choose.hidden = state !== 'choose';
    notFound.hidden = state !== 'not_found';
  }

  // The crumb and the window's title name what is shown.
  function named(text) {
    crumb.textContent = text || 'Player card';
    document.title = text ? text + ' - ' + PAGE_TITLE : PAGE_TITLE;
  }

  function missing(text) {
    named(null);
    notFoundNote.textContent = text;
    showState('not_found');
  }

  function showCard(card, ask) {
    named(card.name);
    var link = marks.cardLink(ask.card ? 'card=' + ask.card : 'name=' + encodeURIComponent(ask.name));
    var heading = cards.draw(playerCard, card, { share: { url: link, done: 'Card link copied.' } });
    showState('card');
    heading.focus();
  }

  // A name several players have: each with their main, flag, class and when
  // they came to Ember, as a link to their card.
  function showChoices(players, ask) {
    named(ask.name);
    chooseTitle.textContent = 'Players named “' + ask.name + '”';
    while (choices.firstChild) choices.removeChild(choices.firstChild);
    players.forEach(function (hit) {
      var link = choices.appendChild(el('li', 'player-choice')).appendChild(el('a', 'player-choice-link'));
      link.href = '#card=' + hit.card;
      if (hit.main === null) link.appendChild(el('span', 'face none small')).setAttribute('aria-hidden', 'true');
      else link.appendChild(marks.face(hit.main, 'small'));
      if (hit.country) link.appendChild(marks.flag(hit.country));
      link.appendChild(el('span', 'player-choice-name', hit.name));
      if (hit.badge) link.appendChild(marks.badge(hit.badge));
      if (hit.since !== null) link.appendChild(el('span', 'card-k player-choice-since', 'On Ember since ' + blocks.month(hit.since)));
    });
    showState('choose');
    chooseTitle.focus();
  }

  function answered(answer, ask) {
    if (answer.kind !== 'ok') return showState(answer.kind);
    var found = answers.lookup(answer.body);
    if (!found) return showState('failed', UNREAD);
    if (found.found === 'players') return showChoices(found.players, ask);
    if (found.found === 'card') return showCard(found.card, ask);
    missing(ask.card ? 'This card link does not open any more. A link to a card opens for a week after it was made.' :
      'No player is named “' + ask.name + '”. Names are matched in full, without case.');
  }

  function load() {
    var ask = wanted();
    status.textContent = '';
    if (!ask) return missing('This address names no player.');
    if (ask.broken) return missing('This is not a card link.');
    if (ask.name && Array.from(ask.name).length < MIN_NAME) return missing('A name to find needs at least ' + MIN_NAME + ' characters.');
    var mine = ++asked;
    showState('loading');
    var found = account.ask(CARD, {
      method: 'POST', cache: 'no-store', credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(ask)
    });
    // The flag index first, so the card is drawn once, with its flags.
    Promise.all([found, marks.flagsReady()]).then(function (both) {
      if (mine === asked) answered(both[0], ask);
    });
  }

  document.getElementById('check-again').addEventListener('click', load);
  document.getElementById('use-another').addEventListener('click', sessions.useAnother);
  window.addEventListener('hashchange', load);
  load();
}());
