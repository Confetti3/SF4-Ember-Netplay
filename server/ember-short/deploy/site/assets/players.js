// What the rooms, rankings and replays pages share about players: the
// fighters and their portraits, country names and flags, and the class pills.
// Loaded before each page's own script (both deferred, so in this order) and
// sets one global, EmberPlayers. Names are the players' own words, so they
// only ever go in as text.
(function () {
  'use strict';
  var FACE = 32;
  // Native fighter IDs, as src/common/FighterMetadata.inc orders them and
  // assets/faces.webp lays them out.
  var FIGHTERS = ['Ryu', 'Ken', 'Chun-Li', 'E. Honda', 'Blanka', 'Zangief', 'Guile', 'Dhalsim', 'Balrog', 'Vega',
    'Sagat', 'M. Bison', 'C. Viper', 'Rufus', 'El Fuerte', 'Abel', 'Seth', 'Akuma', 'Gouken', 'T. Hawk', 'Cammy',
    'Fei Long', 'Dee Jay', 'Sakura', 'Rose', 'Gen', 'Dan', 'Guy', 'Cody', 'Ibuki', 'Makoto', 'Dudley', 'Adon',
    'Hakan', 'Juri', 'Yun', 'Yang', 'Evil Ryu', 'Oni', 'Rolento', 'Elena', 'Poison', 'Hugo', 'Decapre'];
  // USF4's classes, band 1 to 10 (src/ui/NameMarks.cxx), and the colour
  // family each pill takes.
  var CLASSES = ['D', 'D+', 'C', 'C+', 'B', 'B+', 'A', 'A+', 'Master', 'Grand Master'];
  var TIERS = ['d', 'd', 'c', 'c', 'b', 'b', 'a', 'a', 'm', 'gm'];
  // The flags in assets/flags.png, laid out as assets/flags.json says (both
  // copies of the repository's assets/flags, flag-icons under MIT, which
  // scripts/check-flag-atlas.py keeps equal). Drawn FLAG_WIDTH wide. Until the
  // index loads, or if it cannot, a country shows its code.
  var FLAG_WIDTH = 20;
  var flagAtlas = null;

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function fighterName(id) { return id >= 0 && id < FIGHTERS.length ? FIGHTERS[id] : 'Unknown fighter'; }

  // A fighter's portrait. For an ID that is no fighter it is a "?" tile, which
  // takes missing.title as its title and is hidden from screen readers when
  // missing.hidden is set.
  function face(id, missing) {
    var tile = el('span', 'room-face');
    if (typeof id === 'number' && id >= 0 && id < FIGHTERS.length) {
      tile.style.backgroundPosition = (-id * FACE) + 'px 0';
      tile.title = FIGHTERS[id];
      tile.setAttribute('role', 'img');
      tile.setAttribute('aria-label', FIGHTERS[id]);
    } else {
      tile.className += ' none';
      tile.textContent = '?';
      tile.title = missing.title;
      if (missing.hidden) tile.setAttribute('aria-hidden', 'true');
    }
    return tile;
  }

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

  // A country's flag from the atlas, or its code where the atlas has none; an
  // empty code gives an empty, hidden slot.
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

  function validAtlas(index) {
    var whole = function (value) { return typeof value === 'number' && value >= 0 && value % 1 === 0; };
    return index && whole(index.cellWidth) && index.cellWidth > 0 && whole(index.cellHeight) && whole(index.padding) &&
      whole(index.columns) && index.columns > 0 && whole(index.width) && whole(index.height) &&
      index.countries && typeof index.countries === 'object' ? index : null;
  }
  // Reads the flag index once, beside a page's own list. When it arrives and is
  // usable, loaded() is called so the page can draw its list again with flags.
  function loadFlags(loaded) {
    fetch('/assets/flags.json', { credentials: 'omit' })
      .then(function (response) { return response.ok ? response.json() : null; })
      .then(function (index) {
        flagAtlas = validAtlas(index);
        if (flagAtlas) loaded();
      })
      .catch(function () {});
  }

  // A class pill for a band, 1 to 10.
  function classPill(band) {
    var node = el('span', 'rank-class tier-' + TIERS[band - 1], CLASSES[band - 1]);
    node.title = 'Class ' + CLASSES[band - 1];
    return node;
  }
  // The pill for a replay side's badge ({ band, developer }): the developer
  // mark, the class, or nothing (null) where there is no class yet.
  function badgePill(badge) {
    if (!badge) return null;
    if (badge.developer) {
      var node = el('span', 'rank-class tier-dev', 'Dev');
      node.title = 'Ember developer';
      return node;
    }
    return badge.band < 1 ? null : classPill(badge.band);
  }

  window.EmberPlayers = {
    fighters: FIGHTERS,
    classes: CLASSES,
    tiers: TIERS,
    fighterName: fighterName,
    face: face,
    countryName: countryName,
    flag: flag,
    loadFlags: loadFlags,
    classPill: classPill,
    badgePill: badgePill
  };
}());
