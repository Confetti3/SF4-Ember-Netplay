// needs: site marks blocks
// A player's card as Ember's Profile draws it (docs/design/PLAYER_CARD.md),
// for /me (the player's own, with the parts only they see) and /players
// (anyone's): the hero with the main fighter's face in its frame, the name
// line, the FP and BP lines with their board places, the counts behind the
// awards, the record against the viewer and the fighters by points. It draws
// answers.js's card model. Titles are left for later (SITE_PLAYER_CARDS.md).
// Names are the players' own words, so they only ever go in as text.
(function () {
  'use strict';
  var site = window.EmberSite;
  var marks = window.EmberMarks;
  var blocks = window.EmberBlocks;
  var el = site.el;
  var fact = blocks.fact, cell = blocks.cell, rowHead = blocks.rowHead, ownMark = blocks.ownMark;
  var FIGHTERS = marks.FIGHTERS, FRAMES = marks.FRAMES;
  var BOARD = '/points';

  // "24 November", with the year when it is not this one: the day of a last game.
  function day(seconds) {
    var date = new Date(seconds * 1000);
    var thisYear = date.getFullYear() === new Date().getFullYear();
    return date.toLocaleDateString('en-GB', thisYear ? { day: 'numeric', month: 'long' } : { day: 'numeric', month: 'long', year: 'numeric' });
  }

  // A board place as a link to that board, or what stands for none.
  function placeLink(placeOnBoard, hash, words) {
    if (!placeOnBoard) return el('span', 'card-unlisted', 'Not listed');
    var link = el('a', '', site.ordinal(placeOnBoard) + (words || ''));
    link.href = BOARD + hash;
    return link;
  }

  // One points line of the hero: what it counts, the points and the frame
  // with the place on its board.
  function pointsLine(label, points, unit, tier, placeOnBoard, hash, board) {
    var line = el('div', 'card-line');
    line.appendChild(el('div', 'card-k', label));
    line.appendChild(el('div', 'pts big', marks.points(points, unit)));
    var under = el('div', 'card-k');
    under.appendChild(marks.frame(tier, ' frame'));
    if (placeOnBoard) {
      under.appendChild(document.createTextNode(' · '));
      under.appendChild(placeLink(placeOnBoard, hash, ' on the ' + board + ' board'));
    }
    line.appendChild(under);
    return line;
  }

  function hero(card, options) {
    var mainPoints = card.main === null ? null : card.fighters[card.main] || null;
    var panel = el('section', 'panel card-panel');
    panel.setAttribute('aria-label', options.own ? 'Player card' : (card.name || 'This player') + '’s card');
    var top = el('div', 'card-hero');
    var picture;
    if (card.main === null) {
      picture = el('span', 'face none hero framed frame-' + card.account.tier);
      picture.setAttribute('aria-label', 'No main fighter, ' + FRAMES[card.account.tier] + ' frame');
    } else {
      var tier = mainPoints ? mainPoints.tier : 0;
      picture = marks.face(card.main, 'hero', tier);
      picture.removeAttribute('aria-hidden');
      picture.setAttribute('aria-label', FIGHTERS[card.main] + ', ' + FRAMES[tier] + ' frame');
    }
    picture.setAttribute('role', 'img');
    top.appendChild(picture);

    var who = el('div', 'card-who');
    var nameLine = el('div', 'card-nameline');
    if (card.country) nameLine.appendChild(marks.flag(card.country));
    var heading = nameLine.appendChild(el('h2', 'card-name', card.name || 'No name yet'));
    heading.tabIndex = -1;
    if (card.badge) nameLine.appendChild(marks.badge(card.badge));
    who.appendChild(nameLine);
    if (card.since !== null) who.appendChild(el('p', 'note card-since', 'On Ember since ' + blocks.month(card.since)));
    var lines = el('div', 'card-lines');
    if (card.main !== null && mainPoints) {
      lines.appendChild(pointsLine(FIGHTERS[card.main] + ' · Main', mainPoints.points, 'FP', mainPoints.tier,
        card.places.fighters[card.main], '#fp=' + card.main, FIGHTERS[card.main]));
    } else if (card.main !== null) {
      var none = el('div', 'card-line');
      none.appendChild(el('div', 'card-k', FIGHTERS[card.main] + ' · Main'));
      none.appendChild(el('div', 'pts big', marks.points(0, 'FP')));
      none.appendChild(el('div', 'card-k', 'No points with ' + FIGHTERS[card.main] + ' yet'));
      lines.appendChild(none);
    }
    lines.appendChild(pointsLine('Account', card.account.points, 'BP', card.account.tier, card.places.account, '#bp', 'BP'));
    who.appendChild(lines);
    top.appendChild(who);
    panel.appendChild(top);

    var facts = el('dl', 'card-facts');
    fact(facts, 'Rated games', String(card.awards.games));
    fact(facts, 'Wins', String(card.awards.wins));
    fact(facts, 'Best streak', String(card.awards.streak));
    fact(facts, 'Fighters played', Object.keys(card.fighters).length + ' of ' + FIGHTERS.length);
    fact(facts, 'Peak class', card.awards.peak ? marks.CLASSES[card.awards.peak - 1] : 'None yet');
    panel.appendChild(facts);

    var actions = el('div', 'actions card-actions');
    var share = el('button', 'button small', 'Copy card link');
    share.type = 'button';
    var status = el('p', 'status card-status');
    status.setAttribute('role', 'status');
    status.setAttribute('aria-live', 'polite');
    share.addEventListener('click', function () {
      site.copy(status, options.share.url, options.share.done, 'Copy failed. The card link is ' + options.share.url);
    });
    actions.appendChild(share);
    panel.appendChild(actions);
    panel.appendChild(status);
    return { panel: panel, heading: heading };
  }

  // How far a points count is through its frame, to the next one.
  function progress(label, entry) {
    var item = el('div', 'card-progress');
    item.appendChild(el('div', 'card-k', label));
    item.appendChild(el('div', 'pts mid', entry.points + ' points'));
    var meter = el('div', 'card-meter');
    var fill = el('div', 'card-meter-fill');
    meter.setAttribute('role', 'img');
    if (entry.next === undefined) {
      fill.style.width = '100%';
      meter.setAttribute('aria-label', FRAMES[entry.tier] + ', the highest frame');
      meter.appendChild(fill);
      item.appendChild(meter);
      item.appendChild(el('div', 'card-k', 'The highest frame'));
      return item;
    }
    var span = entry.next - entry.floor, done = entry.points - entry.floor;
    var nextFrame = FRAMES[entry.tier + 1];
    fill.style.width = (100 * done / span).toFixed(1) + '%';
    meter.setAttribute('aria-label', done + ' of the ' + span + ' points from ' + FRAMES[entry.tier] + ' to ' + nextFrame);
    meter.appendChild(fill);
    item.appendChild(meter);
    item.appendChild(el('div', 'card-k', (entry.next - entry.points) + ' points to the ' + nextFrame + ' frame'));
    return item;
  }

  function ownProgress(card, own, ratingHash) {
    var panel = el('section', 'panel card-panel');
    panel.setAttribute('aria-label', 'Shown only to you');
    var head = el('div', 'card-panel-head');
    head.appendChild(el('h2', '', 'Progress'));
    head.appendChild(ownMark('Shown only to you'));
    panel.appendChild(head);
    var three = el('div', 'card-three');
    if (card.main !== null && own.fighters[card.main]) three.appendChild(progress(FIGHTERS[card.main], own.fighters[card.main]));
    three.appendChild(progress('Account', own.account));
    var rate = el('div', 'card-progress');
    rate.appendChild(el('div', 'card-k', 'Win rate'));
    var games = own.account.games, wins = own.account.wins;
    rate.appendChild(el('div', 'pts mid', games ? Math.round(100 * wins / games) + '%' : 'No games yet'));
    var said = el('div', 'card-k card-rate-note', games ? wins + ' of ' + site.plural(games, 'rated game', 'rated games') + '. ' : '');
    var details = el('a', '', 'Rating details');
    details.href = ratingHash;
    said.appendChild(details);
    said.appendChild(document.createTextNode(' show what stands behind your class.'));
    rate.appendChild(said);
    three.appendChild(rate);
    panel.appendChild(three);
    return panel;
  }

  function versusPanel(card) {
    var name = card.name || 'this player';
    var panel = el('section', 'panel card-panel');
    panel.setAttribute('aria-label', 'Your record with ' + name);
    panel.appendChild(el('h2', '', 'You and ' + name));
    var line = el('div', 'card-versus');
    var record = card.versus;
    line.appendChild(el('span', 'pts mid', site.plural(record.won, 'win', 'wins') + ', ' + site.plural(record.lost, 'loss', 'losses')));
    line.appendChild(el('span', 'card-k', site.plural(record.won + record.lost, 'rated game', 'rated games') +
      ' between you · last played ' + day(record.last_at)));
    panel.appendChild(line);
    return panel;
  }

  // The fighters by points, most first, the main marked; the owner's own
  // games and wins with each beside them.
  function fightersPanel(card, own) {
    var panel = el('section', 'panel card-panel');
    panel.setAttribute('aria-label', 'Fighters');
    panel.appendChild(el('h2', '', 'Fighters'));
    var ids = Object.keys(card.fighters).map(Number).sort(function (a, b) {
      return card.fighters[b].points - card.fighters[a].points || a - b;
    });
    if (!ids.length) {
      panel.appendChild(el('p', 'note', 'No fighter played yet. Each starts earning points with its first rated public game.'));
      return panel;
    }
    var columns = [['Fighter'], ['Points', 'num'], ['Frame'], ['Board place']];
    if (own) columns.push(['Games', 'num', true], ['Won', 'num', true]);
    var rows = ids.map(function (id) {
      var entry = card.fighters[id];
      var name = [marks.face(id, 'row', entry.tier), el('span', '', FIGHTERS[id])];
      if (id === card.main) name.push(el('span', 'card-main-chip', 'Main'));
      var cells = [rowHead(name), cell(marks.points(entry.points, 'FP'), 'num pts'), cell(marks.frame(entry.tier)),
        cell(placeLink(card.places.fighters[id], '#fp=' + id))];
      if (own) {
        var counted = own.fighters[id];
        cells.push(cell(String(counted ? counted.games : 0), 'num'), cell(String(counted ? counted.wins : 0), 'num'));
      }
      return cells;
    });
    panel.appendChild(blocks.table((own ? 'Your' : (card.name || 'This player') + '\u2019s') + ' fighters by points', columns, rows,
      'card-fighters' + (own ? ' own' : '')));
    if (own) {
      var left = FIGHTERS.length - ids.length;
      panel.appendChild(el('p', 'note card-after', (left ? site.plural(left, 'fighter', 'fighters') + ' not played yet. Each starts earning points with its first rated public game. ' : '') +
        'Other players see your points and frames, never your games and wins with a fighter.'));
    }
    return panel;
  }

  // The card into target, in place of what it held; answers the card's
  // heading, for a page to move focus to. options: share { url, done } for
  // Copy card link; own (answers.own's) and ratingHash for the player's own card.
  function draw(target, card, options) {
    while (target.firstChild) target.removeChild(target.firstChild);
    var top = hero(card, options);
    target.appendChild(top.panel);
    if (options.own) target.appendChild(ownProgress(card, options.own, options.ratingHash));
    if (card.versus && !options.own) target.appendChild(versusPanel(card));
    target.appendChild(fightersPanel(card, options.own));
    return top.heading;
  }

  window.EmberCard = { draw: draw };
}());
