// Ember replay archive: lists v1/archive (the bridge, behind this site's
// proxy), groups games into events and sets, and shows each match as two
// sides: the fighter's face, the player's flag, name and class. Filters (one
// value each for event, fighter, country and round, plus a search) live in
// the address, so a filtered page or an open set can be shared.
'use strict';

// Native fighter IDs, in order (src/common/FighterMetadata.inc); faces.webp
// holds their faces in the same order.
const FIGHTERS = ['Ryu', 'Ken', 'Chun-Li', 'E. Honda', 'Blanka', 'Zangief', 'Guile', 'Dhalsim', 'Balrog', 'Vega',
  'Sagat', 'M. Bison', 'C. Viper', 'Rufus', 'El Fuerte', 'Abel', 'Seth', 'Akuma', 'Gouken', 'T. Hawk', 'Cammy',
  'Fei Long', 'Dee Jay', 'Sakura', 'Rose', 'Gen', 'Dan', 'Guy', 'Cody', 'Ibuki', 'Makoto', 'Dudley', 'Adon', 'Hakan',
  'Juri', 'Yun', 'Yang', 'Evil Ryu', 'Oni', 'Rolento', 'Elena', 'Poison', 'Hugo', 'Decapre'];
// USF4's classes, band 1 to 10, and their colours, as the rankings page.
const CLASSES = ['D', 'D+', 'C', 'C+', 'B', 'B+', 'A', 'A+', 'Master', 'Grand Master'];
const TIERS = ['d', 'd', 'c', 'c', 'b', 'b', 'a', 'a', 'm', 'gm'];
const ROUNDS = { top8: 'Top 8', winners: 'Winners', losers: 'Losers', grand: 'Grand final', pools: 'Pools' };
// Online games of the same two players this close together are one set.
const SET_GAP = 30 * 60;
const FLAG_WIDTH = 20;

const $ = (id) => document.getElementById(id);
const regionName = (() => {
  try { const names = new Intl.DisplayNames(undefined, { type: 'region' }); return (code) => names.of(code) || code; }
  catch { return (code) => code; }
})();
const fighterName = (id) => (id == null ? 'Unknown fighter' : FIGHTERS[id] || 'Unknown fighter');
const day = (t) => new Date(t * 1000).toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short', year: 'numeric' });
const clock = (t) => new Date(t * 1000).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
const time = (t) => new Date(t * 1000).toLocaleString(undefined, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
const plural = (n, one, many) => n + ' ' + (n === 1 ? one : many);
const el = (tag, props = {}, ...children) => {
  const node = Object.assign(document.createElement(tag), props);
  for (const child of children) if (child != null && child !== false) node.append(child);
  return node;
};
const DOWNLOAD = '<svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true"><path d="M8 2v8M4.5 6.5 8 10l3.5-3.5M3 13h10" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>';

const state = { filter: {}, sort: '', order: '', games: [], sets: {}, events: [], countries: [], next: null, more: false, loading: false, groups: [], openSet: null, flags: null, total: null, matching: null };
// Games a full answer holds. The bridge answers with fewer when a page of a
// filter looked at all it may and found fewer: the page then asks again by
// itself, at most AUTO_PAGES times, until a full answer's worth has come
// since it was asked; beyond that, Older replays goes on.
const LISTED = 400;
const AUTO_PAGES = 25;
let autoPages = 0;
let askedAt = 0;
// `filter` and `sort` (the order: '' newest, 'class' highest class first) both
// go to the bridge, which picks and orders the games.

// --- Pieces -----------------------------------------------------------------

// A fighter's face from faces.webp, sized by the stylesheet.
function face(id, extra = '') {
  const node = el('span', { className: 'face' + (extra ? ' ' + extra : '') });
  if (id == null || id < 0 || id >= FIGHTERS.length) node.classList.add('none');
  else {
    node.style.backgroundSize = FIGHTERS.length * 100 + '% 100%';
    node.style.backgroundPosition = (id / (FIGHTERS.length - 1)) * 100 + '% 0';
  }
  node.setAttribute('role', 'img');
  node.setAttribute('aria-label', fighterName(id));
  node.title = fighterName(id);
  return node;
}

// A country's flag from the atlas, or its code until (or unless) it loads.
function flag(code) {
  if (!code) return null;
  const atlas = state.flags;
  const cell = atlas && Object.prototype.hasOwnProperty.call(atlas.countries, code) ? atlas.countries[code] : -1;
  const node = el('span', { className: 'flag' + (cell >= 0 ? '' : ' code'), textContent: cell >= 0 ? '' : code, title: regionName(code) });
  node.setAttribute('role', 'img');
  node.setAttribute('aria-label', regionName(code));
  if (cell >= 0) {
    const scale = FLAG_WIDTH / atlas.cellWidth;
    const stepX = atlas.cellWidth + 2 * atlas.padding, stepY = atlas.cellHeight + 2 * atlas.padding;
    node.style.width = FLAG_WIDTH + 'px';
    node.style.height = atlas.cellHeight * scale + 'px';
    node.style.backgroundSize = atlas.width * scale + 'px ' + atlas.height * scale + 'px';
    node.style.backgroundPosition = -((cell % atlas.columns) * stepX + atlas.padding) * scale + 'px ' +
      -(Math.floor(cell / atlas.columns) * stepY + atlas.padding) * scale + 'px';
  }
  return node;
}

// A class pill, as on the rankings page; none for a player not on the board.
function pill(band) {
  if (!(band >= 1 && band <= CLASSES.length)) return null;
  const short = band === 9 ? 'M' : band === 10 ? 'GM' : CLASSES[band - 1];
  const node = el('span', { className: 'pill tier-' + TIERS[band - 1], textContent: short, title: 'Class ' + CLASSES[band - 1] + ' on the rankings today' });
  return node;
}

// One side of a game: face, then name, flag and class, then the fighter.
// Drawn as side A or B (B mirrored, on the right) as `as` says, else as played.
function side(game, at, as = at) {
  const name = game.names[at];
  const classes = game.classes || [];
  return el('div', { className: 'side ' + (as ? 'b' : 'a') },
    face(game.fighters[at]),
    el('div', { className: 'who' },
      el('div', { className: 'name-line' },
        // A side nobody named (its player did not share the game) shows its fighter.
        el('span', { className: 'name' + (name ? '' : ' unknown'), textContent: name || fighterName(game.fighters[at]) }),
        flag(game.countries[at]),
        pill(classes[at])),
      name ? el('span', { className: 'fighter', textContent: fighterName(game.fighters[at]) }) : null));
}

function known(set) { return set.online ? null : state.sets[set.key]; }

// The middle of a match: the set's score when it has one, else "vs".
function middle(set, withMeta) {
  const set_ = known(set);
  const score = set_ ? el('span', { className: 'score', textContent: set_.score.join(' – ') }) : el('span', { className: 'score vs', textContent: 'vs' });
  return el('div', { className: 'mid' }, score,
    withMeta && set.games.length > 1 ? el('span', { className: 'meta', textContent: plural(set.games.length, 'game', 'games') }) : null);
}

function downloadLink(href, text, main) {
  const link = el('a', { className: 'btn small' + (main ? ' main' : ''), href, download: '' });
  link.insertAdjacentHTML('afterbegin', DOWNLOAD);
  link.append(text);
  return link;
}

// --- Address and loading -----------------------------------------------------

function readAddress() {
  const params = new URLSearchParams(location.search);
  state.filter = {};
  for (const key of ['event', 'fighter', 'country', 'round', 'q']) if (params.get(key)) state.filter[key] = params.get(key);
  const band = parseInt(params.get('class'), 10);
  if (band >= 1 && band <= CLASSES.length) state.filter.class = String(band);
  state.sort = params.get('sort') === 'class' ? 'class' : '';
  $('q').value = state.filter.q || '';
  state.openSet = params.get('set');
}

function writeAddress(push) {
  const params = new URLSearchParams(asked());
  if (state.openSet) params.set('set', state.openSet);
  const query = params.toString();
  const url = query ? '?' + query : location.pathname;
  if (push) history.pushState(null, '', url); else history.replaceState(null, '', url);
}

// A first page starts a new generation: an older answer is dropped when it
// arrives, and "Older replays" does nothing while any page is loading. Each
// later page is asked for with the cursor the last answer gave.
let requested = 0;
// What the bridge is asked for: the filter, and the order unless newest, in
// one key order, so two askings of the same compare equal.
const asked = () => {
  const all = state.sort ? { ...state.filter, sort: state.sort } : { ...state.filter };
  return Object.fromEntries(Object.keys(all).sort().map((key) => [key, all[key]]));
};
const newestFirst = (a, b) => b.saved_at - a.saved_at || (a.digest < b.digest ? 1 : -1);
async function load(append, auto = false) {
  if (append && (!state.next || state.loading)) return;
  const params = new URLSearchParams(asked());
  if (append) params.set('cursor', state.next);
  else { state.next = null; state.more = false; $('more').hidden = true; }
  if (!auto) { autoPages = 0; askedAt = append ? state.games.length : 0; }
  const mine = ++requested;
  state.loading = true;
  $('events').setAttribute('aria-busy', 'true');
  $('more').disabled = true;
  try {
    const response = await fetch('v1/archive?' + params);
    if (!response.ok) throw new Error(response.status);
    const listed = await response.json();
    if (mine !== requested) return;
    state.games = append ? state.games.concat(listed.games) : listed.games;
    Object.assign(state.sets, listed.sets);
    state.events = listed.events;
    state.countries = listed.countries;
    state.next = listed.next || null;
    state.more = state.next !== null;
    // The order the bridge used: the newest while it hides classes.
    state.order = listed.sort === 'class' ? 'class' : '';
    if (typeof listed.total === 'number') { state.total = listed.total; state.matching = listed.matching; }
  } catch (error) {
    if (mine !== requested) return;
    state.loading = false;
    $('more').disabled = false;
    // A cursor the bridge no longer takes (it began or stopped showing
    // classes, so the order changed): start again from the top.
    if (append && error.message === '400') { load(false); return; }
    $('summary').textContent = 'The archive did not answer. Try again in a minute.';
    $('events').replaceChildren(el('p', { className: 'note' }, el('button', { type: 'button', className: 'btn', textContent: 'Try again', onclick: () => load(false) })));
    $('events').removeAttribute('aria-busy');
    return;
  }
  state.loading = false;
  $('more').disabled = false;
  // Sessions and sets are told apart newest first, whatever the order.
  state.groups = group(state.order ? [...state.games].sort(newestFirst) : state.games);
  render();
  if (wantsMore()) { autoPages += 1; setTimeout(() => load(true, true), 400); }
}

// Whether the page should ask for the next page by itself: while less than a
// full answer's worth of games came since it was asked, there is more, and it
// asked by itself fewer than AUTO_PAGES times.
function wantsMore() {
  return state.more && autoPages < AUTO_PAGES && state.games.length - askedAt < LISTED;
}

// "1,234 games", with the reader's own digit grouping.
const count = (n, one, many) => n.toLocaleString() + ' ' + (n === 1 ? one : many);

// Events, newest first, each with its sets: a tournament set by its ID,
// online games by day and by the same two named players close together.
function group(games) {
  const events = new Map();
  for (const game of games) {
    const online = !game.set;
    const eventKey = online ? 'online:' + day(game.saved_at) : 'event:' + (game.event || 'Tournament sets');
    if (!events.has(eventKey)) {
      events.set(eventKey, { title: online ? day(game.saved_at) : game.event || 'Tournament sets', kind: online ? 'Online matches' : 'Tournament', online, sets: new Map() });
    }
    const event = events.get(eventKey);
    // Online, a game with both names joins the same two players' set; a game
    // only one side named (the other player did not share it) joins that
    // player's session, whoever they played. Either only within SET_GAP of
    // the group's last game.
    const named = game.names.filter(Boolean);
    const pair = named.length === 2 ? 'pair:' + [...named].sort().join('\n') : named.length === 1 ? 'player:' + named[0] : null;
    let key = online ? null : game.set;
    if (online) {
      const last = pair && [...event.sets.values()].find((set) => set.pair === pair && set.games[set.games.length - 1].saved_at - game.saved_at <= SET_GAP);
      key = last ? last.key : game.digest;
    }
    if (!event.sets.has(key)) {
      event.sets.set(key, { key, pair, player: pair && pair.startsWith('player:') ? named[0] : null, event: event.title, online, games: [] });
    }
    event.sets.get(key).games.push(game);
  }
  return [...events.values()].map((event) => ({ ...event, sets: [...event.sets.values()].map((set) => ({ ...set, games: set.games.reverse() })) }));
}

// --- Rendering -------------------------------------------------------------

const FILTERS = [
  ['event', 'Event', 'All events'],
  ['fighter', 'Fighter', 'Any fighter'],
  ['country', 'Country', 'Any country'],
  ['round', 'Round', 'Any round'],
  ['class', 'Class', 'Any class'],
];
const chosen = (key) => state.filter[key];

function filterValue(key) {
  const value = chosen(key);
  if (value == null) return null;
  if (key === 'class') return CLASSES[+value - 1];
  if (key === 'event') return value === 'online' ? 'Online matches' : value;
  if (key === 'fighter') return fighterName(+value);
  if (key === 'country') return regionName(value);
  return ROUNDS[value] || value;
}

function renderFilters() {
  // Event and round mean something only once the archive holds a tournament set.
  const shown = FILTERS.filter(([key]) => state.events.length || chosen(key) != null || (key !== 'event' && key !== 'round'));
  $('filters').replaceChildren(...shown.map(([key, label, any]) => {
    const value = filterValue(key);
    const button = el('button', { type: 'button', className: 'f' + (value ? ' on' : '') },
      key === 'fighter' && value ? face(+state.filter.fighter) : null,
      el('span', { className: 'k', textContent: label }),
      el('b', { textContent: value || any }));
    button.insertAdjacentHTML('beforeend', '<svg width="14" height="14" viewBox="0 0 16 16" aria-hidden="true"><path d="M3 6 L8 11 L13 6" fill="none" stroke="#ff8738" stroke-width="2"/></svg>');
    button.setAttribute('aria-haspopup', 'dialog');
    button.setAttribute('aria-label', label + ': ' + (value || any));
    button.addEventListener('click', () => pick(key, label, any));
    return button;
  }));
  $('clear').hidden = Object.keys(state.filter).length === 0;
  for (const button of document.querySelectorAll('#order button')) {
    button.setAttribute('aria-pressed', String((button.dataset.sort || '') === state.sort));
  }
}

// The side `player` played in `game`.
function sideOf(game, player) { return game.names[0] === player ? 0 : 1; }

// The fighters a session's player met, each face once, as its right side.
function metSide(set, most) {
  const met = [];
  for (const game of set.games) {
    const other = game.fighters[1 - sideOf(game, set.player)];
    if (!met.includes(other)) met.push(other);
  }
  return el('div', { className: 'side b met' },
    el('div', { className: 'faces' }, ...met.slice(0, most).map((id) => face(id, 'small'))),
    met.length > most ? el('span', { className: 'meta', textContent: '+' + (met.length - most) }) : null);
}

// One player's games in a row: their side as in a match, then the fighters
// they met.
function sessionRow(set, dated) {
  const first = set.games[0], last = set.games[set.games.length - 1];
  const open = el('button', { type: 'button', className: 'open' });
  open.setAttribute('aria-label', 'Open ' + set.player + ', ' + plural(set.games.length, 'game', 'games'));
  open.addEventListener('click', () => openSet(set.key));
  return el('li', { className: 'match session' },
    open,
    el('div', { className: 'versus' },
      side(first, sideOf(first, set.player), 0),
      el('div', { className: 'mid' }, el('span', { className: 'score vs', textContent: 'vs' }),
        el('span', { className: 'meta', textContent: plural(set.games.length, 'game', 'games') })),
      metSide(set, 6)),
    el('div', { className: 'act' },
      el('span', { className: 'meta', textContent: (dated ? time : clock)(first.saved_at) + '–' + clock(last.saved_at) }),
      el('span', { className: 'ghost', textContent: 'Games ›' })));
}

// The highest class either player in `set` shows, 0 for none.
function bestClass(set) {
  let best = 0;
  for (const game of set.games) for (const band of game.classes || []) if (band > best) best = band;
  return best;
}

function matchRow(set, dated) {
  if (set.player && set.games.length > 1) return sessionRow(set, dated);
  const first = set.games[0];
  const set_ = known(set);
  const when = set.online ? (dated ? time : clock)(first.saved_at) : [first.round, first.phase].filter(Boolean).join(' · ') || 'Set';
  // One game downloads from the row; a set downloads whole once packed, and
  // otherwise opens to its games.
  const action = set.games.length === 1
    ? downloadLink(first.url, 'Download')
    : set_ && set_.pack ? downloadLink(set_.pack, 'Download set')
      : el('span', { className: 'ghost', textContent: 'Games ›' });
  const open = el('button', { type: 'button', className: 'open' });
  open.setAttribute('aria-label', 'Open ' + (first.names[0] || 'Unknown player') + ' versus ' + (first.names[1] || 'Unknown player') + ', ' + plural(set.games.length, 'game', 'games'));
  open.addEventListener('click', () => openSet(set.key));
  return el('li', { className: 'match' },
    open,
    el('div', { className: 'versus' }, side(first, 0), middle(set, true), side(first, 1)),
    el('div', { className: 'act' }, el('span', { className: 'meta', textContent: when }), action));
}

function renderList() {
  const filtered = Object.keys(state.filter).length > 0;
  const groups = state.groups;
  const games = state.games.length;
  $('total').textContent = state.total != null ? count(state.total, 'game', 'games') : '';
  const order = state.order === 'class' ? ', highest class first' : '';
  const busy = wantsMore() ? ' Loading more…' : '';
  // How many there are, when the bridge knows: all games, or those the filter keeps.
  const of = filtered ? state.matching : state.total;
  const shown = state.more
    ? of != null ? `${count(games, 'game', 'games')} of ${of.toLocaleString()} shown${order}.`
      : `${count(games, 'game', 'games')} shown${order}.`
    : of != null && filtered ? `${count(of, 'game matches', 'games match')}${order}.` : `${count(games, 'game', 'games')}${order}.`;
  $('summary').textContent = games
    ? shown + (state.more ? busy || ' Older replays shows more.' : '')
    : state.more ? 'None found yet.' + (busy || ' Older replays looks further.')
      : filtered ? 'No replays match these filters.' : 'No replays yet. Games shared from Ember show up here.';
  if (state.order === 'class') {
    // One list, the highest class first; rows without a class last, newest first within each.
    const sets = groups.flatMap((event) => event.sets);
    const newest = (set) => set.games[set.games.length - 1].saved_at;
    sets.sort((a, b) => bestClass(b) - bestClass(a) || newest(b) - newest(a));
    $('events').replaceChildren(el('section', { className: 'event' },
      el('div', { className: 'event-head' }, el('h2', { textContent: 'Highest class first' }),
        el('span', { className: 'note', textContent: count(games, 'game', 'games') })),
      el('ol', { className: 'matches' }, ...sets.map((set) => matchRow(set, true)))));
  } else {
    $('events').replaceChildren(...groups.map((event) => el('section', { className: 'event' },
      el('div', { className: 'event-head' }, el('h2', { textContent: event.title }),
        el('span', { className: 'note', textContent: event.kind + ' · ' + plural(event.sets.reduce((n, set) => n + set.games.length, 0), 'game', 'games') })),
      el('ol', { className: 'matches' }, ...event.sets.map((set) => matchRow(set, false))))));
  }
  if (!games && filtered) {
    $('events').append(el('p', {}, el('button', { type: 'button', className: 'btn', textContent: 'Clear filters', onclick: clearFilters })));
  }
  $('events').removeAttribute('aria-busy');
  $('more').hidden = !state.more;
}

function findSet(key) {
  for (const event of state.groups) for (const set of event.sets) if (set.key === key) return set;
  return null;
}

function renderSet(set) {
  const first = set.games[0];
  const set_ = known(set);
  const session = Boolean(set.player && set.games.length > 1);
  $('set-event').textContent = set.online ? (session ? 'Online games · ' : 'Online match · ') + set.event : set.event;
  const size = set.games.reduce((n, game) => n + game.size, 0);
  const title = session ? 'Games of ' + set.player : set.online ? 'Online match' : [first.round, first.phase].filter(Boolean).join(' · ') || 'Set';
  $('set-hero').replaceChildren(
    el('span', { className: 'label', textContent: title }),
    session
      ? el('div', { className: 'versus' }, side(first, sideOf(first, set.player), 0),
        el('div', { className: 'mid' }, el('span', { className: 'score vs', textContent: 'vs' }), el('span', { className: 'meta', textContent: plural(set.games.length, 'game', 'games') })), metSide(set, 12))
      : el('div', { className: 'versus' }, side(first, 0), middle(set, false), side(first, 1)),
    el('p', { className: 'note', textContent: `${day(first.saved_at)} · ${plural(set.games.length, 'game', 'games')} · ${Math.ceil(size / 1024)} KB` }));
  const actions = [];
  if (set_ && set_.pack) actions.push(downloadLink(set_.pack, 'Download set (.emberpack)', true));
  else if (set.games.length === 1) actions.push(downloadLink(first.url, 'Download replay', true));
  const copy = el('button', { type: 'button', className: 'btn', textContent: 'Copy link' });
  copy.addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(location.href); copy.textContent = 'Link copied'; }
    catch { copy.textContent = 'Copy the address bar'; }
    setTimeout(() => { copy.textContent = 'Copy link'; }, 2000);
  });
  actions.push(copy);
  $('set-actions').replaceChildren(...actions);
  $('set-games').replaceChildren(...set.games.map((game, i) => el('li', { className: 'game' },
    el('span', { className: 'n', textContent: 'Game ' + (i + 1) }),
    el('div', { className: 'pair' }, face(game.fighters[0]), face(game.fighters[1], 'b'),
      el('span', { textContent: [0, 1].map((at) => (game.names[at] ? game.names[at] + ' (' + fighterName(game.fighters[at]) + ')' : fighterName(game.fighters[at]))).join(' vs ') + ' · ' + clock(game.saved_at) })),
    downloadLink(game.url, 'Download'))));
}

function render() {
  renderFilters();
  const set = state.openSet && findSet(state.openSet);
  $('list-view').hidden = !!set;
  $('set-view').hidden = !set;
  if (set) renderSet(set); else renderList();
}

function openSet(key) { state.openSet = key; writeAddress(true); render(); scrollTo(0, 0); $('back').focus(); }
function closeSet() { state.openSet = null; writeAddress(true); render(); }
function refresh() { state.openSet = null; writeAddress(false); load(false); }
function clearFilters() { state.filter = {}; $('q').value = ''; refresh(); }
function setSort(sort) { state.sort = sort || ''; refresh(); }

// The popup grid: "Any" first, then every value; picking one closes it.
function pick(key, label, any) {
  const choices = {
    event: [['online', 'Online matches'], ...state.events.map((title) => [title, title])],
    fighter: FIGHTERS.map((name, id) => [String(id), name]).sort((a, b) => a[1].localeCompare(b[1])),
    country: state.countries.map((code) => [code, regionName(code)]).sort((a, b) => a[1].localeCompare(b[1])),
    round: Object.entries(ROUNDS),
    class: CLASSES.map((name, at) => [String(at + 1), name]).reverse(),
  }[key];
  const dialog = $('pick');
  const tiles = key === 'fighter';
  dialog.style.setProperty('--w', tiles ? '980px' : key === 'event' ? '640px' : '720px');
  $('pick-title').textContent = tiles ? 'Fighter, played by either player' : key === 'class' ? 'Class, of either player (rankings today)' : label;
  const options = $('pick-options');
  options.className = 'options' + (tiles ? ' tiles' : '');
  options.style.setProperty('--cell', tiles ? '100px' : key === 'event' ? '100%' : '170px');
  const current = chosen(key) ?? '';
  options.replaceChildren(...[['', any], ...choices].map(([value, text]) => {
    const button = el('button', { type: 'button', className: 'o' + (value ? '' : ' any') },
      tiles && value ? face(+value) : key === 'country' && value ? flag(value) : key === 'class' && value ? pill(+value) : null,
      el('span', { textContent: text }));
    button.setAttribute('aria-pressed', String(value === current));
    button.addEventListener('click', () => {
      dialog.close();
      if (value) state.filter[key] = value; else delete state.filter[key];
      refresh();
    });
    return button;
  }));
  dialog.showModal();
  (options.querySelector('[aria-pressed=true]') || options.firstChild).focus();
}

// --- Keys --------------------------------------------------------------------

// Arrow keys move between options and matches in reading order, rows by
// position, for a pad or remote that sends arrow keys; "/" goes to search.
document.addEventListener('keydown', (event) => {
  const here = document.activeElement;
  if (event.key === '/' && !(here && here.matches('input'))) { event.preventDefault(); $('q').focus(); return; }
  const moves = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: 'up', ArrowDown: 'down' };
  const move = moves[event.key];
  if (move == null || !here || !here.matches('.o, .open, .f')) return;
  const all = [...(here.closest('dialog, main') || document).querySelectorAll('.o, .open, .f')].filter((node) => node.offsetParent);
  const at = all.indexOf(here);
  let next;
  if (typeof move === 'number') next = all[at + move];
  else {
    const box = here.getBoundingClientRect();
    const candidates = all.filter((node) => {
      const other = node.getBoundingClientRect();
      return move === 'down' ? other.top > box.bottom - 2 : other.bottom < box.top + 2;
    });
    const gap = (r) => Math.abs(move === 'down' ? r.top - box.bottom : box.top - r.bottom);
    candidates.sort((a, b) => {
      const ra = a.getBoundingClientRect(), rb = b.getBoundingClientRect();
      return gap(ra) - gap(rb) || Math.abs(ra.left - box.left) - Math.abs(rb.left - box.left);
    });
    next = candidates[0];
  }
  if (next) { event.preventDefault(); next.focus(); next.scrollIntoView({ block: 'nearest' }); }
});

$('pick-close').addEventListener('click', () => $('pick').close());
$('back').addEventListener('click', closeSet);
$('clear').addEventListener('click', clearFilters);
for (const button of document.querySelectorAll('#order button')) {
  button.addEventListener('click', () => setSort(button.dataset.sort));
}
$('more').addEventListener('click', () => load(true));
window.addEventListener('popstate', () => {
  const before = JSON.stringify(asked());
  readAddress();
  if (JSON.stringify(asked()) !== before) load(false); else render();
});
let typing;
$('q').addEventListener('input', () => {
  clearTimeout(typing);
  typing = setTimeout(() => {
    const q = $('q').value.trim();
    if (q) state.filter.q = q; else delete state.filter.q;
    refresh();
  }, 300);
});

// The flag atlas; until it loads (or if it cannot), a country shows its code.
fetch('assets/flags.json')
  .then((response) => (response.ok ? response.json() : null))
  .then((atlas) => {
    if (atlas && atlas.countries && atlas.cellWidth > 0 && atlas.columns > 0) {
      state.flags = atlas;
      if (state.groups.length) render();
    }
  })
  .catch(() => {});
readAddress();
load(false);
