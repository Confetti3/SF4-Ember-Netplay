// The rankings page: the board Ember shows under Online play, Rankings
// (/rankings.json, nginx's proxy of the bridge's GET /v1/ranking/board/public),
// with search, class, country and region filters, grouping and order kept in
// the URL fragment so a view can be shared. Like the board in Ember it shows
// a name, a flag and a class, never a rating, a position or a number for a
// player. Names are the players' own words, so they only ever go in as text.
(function () {
  'use strict';
  var site = window.EmberSite;
  // While the tab is shown the board is asked for again this often; nginx and
  // the bridge each keep it 15 s, so a change shows within about a minute.
  var REFRESH_MS = 30000;
  // USF4's classes, band 1 to 10 (src/ui/NameMarks.cxx), and the colour
  // family each pill takes.
  var CLASSES = ['D', 'D+', 'C', 'C+', 'B', 'B+', 'A', 'A+', 'Master', 'Grand Master'];
  var TIERS = ['d', 'd', 'c', 'c', 'b', 'b', 'a', 'a', 'm', 'gm'];
  var BANDS = CLASSES.length;
  // Regions for the region filter, by the country codes the game sends.
  var REGIONS = [
    ['eu', 'Europe', 'AD AL AT AX BA BE BG BY CH CY CZ DE DK EE ES FI FO FR GB GG GI GR HR HU IE IM IS IT JE LI LT LU LV MC MD ME MK MT NL NO PL PT RO RS RU SE SI SJ SK SM UA VA XK'],
    ['na', 'North America', 'BM CA GL PM US UM'],
    ['la', 'Latin America and Caribbean', 'AG AI AR AW BB BL BO BQ BR BS BZ CL CO CR CU CW DM DO EC FK GD GF GP GT GY HN HT JM KN KY LC MF MQ MS MX NI PA PE PR PY SR SV SX TC TT UY VC VE VG VI'],
    ['me', 'Middle East', 'AE BH IL IQ IR JO KW LB OM PS QA SA SY TR YE'],
    ['as', 'Asia', 'AF AM AZ BD BN BT CN GE HK ID IN JP KG KH KP KR KZ LA LK MM MN MO MV MY NP PH PK SG TH TJ TL TM TW UZ VN'],
    ['oc', 'Oceania', 'AS AU CK FJ FM GU KI MH MP NC NF NR NU NZ PF PG PN PW SB TK TO TV VU WF WS'],
    ['af', 'Africa', 'AO BF BI BJ BW CD CF CG CI CM CV DJ DZ EG EH ER ET GA GH GM GN GQ GW KE KM LR LS LY MA MG ML MR MU MW MZ NA NE NG RE RW SC SD SH SL SN SO SS ST SZ TD TG TN TZ UG YT ZA ZM ZW']
  ];
  var REGION_OF = {};
  REGIONS.forEach(function (region) {
    region[2].split(' ').forEach(function (code) { REGION_OF[code] = region[0]; });
  });
  // The flags in assets/flags.png, laid out as assets/flags.json says (both
  // copies of the repository's assets/flags, flag-icons under MIT, which
  // scripts/check-flag-atlas.py keeps equal). Drawn FLAG_WIDTH wide. Until the
  // index loads, or if it cannot, a country shows its code.
  var FLAG_WIDTH = 20;
  var flagAtlas = null;
  var OTHER_REGION = 'other';
  var NO_COUNTRY = 'none';
  var COUNTRY = /^[A-Z]{2}$/;

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
  var view = { q: '', needle: '', classes: {}, country: '', region: '', group: 'class', order: 'recent' };
  var loading = false;
  var timer = null;
  var typingTimer = null;

  var regionNames = {};
  REGIONS.forEach(function (region) { regionNames[region[0]] = region[1]; });
  regionNames[OTHER_REGION] = 'Other';
  var graphemes = null;
  try { graphemes = new Intl.Segmenter('en', { granularity: 'grapheme' }); } catch (error) { graphemes = null; }
  var displayNames = null;
  try { displayNames = new Intl.DisplayNames(['en'], { type: 'region' }); } catch (error) { displayNames = null; }
  var countryNames = {};
  function countryName(code) {
    if (!code) return 'No flag';
    if (!countryNames[code]) {
      var name = null;
      try { name = displayNames ? displayNames.of(code) : null; } catch (error) { name = null; }
      countryNames[code] = name && name !== code ? name : code;
    }
    return countryNames[code];
  }
  function regionOf(player) { return player.country ? REGION_OF[player.country] || OTHER_REGION : OTHER_REGION; }

  // Case and accents fold away for search: "eric" finds "Éric". Folding a
  // character at a time keeps a map back to the name for the highlight.
  // One code point at a time, for the name and the query alike, so a name
  // always matches itself; final sigma folds like any sigma.
  function foldChar(ch) {
    var folded = ch.normalize ? ch.normalize('NFKD').replace(/[\u0300-\u036f]/g, '') : ch;
    return folded.toLowerCase().replace(/\u03c2/g, '\u03c3');
  }
  function fold(text) {
    var chars = Array.from(text), folded = '', starts = [];
    for (var i = 0; i < chars.length; i++) {
      var piece = foldChar(chars[i]);
      for (var j = 0; j < piece.length; j++) starts.push(i);
      folded += piece;
    }
    return { text: folded, starts: starts, chars: chars };
  }

  // A query as kept: at most 64 characters, never half of one, and folded
  // once for matching.
  function setQuery(text) {
    var clipped = Array.from(String(text)).slice(0, 64).join('');
    view.q = clipped.toWellFormed ? clipped.toWellFormed() : clipped;
    view.needle = fold(view.q).text.trim();
  }

  var el = site.el;
  function plural(count, one, many) { return count + ' ' + (count === 1 ? one : many); }
  function ago(seconds) {
    if (seconds < 60) return 'just now';
    var minutes = Math.floor(seconds / 60);
    if (minutes < 60) return minutes + ' min ago';
    var hours = Math.floor(minutes / 60);
    return hours < 48 ? hours + ' h ago' : Math.floor(hours / 24) + ' days ago';
  }

  function pill(band) {
    var node = el('span', 'rank-class tier-' + TIERS[band - 1], CLASSES[band - 1]);
    node.title = 'Class ' + CLASSES[band - 1];
    return node;
  }
  // A country's flag from the atlas, or its code where the atlas has none.
  function flag(code) {
    var atlas = flagAtlas;
    var cell = atlas && code && Object.prototype.hasOwnProperty.call(atlas.countries, code) ? atlas.countries[code] : -1;
    var node = el('span', 'rank-flag' + (code ? (cell >= 0 ? ' image' : '') : ' none'), cell >= 0 ? '' : code || '');
    if (cell >= 0) {
      var scale = FLAG_WIDTH / atlas.cellWidth;
      var stepX = atlas.cellWidth + 2 * atlas.padding, stepY = atlas.cellHeight + 2 * atlas.padding;
      node.style.width = FLAG_WIDTH + 'px';
      node.style.height = atlas.cellHeight * scale + 'px';
      node.style.backgroundSize = atlas.width * scale + 'px ' + atlas.height * scale + 'px';
      node.style.backgroundPosition = -(cell % atlas.columns * stepX + atlas.padding) * scale + 'px ' +
        -(Math.floor(cell / atlas.columns) * stepY + atlas.padding) * scale + 'px';
      node.setAttribute('role', 'img');
    }
    if (code) {
      node.title = countryName(code);
      node.setAttribute('aria-label', countryName(code));
    } else {
      node.setAttribute('aria-hidden', 'true');
    }
    return node;
  }

  // The view in the URL fragment, which browsers never send: q, class (bands
  // by comma), country (a code or "none"), region, group and order.
  function readHash() {
    var params;
    try { params = new URLSearchParams(location.hash.replace(/^#/, '')); } catch (error) { return; }
    setQuery(params.get('q') || '');
    view.classes = {};
    (params.get('class') || '').split(',').forEach(function (value) {
      var band = parseInt(value, 10);
      if (band >= 1 && band <= BANDS) view.classes[band] = true;
    });
    var country = (params.get('country') || '').toUpperCase();
    view.country = COUNTRY.test(country) ? country : country === 'NONE' ? NO_COUNTRY : '';
    var region = params.get('region') || '';
    view.region = Object.prototype.hasOwnProperty.call(regionNames, region) ? region : '';
    var group = params.get('group');
    view.group = group === 'country' || group === 'none' ? group : 'class';
    view.order = params.get('order') === 'name' ? 'name' : 'recent';
  }
  function hashOf() {
    var parts = [];
    if (view.q) parts.push('q=' + encodeURIComponent(view.q));
    var bands = selectedBands();
    if (bands.length) parts.push('class=' + bands.join(','));
    if (view.country) parts.push('country=' + view.country);
    if (view.region) parts.push('region=' + view.region);
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
    return !!(view.q || selectedBands().length || view.country || view.region);
  }

  // Each filter, and every filter but one: the counts beside a filter's
  // choices show what choosing it would list given the others.
  function passes(player, skip) {
    if (skip !== 'class' && selectedBands().length && !view.classes[player.band]) return false;
    if (skip !== 'country' && view.country && (player.country || NO_COUNTRY) !== view.country) return false;
    if (skip !== 'region' && view.region && regionOf(player) !== view.region) return false;
    if (view.needle && player.folded.text.indexOf(view.needle) < 0) return false;
    return true;
  }

  // Widens code points from..last to whole characters as the reader sees
  // them, so a mark never splits an accent from its letter or a joined emoji.
  function wholeCharacters(chars, from, last) {
    if (!graphemes) return [from, last];
    var start = 0, first = from, end = last;
    for (var it = graphemes.segment(chars.join(''))[Symbol.iterator](), step = it.next(); !step.done; step = it.next()) {
      var length = Array.from(step.value.segment).length;
      if (from >= start && from < start + length) first = start;
      if (last >= start && last < start + length) end = start + length - 1;
      start += length;
    }
    return [first, end];
  }

  function name(player) {
    var node = el('span', 'rank-name');
    var needle = view.needle;
    var at = needle ? player.folded.text.indexOf(needle) : -1;
    if (at < 0) {
      node.textContent = player.name;
      return node;
    }
    var chars = player.folded.chars;
    var span = wholeCharacters(chars, player.folded.starts[at], player.folded.starts[at + needle.length - 1]);
    var from = span[0], last = span[1];
    node.appendChild(document.createTextNode(chars.slice(0, from).join('')));
    node.appendChild(el('mark', '', chars.slice(from, last + 1).join('')));
    node.appendChild(document.createTextNode(chars.slice(last + 1).join('')));
    return node;
  }

  function row(player) {
    var item = el('li', 'rank-row');
    item.appendChild(flag(player.country));
    item.appendChild(name(player));
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
  var SVG = 'http://www.w3.org/2000/svg';
  var SHORT = ['D', 'D+', 'C', 'C+', 'B', 'B+', 'A', 'A+', 'M', 'GM'];
  var CHART_HEIGHT = 170, CHART_TOP = 12, CHART_BOTTOM = 24, CHART_LEFT = 30, BAR_MAX = 24, SMOOTHING = 0.85, OUTLINE_STEPS = 80;
  var chartCounts = null;

  function svg(tag, attributes, parent) {
    var node = document.createElementNS(SVG, tag);
    Object.keys(attributes).forEach(function (name) { node.setAttribute(name, attributes[name]); });
    if (parent) parent.appendChild(node);
    return node;
  }
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
    chartPlot.setAttribute('aria-label', 'Players by class: ' + counts.map(function (count, index) {
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
    var all = el('button', 'rank-chip all', 'All classes');
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

  function fillSelect(select, entries, current, allLabel) {
    while (select.options.length) select.remove(0);
    select.add(new Option(allLabel, ''));
    entries.forEach(function (entry) {
      var option = new Option(entry.label + ' (' + entry.count + ')', entry.value);
      if (!entry.count) option.className = 'empty';
      select.add(option);
    });
    select.value = current;
    if (select.value !== current) select.value = '';
  }

  function countryOptions(players) {
    var counts = {}, present = {};
    players.forEach(function (player) {
      var code = player.country || NO_COUNTRY;
      present[code] = true;
      if (passes(player, 'country')) counts[code] = (counts[code] || 0) + 1;
    });
    if (view.country) present[view.country] = true;
    var entries = Object.keys(present).map(function (code) {
      return { value: code, label: code === NO_COUNTRY ? 'No flag' : countryName(code), count: counts[code] || 0 };
    }).sort(function (a, b) {
      if (a.value === NO_COUNTRY) return 1;
      if (b.value === NO_COUNTRY) return -1;
      return a.label.localeCompare(b.label);
    });
    fillSelect(countrySelect, entries, view.country, 'All countries');
  }

  function regionOptions(players) {
    var counts = {}, present = {};
    players.forEach(function (player) {
      var region = regionOf(player);
      present[region] = true;
      if (passes(player, 'region')) counts[region] = (counts[region] || 0) + 1;
    });
    if (view.region) present[view.region] = true;
    var order = REGIONS.map(function (region) { return region[0]; }).concat([OTHER_REGION]);
    var entries = order.filter(function (code) { return present[code]; }).map(function (code) {
      return { value: code, label: regionNames[code], count: counts[code] || 0 };
    });
    fillSelect(regionSelect, entries, view.region, 'All regions');
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

    if (data.phase === 'off') {
      board.appendChild(message('This service does not offer rankings right now.', 'Rankings come from public room games. In Ember, open Online play, Public rooms to play.'));
      return;
    }
    if (data.phase === 'hidden') {
      board.appendChild(message('Classes are not shown yet.', 'Public room games still count. The board opens here and in Ember once classes are shown.'));
      return;
    }
    if (!players) {
      board.appendChild(message('The board is not available right now.', 'Try again in a minute, or look in Ember under Online play, Rankings.'));
      return;
    }
    if (players.length === 0) {
      summary.textContent = summaryText(players, 0);
      board.appendChild(message('Nobody is on the board yet.', 'Players appear once their class settles after a few public room games, and they have opened Rankings in Ember.'));
      return;
    }

    var counts = classCounts(players);
    classChips(counts);
    classChart(counts);
    countryOptions(players);
    regionOptions(players);
    if (search.value !== view.q) search.value = view.q;
    groupSelect.value = view.group;
    orderSelect.value = view.order;

    var shown = players.filter(function (player) { return passes(player); });
    summary.textContent = summaryText(players, shown.length);
    if (shown.length === 0) {
      board.appendChild(message('No players match these filters.', view.q ? 'Names are matched without case or accents. Players who have not opened Rankings lately may show an older name.' : '', true));
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
    clearTimeout(typingTimer);
    writeHash();
    render();
  }

  function toggleClass(band) {
    if (view.classes[band]) delete view.classes[band];
    else view.classes[band] = true;
    update();
  }

  function clearFilters() {
    setQuery('');
    view.classes = {};
    view.country = '';
    view.region = '';
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
      var country = typeof listing.country === 'string' && COUNTRY.test(listing.country) ? listing.country : '';
      players.push({ name: listing.name, country: country, band: entry.band, index: players.length, folded: fold(listing.name) });
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
    fetch('/rankings.json', { cache: 'no-store', credentials: 'omit' })
      .then(function (response) {
        if (response.status === 404) return { off: true };
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
      })
      .then(function (body) {
        if (body.off) {
          data.phase = 'off';
          data.players = null;
        } else {
          var players = valid(body);
          if (!players) throw new Error('unexpected board');
          data.phase = body.shown ? 'ready' : 'hidden';
          data.players = players;
          data.listedAt = typeof body.listed_at === 'number' ? body.listed_at : Math.floor(Date.now() / 1000);
        }
        data.loadedAt = Date.now();
        data.failed = false;
      })
      .catch(function () {
        data.failed = true;
        if (!data.players) data.phase = 'failed';
      })
      .then(function () {
        loading = false;
        refreshButton.disabled = false;
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
  search.addEventListener('input', function () {
    // The query is kept at once; only drawing the board waits for a pause.
    setQuery(search.value);
    clearTimeout(typingTimer);
    typingTimer = setTimeout(update, 120);
  });
  search.addEventListener('keydown', function (event) {
    if (event.key === 'Escape' && search.value) {
      event.preventDefault();
      search.value = '';
      setQuery('');
      update();
    }
  });
  countrySelect.addEventListener('change', function () { view.country = countrySelect.value; update(); });
  regionSelect.addEventListener('change', function () { view.region = regionSelect.value; update(); });
  groupSelect.addEventListener('change', function () { view.group = groupSelect.value; update(); });
  orderSelect.addEventListener('change', function () { view.order = orderSelect.value; update(); });
  clearButton.addEventListener('click', clearFilters);
  shareButton.addEventListener('click', function () {
    copy('https://embernetplay.link/rankings' + hashOf(), 'Link to this view copied.', 'Copy failed. Copy the address from the address bar.');
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
  function validAtlas(index) {
    var whole = function (value) { return typeof value === 'number' && value >= 0 && value % 1 === 0; };
    return index && whole(index.cellWidth) && index.cellWidth > 0 && whole(index.cellHeight) && whole(index.padding) &&
      whole(index.columns) && index.columns > 0 && whole(index.width) && whole(index.height) &&
      index.countries && typeof index.countries === 'object' ? index : null;
  }
  fetch('/assets/flags.json', { credentials: 'omit' })
    .then(function (response) { return response.ok ? response.json() : null; })
    .then(function (index) {
      flagAtlas = validAtlas(index);
      if (flagAtlas && data.players) render();
    })
    .catch(function () {});
  readHash();
  load(false);
}());
