// Which game a rooms or rankings page shows: ?game=sfv is Street Fighter V's,
// in SFV Ember's own look (site.css, html[data-game="sfv"]), and anything else
// is Ultra Street Fighter IV's. Loaded in the head without defer, so the page
// is drawn in its game's colours from the first paint. The page's own script
// changes game with EmberGame.set. Words that are one game's carry
// data-game-text and site.css shows them only in that game's view, so they
// are right from the first paint too. EmberGame.set also points a link with
// data-sfv-href there in SFV's view (and back to its own href in SF4's), and
// marks the shown game on the Game switch ([data-game-switch]).
(function () {
  'use strict';
  var root = document.documentElement;
  var key = /(?:^|[?&])game=sfv(?:&|$)/.test(location.search) ? 'sfv' : 'usf4';

  function apply() {
    root.setAttribute('data-game', key);
    if (document.readyState === 'loading') return;
    var i;
    var links = document.querySelectorAll('a[data-sfv-href]');
    for (i = 0; i < links.length; i++) {
      var link = links[i];
      if (!link.hasAttribute('data-usf4-href')) link.setAttribute('data-usf4-href', link.getAttribute('href'));
      link.setAttribute('href', link.getAttribute(key === 'sfv' ? 'data-sfv-href' : 'data-usf4-href'));
    }
    var buttons = document.querySelectorAll('[data-game-switch] [data-game]');
    for (i = 0; i < buttons.length; i++) buttons[i].setAttribute('aria-pressed', buttons[i].getAttribute('data-game') === key ? 'true' : 'false');
  }

  apply();
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', apply);
  window.EmberGame = {
    key: function () { return key; },
    // The address's query for the shown game: none for USF4's, as before games.
    query: function () { return key === 'sfv' ? '?game=sfv' : ''; },
    set: function (next) {
      key = next === 'sfv' ? 'sfv' : 'usf4';
      apply();
    }
  };
}());
