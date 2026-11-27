// needs: marks
// The account and points answers read once, into the models the pages draw
// (docs/design/SITE_PLAYER_CARDS.md): a board, a card with its places, the
// player's own points and rating, the players a name finds, and the whole
// signed-in answer of /account/me.json. Each reader answers its model, or null
// for an answer it cannot read, so a page shows that plainly instead of
// drawing half of it. scripts/site/check_samples.js runs these readers over
// the preview's samples.
(function () {
  'use strict';
  var marks = window.EmberMarks;
  var FRAMES = marks.FRAMES;
  var RULE_NUMBERS = ['start_rating', 'start_deviation', 'start_volatility', 'margin', 'settled_deviation',
    'unsure_deviation', 'full_gap', 'min_gap', 'min_weight', 'upset_weight', 'full_repeats', 'repeat_weight', 'sitting_secs'];

  function count(value) { return Number.isInteger(value) && value >= 0; }
  function place(value) { return Number.isInteger(value) && value >= 1; }
  function number(value) { return typeof value === 'number' && isFinite(value); }
  function object(value) { return !!value && typeof value === 'object'; }

  // A board (GET /points.json): { players, total, listedAt }, each player
  // { place, name, country, band (0 for none), points, tier, card ('' for a
  // line without one) }. fighter is the board asked for, null for the account's.
  function board(body, fighter) {
    if (!object(body) || !Array.isArray(body.players) || !count(body.total) || body.total < body.players.length) return null;
    if (fighter === null ? body.fighter !== undefined : body.fighter !== fighter) return null;
    var players = [];
    var read = body.players.every(function (line) {
      var ok = object(line) && place(line.place) && typeof line.name === 'string' && line.name !== '' &&
        count(line.points) && marks.isTier(line.tier) && (line.card === undefined || marks.TOKEN.test(line.card)) &&
        (line.band === undefined || marks.isBand(line.band));
      if (ok) {
        players.push({ place: line.place, name: line.name, country: marks.country(line.country), band: line.band || 0,
          points: line.points, tier: line.tier, card: line.card || '' });
      }
      return ok;
    });
    return read ? { players: players, total: body.total, listedAt: count(body.listed_at) ? body.listed_at : 0 } : null;
  }

  // A PlayerCard and its places: { name, country, badge, main, since,
  // account, fighters (by fighter), awards, versus, places: { account,
  // fighters (by fighter) } }; null members for what the card does not say.
  function card(answer, places) {
    if (!object(answer) || !object(answer.account) || !count(answer.account.points) || !marks.isTier(answer.account.tier)) return null;
    if (!Array.isArray(answer.fighters) || !object(places) || !Array.isArray(places.fighters)) return null;
    var fighters = {};
    var fightersRead = answer.fighters.every(function (entry) {
      var ok = object(entry) && marks.isFighter(entry.fighter) && count(entry.points) && entry.points > 0 &&
        marks.isTier(entry.tier) && !fighters[entry.fighter];
      if (ok) fighters[entry.fighter] = { fighter: entry.fighter, points: entry.points, tier: entry.tier };
      return ok;
    });
    var boardPlaces = {};
    var placesRead = (places.account === undefined || place(places.account)) && places.fighters.every(function (pair) {
      var ok = Array.isArray(pair) && marks.isFighter(pair[0]) && place(pair[1]);
      if (ok) boardPlaces[pair[0]] = pair[1];
      return ok;
    });
    var awards = object(answer.awards) ? answer.awards : {};
    var versus = answer.versus;
    var versusRead = versus === undefined || (object(versus) && count(versus.won) && count(versus.lost) && versus.won + versus.lost > 0 && count(versus.last_at));
    var main = answer.main === undefined ? null : answer.main;
    if (!fightersRead || !placesRead || !versusRead || (main !== null && !marks.isFighter(main))) return null;
    if (answer.badge !== undefined && !marks.isBadge(answer.badge)) return null;
    return {
      name: typeof answer.name === 'string' && answer.name ? answer.name : null,
      country: marks.country(answer.country),
      badge: answer.badge || null,
      main: main,
      since: count(answer.since) ? answer.since : null,
      account: { points: answer.account.points, tier: answer.account.tier },
      fighters: fighters,
      awards: {
        games: count(awards.games) ? awards.games : 0,
        wins: count(awards.wins) ? awards.wins : 0,
        streak: count(awards.best_streak) ? awards.best_streak : 0,
        peak: marks.isBand(awards.peak_class) ? awards.peak_class : 0
      },
      versus: versus || null,
      places: { account: places.account || null, fighters: boardPlaces }
    };
  }

  // The player's own points (OwnLevels, counted): { account, fighters (by
  // fighter) }, each with its games, wins and frame from floor to next.
  function own(points) {
    var frame = function (entry) {
      return object(entry) && count(entry.points) && marks.isTier(entry.tier) && count(entry.floor) && entry.floor <= entry.points &&
        (entry.next === undefined ? entry.tier === FRAMES.length - 1 : count(entry.next) && entry.next > entry.points) &&
        count(entry.games) && count(entry.wins);
    };
    if (!object(points) || points.points !== 'counted' || !frame(points.account) || !Array.isArray(points.fighters)) return null;
    var fighters = {};
    var read = points.fighters.every(function (entry) {
      var ok = frame(entry) && marks.isFighter(entry.fighter) && !fighters[entry.fighter];
      if (ok) fighters[entry.fighter] = entry;
      return ok;
    });
    return read ? { account: points.account, fighters: fighters } : null;
  }

  function outcome(result) { return object(result) && number(result.weight) && result.weight > 0 && result.weight <= 1 && number(result.change); }

  // The rating details: { rules, rating }, rating null before the first rated
  // game; null when either cannot be read.
  function rating(answer, rules) {
    var floors = object(rules) && rules.floors;
    var rulesRead = object(rules) && RULE_NUMBERS.every(function (name) { return number(rules[name]); }) &&
      Array.isArray(floors) && floors.length === marks.CLASSES.length - 1 &&
      floors.every(function (floor, at) { return number(floor) && (at === 0 || floor > floors[at - 1]); }) &&
      rules.min_gap > rules.full_gap && rules.min_weight > 0 && rules.min_weight < 1;
    if (!rulesRead) return null;
    if (answer === undefined) return { rules: rules, rating: null };
    var ratingRead = object(answer) && typeof answer.takes_part === 'boolean' && typeof answer.settled === 'boolean' &&
      number(answer.rating) && number(answer.deviation) && number(answer.volatility) &&
      (answer.band === 0 || marks.isBand(answer.band)) &&
      (answer.up_at === undefined || number(answer.up_at)) && (answer.down_below === undefined || number(answer.down_below)) &&
      (answer.faint_in_days === undefined || count(answer.faint_in_days)) &&
      Array.isArray(answer.examples) && answer.examples.every(function (example) {
        return object(example) && number(example.opponent) && (example.band === 0 || marks.isBand(example.band)) &&
          outcome(example.win) && outcome(example.loss);
      });
    return ratingRead ? { rules: rules, rating: answer } : null;
  }

  // The players a name finds (SearchHit), to choose from.
  function hits(players) {
    if (!Array.isArray(players) || !players.length) return null;
    var read = players.every(function (hit) {
      return object(hit) && marks.TOKEN.test(hit.card) && typeof hit.name === 'string' && hit.name !== '' &&
        (hit.main === undefined || marks.isFighter(hit.main)) && (hit.badge === undefined || marks.isBadge(hit.badge)) &&
        (hit.since === undefined || count(hit.since));
    });
    return read ? players.map(function (hit) {
      return { card: hit.card, name: hit.name, country: marks.country(hit.country), main: hit.main === undefined ? null : hit.main,
        badge: hit.badge || null, since: count(hit.since) ? hit.since : null };
    }) : null;
  }

  // What POST /account/card.json found: { found: 'card', card }, { found:
  // 'players', players } or { found: 'nobody' }; null for an answer it cannot read.
  function lookup(body) {
    if (!object(body)) return null;
    if (body.players !== undefined) {
      var players = hits(body.players);
      return players && { found: 'players', players: players };
    }
    if (body.card === null) return { found: 'nobody' };
    var read = card(body.card, body.places);
    return read && { found: 'card', card: read };
  }

  // GET /account/me.json: { state: 'signed_out' }, { state: 'no_account' }, or
  // { state: 'signed_in', card, own, link, details }, details the rating's
  // ({ rules, rating }) or null when it cannot be read, which leaves the card
  // as it is. null for an answer whose card cannot be read.
  function session(body) {
    if (!object(body)) return null;
    if (body.state === 'signed_out' || body.state === 'no_account') return { state: body.state };
    if (body.state !== 'signed_in' || !marks.TOKEN.test(body.link)) return null;
    var read = card(body.card, body.places), points = own(body.points);
    if (!read || !points) return null;
    return { state: 'signed_in', card: read, own: points, link: body.link, details: rating(body.rating, body.rules) };
  }

  window.EmberAnswers = { board: board, card: card, own: own, rating: rating, hits: hits, lookup: lookup, session: session };
}());
