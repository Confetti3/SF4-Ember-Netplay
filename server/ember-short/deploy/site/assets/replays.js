// The replays page: the replay channel Ember shows under Replays, Replay channel
// (/replays.json, nginx's proxy of the bridge's GET /v1/replays/public), newest
// first, with fighter, class and name filters kept in the URL fragment so a
// view can be shared. Each entry links its file (/replays/ID.emberreplay) to
// download and open in Ember. Names are the players' own words, so they only
// ever go in as text.
(function () {
  'use strict';
  // While the tab is shown and nothing is filtered, the first page is asked for
  // again this often; nginx and the bridge each keep it 15 s.
  var REFRESH_MS = 30000;
  var FACE = 32;
  var PAGE_SIZE = 50;
  var MAX_NAME_FILTER = 32;
  // Native fighter IDs, as src/common/FighterMetadata.inc orders them and
  // assets/faces.webp lays them out.
  var FIGHTERS = ['Ryu', 'Ken', 'Chun-Li', 'E. Honda', 'Blanka', 'Zangief', 'Guile', 'Dhalsim', 'Balrog', 'Vega',
    'Sagat', 'M. Bison', 'C. Viper', 'Rufus', 'El Fuerte', 'Abel', 'Seth', 'Akuma', 'Gouken', 'T. Hawk', 'Cammy',
    'Fei Long', 'Dee Jay', 'Sakura', 'Rose', 'Gen', 'Dan', 'Guy', 'Cody', 'Ibuki', 'Makoto', 'Dudley', 'Adon',
    'Hakan', 'Juri', 'Yun', 'Yang', 'Evil Ryu', 'Oni', 'Rolento', 'Elena', 'Poison', 'Hugo', 'Decapre'];
  // USF4's classes, band 1 to 10 (src/ui/NameMarks.cxx), and the colour
  // family each pill takes; the same as the rankings page.
  var CLASSES = ['D', 'D+', 'C', 'C+', 'B', 'B+', 'A', 'A+', 'Master', 'Grand Master'];
  var TIERS = ['d', 'd', 'c', 'c', 'b', 'b', 'a', 'a', 'm', 'gm'];
  var BANDS = CLASSES.length;
  var ID = /^[0-9a-f]{32}$/;
  var COUNTRY = /^[A-Z]{2}$/;
  // The flags in assets/flags.png, laid out as assets/flags.json says, drawn
  // FLAG_WIDTH wide. Until the index loads, or if it cannot, a country shows
  // its code.
  var FLAG_WIDTH = 20;
  var flagAtlas = null;

  var classesBox = document.getElementById('classes');
  var search = document.getElementById('search');
  var fighterSelect = document.getElementById('fighter');
  var bar = document.getElementById('bar');
  var summary = document.getElementById('summary');
  var clearButton = document.getElementById('clear');
  var refreshButton = document.getElementById('refresh');
  var board = document.getElementById('board');
  var moreBox = document.getElementById('more-box');
  var moreButton = document.getElementById('more');
  var status = document.getElementById('status');

  // replays: null until the first answer; each entry as valid() makes it.
  // phase: 'loading', 'ready', 'off' (no replay channel here) or 'failed'.
  // more: whether older entries follow the last one; cursor: the id to ask
  // after. extra: whether pages beyond the first are in the list.
  var data = { replays: null, more: false, cursor: '', extra: false, phase: 'loading', failed: false, loadedAt: 0, key: '' };
  var view = { q: '', fighter: '', band: 0 };
  // Each request carries a number; only the newest filter change's answer is used.
  var requestNumber = 0;
  var firstLoading = false;
  var moreLoading = false;
  var timer = null;
  var typingTimer = null;

  var graphemes = null;
  try { graphemes = new Intl.Segmenter('en', { granularity: 'grapheme' }); } catch (error) { graphemes = null; }
  var displayNames = null;
  try { displayNames = new Intl.DisplayNames(['en'], { type: 'region' }); } catch (error) { displayNames = null; }
  var countryNames = {};
  function countryName(code) {
    if (!countryNames[code]) {
      var name = null;
      try { name = displayNames ? displayNames.of(code) : null; } catch (error) { name = null; }
      countryNames[code] = name && name !== code ? name : code;
    }
    return countryNames[code];
  }
  var dateFormat = null;
  try { dateFormat = new Intl.DateTimeFormat(undefined, { dateStyle: 'medium', timeStyle: 'short' }); } catch (error) { dateFormat = null; }

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function plural(count, one, many) { return count + ' ' + (count === 1 ? one : many); }
  function fighterName(id) { return id >= 0 && id < FIGHTERS.length ? FIGHTERS[id] : 'Unknown fighter'; }

  // A query as kept: at most 32 characters (what the bridge accepts), never
  // half of one, without control characters or edge spaces.
  function setQuery(text) {
    var clipped = Array.from(String(text)).filter(function (ch) {
      var code = ch.codePointAt(0);
      return code >= 0x20 && code !== 0x7f && !(code >= 0x80 && code < 0xa0);
    }).slice(0, MAX_NAME_FILTER).join('');
    view.q = clipped.toWellFormed ? clipped.toWellFormed() : clipped;
  }
  function query() { return view.q.trim(); }

  function pill(badge) {
    if (!badge) return null;
    var node;
    if (badge.developer) {
      node = el('span', 'rank-class tier-dev', 'Dev');
      node.title = 'Ember developer';
      return node;
    }
    if (badge.band < 1) return null;
    node = el('span', 'rank-class tier-' + TIERS[badge.band - 1], CLASSES[badge.band - 1]);
    node.title = 'Class ' + CLASSES[badge.band - 1];
    return node;
  }
  // A country's flag from the atlas, or its code where the atlas has none.
  function flag(code) {
    if (!code) return null;
    var atlas = flagAtlas;
    var cell = atlas && Object.prototype.hasOwnProperty.call(atlas.countries, code) ? atlas.countries[code] : -1;
    var node = el('span', 'rank-flag' + (cell >= 0 ? ' image' : ''), cell >= 0 ? '' : code);
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
    node.title = countryName(code);
    node.setAttribute('aria-label', countryName(code));
    return node;
  }
  function face(id) {
    var tile = el('span', 'room-face');
    if (id >= 0 && id < FIGHTERS.length) {
      tile.style.backgroundPosition = (-id * FACE) + 'px 0';
      tile.setAttribute('role', 'img');
      tile.setAttribute('aria-label', FIGHTERS[id]);
    } else {
      tile.className += ' none';
      tile.textContent = '?';
      tile.setAttribute('aria-hidden', 'true');
    }
    tile.title = fighterName(id);
    return tile;
  }

  function when(seconds) {
    var date = new Date(seconds * 1000);
    if (isNaN(date.getTime())) return { text: '', iso: '' };
    var text;
    try { text = dateFormat ? dateFormat.format(date) : date.toLocaleString(); } catch (error) { text = date.toLocaleString(); }
    return { text: text, iso: date.toISOString() };
  }

  function side(entry, index) {
    var won = entry.winner === index;
    var node = el('div', 'replay-side' + (won ? ' won' : ''));
    var player = entry.sides[index];
    node.appendChild(face(player.fighter));
    var who = el('span', 'replay-who');
    var line = el('span', 'replay-player');
    // An empty slot where a player shows no flag, so every name starts in line.
    line.appendChild(flag(player.country) || el('span', 'rank-flag none'));
    var name = el('span', 'rank-name', player.name);
    name.title = player.name;
    line.appendChild(name);
    var badge = pill(player.badge);
    if (badge) line.appendChild(badge);
    who.appendChild(line);
    var under = el('span', 'replay-under');
    under.appendChild(el('span', 'replay-fighter', fighterName(player.fighter)));
    if (won) under.appendChild(el('span', 'replay-win', 'Winner'));
    who.appendChild(under);
    node.appendChild(who);
    return node;
  }

  function row(entry) {
    var item = el('li', 'replay-row');
    item.appendChild(side(entry, 0));
    item.appendChild(el('span', 'replay-vs', 'vs'));
    item.appendChild(side(entry, 1));

    var end = el('div', 'replay-end');
    var meta = el('div', 'replay-meta');
    var time = when(entry.played);
    var stamp = el('time', 'replay-date', time.text);
    if (time.iso) stamp.setAttribute('datetime', time.iso);
    meta.appendChild(stamp);
    meta.appendChild(el('span', 'room-chip', entry.room === 'private' ? 'Private room' : 'Public room'));
    end.appendChild(meta);
    var link = el('a', 'room-action primary', 'Download');
    link.href = '/replays/' + entry.id + '.emberreplay';
    link.setAttribute('download', '');
    link.rel = 'nofollow';
    var size = entry.size > 0 ? ' (' + Math.max(1, Math.round(entry.size / 1024)) + ' KB)' : '';
    link.title = 'Download this replay' + size;
    link.setAttribute('aria-label', 'Download replay: ' + entry.sides[0].name + ' versus ' + entry.sides[1].name);
    end.appendChild(link);
    item.appendChild(end);
    return item;
  }

  // The view in the URL fragment, which browsers never send: q, fighter (a
  // native ID) and class (a band).
  function readHash() {
    var params;
    try { params = new URLSearchParams(location.hash.replace(/^#/, '')); } catch (error) { return; }
    setQuery(params.get('q') || '');
    var fighter = params.get('fighter');
    var id = fighter !== null && /^[0-9]{1,2}$/.test(fighter) ? parseInt(fighter, 10) : -1;
    view.fighter = id >= 0 && id < FIGHTERS.length ? String(id) : '';
    var band = parseInt(params.get('class'), 10);
    view.band = band >= 1 && band <= BANDS ? band : 0;
  }
  function hashOf() {
    var parts = [];
    if (view.q) parts.push('q=' + encodeURIComponent(view.q));
    if (view.fighter !== '') parts.push('fighter=' + view.fighter);
    if (view.band) parts.push('class=' + view.band);
    return parts.length ? '#' + parts.join('&') : '';
  }
  function writeHash() {
    var hash = hashOf();
    if (hash === location.hash || (!hash && !location.hash)) return;
    try { history.replaceState(null, '', location.pathname + location.search + hash); } catch (error) { /* file: pages */ }
  }
  function filtered() { return !!(query() || view.fighter !== '' || view.band); }

  // The request for a page: the filters, and the cursor for an older page.
  function address(before) {
    var params = [];
    if (view.fighter !== '') params.push('fighter=' + view.fighter);
    if (view.band) params.push('band=' + view.band);
    if (query()) params.push('name=' + encodeURIComponent(query()));
    if (before) params.push('before=' + before);
    return '/replays.json' + (params.length ? '?' + params.join('&') : '');
  }

  function valid(body) {
    if (!body || typeof body !== 'object' || !Array.isArray(body.replays)) return null;
    if (body.more !== undefined && typeof body.more !== 'boolean') return null;
    var whole = function (value, max) { return typeof value === 'number' && value % 1 === 0 && value >= 0 && value <= max; };
    var replays = [];
    var cursor = '';
    body.replays.forEach(function (raw) {
      if (!raw || typeof raw.id !== 'string' || !ID.test(raw.id)) return;
      cursor = raw.id;
      if (!whole(raw.played_at, 8.64e12 / 1000) || (raw.room !== 'public' && raw.room !== 'private')) return;
      if (!Array.isArray(raw.sides) || raw.sides.length !== 2) return;
      var sides = [];
      for (var i = 0; i < 2; i++) {
        var one = raw.sides[i];
        if (!one || typeof one.name !== 'string' || !one.name || one.name.length > 256 || !whole(one.fighter, 63)) return;
        var badge = null;
        if (one.badge !== undefined && one.badge !== null) {
          if (typeof one.badge !== 'object' || !whole(one.badge.band, BANDS)) return;
          badge = { band: one.badge.band, developer: one.badge.developer === true };
        }
        sides.push({
          name: one.name,
          country: typeof one.country === 'string' && COUNTRY.test(one.country) ? one.country : '',
          fighter: one.fighter,
          badge: badge
        });
      }
      var winner = raw.winner === 0 || raw.winner === 1 ? raw.winner : -1;
      replays.push({
        id: raw.id,
        played: raw.played_at,
        room: raw.room,
        sides: sides,
        winner: winner,
        size: whole(raw.size, 1e9) ? raw.size : 0
      });
    });
    return { replays: replays, more: body.more === true && cursor !== '', cursor: cursor };
  }

  // The list as text, so a refresh can tell whether anything changed.
  function keyOf(phase, replays, more) {
    return phase + (more ? '+' : '') + '\n' + (replays || []).map(function (entry) {
      return entry.id + ' ' + entry.sides.map(function (one) {
        return one.name + '|' + one.country + '|' + one.fighter + '|' + (one.badge ? (one.badge.developer ? 'D' : one.badge.band) : '');
      }).join(' ') + ' ' + entry.winner;
    }).join('\n');
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

  function summaryText() {
    var replays = data.replays || [];
    var stale = data.failed ? ' Could not refresh; showing the last list.' : '';
    if (!replays.length) return '';
    var text = plural(replays.length, 'replay', 'replays') + (filtered() ? ' match.' : ' shown.');
    return text + (data.more ? ' Older ones follow.' : '') + stale;
  }

  function classChips() {
    var active = document.activeElement;
    var focused = active && classesBox.contains(active) ? active.getAttribute('data-band') : null;
    while (classesBox.firstChild) classesBox.removeChild(classesBox.firstChild);
    var all = el('button', 'rank-chip all', 'All classes');
    all.type = 'button';
    all.setAttribute('data-band', '');
    all.setAttribute('aria-pressed', view.band ? 'false' : 'true');
    classesBox.appendChild(all);
    for (var b = BANDS; b >= 1; b--) {
      var chip = el('button', 'rank-chip tier-' + TIERS[b - 1]);
      chip.type = 'button';
      chip.setAttribute('data-band', String(b));
      chip.setAttribute('aria-pressed', view.band === b ? 'true' : 'false');
      chip.setAttribute('aria-label', 'Class ' + CLASSES[b - 1]);
      chip.appendChild(el('span', 'rank-chip-name', CLASSES[b - 1]));
      classesBox.appendChild(chip);
    }
    if (focused !== null) {
      var again = classesBox.querySelector('[data-band="' + focused + '"]');
      if (again) again.focus();
    }
  }

  function render() {
    board.setAttribute('aria-busy', String(data.phase === 'loading'));
    while (board.firstChild) board.removeChild(board.firstChild);
    var replays = data.replays;
    var ready = data.phase === 'ready' && replays;
    classChips();
    if (fighterSelect.value !== view.fighter) fighterSelect.value = view.fighter;
    if (search.value !== view.q) search.value = view.q;
    // Refresh stays once there is an answer, whatever it said.
    bar.hidden = data.phase === 'loading';
    clearButton.hidden = !filtered();
    moreBox.hidden = !(ready && data.more);
    moreButton.disabled = moreLoading;
    summary.textContent = ready ? summaryText() : '';

    if (data.phase === 'loading') {
      board.appendChild(el('p', 'room-summary', 'Loading replays...'));
      var skeleton = el('div', 'rank-skeleton');
      skeleton.setAttribute('aria-hidden', 'true');
      for (var i = 0; i < 6; i++) skeleton.appendChild(el('span'));
      board.appendChild(skeleton);
      return;
    }
    if (data.phase === 'off') {
      board.appendChild(message('This service does not offer the replay channel right now.', 'Replays from rooms are listed here once it does.'));
      return;
    }
    if (!replays) {
      board.appendChild(message('The replays are not available right now.', 'Try again in a minute, or look in Ember under Replays, Replay channel.'));
      return;
    }
    if (replays.length === 0) {
      board.appendChild(filtered() ?
        message('No replays match these filters.', 'Names are matched without case, anywhere in either player\'s name.', true) :
        message('No replays have been shared yet.', 'Games from Ember rooms appear here once their players have Discord connected and are sharing.'));
      return;
    }
    var list = el('ul', 'replay-list');
    var fragment = document.createDocumentFragment();
    replays.forEach(function (entry) { fragment.appendChild(row(entry)); });
    list.appendChild(fragment);
    board.appendChild(list);
  }

  function schedule(delay) {
    clearTimeout(timer);
    timer = document.hidden || filtered() ? null : setTimeout(function () { load(true); }, delay);
  }

  // A fresh first page for the current filters. quiet: a timed refresh, which
  // draws the list again only when it changed, so the page does not jump under
  // someone reading it. Entries a refresh finds above the list are put on top
  // of it; the pages already loaded below stay.
  function load(quiet) {
    if (quiet && (firstLoading || moreLoading)) return;
    var number = ++requestNumber;
    firstLoading = true;
    moreLoading = false;
    refreshButton.disabled = true;
    fetch(address(''), { cache: 'no-store', credentials: 'omit' })
      .then(function (response) {
        if (response.status === 404) return { off: true };
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
      })
      .then(function (body) {
        if (number !== requestNumber) return null;
        if (body.off) return { off: true };
        var page = valid(body);
        if (!page) throw new Error('unexpected list');
        return page;
      })
      .catch(function () { return number === requestNumber ? { failed: true } : null; })
      .then(function (result) {
        if (number !== requestNumber || !result) return;
        firstLoading = false;
        refreshButton.disabled = false;
        var before = keyOf(data.phase, data.replays, data.more);
        if (result.failed) {
          data.failed = true;
          if (!data.replays) data.phase = 'failed';
        } else if (result.off) {
          data.phase = 'off';
          data.replays = null;
          data.failed = false;
        } else {
          data.phase = 'ready';
          data.failed = false;
          data.loadedAt = Date.now();
          var known = data.replays && data.extra && quiet ? data.replays : null;
          if (known) {
            // An entry the first page no longer has in its span was hidden
            // (its players stopped sharing): it goes from the pages below too.
            var listed = {};
            result.replays.forEach(function (entry) { listed[entry.id] = true; });
            var oldest = result.replays.length ? result.replays[result.replays.length - 1].played_at : Infinity;
            known = known.filter(function (entry) { return listed[entry.id] || entry.played_at < oldest; });
          }
          var at = known && known.length ? result.replays.findIndex(function (entry) { return entry.id === known[0].id; }) : -1;
          if (at >= 0) {
            // New entries on top of the pages already loaded.
            data.replays = result.replays.slice(0, at).concat(known);
          } else {
            data.replays = result.replays;
            data.more = result.more;
            data.cursor = result.cursor;
            data.extra = false;
          }
        }
        var after = keyOf(data.phase, data.replays, data.more);
        if (!quiet || before !== after) {
          render();
        } else {
          var text = data.phase === 'ready' && data.replays ? summaryText() : '';
          if (summary.textContent !== text) summary.textContent = text;
        }
        schedule(REFRESH_MS);
      });
  }

  function loadMore() {
    if (!data.more || !data.cursor || moreLoading || firstLoading) return;
    var number = requestNumber;
    moreLoading = true;
    moreButton.disabled = true;
    status.textContent = '';
    fetch(address(data.cursor), { cache: 'no-store', credentials: 'omit' })
      .then(function (response) {
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
      })
      .then(function (body) {
        var page = valid(body);
        if (!page) throw new Error('unexpected list');
        return page;
      })
      .catch(function () { return null; })
      .then(function (page) {
        // A filter change in the meantime started over; this page is not for it.
        if (number !== requestNumber) return;
        moreLoading = false;
        if (!page) {
          status.textContent = 'Could not load older replays. Try again.';
          moreButton.disabled = false;
          schedule(REFRESH_MS);
          return;
        }
        var seen = {};
        data.replays.forEach(function (entry) { seen[entry.id] = true; });
        var added = page.replays.filter(function (entry) { return !seen[entry.id]; });
        data.replays = data.replays.concat(added);
        data.more = page.more;
        data.cursor = page.cursor || data.cursor;
        data.extra = true;
        status.textContent = 'Loaded ' + plural(added.length, 'older replay', 'older replays') + '.';
        render();
        schedule(Math.max(0, REFRESH_MS - (Date.now() - data.loadedAt)));
      });
  }

  // A change to the filters: a new first page, kept in the address.
  function update() {
    clearTimeout(typingTimer);
    writeHash();
    status.textContent = '';
    data.phase = 'loading';
    data.replays = null;
    data.more = false;
    data.cursor = '';
    data.extra = false;
    data.failed = false;
    load(false);
    render();
  }

  function clearFilters() {
    setQuery('');
    view.fighter = '';
    view.band = 0;
    update();
    search.focus();
  }

  classesBox.addEventListener('click', function (event) {
    var button = event.target.closest ? event.target.closest('button') : null;
    if (!button || !classesBox.contains(button)) return;
    var band = parseInt(button.getAttribute('data-band'), 10);
    view.band = band && band !== view.band ? band : 0;
    update();
  });
  search.addEventListener('input', function () {
    // The query is kept at once; only asking waits for a pause.
    setQuery(search.value);
    clearTimeout(typingTimer);
    typingTimer = setTimeout(update, 400);
  });
  search.addEventListener('keydown', function (event) {
    if (event.key === 'Escape' && search.value) {
      event.preventDefault();
      search.value = '';
      setQuery('');
      update();
    }
  });
  fighterSelect.addEventListener('change', function () { view.fighter = fighterSelect.value; update(); });
  clearButton.addEventListener('click', clearFilters);
  refreshButton.addEventListener('click', function () { status.textContent = ''; load(false); });
  moreButton.addEventListener('click', loadMore);
  document.addEventListener('keydown', function (event) {
    if (event.key !== '/' || event.ctrlKey || event.metaKey || event.altKey) return;
    var target = event.target;
    if (target && (target.tagName === 'INPUT' || target.tagName === 'SELECT' || target.tagName === 'TEXTAREA' || target.isContentEditable)) return;
    event.preventDefault();
    search.focus();
    search.select();
  });
  window.addEventListener('hashchange', function () {
    if (location.hash === hashOf()) return;
    readHash();
    update();
  });
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      clearTimeout(timer);
      timer = null;
    } else {
      schedule(Math.max(0, REFRESH_MS - (Date.now() - data.loadedAt)));
    }
  });

  // The fighter filter lists every fighter by name; the value is the native ID.
  FIGHTERS.map(function (name, id) { return { name: name, id: id }; })
    .sort(function (a, b) { return a.name.localeCompare(b.name, undefined, { sensitivity: 'base', numeric: true }); })
    .forEach(function (fighter) { fighterSelect.add(new Option(fighter.name, String(fighter.id))); });

  // The flag index, read once beside the list; the list is drawn again with
  // flags when it arrives.
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
      if (flagAtlas && data.replays) render();
    })
    .catch(function () {});
  readHash();
  render();
  load(false);
}());
