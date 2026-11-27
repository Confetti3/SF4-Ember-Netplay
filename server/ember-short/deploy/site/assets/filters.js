// needs: site marks
// The filters the boards share (rankings.js, points.js): finding a player by
// name without case or accents, with the found part marked, and the country
// and region choices with what choosing each would list. controller() is the
// three controls with their state, kept in the URL fragment by the page as
// q=, country= and region=. A board's players are objects with `name`,
// `country` ('' for no flag) and `folded` (fold(name)).
(function () {
  'use strict';
  var el = window.EmberSite.el;
  var marks = window.EmberMarks;

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
  var OTHER_REGION = 'other';
  // The country filter's value for players without a flag.
  var NO_COUNTRY = 'none';
  // The longest query kept, in characters.
  var MAX_QUERY = 64;
  var REGION_OF = {};
  var regionNames = {};
  REGIONS.forEach(function (region) {
    regionNames[region[0]] = region[1];
    region[2].split(' ').forEach(function (code) { REGION_OF[code] = region[0]; });
  });
  regionNames[OTHER_REGION] = 'Other';

  function regionOf(player) { return player.country ? REGION_OF[player.country] || OTHER_REGION : OTHER_REGION; }
  function isRegion(code) { return Object.prototype.hasOwnProperty.call(regionNames, code); }
  // A country filter as an address writes it: a code, "none", or '' for all.
  function readCountry(text) {
    var code = String(text || '').toUpperCase();
    return marks.country(code) || (code === 'NONE' ? NO_COUNTRY : '');
  }

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

  // A query as kept: at most MAX_QUERY characters, never half of one, and
  // folded once for matching.
  function query(text) {
    var clipped = Array.from(String(text)).slice(0, MAX_QUERY).join('');
    var kept = clipped.toWellFormed ? clipped.toWellFormed() : clipped;
    return { text: kept, needle: fold(kept).text.trim() };
  }
  function matches(player, needle) { return !needle || player.folded.text.indexOf(needle) >= 0; }

  var graphemes = null;
  try { graphemes = new Intl.Segmenter('en', { granularity: 'grapheme' }); } catch (error) { graphemes = null; }
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

  // A player's name as a node of `tag`, the part the needle found marked.
  function name(tag, className, player, needle) {
    var node = el(tag, className);
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

  // The country choices: each country on the board, by name, players without
  // a flag last, each counting the players `passes` lets through (every
  // filter but the country's).
  function fillCountries(select, players, current, passes) {
    var counts = {}, present = {};
    players.forEach(function (player) {
      var code = player.country || NO_COUNTRY;
      present[code] = true;
      if (passes(player)) counts[code] = (counts[code] || 0) + 1;
    });
    if (current) present[current] = true;
    var entries = Object.keys(present).map(function (code) {
      return { value: code, label: code === NO_COUNTRY ? 'No flag' : marks.countryName(code), count: counts[code] || 0 };
    }).sort(function (a, b) {
      if (a.value === NO_COUNTRY) return 1;
      if (b.value === NO_COUNTRY) return -1;
      return a.label.localeCompare(b.label);
    });
    fillSelect(select, entries, current, 'All countries');
  }

  // The region choices, in REGIONS order, as fillCountries counts them.
  function fillRegions(select, players, current, passes) {
    var counts = {}, present = {};
    players.forEach(function (player) {
      var region = regionOf(player);
      present[region] = true;
      if (passes(player)) counts[region] = (counts[region] || 0) + 1;
    });
    if (current) present[current] = true;
    var order = REGIONS.map(function (region) { return region[0]; }).concat([OTHER_REGION]);
    var entries = order.filter(function (code) { return present[code]; }).map(function (code) {
      return { value: code, label: regionNames[code], count: counts[code] || 0 };
    });
    fillSelect(select, entries, current, 'All regions');
  }

  // Whether a player is in the chosen country ('' for all) and region.
  function inCountry(player, chosen) { return !chosen || (player.country || NO_COUNTRY) === chosen; }
  function inRegion(player, chosen) { return !chosen || regionOf(player) === chosen; }

  // The find box and the country and region choices of a board (controls:
  // { search, country, region }), calling onChange after each change; typing
  // waits for a pause first. state is { q, needle, country, region }. The page
  // keeps the fragment (read, parts) and draws (fill, passes, name), and calls
  // settle when it draws, so a pause still pending does not draw again.
  function controller(controls, onChange) {
    var state = { q: '', needle: '', country: '', region: '' };
    var typing = null;
    function setQuery(text) {
      var kept = query(text);
      state.q = kept.text;
      state.needle = kept.needle;
    }
    controls.search.addEventListener('input', function () {
      // The query is kept at once; only drawing the board waits for a pause.
      setQuery(controls.search.value);
      clearTimeout(typing);
      typing = setTimeout(onChange, 120);
    });
    controls.search.addEventListener('keydown', function (event) {
      if (event.key === 'Escape' && controls.search.value) {
        event.preventDefault();
        controls.search.value = '';
        setQuery('');
        onChange();
      }
    });
    controls.country.addEventListener('change', function () { state.country = controls.country.value; onChange(); });
    controls.region.addEventListener('change', function () { state.region = controls.region.value; onChange(); });
    return {
      state: state,
      // The filters an address's fragment names (URLSearchParams).
      read: function (params) {
        setQuery(params.get('q') || '');
        state.country = readCountry(params.get('country'));
        var region = params.get('region') || '';
        state.region = isRegion(region) ? region : '';
      },
      // Each filter as the fragment writes it, '' for one not set.
      parts: function () {
        return {
          q: state.q ? 'q=' + encodeURIComponent(state.q) : '',
          country: state.country ? 'country=' + state.country : '',
          region: state.region ? 'region=' + state.region : ''
        };
      },
      any: function () { return !!(state.q || state.country || state.region); },
      // Whether a player passes these filters, all or all but `skip` ('country' or 'region').
      passes: function (player, skip) {
        if (skip !== 'country' && !inCountry(player, state.country)) return false;
        if (skip !== 'region' && !inRegion(player, state.region)) return false;
        return matches(player, state.needle);
      },
      // The choices for these players, each counting those `passesFor`
      // (the page's whole filter, given what to skip) lets through.
      fill: function (players, passesFor) {
        fillCountries(controls.country, players, state.country, function (player) { return passesFor(player, 'country'); });
        fillRegions(controls.region, players, state.region, function (player) { return passesFor(player, 'region'); });
        if (controls.search.value !== state.q) controls.search.value = state.q;
      },
      name: function (tag, className, player) { return name(tag, className, player, state.needle); },
      clear: function () {
        setQuery('');
        state.country = state.region = '';
      },
      settle: function () { clearTimeout(typing); }
    };
  }

  window.EmberFilters = { NO_COUNTRY: NO_COUNTRY, fold: fold, controller: controller };
}());
