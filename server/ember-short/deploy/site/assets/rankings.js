// needs: site marks account filters
// The rankings page: the board Ember shows under Online play, Rankings
// (/rankings.json, nginx's proxy of the bridge's GET /v1/ranking/board/public),
// with search, class, country and region filters, grouping and order kept in
// the URL fragment so a view can be shared. Like the board in Ember it shows
// a name, a flag and a class, never a rating, a position or a number for a
// player. Names are the players' own words, so they only ever go in as text.
(function () {
  'use strict';
  var site = window.EmberSite;
  var marks = window.EmberMarks;
  var filters = window.EmberFilters;
  var account = window.EmberAccount;
  // While the tab is shown the board is asked for again this often; nginx and
  // the bridge each keep it 15 s, so a change shows within about a minute.
  var REFRESH_MS = 30000;
  // The games the page lists, one board at a time (the bridge's `game`, as
  // Ember's own Rankings is its game's): Ultra Street Fighter IV's from
  // /rankings.json and Street Fighter V's from /rankings-sfv.json, chosen by
  // the Game buttons and kept in the address as ?game=sfv. Every game is rated
  // in the same ten bands; each names them as its own ranks, band 1 to 10:
  // USF4's classes (marks.js) and SFV's league tiers (SFV Ember's
  // SfvProduct.cxx), each with its chart's short label, the colour each
  // band's pill and column take (the tier-* rules in site.css: USF4's colour
  // families, SFV's own league colours), what the game calls a rank and the
  // Ember that plays it.
  var GAMES = {
    usf4: {
      key: 'usf4', board: '/rankings.json', app: 'Ember', rank: 'class', ranks: 'Classes',
      classes: marks.CLASSES, short: marks.SHORT, tiers: marks.TIERS
    },
    sfv: {
      key: 'sfv', board: '/rankings-sfv.json', app: 'SFV Ember', rank: 'league', ranks: 'Leagues',
      classes: ['Rookie', 'Bronze', 'Silver', 'Gold', 'Platinum', 'Diamond', 'Master', 'Grand Master', 'Ultimate Grand Master', 'Warlord'],
      short: ['R', 'B', 'S', 'G', 'P', 'D', 'M', 'GM', 'UGM', 'W'],
      tiers: ['rookie', 'bronze', 'silver', 'gold', 'platinum', 'diamond', 'master', 'grand', 'ultimate', 'warlord']
    }
  };
  var game = /(?:^|[?&])game=sfv(?:&|$)/.test(location.search) ? GAMES.sfv : GAMES.usf4;
  // The shown game's rank names and colours.
  var CLASSES = game.classes, SHORT = game.short, TIERS = game.tiers;
  var BANDS = CLASSES.length;
  var NO_COUNTRY = filters.NO_COUNTRY;

  var games = document.getElementById('games');
  var classesBox = document.getElementById('classes');
  var toolbar = document.getElementById('toolbar');
  var bar = document.getElementById('bar');
  var search = document.getElementById('search');
  var countrySelect = document.getElementById('country');
  var regionSelect = document.getElementById('region');
  var groupSelect = document.getElementById('group');
  var orderSelect = document.getElementById('order');
  var summary = document.getElementById('summary');
  var clearButton = document.getElementById('clear');
  var shareButton = document.getElementById('share');
  var copyListButton = document.getElementById('copy-list');
  var refreshButton = document.getElementById('refresh');
  var jump = document.getElementById('jump');
  var chart = document.getElementById('chart');
  var chartPlot = document.getElementById('chart-plot');
  var chartTip = document.getElementById('chart-tip');
  var board = document.getElementById('board');
  var status = document.getElementById('status');

  // players: null until the first answer; each { name, country, band, index, folded }.
  // phase: 'loading', 'ready', 'off' (no rankings here), 'hidden' (classes not shown yet) or 'failed'.
  var data = { players: null, listedAt: 0, loadedAt: 0, phase: 'loading', failed: false, key: '' };
  // The class, grouping and order; the name, country and region filters are filter's.
  var view = { classes: {}, group: 'class', order: 'recent' };
  var loading = false;
  var timer = null;

  // The name, country and region filters (filters.js); every change draws the board again.
  var filter = filters.controller({ search: search, country: countrySelect, region: regionSelect }, update);
  var countryName = marks.countryName;
  var flag = marks.flag;


  var el = site.el;
  var plural = site.plural;
  function ago(seconds) {
    if (seconds < 60) return 'just now';
    var minutes = Math.floor(seconds / 60);
    if (minutes < 60) return minutes + ' min ago';
    var hours = Math.floor(minutes / 60);
    return hours < 48 ? hours + ' h ago' : Math.floor(hours / 24) + ' days ago';
  }

  function pill(band) {
    var node = el('span', 'rank-class tier-' + TIERS[band - 1], CLASSES[band - 1]);
    node.title = (game.rank === 'league' ? 'League ' : 'Class ') + CLASSES[band - 1];
    return node;
  }

  // The view in the URL fragment, which browsers never send: q, class (bands
  // by comma), country (a code or "none"), region, group and order.
  function readHash() {
    var params;
    try { params = new URLSearchParams(location.hash.replace(/^#/, '')); } catch (error) { return; }
    filter.read(params);
    view.classes = {};
    (params.get('class') || '').split(',').forEach(function (value) {
      var band = parseInt(value, 10);
      if (band >= 1 && band <= BANDS) view.classes[band] = true;
    });
    var group = params.get('group');
    view.group = group === 'country' || group === 'none' ? group : 'class';
    view.order = params.get('order') === 'name' ? 'name' : 'recent';
  }
  function hashOf() {
    var parts = [], set = filter.parts();
    if (set.q) parts.push(set.q);
    var bands = selectedBands();
    if (bands.length) parts.push('class=' + bands.join(','));
    if (set.country) parts.push(set.country);
    if (set.region) parts.push(set.region);
    if (view.group !== 'class') parts.push('group=' + view.group);
    if (view.order !== 'recent') parts.push('order=' + view.order);
    return parts.length ? '#' + parts.join('&') : '';
  }
  function writeHash() {
    var hash = hashOf();
    if (hash === location.hash || (!hash && !location.hash)) return;
    try { history.replaceState(null, '', location.pathname + location.search + hash); } catch (error) { /* file: pages */ }
  }
  function selectedBands() {
    return Object.keys(view.classes).map(Number).sort(function (a, b) { return b - a; });
  }
  function filtered() {
    return filter.any() || selectedBands().length > 0;
  }

  // Each filter, and every filter but one: the counts beside a filter's
  // choices show what choosing it would list given the others.
  function passes(player, skip) {
    if (skip !== 'class' && selectedBands().length && !view.classes[player.band]) return false;
    return filter.passes(player, skip);
  }

  function row(player) {
    var item = el('li', 'rank-row');
    item.appendChild(flag(player.country));
    item.appendChild(filter.name('span', 'rank-name', player));
    item.appendChild(pill(player.band));
    return item;
  }

  // The class distribution: a column per class, D to Grand Master, counting
  // the players the other filters leave (as the class chips do), in its
  // class's colour, with a smoothed outline over the columns. Chosen classes
  // stay bright. The colours repeat what each column's label says. A column
  // is a click target for its class, like its chip; the chips carry the same
  // counts for keyboards and screen readers, so the drawing itself is hidden
  // from them and the plot is described in one line instead.
  var CHART_HEIGHT = 170, CHART_TOP = 12, CHART_BOTTOM = 24, CHART_LEFT = 30, BAR_MAX = 24, SMOOTHING = 0.85, OUTLINE_STEPS = 80;
  var chartCounts = null;
  var svg = site.svg;

  // The top of the y axis: 1, 2 or 5 times a power of ten per tick, about four ticks.
  function niceTop(max) {
    var raw = Math.max(1, max) / 4, power = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10));
    var step = Math.max(1, [1, 2, 5, 10].map(function (k) { return k * power; }).filter(function (k) { return k >= raw; })[0]);
    return { top: step * Math.ceil(Math.max(1, max) / step), step: step };
  }
  // The outline: each class's count spread as a normal curve a little under
  // a class wide, scaled so its area over the drawn range (D's left edge to
  // Grand Master's right, sampled as drawn) is that count. So the outline's
  // area is the columns' total, and the end classes lose nothing past the edge.
  function outline(counts) {
    var xs = [], values = [], step = BANDS / OUTLINE_STEPS, i, j;
    for (j = 0; j <= OUTLINE_STEPS; j++) {
      xs.push(-0.5 + j * step);
      values.push(0);
    }
    for (i = 0; i < counts.length; i++) {
      if (!counts[i]) continue;
      var kernel = xs.map(function (x) {
        var d = (x - i) / SMOOTHING;
        return Math.exp(-d * d / 2);
      });
      var area = 0;
      for (j = 0; j < OUTLINE_STEPS; j++) area += (kernel[j] + kernel[j + 1]) / 2 * step;
      for (j = 0; j <= OUTLINE_STEPS; j++) values[j] += counts[i] * kernel[j] / area;
    }
    return { xs: xs, values: values };
  }

  function drawChart() {
    var counts = chartCounts;
    while (chartPlot.firstChild) chartPlot.removeChild(chartPlot.firstChild);
    chartTip.hidden = true;
    var width = chartPlot.clientWidth;
    if (!counts || width < 120) return;
    var total = counts.reduce(function (a, b) { return a + b; }, 0);
    var plotWidth = width - CHART_LEFT, plotHeight = CHART_HEIGHT - CHART_TOP - CHART_BOTTOM;
    var slot = plotWidth / BANDS, barWidth = Math.min(BAR_MAX, slot * 0.6);
    var curve = outline(counts);
    var axis = niceTop(Math.max.apply(null, counts.concat(curve.values)));
    var y = function (value) { return CHART_TOP + plotHeight - value / axis.top * plotHeight; };
    var root = svg('svg', { width: width, height: CHART_HEIGHT, viewBox: '0 0 ' + width + ' ' + CHART_HEIGHT, 'aria-hidden': 'true', focusable: 'false' }, chartPlot);
    for (var tick = 0; tick <= axis.top; tick += axis.step) {
      svg('line', { 'class': 'rank-chart-grid', x1: CHART_LEFT, x2: width, y1: Math.round(y(tick)) + 0.5, y2: Math.round(y(tick)) + 0.5 }, root);
      svg('text', { 'class': 'rank-chart-tick', x: CHART_LEFT - 8, y: y(tick) + 4, 'text-anchor': 'end' }, root).textContent = String(tick);
    }
    var chosen = selectedBands().length > 0;
    var bars = counts.map(function (count, index) {
      var middle = CHART_LEFT + slot * (index + 0.5);
      var top = y(count), left = middle - barWidth / 2, bottom = y(0), r = Math.min(4, barWidth / 2, bottom - top);
      svg('text', { 'class': 'rank-chart-label', x: middle, y: CHART_HEIGHT - 6, 'text-anchor': 'middle' }, root).textContent = SHORT[index];
      return svg('path', {
        'class': 'rank-chart-bar tier-' + TIERS[index] + (chosen && !view.classes[index + 1] ? ' dim' : ''),
        d: count ? 'M' + left + ' ' + bottom + 'V' + (top + r) + 'Q' + left + ' ' + top + ' ' + (left + r) + ' ' + top +
          'H' + (left + barWidth - r) + 'Q' + (left + barWidth) + ' ' + top + ' ' + (left + barWidth) + ' ' + (top + r) + 'V' + bottom + 'Z' : 'M0 0'
      }, root);
    });
    var points = curve.xs.map(function (x, j) {
      return (CHART_LEFT + slot * (x + 0.5)).toFixed(1) + ' ' + y(curve.values[j]).toFixed(1);
    });
    svg('path', { 'class': 'rank-chart-curve', d: 'M' + points.join('L') }, root);
    counts.forEach(function (count, index) {
      var hit = svg('rect', { 'class': 'rank-chart-hit', x: CHART_LEFT + slot * index, y: 0, width: slot, height: CHART_HEIGHT }, root);
      hit.addEventListener('pointerenter', function () {
        bars[index].classList.add('hover');
        chartTip.textContent = '';
        chartTip.appendChild(el('strong', '', plural(count, 'player', 'players')));
        chartTip.appendChild(el('span', '', CLASSES[index] + ', ' + Math.round(100 * count / total) + '%'));
        chartTip.hidden = false;
        var left = CHART_LEFT + slot * (index + 0.5) - chartTip.offsetWidth / 2;
        chartTip.style.left = chartPlot.offsetLeft + Math.max(0, Math.min(width - chartTip.offsetWidth, left)) + 'px';
        chartTip.style.top = chartPlot.offsetTop + Math.max(0, y(count) - chartTip.offsetHeight - 10) + 'px';
      });
      hit.addEventListener('pointerleave', function () {
        bars[index].classList.remove('hover');
        chartTip.hidden = true;
      });
      hit.addEventListener('click', function () { toggleClass(index + 1); });
    });
    chartPlot.setAttribute('aria-label', 'Players by ' + game.rank + ': ' + counts.map(function (count, index) {
      return CLASSES[index] + ' ' + count;
    }).join(', ') + '.');
  }

  function classChart(counts) {
    chartCounts = counts.some(function (count) { return count > 0; }) ? counts : null;
    chart.hidden = !chartCounts;
    drawChart();
  }

  function message(text, detail, withClear) {
    var node = el('div', 'room-card room-message');
    node.appendChild(el('p', 'room-message-title', text));
    if (detail) node.appendChild(el('p', 'note', detail));
    if (withClear) {
      var button = el('button', 'room-action', 'Clear filters');
      button.type = 'button';
      button.addEventListener('click', clearFilters);
      node.appendChild(button);
    }
    return node;
  }

  // Players per class (index band - 1) under every filter but the class
  // filter: what choosing each class would list. The chips and the chart
  // both show it.
  function classCounts(players) {
    var counts = [];
    for (var band = 1; band <= BANDS; band++) counts.push(0);
    players.forEach(function (player) { if (passes(player, 'class')) counts[player.band - 1]++; });
    return counts;
  }

  function classChips(counts) {
    // The chips are made again on each change; the one in focus stays so.
    var active = document.activeElement;
    var focused = active && classesBox.contains(active) ? active.getAttribute('data-band') : null;
    while (classesBox.firstChild) classesBox.removeChild(classesBox.firstChild);
    var all = el('button', 'rank-chip all', 'All ' + game.ranks.toLowerCase());
    all.type = 'button';
    all.setAttribute('data-band', '');
    all.setAttribute('aria-pressed', selectedBands().length ? 'false' : 'true');
    classesBox.appendChild(all);
    for (var b = BANDS; b >= 1; b--) {
      var chip = el('button', 'rank-chip tier-' + TIERS[b - 1] + (counts[b - 1] ? '' : ' empty'));
      chip.type = 'button';
      chip.setAttribute('data-band', String(b));
      chip.setAttribute('aria-pressed', view.classes[b] ? 'true' : 'false');
      chip.setAttribute('aria-label', CLASSES[b - 1] + ', ' + plural(counts[b - 1], 'player', 'players'));
      chip.appendChild(el('span', 'rank-chip-name', CLASSES[b - 1]));
      chip.appendChild(el('span', 'rank-chip-count', String(counts[b - 1])));
      classesBox.appendChild(chip);
    }
    if (focused !== null) {
      var again = classesBox.querySelector('[data-band="' + focused + '"]');
      if (again) again.focus();
    }
  }

  // Groups in board order: classes highest first, countries by name with
  // players without a flag last, or one group of everyone.
  function groupsOf(shown) {
    var byKey = {}, groups = [];
    shown.forEach(function (player) {
      var key = view.group === 'class' ? String(player.band) : view.group === 'country' ? player.country || NO_COUNTRY : 'all';
      if (!byKey[key]) {
        byKey[key] = { key: key, players: [] };
        groups.push(byKey[key]);
      }
      byKey[key].players.push(player);
    });
    if (view.group === 'class') groups.sort(function (a, b) { return Number(b.key) - Number(a.key); });
    if (view.group === 'country') {
      groups.sort(function (a, b) {
        if (a.key === NO_COUNTRY) return 1;
        if (b.key === NO_COUNTRY) return -1;
        return countryName(a.key).localeCompare(countryName(b.key));
      });
    }
    return groups;
  }

  function ordered(players) {
    var list = players.slice();
    if (view.order === 'name') {
      list.sort(function (a, b) { return a.name.localeCompare(b.name, undefined, { sensitivity: 'base', numeric: true }) || a.index - b.index; });
    } else if (view.group !== 'class') {
      // The board's own order: class, then most recent to play.
      list.sort(function (a, b) { return a.index - b.index; });
    }
    return list;
  }

  function groupHead(group) {
    var head = el('h2', 'rank-group-head');
    if (view.group === 'class') {
      head.appendChild(pill(Number(group.key)));
      head.appendChild(el('span', 'rank-group-title', CLASSES[Number(group.key) - 1]));
    } else {
      var code = group.key === NO_COUNTRY ? '' : group.key;
      head.appendChild(flag(code));
      head.appendChild(el('span', 'rank-group-title', countryName(code)));
    }
    head.appendChild(el('span', 'rank-group-count', plural(group.players.length, 'player', 'players')));
    return head;
  }

  function jumpLinks(groups) {
    while (jump.firstChild) jump.removeChild(jump.firstChild);
    jump.hidden = view.group === 'none' || groups.length < 2;
    if (jump.hidden) return;
    jump.appendChild(el('span', 'rank-jump-label', 'Jump to'));
    groups.forEach(function (group) {
      var label = view.group === 'class' ? CLASSES[Number(group.key) - 1] : group.key === NO_COUNTRY ? 'No flag' : group.key;
      var button = el('button', 'rank-jump-link', label);
      button.type = 'button';
      if (view.group === 'country') button.title = countryName(group.key === NO_COUNTRY ? '' : group.key);
      button.addEventListener('click', function () {
        var target = document.getElementById('group-' + group.key);
        if (target) {
          target.scrollIntoView({ block: 'start' });
          target.focus({ preventScroll: true });
        }
      });
      jump.appendChild(button);
    });
  }

  // The line over the board: how many players are listed or shown, and how
  // fresh the list is.
  function summaryText(players, shown) {
    var updated = data.listedAt ? ' Updated ' + ago(Math.max(0, Math.floor(Date.now() / 1000) - data.listedAt)) + '.' : '';
    var stale = data.failed ? ' Could not refresh; showing the last list.' : '';
    if (players.length === 0) return 'Nobody is listed yet.' + updated + stale;
    return (shown === players.length ? plural(players.length, 'player', 'players') + ' listed.' :
      'Showing ' + shown + ' of ' + plural(players.length, 'player', 'players') + '.') + updated + stale;
  }

  function render() {
    board.setAttribute('aria-busy', String(data.phase === 'loading'));
    while (board.firstChild) board.removeChild(board.firstChild);
    var players = data.players;
    var ready = data.phase === 'ready' && players && players.length > 0;
    classesBox.hidden = toolbar.hidden = !ready;
    if (!ready) {
      chart.hidden = true;
      chartCounts = null;
    }
    // Refresh stays once there is an answer, whatever it said.
    bar.hidden = data.phase === 'loading';
    jump.hidden = true;
    clearButton.hidden = !ready || !filtered();
    shareButton.hidden = copyListButton.hidden = !ready;
    summary.textContent = '';

    // Only after a change of game: the page's first board loads under its own
    // placeholder.
    if (data.phase === 'loading') {
      board.appendChild(el('p', 'room-summary', 'Loading players...'));
      return;
    }
    if (data.phase === 'off') {
      board.appendChild(message('This service does not offer rankings right now.', 'Rankings come from public room games. In ' + game.app + ', open Online play, Public rooms to play.'));
      return;
    }
    if (data.phase === 'hidden') {
      board.appendChild(message(game.ranks + ' are not shown yet.', 'Public room games still count. The board opens here and in ' + game.app + ' once ' + game.ranks.toLowerCase() + ' are shown.'));
      return;
    }
    if (!players) {
      board.appendChild(message('The board is not available right now.', 'Try again in a minute, or look in ' + game.app + ' under Online play, Rankings.'));
      return;
    }
    if (players.length === 0) {
      summary.textContent = summaryText(players, 0);
      board.appendChild(message('Nobody is on the board yet.', 'Players appear once their ' + game.rank + ' settles after a few public room games, and they have opened Rankings in ' + game.app + '.'));
      return;
    }

    var counts = classCounts(players);
    classChips(counts);
    classChart(counts);
    filter.fill(players, passes);
    groupSelect.value = view.group;
    orderSelect.value = view.order;

    var shown = players.filter(function (player) { return passes(player); });
    summary.textContent = summaryText(players, shown.length);
    if (shown.length === 0) {
      board.appendChild(message('No players match these filters.', filter.state.q ? 'Names are matched without case or accents. Players who have not opened Rankings lately may show an older name.' : '', true));
      return;
    }
    var groups = groupsOf(shown);
    jumpLinks(groups);
    var fragment = document.createDocumentFragment();
    groups.forEach(function (group) {
      var section = el('section', 'rank-group' + (view.group === 'none' ? '' : ' by-' + view.group));
      if (view.group !== 'none') {
        section.id = 'group-' + group.key;
        section.tabIndex = -1;
        section.appendChild(groupHead(group));
      }
      var list = el('ul', 'rank-list');
      ordered(group.players).forEach(function (player) { list.appendChild(row(player)); });
      section.appendChild(list);
      fragment.appendChild(section);
    });
    board.appendChild(fragment);
  }

  function update() {
    filter.settle();
    writeHash();
    render();
  }

  function toggleClass(band) {
    if (view.classes[band]) delete view.classes[band];
    else view.classes[band] = true;
    update();
  }

  function clearFilters() {
    filter.clear();
    view.classes = {};
    update();
    search.focus();
  }

  function copy(text, done, failed) { site.copy(status, text, done, failed); }

  // The players shown, as lines of text in the order on screen.
  function listText() {
    var lines = [];
    var shown = (data.players || []).filter(function (player) { return passes(player); });
    groupsOf(shown).forEach(function (group) {
      ordered(group.players).forEach(function (player) {
        lines.push(player.name + (player.country ? ' (' + player.country + ')' : '') + ': ' + CLASSES[player.band - 1]);
      });
    });
    return lines.join('\n');
  }

  function valid(body) {
    if (!body || typeof body.shown !== 'boolean' || !Array.isArray(body.players)) return null;
    var players = [];
    body.players.forEach(function (entry) {
      var listing = entry && entry.listing;
      if (!listing || typeof listing.name !== 'string' || !listing.name) return;
      if (typeof entry.band !== 'number' || entry.band < 1 || entry.band > BANDS || entry.band % 1) return;
      players.push({ name: listing.name, country: marks.country(listing.country), band: entry.band, index: players.length, folded: filters.fold(listing.name) });
    });
    return players;
  }

  // The board as text, so a refresh can tell whether anything changed.
  function keyOf(phase, players) {
    return phase + '\n' + (players || []).map(function (player) {
      return player.band + ' ' + player.country + ' ' + player.name;
    }).join('\n');
  }

  function schedule(delay) {
    clearTimeout(timer);
    timer = document.hidden ? null : setTimeout(function () { load(true); }, delay);
  }

  // quiet: a timed refresh, which draws the board again only when it changed,
  // so the page does not jump under someone reading or filtering it.
  function load(quiet) {
    if (loading) return;
    loading = true;
    refreshButton.disabled = true;
    var asked = game;
    account.ask(asked.board, { cache: 'no-store', credentials: 'omit' })
      .then(function (answer) {
        // The other game was chosen while this board was on its way: it is
        // not this page's board any more (the next step loads that one).
        if (asked !== game) return;
        var players = answer.kind === 'ok' ? valid(answer.body) : null;
        if (answer.kind === 'off') {
          data.phase = 'off';
          data.players = null;
        } else if (players) {
          data.phase = answer.body.shown ? 'ready' : 'hidden';
          data.players = players;
          data.listedAt = typeof answer.body.listed_at === 'number' ? answer.body.listed_at : Math.floor(Date.now() / 1000);
        } else {
          // Any other answer, none at all, or one this page cannot read: the last board stays.
          data.failed = true;
          if (!data.players) data.phase = 'failed';
          return;
        }
        data.loadedAt = Date.now();
        data.failed = false;
      })
      .then(function () {
        loading = false;
        refreshButton.disabled = false;
        if (asked !== game) {
          load(false);
          return;
        }
        var key = keyOf(data.phase, data.players);
        if (!quiet || key !== data.key) {
          data.key = key;
          render();
        } else if (data.phase === 'ready' && data.players.length) {
          var text = summaryText(data.players, data.players.filter(function (player) { return passes(player); }).length);
          if (summary.textContent !== text) summary.textContent = text;
        }
        schedule(REFRESH_MS);
      });
  }

  classesBox.addEventListener('click', function (event) {
    var button = event.target.closest ? event.target.closest('button') : null;
    if (!button || !classesBox.contains(button)) return;
    var band = parseInt(button.getAttribute('data-band'), 10);
    if (band) {
      toggleClass(band);
    } else {
      view.classes = {};
      update();
    }
  });
  groupSelect.addEventListener('change', function () { view.group = groupSelect.value; update(); });
  orderSelect.addEventListener('change', function () { view.order = orderSelect.value; update(); });
  clearButton.addEventListener('click', clearFilters);
  shareButton.addEventListener('click', function () {
    copy(location.origin + '/rankings' + gameQuery() + hashOf(), 'Link to this view copied.', 'Copy failed. Copy the address from the address bar.');
  });
  copyListButton.addEventListener('click', function () {
    var text = listText();
    var count = text ? text.split('\n').length : 0;
    copy(text, 'Copied ' + plural(count, 'player', 'players') + '.', 'Copy failed.');
  });
  refreshButton.addEventListener('click', function () { load(false); });
  document.addEventListener('keydown', function (event) {
    if (event.key !== '/' || event.ctrlKey || event.metaKey || event.altKey || toolbar.hidden) return;
    var target = event.target;
    if (target && (target.tagName === 'INPUT' || target.tagName === 'SELECT' || target.tagName === 'TEXTAREA' || target.isContentEditable)) return;
    event.preventDefault();
    search.focus();
    search.select();
  });
  window.addEventListener('hashchange', function () {
    if (location.hash === hashOf()) return;
    readHash();
    render();
  });
  var resizeTimer = null;
  window.addEventListener('resize', function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(drawChart, 100);
  });
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      clearTimeout(timer);
      timer = null;
    } else {
      schedule(Math.max(0, REFRESH_MS - (Date.now() - data.loadedAt)));
    }
  });
  // The flag index, read once beside the board; the board is drawn again
  // with flags when it arrives.
  marks.flagsReady().then(function (loaded) {
    if (loaded && data.players) render();
  });

  // The shown game in the address: nothing for USF4's, as before games.
  function gameQuery() { return game.key === 'usf4' ? '' : '?game=' + game.key; }
  // The shown game's look, words and links, and the switch's mark (game.js).
  function gameButtons() {
    if (window.EmberGame) window.EmberGame.set(game.key);
    groupSelect.options[0].textContent = 'By ' + game.rank;
    classesBox.setAttribute('aria-label', 'Show ' + game.ranks.toLowerCase());
    jump.setAttribute('aria-label', 'Jump to a ' + game.rank);
  }
  // Another game: its board from the start, under the same filters (the
  // bands are the same in every game).
  games.addEventListener('click', function (event) {
    var button = event.target.closest ? event.target.closest('button') : null;
    if (!button || !games.contains(button)) return;
    var chosen = GAMES[button.getAttribute('data-game')];
    if (!chosen || chosen === game) return;
    game = chosen;
    CLASSES = game.classes;
    SHORT = game.short;
    TIERS = game.tiers;
    data = { players: null, listedAt: 0, loadedAt: 0, phase: 'loading', failed: false, key: '' };
    try { history.replaceState(null, '', location.pathname + gameQuery() + hashOf()); } catch (error) { /* file: pages */ }
    gameButtons();
    render();
    load(false);
  });
  gameButtons();
  readHash();
  load(false);
}());
