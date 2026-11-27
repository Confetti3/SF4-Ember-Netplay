// needs: site
// The marks a player is drawn with wherever the site shows players: fighter
// names and faces, the class pill, the frame of a points count and the flag.
// One copy of each table, which the page scripts read as window.EmberMarks.
// Every page that runs scripts loads this after site.js (scripts/site/shell.py),
// so the header's account link can draw a face too. Names are the players' own
// words, so they only ever go in as text.
(function () {
  'use strict';
  var el = window.EmberSite.el;

  // Native fighter IDs, as src/common/FighterMetadata.inc orders them and
  // assets/faces.css places their faces (class f0 to f43).
  var FIGHTERS = ['Ryu', 'Ken', 'Chun-Li', 'E. Honda', 'Blanka', 'Zangief', 'Guile', 'Dhalsim', 'Balrog', 'Vega',
    'Sagat', 'M. Bison', 'C. Viper', 'Rufus', 'El Fuerte', 'Abel', 'Seth', 'Akuma', 'Gouken', 'T. Hawk', 'Cammy',
    'Fei Long', 'Dee Jay', 'Sakura', 'Rose', 'Gen', 'Dan', 'Guy', 'Cody', 'Ibuki', 'Makoto', 'Dudley', 'Adon',
    'Hakan', 'Juri', 'Yun', 'Yang', 'Evil Ryu', 'Oni', 'Rolento', 'Elena', 'Poison', 'Hugo', 'Decapre'];
  // The fighters in the game's character select order (src/common/FighterCatalog.hxx,
  // RosterDisplayOrder), so a picker reads as the one in the game.
  var ROSTER = [43, 0, 1, 3, 29, 30, 31, 16, 18, 17, 25, 26, 23, 38, 40, 35, 34, 2, 7, 15, 12, 11, 10, 20, 22,
    28, 27, 33, 39, 41, 37, 6, 4, 5, 13, 14, 9, 8, 21, 19, 32, 24, 36, 42];
  // USF4's classes, band 1 to 10 (src/ui/NameMarks.cxx): the name, the chart's
  // short label and the colour of the pill (site.css, tier-*).
  var CLASSES = ['D', 'D+', 'C', 'C+', 'B', 'B+', 'A', 'A+', 'Master', 'Grand Master'];
  var SHORT = ['D', 'D+', 'C', 'C+', 'B', 'B+', 'A', 'A+', 'M', 'GM'];
  var TIERS = ['d', 'd', 'c', 'c', 'b', 'b', 'a', 'a', 'm', 'gm'];
  // The frame of a points count, tier 0 to 4 (docs/design/FIGHTER_LEVELS.md).
  // A tier colours a frame or a dot (site.css, frame-0 to frame-4), never the number.
  var FRAMES = ['Ash', 'Copper', 'Silver', 'Gold', 'Ember'];

  // A card token as an answer carries one (ember_protocol::cards::is_card_token).
  var TOKEN = /^c1\.[A-Za-z0-9_-]{1,96}$/;
  // The address of a player's card here: #card=<token> or #name=<name>.
  function cardLink(selector) { return location.origin + '/players#' + selector; }

  function isFighter(id) { return Number.isInteger(id) && id >= 0 && id < FIGHTERS.length; }
  function isBand(band) { return Number.isInteger(band) && band >= 1 && band <= CLASSES.length; }
  function isTier(tier) { return Number.isInteger(tier) && tier >= 0 && tier < FRAMES.length; }

  // A fighter's face; size is a class of site.css or card.css (small, large,
  // tiny, row, board, hero), none for the 48 px face. With a tier the face
  // wears that frame.
  function face(id, size, tier) {
    var node = el('span', 'face f' + id + (size ? ' ' + size : '') + (tier === undefined ? '' : ' framed frame-' + tier));
    node.setAttribute('aria-hidden', 'true');
    return node;
  }

  // Points as the game writes them: "1986 FP", with no separators.
  function points(count, unit) { return count + ' ' + unit; }

  function frameDot(tier) {
    var dot = el('span', 'frame-dot frame-' + tier);
    dot.setAttribute('aria-hidden', 'true');
    return dot;
  }
  // A frame's dot and name, as a table cell or a line says it.
  function frame(tier, words) {
    var node = el('span', 'frame-name');
    node.appendChild(frameDot(tier));
    node.appendChild(document.createTextNode(FRAMES[tier] + (words || '')));
    return node;
  }

  // A class pill for band 1 to 10, faint while the rating is not sure of it.
  function classPill(band, settled) {
    var name = CLASSES[band - 1];
    var node = el('span', 'rank-class tier-' + TIERS[band - 1] + (settled ? '' : ' faint'), name);
    node.title = 'Class ' + name + (settled ? '' : ', not settled yet');
    return node;
  }
  // A SkillBadge as a pill: the developer mark, placing (band 0) or the class.
  function badge(skill) {
    if (skill.developer) {
      var dev = el('span', 'rank-class dev', 'Dev');
      dev.title = 'Ember developer';
      return dev;
    }
    if (skill.band === 0) {
      var placing = el('span', 'rank-class placing', '?');
      placing.title = 'Placing: no class yet';
      placing.setAttribute('aria-label', 'Placing, no class yet');
      return placing;
    }
    return classPill(skill.band, skill.settled);
  }
  // Whether a SkillBadge from an answer is one this page can draw.
  function isBadge(skill) {
    return !!skill && typeof skill === 'object' && typeof skill.settled === 'boolean' &&
      (skill.developer === true || skill.band === 0 || isBand(skill.band));
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

  // The flags in assets/flags.png, laid out as assets/flags.json says (both
  // copies of the repository's assets/flags, flag-icons under MIT, which
  // scripts/check-flag-atlas.py keeps equal). Drawn FLAG_WIDTH wide. Until the
  // index loads, or if it cannot, a country shows its code.
  var FLAG_WIDTH = 20;
  var COUNTRY = /^[A-Z]{2}$/;
  var flagAtlas = null;
  var atlasLoad = null;

  function validAtlas(index) {
    var whole = function (value) { return typeof value === 'number' && value >= 0 && value % 1 === 0; };
    return index && whole(index.cellWidth) && index.cellWidth > 0 && whole(index.cellHeight) && whole(index.padding) &&
      whole(index.columns) && index.columns > 0 && whole(index.width) && whole(index.height) &&
      index.countries && typeof index.countries === 'object' ? index : null;
  }
  // The flag index, read once for the page: a page draws its players again
  // when this settles true.
  function flagsReady() {
    if (!atlasLoad) {
      atlasLoad = fetch('/assets/flags.json', { credentials: 'omit' })
        .then(function (response) { return response.ok ? response.json() : null; })
        .then(function (index) {
          flagAtlas = validAtlas(index);
          return !!flagAtlas;
        })
        .catch(function () { return false; });
    }
    return atlasLoad;
  }

  // A country's flag from the atlas, or its code where the atlas has none.
  // The size and place are the atlas's numbers, so they are set here.
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
  // A country code from an answer, or '' for none or one that is not a code.
  function country(code) { return typeof code === 'string' && COUNTRY.test(code) ? code : ''; }

  window.EmberMarks = {
    FIGHTERS: FIGHTERS, ROSTER: ROSTER, CLASSES: CLASSES, SHORT: SHORT, TIERS: TIERS, FRAMES: FRAMES, TOKEN: TOKEN,
    cardLink: cardLink, isFighter: isFighter, isBand: isBand, isTier: isTier, isBadge: isBadge,
    face: face, points: points, frameDot: frameDot, frame: frame, classPill: classPill, badge: badge,
    countryName: countryName, country: country, flag: flag, flagsReady: flagsReady
  };
}());
