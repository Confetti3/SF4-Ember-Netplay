// needs: marks session card rating
// /me: the visitor's own card (docs/design/SITE_PLAYER_CARDS.md). What it
// shows is the session's state (session.js): signed out, the Connect Discord
// panel, whose button is a plain link to the sign-in; signed in to a Discord
// account no Ember ID has connected, how to connect one; signed in, the card
// (#card) and the rating details (#rating), each its own view under the tabs.
// #sign-in-failed is where a sign-in that did not finish comes back to. A
// visitor who went to sign in from a player's card goes back to it.
(function () {
  'use strict';
  var marks = window.EmberMarks;
  var sessions = window.EmberSession;
  var cards = window.EmberCard;
  var rating = window.EmberRating;
  var LEADS = {
    out: 'Your points, your place on every board and the numbers behind your class, in one place.',
    card: 'The card other players open from a room, a board or your link. The parts marked with a lock are shown only to you.',
    rating: 'The three numbers behind your class, what each one means, and how a game moves them.'
  };
  var WORDS = {
    connect: 'Connect Discord to open your card',
    failed: ['Your card is not available right now', 'Try again in a minute, or look in Ember under Profile.']
  };

  var lead = document.getElementById('lead');
  var tabs = document.getElementById('tabs');
  var tabLinks = { card: document.getElementById('tab-card'), rating: document.getElementById('tab-rating') };
  var status = document.getElementById('status');
  var failedNote = document.getElementById('sign-in-failed');
  var cardView = document.getElementById('card-view');
  var ownCard = document.getElementById('own-card');
  var ratingView = document.getElementById('rating-view');
  var ratingMissing = document.getElementById('rating-missing');
  var checkAgain = document.getElementById('check-again');
  var signOutButton = document.getElementById('sign-out');

  // The state on screen (session.js's, or 'loading'), and whether the rating
  // details could be read when signed in.
  var shown = 'loading';
  var detailsRead = false;

  function view() { return location.hash === '#rating' ? 'rating' : 'card'; }

  // The two signed-in views under their tabs, from the hash.
  function showView() {
    var current = view();
    cardView.hidden = current !== 'card';
    ratingView.hidden = current !== 'rating' || !detailsRead;
    ratingMissing.hidden = current !== 'rating' || detailsRead;
    if (current === 'rating' && detailsRead) rating.shown(ratingView);
    lead.textContent = LEADS[current];
    Object.keys(tabLinks).forEach(function (name) {
      if (name === current) tabLinks[name].setAttribute('aria-current', 'page');
      else tabLinks[name].removeAttribute('aria-current');
    });
  }

  function showState(state) {
    shown = state;
    sessions.show(state, WORDS);
    failedNote.hidden = !(state === 'signed_out' && location.hash === '#sign-in-failed');
    tabs.hidden = state !== 'signed_in';
    if (state === 'signed_in') {
      showView();
    } else {
      cardView.hidden = ratingView.hidden = ratingMissing.hidden = true;
      lead.textContent = LEADS.out;
    }
  }

  // The signed-in model drawn into both views. Rating details that cannot be
  // read leave the card as it is and say so in their own view.
  function drawOwn(model) {
    cards.draw(ownCard, model.card, {
      own: model.own,
      ratingHash: '#rating',
      share: { url: marks.cardLink('card=' + model.link), done: 'Card link copied.' }
    });
    detailsRead = !!model.details;
    if (model.details) rating.draw(ratingView, model.details);
  }

  function show(model) {
    if (model.state === 'signed_in') {
      var back = sessions.returnTo();
      if (back) {
        location.replace(back);
        return;
      }
      drawOwn(model);
    }
    showState(model.state);
  }

  checkAgain.addEventListener('click', function () {
    checkAgain.disabled = true;
    status.textContent = '';
    sessions.again().then(function (model) {
      checkAgain.disabled = false;
      show(model);
      if (model.state === 'no_account') status.textContent = 'Ember has still not seen this Discord account.';
    });
  });
  document.getElementById('use-another').addEventListener('click', sessions.useAnother);
  signOutButton.addEventListener('click', function () {
    signOutButton.disabled = true;
    sessions.signOut().then(function (done) {
      signOutButton.disabled = false;
      if (!done) {
        status.textContent = 'Could not sign out. Try again in a minute.';
        return;
      }
      status.textContent = 'Signed out.';
      try { history.replaceState(null, '', location.pathname); } catch (error) { /* file: pages */ }
      showState('signed_out');
    });
  });
  window.addEventListener('hashchange', function () {
    if (shown === 'signed_in') showView();
  });

  showState('loading');
  // The flag index first, so the card is drawn once, with its flag.
  Promise.all([sessions.session(), marks.flagsReady()]).then(function (both) { show(both[0]); });
}());
