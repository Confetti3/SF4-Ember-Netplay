// Ember replay archive: lists /v1/archive (the bridge, behind this site's
// proxy), groups games into events and sets, and filters by one value each
// for event, fighter, country and round, every choice in one popup grid.
// Filters live in the address, so a filtered page can be shared.
'use strict';

// Native fighter IDs, in order (src/common/FighterMetadata.inc).
const FIGHTERS = ['Ryu', 'Ken', 'Chun-Li', 'E. Honda', 'Blanka', 'Zangief', 'Guile', 'Dhalsim', 'Balrog', 'Vega',
  'Sagat', 'M. Bison', 'C. Viper', 'Rufus', 'El Fuerte', 'Abel', 'Seth', 'Akuma', 'Gouken', 'T. Hawk', 'Cammy',
  'Fei Long', 'Dee Jay', 'Sakura', 'Rose', 'Gen', 'Dan', 'Guy', 'Cody', 'Ibuki', 'Makoto', 'Dudley', 'Adon', 'Hakan',
  'Juri', 'Yun', 'Yang', 'Evil Ryu', 'Oni', 'Rolento', 'Elena', 'Poison', 'Hugo', 'Decapre'];
const ROUNDS = { top8: 'Top 8', winners: 'Winners', losers: 'Losers', grand: 'Grand final', pools: 'Pools' };
// Online games of the same two players this close together are one set.
const SET_GAP = 30 * 60;

const $ = (id) => document.getElementById(id);
const regionName = (() => {
  try { const names = new Intl.DisplayNames(undefined, { type: 'region' }); return (code) => names.of(code) || code; }
  catch { return (code) => code; }
})();
const fighterName = (id) => (id == null ? '?' : FIGHTERS[id] || '?');
const day = (t) => new Date(t * 1000).toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' });
const time = (t) => new Date(t * 1000).toLocaleString(undefined, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
const el = (tag, props = {}, ...children) => {
  const node = Object.assign(document.createElement(tag), props);
  for (const child of children) if (child != null) node.append(child);
  return node;
};

const state = { filter: {}, games: [], sets: {}, events: [], countries: [], more: false, groups: [], openSet: null };

function readAddress() {
  const params = new URLSearchParams(location.search);
  state.filter = {};
  for (const key of ['event', 'fighter', 'country', 'round', 'q']) if (params.get(key)) state.filter[key] = params.get(key);
  $('q').value = state.filter.q || '';
  state.openSet = params.get('set');
}

function writeAddress() {
  const params = new URLSearchParams(state.filter);
  if (state.openSet) params.set('set', state.openSet);
  const query = params.toString();
  history.replaceState(null, '', query ? '?' + query : location.pathname);
}

async function load(append) {
  const params = new URLSearchParams(state.filter);
  if (append && state.games.length) params.set('before', state.games[state.games.length - 1].saved_at);
  $('events').setAttribute('aria-busy', 'true');
  try {
    const response = await fetch('/v1/archive?' + params);
    if (!response.ok) throw new Error(response.status);
    const listed = await response.json();
    state.games = append ? state.games.concat(listed.games) : listed.games;
    Object.assign(state.sets, listed.sets);
    state.events = listed.events;
    state.countries = listed.countries;
    state.more = listed.more;
  } catch {
    $('summary').textContent = 'The archive did not answer. Try again in a minute.';
    $('events').removeAttribute('aria-busy');
    return;
  }
  state.groups = group(state.games);
  render();
}

// Events, newest first, each with its sets: a tournament set by its ID,
// online games by day and by the same two players close together.
function group(games) {
  const events = new Map();
  for (const game of games) {
    const online = !game.set;
    const eventKey = online ? 'online:' + day(game.saved_at) : 'event:' + (game.event || 'Tournament sets');
    if (!events.has(eventKey)) {
      events.set(eventKey, { title: online ? 'Online matches · ' + day(game.saved_at) : game.event || 'Tournament sets', online, sets: new Map() });
    }
    const event = events.get(eventKey);
    const pair = [game.names[0] || '?', game.names[1] || '?'].sort().join('\n');
    let key = online ? null : game.set;
    if (online) {
      const last = [...event.sets.values()].find((set) => set.pair === pair && set.games[set.games.length - 1].saved_at - game.saved_at <= SET_GAP);
      key = last ? last.key : game.crc;
    }
    if (!event.sets.has(key)) event.sets.set(key, { key, pair, event: event.title, online, games: [] });
    event.sets.get(key).games.push(game);
  }
  return [...events.values()].map((event) => ({ ...event, sets: [...event.sets.values()].map((set) => ({ ...set, games: set.games.reverse() })) }));
}

function tag(game, side) {
  const name = game.names[side] || 'Player ' + (side + 1);
  const country = game.countries[side];
  return el('span', { className: 'tag' }, country ? el('span', { className: 'flag', title: regionName(country), textContent: country }) : null, name);
}

function score(set) {
  const known = set.online ? null : state.sets[set.key];
  return known ? known.score.join('–') : 'vs';
}

function fighters(set) {
  const first = set.games[0];
  return fighterName(first.fighters[0]) + ' vs ' + fighterName(first.fighters[1]);
}

function renderFilters() {
  const values = {
    Event: state.filter.event === 'online' ? 'Online matches' : state.filter.event || 'All events',
    Fighter: state.filter.fighter != null ? fighterName(+state.filter.fighter) : 'Any fighter',
    Country: state.filter.country ? regionName(state.filter.country) : 'Any country',
    Round: state.filter.round ? ROUNDS[state.filter.round] || state.filter.round : 'Any round',
  };
  const keys = { Event: 'event', Fighter: 'fighter', Country: 'country', Round: 'round' };
  const box = $('filters');
  box.replaceChildren(...Object.keys(values).map((label) => {
    const on = state.filter[keys[label]] != null;
    const button = el('button', { type: 'button', className: 'f' + (on ? ' on' : '') },
      el('span', {}, el('span', { className: 'label', textContent: label }), el('b', { textContent: values[label] })));
    button.insertAdjacentHTML('beforeend', '<svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true"><path d="M3 6 L8 11 L13 6" fill="none" stroke="#ff8738" stroke-width="2"/></svg>');
    button.setAttribute('aria-label', label + ', ' + values[label]);
    button.addEventListener('click', () => pick(keys[label], label));
    return button;
  }), el('button', { type: 'button', className: 'btn', textContent: 'Clear', onclick: () => { state.filter = {}; $('q').value = ''; refresh(); } }));
}

function renderList() {
  const sets = state.groups.reduce((n, event) => n + event.sets.length, 0);
  $('summary').textContent = state.games.length
    ? `${sets} sets · ${state.games.length} games${state.more ? ' (newest first)' : ''}`
    : 'No replays match. Clear a filter above.';
  $('events').replaceChildren(...state.groups.map((event) => el('section', { className: 'event' },
    el('div', { className: 'event-head' }, el('h2', { textContent: event.title }),
      el('span', { className: 'note', textContent: event.sets.length + (event.sets.length === 1 ? ' set' : ' sets') })),
    el('div', { className: 'grid' }, ...event.sets.map((set) => {
      const first = set.games[0];
      const card = el('button', { type: 'button', className: 'card' },
        el('span', { className: 'label', textContent: set.online ? time(first.saved_at) : first.round || 'Set' }),
        el('span', { className: 'vs' }, tag(first, 0), el('span', { className: 'score', textContent: score(set) }), tag(first, 1)),
        el('span', { className: 'note', textContent: fighters(set) + ' · ' + set.games.length + (set.games.length === 1 ? ' game' : ' games') }));
      card.addEventListener('click', () => openSet(set.key));
      return card;
    })))));
  $('events').removeAttribute('aria-busy');
  $('more').hidden = !state.more;
}

function findSet(key) {
  for (const event of state.groups) for (const set of event.sets) if (set.key === key) return set;
  return null;
}

function renderSet(set) {
  const first = set.games[0];
  $('set-event').textContent = set.event;
  $('set-round').textContent = set.online ? 'Online match' : first.round || 'Set';
  $('set-versus').replaceChildren(tag(first, 0), el('span', { className: 'score', textContent: score(set) }), tag(first, 1));
  const size = set.games.reduce((n, game) => n + game.size, 0);
  $('set-meta').textContent = `${day(first.saved_at)} · ${set.games.length} ${set.games.length === 1 ? 'game' : 'games'} · ${Math.ceil(size / 1024)} KB${first.phase ? ' · ' + first.phase : ''}`;
  const actions = [];
  const pack = !set.online && state.sets[set.key];
  if (pack) actions.push(el('a', { className: 'btn main', href: pack.pack, download: '', textContent: 'Download set (.emberpack)' }));
  const copy = el('button', { type: 'button', className: 'btn', textContent: 'Copy link' });
  copy.addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(location.href); copy.textContent = 'Link copied'; } catch { copy.textContent = location.href; }
  });
  actions.push(copy);
  $('set-actions').replaceChildren(...actions);
  $('set-games').replaceChildren(...set.games.map((game, i) => el('div', { className: 'game' },
    el('b', { textContent: 'Game ' + (i + 1) }),
    el('span', { textContent: fighterName(game.fighters[0]) + ' vs ' + fighterName(game.fighters[1]) }),
    el('span', { className: 'when', textContent: time(game.saved_at) }),
    el('a', { href: game.url, download: '', textContent: '.emberreplay' }))));
}

function render() {
  renderFilters();
  const set = state.openSet && findSet(state.openSet);
  $('list-view').hidden = !!set;
  $('set-view').hidden = !set;
  if (set) { renderSet(set); $('back').focus(); } else renderList();
}

function openSet(key) { state.openSet = key; writeAddress(); render(); scrollTo(0, 0); }

function refresh() { state.openSet = null; writeAddress(); load(false); }

// The popup grid: "Any" first, then every value; picking one closes it.
function pick(key, label) {
  const choices = {
    event: [['online', 'Online matches'], ...state.events.map((title) => [title, title])],
    fighter: FIGHTERS.map((name, id) => [String(id), name]).sort((a, b) => a[1].localeCompare(b[1])),
    country: state.countries.map((code) => [code, regionName(code)]).sort((a, b) => a[1].localeCompare(b[1])),
    round: Object.entries(ROUNDS),
  }[key];
  const any = { event: 'All events', fighter: 'Any fighter', country: 'Any country', round: 'Any round' }[key];
  const dialog = $('pick');
  const tiles = key === 'fighter';
  dialog.style.setProperty('--w', tiles ? '1080px' : key === 'event' ? '640px' : '760px');
  $('pick-title').textContent = tiles ? 'Fighter, used by either player' : label;
  const options = $('pick-options');
  options.className = 'options' + (tiles ? ' tiles' : '');
  options.style.setProperty('--cell', tiles ? '110px' : key === 'event' ? '100%' : '180px');
  const current = state.filter[key] ?? '';
  options.replaceChildren(...[['', any], ...choices].map(([value, text]) => {
    const button = el('button', { type: 'button', className: 'o' + (value ? '' : ' any'), textContent: text });
    button.setAttribute('aria-pressed', String(value === current));
    button.addEventListener('click', () => {
      if (value) state.filter[key] = value; else delete state.filter[key];
      dialog.close();
      refresh();
    });
    return button;
  }));
  dialog.showModal();
  (options.querySelector('[aria-pressed=true]') || options.firstChild).focus();
}

// Arrow keys move between options and cards in reading order, rows by
// position, for a pad or remote that sends arrow keys.
document.addEventListener('keydown', (event) => {
  const moves = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: 'up', ArrowDown: 'down' };
  const move = moves[event.key];
  const here = document.activeElement;
  if (move == null || !here || !here.matches('.o, .card, .f')) return;
  const all = [...(here.closest('dialog, main') || document).querySelectorAll('.o, .card, .f')].filter((node) => node.offsetParent);
  const at = all.indexOf(here);
  let next;
  if (typeof move === 'number') next = all[at + move];
  else {
    const box = here.getBoundingClientRect();
    const candidates = all.filter((node) => {
      const other = node.getBoundingClientRect();
      return move === 'down' ? other.top > box.bottom - 2 : other.bottom < box.top + 2;
    });
    candidates.sort((a, b) => {
      const ra = a.getBoundingClientRect(), rb = b.getBoundingClientRect();
      const dy = (r) => Math.abs((move === 'down' ? r.top - box.bottom : box.top - r.bottom));
      return dy(ra) - dy(rb) || Math.abs(ra.left - box.left) - Math.abs(rb.left - box.left);
    });
    next = candidates[0];
  }
  if (next) { event.preventDefault(); next.focus(); next.scrollIntoView({ block: 'nearest' }); }
});

$('pick-close').addEventListener('click', () => $('pick').close());
$('back').addEventListener('click', () => { state.openSet = null; writeAddress(); render(); });
$('more').addEventListener('click', () => load(true));
let typing;
$('q').addEventListener('input', () => {
  clearTimeout(typing);
  typing = setTimeout(() => {
    const q = $('q').value.trim();
    if (q) state.filter.q = q; else delete state.filter.q;
    refresh();
  }, 300);
});
readAddress();
load(false);
