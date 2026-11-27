// needs: site marks account blocks filters answers session
// /points: the points boards (docs/design/SITE_PLAYER_CARDS.md): the account
// board, Battle points (#bp, /points.json), and each fighter's, Fighter points
// (#fp=20, /points.json?fighter=20), with a fighter picker in the game's
// character select order, the name, country and region filters of the class
// board, and the signed-in player's own place (session.js). The view and the
// filters are kept in the URL fragment so a view can be shared. #fp without a
// fighter shows the signed-in player's main, or else the first fighter of the
// roster's native order, Ryu. A board lists its first 500 players; this page
// shows them 50 at a time. Names are the players' own words, so they only ever
// go in as text.
(function () {
  'use strict';
  var site = window.EmberSite;
  var marks = window.EmberMarks;
  var account = window.EmberAccount;
  var blocks = window.EmberBlocks;
  var filters = window.EmberFilters;
  var answers = window.EmberAnswers;
  var sessions = window.EmberSession;
  var el = site.el;
  var FIGHTERS = marks.FIGHTERS;
  var PAGE = 50;
  var FALLBACK_FIGHTER = 0;
  var ACCOUNT = 'bp';
  // What a board that did not come says, by the kind of answer (account.js).
  var NOT_SHOWN = {
    off: ['This service does not offer points boards right now.', 'Points come from public room games. In Ember, open Online play, Public rooms to play.'],
    limited: ['Too many requests. Try again in a minute.', ''],
    failed: ['The board is not available right now.', 'Try again in a minute, or look in Ember under Profile.']
  };

  var viewLinks = { bp: document.getElementById('view-bp'), fp: document.getElementById('view-fp') };
  var pickerPanel = document.getElementById('picker-panel');
  var picker = document.getElementById('picker');
  var boardFace = document.getElementById('board-face');
  var boardTitle = document.getElementById('board-title');
  var boardCount = document.getElementById('board-count');
  var shareButton = document.getElementById('share');
  var toolbar = document.getElementById('toolbar');
  var search = document.getElementById('search');
  var you = document.getElementById('you');
  var summary = document.getElementById('summary');
  var board = document.getElementById('board');
  var moreBox = document.getElementById('more-box');
  var moreButton = document.getElementById('more');
  var status = document.getElementById('status');

  // board: ACCOUNT, or a fighter's ID; null while #fp waits for the session
  // to say whose main to show. limit: how many of the matching players show.
  var view = { board: null, limit: PAGE };
  // The name, country and region filters (filters.js).
  var filter = filters.controller({ search: search, country: document.getElementById('country'), region: document.getElementById('region') }, update);
  // Each board asked for this visit, by view.board: { phase: 'loading',
  // 'ready', or the kind of answer that stood for it (NOT_SHOWN) } and, when
  // ready, answers.board's players, total and listedAt.
  var boards = {};
  // The session (session.js) once it is in; null until then.
  var session = null;
  // The fighter #fp shows when it names none: the last one chosen this visit.
  var lastFighter = null;

  function isAccount() { return view.board === ACCOUNT; }
  function isFighterBoard() { return view.board !== ACCOUNT && view.board !== null; }
  function unit() { return isAccount() ? 'BP' : 'FP'; }
  function boardName() { return isAccount() ? 'Battle points' : isFighterBoard() ? FIGHTERS[view.board] : 'Fighter points'; }

  // The fighter #fp shows by itself: the one last chosen, the player's main,
  // or Ryu; null while the session that names the main is on its way.
  function defaultFighter() {
    if (lastFighter !== null) return lastFighter;
    if (!session) return null;
    return session.state === 'signed_in' && session.card.main !== null ? session.card.main : FALLBACK_FIGHTER;
  }

  function readHash() {
    var params;
    try { params = new URLSearchParams(location.hash.replace(/^#/, '')); } catch (error) { params = new URLSearchParams(''); }
    if (params.has('fp')) {
      var asked = params.get('fp');
      var id = /^\d{1,2}$/.test(asked) ? Number(asked) : -1;
      view.board = marks.isFighter(id) ? id : defaultFighter();
    } else {
      view.board = ACCOUNT;
    }
    filter.read(params);
    view.limit = PAGE;
  }
  function hashOf() {
    var set = filter.parts();
    return '#' + [isAccount() ? 'bp' : 'fp=' + view.board, set.q, set.country, set.region].filter(Boolean).join('&');
  }
  function writeHash() {
    if (view.board === null || location.hash === hashOf()) return;
    try { history.replaceState(null, '', location.pathname + hashOf()); } catch (error) { /* file: pages */ }
  }

  // A board is asked for once a visit; one that did not come is asked again
  // when it is shown again.
  function load() {
    var key = view.board;
    var kept = key === null ? null : boards[key];
    if (key === null || (kept && (kept.phase === 'loading' || kept.phase === 'ready' || kept.phase === 'off'))) {
      render();
      return;
    }
    boards[key] = { phase: 'loading' };
    render();
    var asked = account.ask(key === ACCOUNT ? '/points.json' : '/points.json?fighter=' + key, { cache: 'no-store', credentials: 'omit' })
      .then(function (answer) {
        if (answer.kind !== 'ok') return { phase: answer.kind === 'off' || answer.kind === 'limited' ? answer.kind : 'failed' };
        var read = answers.board(answer.body, key === ACCOUNT ? null : key);
        if (!read) return { phase: 'failed' };
        read.players.forEach(function (player) { player.folded = filters.fold(player.name); });
        read.phase = 'ready';
        return read;
      });
    // The flag index first, so the board is drawn once, with its flags.
    Promise.all([asked, marks.flagsReady()]).then(function (both) {
      boards[key] = both[0];
      if (view.board === key) render();
    });
  }

  function viewChips() {
    var current = isAccount() ? 'bp' : 'fp';
    Object.keys(viewLinks).forEach(function (name) {
      if (name === current) viewLinks[name].setAttribute('aria-current', 'true');
      else viewLinks[name].removeAttribute('aria-current');
    });
  }

  // The picker is one stop for the keyboard (a radio group): the chosen
  // fighter, or the first, takes focus; the arrow keys, Home and End move
  // through the roster and choose.
  function fillPicker() {
    marks.ROSTER.forEach(function (id) {
      var button = el('button', 'fighter-pick');
      button.type = 'button';
      button.setAttribute('role', 'radio');
      button.setAttribute('data-fighter', String(id));
      button.setAttribute('aria-label', FIGHTERS[id]);
      button.title = FIGHTERS[id];
      button.appendChild(marks.face(id));
      picker.appendChild(button);
    });
  }
  function markPicker() {
    pickerPanel.hidden = isAccount();
    var buttons = picker.children, any = false;
    for (var i = 0; i < buttons.length; i++) {
      var chosen = Number(buttons[i].getAttribute('data-fighter')) === view.board;
      any = any || chosen;
      buttons[i].setAttribute('aria-checked', chosen ? 'true' : 'false');
      buttons[i].tabIndex = chosen ? 0 : -1;
    }
    if (!any && buttons.length) buttons[0].tabIndex = 0;
  }
  function choose(button) {
    view.board = lastFighter = Number(button.getAttribute('data-fighter'));
    view.limit = PAGE;
    writeHash();
    load();
  }
  var STEPS = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 };

  function boardHead(state) {
    while (boardFace.firstChild) boardFace.removeChild(boardFace.firstChild);
    boardFace.hidden = !isFighterBoard();
    if (isFighterBoard()) boardFace.appendChild(marks.face(view.board, 'board'));
    boardTitle.textContent = boardName();
    boardCount.textContent = state.phase !== 'ready' || !state.total ? '' : isAccount() ?
      site.plural(state.total, 'player', 'players') + ', every fighter counted together' :
      site.plural(state.total, 'player has', 'players have') + ' points with ' + FIGHTERS[view.board];
  }

  // The player's own line over the board, from the session: their place,
  // name, frame and points, or what stands in for them.
  function ownLine() {
    while (you.firstChild) you.removeChild(you.firstChild);
    var state = session ? session.state : 'loading';
    you.hidden = view.board === null || (state !== 'signed_out' && state !== 'no_account' && state !== 'signed_in');
    if (you.hidden) return;
    if (state === 'signed_out' || state === 'no_account') {
      var signedOut = state === 'signed_out';
      you.appendChild(el('span', 'note board-you-text', signedOut ? 'Connect Discord to see your own place on every board and to open player cards.' :
        'Ember has not seen the Discord account you signed in with, so there is no place to show.'));
      var link = you.appendChild(el('a', 'button small', signedOut ? 'Connect Discord' : 'How to connect it'));
      link.href = signedOut ? sessions.SIGN_IN : '/me';
      return;
    }
    var own = isAccount() ? session.own.account : session.own.fighters[view.board];
    if (!own) {
      you.appendChild(el('span', 'note board-you-text', 'You have no points with ' + FIGHTERS[view.board] + ' yet. A fighter starts earning points with its first rated public game.'));
      return;
    }
    var placed = isAccount() ? session.card.places.account : session.card.places.fighters[view.board];
    you.appendChild(el('span', 'board-you-label', 'Your place'));
    you.appendChild(el('span', 'pts board-you-place', placed ? site.ordinal(placed) : 'Not listed'));
    you.appendChild(el('span', 'board-you-name', session.card.name || 'You'));
    you.appendChild(marks.frame(own.tier));
    you.appendChild(el('span', 'pts', marks.points(own.points, unit())));
  }

  // One player's line: their place, flag and name as a link to their card
  // (plain for the rare line the service sends without a card), class, frame
  // and points. In a narrow window the frame's column goes and its dot stands
  // before the points (points.css).
  function line(player) {
    var name = filter.name(player.card ? 'a' : 'span', 'board-name', player);
    if (player.card) name.href = '/players#card=' + player.card;
    return [blocks.cell(String(player.place), 'num board-place'), blocks.rowHead([marks.flag(player.country), name], 'board-who'),
      blocks.cell(player.band ? marks.classPill(player.band, true) : ''), blocks.cell(marks.frame(player.tier), 'board-frame'),
      blocks.cell([marks.frameDot(player.tier), document.createTextNode(marks.points(player.points, unit()))], 'num pts')];
  }

  function message(text, detail, withClear) {
    var node = el('div', 'board-message');
    node.appendChild(el('p', 'board-message-title', text));
    if (detail) node.appendChild(el('p', 'note', detail));
    if (withClear) {
      var button = node.appendChild(el('button', 'button small', 'Clear filters'));
      button.type = 'button';
      button.addEventListener('click', clearFilters);
    }
    return node;
  }

  function summaryText(state, shown) {
    var listed = state.players.length;
    var count = shown === listed ? site.plural(listed, 'player', 'players') + ' listed.' : 'Showing ' + shown + ' of ' + site.plural(listed, 'player', 'players') + '.';
    var first = state.total > listed ? ' The board lists the first ' + listed + ' of ' + state.total + '.' : '';
    return count + first;
  }

  function passes(player, skip) { return filter.passes(player, skip); }

  function render() {
    viewChips();
    markPicker();
    var state = view.board === null ? { phase: 'loading' } : boards[view.board];
    board.setAttribute('aria-busy', String(state.phase === 'loading'));
    while (board.firstChild) board.removeChild(board.firstChild);
    boardHead(state);
    ownLine();
    var ready = state.phase === 'ready' && state.players.length > 0;
    toolbar.hidden = shareButton.hidden = !ready;
    moreBox.hidden = true;
    summary.textContent = '';
    if (state.phase === 'loading') {
      board.appendChild(el('p', 'note', 'Loading players...'));
      var skeleton = board.appendChild(el('div', 'rank-skeleton'));
      skeleton.setAttribute('aria-hidden', 'true');
      for (var i = 0; i < 6; i++) skeleton.appendChild(el('span'));
      return;
    }
    if (NOT_SHOWN[state.phase]) {
      board.appendChild(message(NOT_SHOWN[state.phase][0], NOT_SHOWN[state.phase][1]));
      return;
    }
    if (!ready) {
      board.appendChild(message(isAccount() ? 'Nobody is on this board yet.' : 'Nobody has points with ' + FIGHTERS[view.board] + ' yet.',
        'Players appear with their first rated public room game, once they take part in rankings.'));
      return;
    }
    filter.fill(state.players, passes);
    var matching = state.players.filter(function (player) { return passes(player); });
    summary.textContent = summaryText(state, matching.length);
    if (!matching.length) {
      board.appendChild(message('No players match these filters.', filter.state.q ? 'Names are matched without case or accents.' : '', true));
      return;
    }
    board.appendChild(blocks.table(boardName() + ': players by points',
      [['Place', 'num'], ['Player'], ['Class'], ['Frame', 'board-frame'], ['Points', 'num']],
      matching.slice(0, view.limit).map(line), 'board-table'));
    var left = matching.length - view.limit;
    moreBox.hidden = left <= 0;
    moreButton.textContent = 'Show ' + Math.min(PAGE, left) + ' more';
  }

  function update() {
    filter.settle();
    view.limit = PAGE;
    writeHash();
    render();
  }
  function clearFilters() {
    filter.clear();
    update();
    search.focus();
  }

  // The view the address names: its board loaded, the address made whole.
  function follow() {
    readHash();
    if (isFighterBoard()) lastFighter = view.board;
    writeHash();
    load();
  }

  picker.addEventListener('click', function (event) {
    var button = event.target.closest ? event.target.closest('button') : null;
    if (button && picker.contains(button)) choose(button);
  });
  picker.addEventListener('keydown', function (event) {
    var buttons = Array.prototype.slice.call(picker.children);
    var at = buttons.indexOf(document.activeElement);
    var to = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1 : STEPS[event.key] ? at + STEPS[event.key] : -1;
    if (at < 0 || to < 0 || to >= buttons.length) return;
    event.preventDefault();
    choose(buttons[to]);
    buttons[to].focus();
  });
  moreButton.addEventListener('click', function () {
    view.limit += PAGE;
    render();
  });
  shareButton.addEventListener('click', function () {
    site.copy(status, location.origin + '/points' + hashOf(), 'Link to this view copied.', 'Copy failed. Copy the address from the address bar.');
  });
  window.addEventListener('hashchange', function () {
    if (location.hash !== hashOf()) follow();
  });

  fillPicker();
  follow();
  sessions.session().then(function (answer) {
    session = answer;
    // #fp waited for the session to name the player's main.
    if (view.board === null) follow();
    else render();
  });
}());
