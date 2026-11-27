// needs: site marks blocks
// The rating details on /me (#rating): the three numbers behind the player's
// class, what each means, where the rating sits among the classes and what a
// next game would move (docs/design/SITE_PLAYER_CARDS.md, GET /v1/website/me).
// Every number it explains is the answer's: the player's `rating`, and `rules`,
// the numbers the bridge rates games by, so the page cannot fall behind a
// retune. Nothing here works a rating out; the examples are the bridge's.
// It draws answers.js's rating model.
(function () {
  'use strict';
  var site = window.EmberSite;
  var marks = window.EmberMarks;
  var blocks = window.EmberBlocks;
  var el = site.el, svg = site.svg;
  var fact = blocks.fact, table = blocks.table, cell = blocks.cell;
  var CLASSES = marks.CLASSES, TIERS = marks.TIERS;
  // Two deviations either side of a rating is where it lies 95 times in 100:
  // the bar under the classes, and what the Deviation panel says.
  var SURE_DEVIATIONS = 2;
  // How far from the starting volatility, as a share of it, still reads as
  // steady results.
  var STEADY = 0.05;
  var FRACTIONS = { 2: 'half', 3: 'a third', 4: 'a quarter', 5: 'a fifth', 10: 'a tenth' };
  var COUNTS = ['no', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine'];

  function whole(value) { return String(Math.round(value)); }
  function capital(text) { return text.charAt(0).toUpperCase() + text.slice(1); }
  // How much of a game a weight counts, as a sentence says it ("in full",
  // "a tenth", "half", "85%") and as the gap chart marks it ("Full", "1/10", "85%").
  function share(weight) {
    if (weight >= 1) return { words: 'in full', mark: 'Full' };
    var parts = Math.round(1 / weight), exact = Math.abs(parts * weight - 1) < 1e-9;
    var percent = Math.round(weight * 100) + '%';
    return { words: exact && FRACTIONS[parts] ? FRACTIONS[parts] : percent, mark: exact ? '1/' + parts : percent };
  }
  function duration(seconds) {
    return seconds % 3600 === 0 ? site.plural(seconds / 3600, 'hour', 'hours') : site.plural(Math.round(seconds / 60), 'minute', 'minutes');
  }
  // A class as a sentence names it: "class A", "Master".
  function classWord(band) { return CLASSES[band - 1].length <= 2 ? 'class ' + CLASSES[band - 1] : CLASSES[band - 1]; }
  // A class as a screen reader should say it: "B plus".
  function spoken(band) { return classWord(band).replace('+', ' plus'); }

  // The classes along the rating line: each class from its floor to the next,
  // and the open classes at either end as wide as the narrowest class, so the
  // line has ends to draw.
  function ladderOf(rules) {
    var floors = rules.floors;
    var step = Infinity;
    for (var at = 1; at < floors.length; at++) step = Math.min(step, floors[at] - floors[at - 1]);
    var low = floors[0] - step, high = floors[floors.length - 1] + step;
    var edges = [low].concat(floors, [high]);
    return {
      floors: floors, low: low, high: high, step: step,
      // The rating where band starts and where the next starts; null at an open end.
      from: function (band) { return band >= 2 ? floors[band - 2] : null; },
      to: function (band) { return band <= floors.length ? floors[band - 1] : null; },
      width: function (band) { return edges[band] - edges[band - 1]; },
      // The class a rating falls in.
      at: function (value) { return 1 + floors.filter(function (floor) { return value >= floor; }).length; },
      // Where a rating sits along the line, in percent, kept on the line.
      place: function (value) { return (Math.min(high, Math.max(low, value)) - low) / (high - low) * 100; }
    };
  }

  // Where a class runs: "from 1700 to 1799", "below 1200", "from 2050 up".
  function runs(ladder, band) {
    var from = ladder.from(band), to = ladder.to(band);
    if (from === null) return 'below ' + whole(to);
    if (to === null) return 'from ' + whole(from) + ' up';
    return 'from ' + whole(from) + ' to ' + whole(to - 1);
  }
  // "Class A runs from 1700 to 1799."
  function classRange(ladder, band) { return capital(classWord(band)) + ' runs ' + runs(ladder, band) + '.'; }

  function numberPanel(label, value, about, yours) {
    var panel = el('section', 'panel rating-number');
    panel.appendChild(el('div', 'card-k', label));
    panel.appendChild(el('div', 'rating-value', value));
    panel.appendChild(el('p', 'note', about));
    panel.appendChild(el('p', 'rating-yours', yours));
    return panel;
  }

  function threeNumbers(rating, rules, ladder) {
    var low = whole(rating.rating - SURE_DEVIATIONS * rating.deviation), high = whole(rating.rating + SURE_DEVIATIONS * rating.deviation);
    var sure = 'Your rating is 95% sure to lie between ' + low + ' and ' + high + '.';
    var settled = whole(rules.settled_deviation);
    var deviation = rating.band === 0 ? 'Placing: a class shows once it is ' + settled + ' or less. ' + sure :
      rating.settled ? 'Settled. ' + sure : 'Not settled: your class shows faint until the deviation is ' + settled + ' or less again. ' + sure;
    var drift = (rating.volatility - rules.start_volatility) / rules.start_volatility;
    var volatility = Math.abs(drift) <= STEADY ? 'Close to the starting value: your results have been steady.' :
      drift > 0 ? 'Above the starting value: your results have swung more than most.' : 'Below the starting value: your results have been steadier than most.';
    var three = el('div', 'rating-three');
    three.appendChild(numberPanel('Rating', whole(rating.rating),
      'Ember’s estimate of how strong you are. Everyone starts at ' + whole(rules.start_rating) +
      '. A win raises it and a loss lowers it, by more when the result was a surprise. Your class is read from it.',
      rating.band === 0 ? 'No class yet.' : classRange(ladder, rating.band)));
    three.appendChild(numberPanel('Deviation', whole(rating.deviation),
      'How unsure the rating is. It starts at ' + whole(rules.start_deviation) + ' and falls as you play. A class shows once it is ' +
      settled + ' or less. It grows a little each day you do not play.', deviation));
    three.appendChild(numberPanel('Volatility', rating.volatility.toFixed(4),
      'How steady your results are. It starts at ' + rules.start_volatility + ' and rises when you swing between upset wins and bad losses. ' +
      'A higher value lets the rating move faster and makes the deviation grow faster during a break.', volatility));
    return three;
  }

  // Where the rating sits: the classes along a line, each class's floor under
  // it, the rating as a white mark and the range it is 95% sure of as a bar.
  function ladderPanel(rating, rules, ladder) {
    var panel = el('section', 'panel');
    panel.appendChild(el('h2', '', 'Where your rating sits'));
    var low = rating.rating - SURE_DEVIATIONS * rating.deviation, high = rating.rating + SURE_DEVIATIONS * rating.deviation;
    var scroll = blocks.scrollBox('Where your rating sits among the classes');
    var line = el('div', 'rating-ladder');
    line.setAttribute('role', 'img');
    line.setAttribute('aria-label', 'Your rating of ' + whole(rating.rating) + (rating.band ? ' is in ' + spoken(rating.band) +
      ', which runs ' + runs(ladder, rating.band) + '.' : ' has no class yet.') +
      ' The range it is 95% sure to lie in runs from ' + whole(low) + ' in ' + spoken(ladder.at(low)) + ' to ' + whole(high) + ' in ' + spoken(ladder.at(high)) + '.');
    var mark = el('div', 'rating-mark');
    mark.style.left = ladder.place(rating.rating).toFixed(2) + '%';
    mark.appendChild(el('b', '', 'Your rating ' + whole(rating.rating)));
    mark.appendChild(el('i'));
    line.appendChild(mark);
    var segments = el('div', 'rating-segments');
    CLASSES.forEach(function (name, at) {
      var band = at + 1;
      var segment = el('span', 'rating-segment tier-' + TIERS[at] + (band === rating.band ? ' on' : ''), name);
      segment.style.flexGrow = String(ladder.width(band));
      segments.appendChild(segment);
    });
    line.appendChild(segments);
    var range = el('div', 'rating-range');
    range.style.left = ladder.place(low).toFixed(2) + '%';
    range.style.width = (ladder.place(high) - ladder.place(low)).toFixed(2) + '%';
    line.appendChild(range);
    ladder.floors.forEach(function (floor) {
      var tick = el('span', 'rating-tick', whole(floor));
      tick.style.left = ladder.place(floor).toFixed(2) + '%';
      line.appendChild(tick);
    });
    scroll.appendChild(line);
    panel.appendChild(scroll);
    panel.appendChild(el('p', 'note', 'The white line is your rating. The bar under the classes is the range it is 95% sure to lie in: ' +
      COUNTS[SURE_DEVIATIONS] + ' deviations either side. The numbers under the classes are where each class begins.'));
    panel.appendChild(classFacts(rating, rules));
    panel.appendChild(el('p', 'note rating-after', 'A class changes only when the rating has gone ' + whole(rules.margin) +
      ' points past its edge, so one good or bad evening rarely moves it. It never drops on its own: after a long break it shows faint until a few games settle it again.'));
    return panel;
  }

  // The class shown and where it moves; a placing rating has neither.
  function classFacts(rating, rules) {
    var facts = el('dl', 'card-facts rating-facts');
    if (rating.band === 0) {
      fact(facts, 'Class shown', marks.badge({ band: 0, settled: false }));
      fact(facts, 'A class shows', 'Once the deviation is ' + whole(rules.settled_deviation) + ' or less');
      return facts;
    }
    fact(facts, 'Class shown', marks.classPill(rating.band, rating.settled));
    if (rating.up_at !== undefined) fact(facts, 'Moves up to ' + CLASSES[rating.band], 'At ' + whole(rating.up_at));
    else fact(facts, 'Moves up', 'The top class');
    if (rating.down_below !== undefined) fact(facts, 'Moves down to ' + CLASSES[rating.band - 2], 'Below ' + whole(rating.down_below));
    else fact(facts, 'Moves down', 'The lowest class');
    if (rating.faint_in_days !== undefined) fact(facts, 'Shows faint after a break of', site.plural(rating.faint_in_days, 'day', 'days'));
    else if (!rating.settled) fact(facts, 'Shows faint', 'Until a few games settle it');
    return facts;
  }

  // A cell that opens on a strong part: "<strong>In full</strong>, except ...".
  function ruleCell(strong, rest) {
    var node = el('td');
    node.appendChild(el('strong', '', strong));
    if (rest) node.appendChild(document.createTextNode(rest));
    return node;
  }

  function rulesPanel(rules) {
    var panel = el('section', 'panel rating-wide');
    panel.appendChild(el('h2', '', 'How much a game counts'));
    panel.appendChild(el('p', 'note', 'Not every game moves a rating the same. These rules keep a game against anyone safe to play.'));
    var full = whole(rules.full_gap), far = whole(rules.min_gap);
    panel.appendChild(table('How much a game counts', [['The game'], ['Counts']], [
      [cell('Your opponent is within ' + full + ' points of you'), ruleCell('In full')],
      [cell('You are ' + full + ' to ' + far + ' points apart'), ruleCell('Less the wider the gap', ', down to ' + share(rules.min_weight).words + ' at ' + far)],
      [cell('You win as the lower-rated player'), ruleCell('At least ' + share(rules.upset_weight).words, ', whatever the gap')],
      [cell('More than ' + rules.full_repeats + ' games against the same player in one sitting'),
        ruleCell(capital(share(rules.repeat_weight).words) + ' each.', ' A sitting ends after ' + duration(rules.sitting_secs) + ' without a game between you')],
      [cell('Either rating is still unsure (a deviation above ' + whole(rules.unsure_deviation) + ')'),
        ruleCell('In full', ', except a loss to a clearly weaker player, which stays light')]
    ], 'rating-rules'));
    return panel;
  }

  // "about a class and a half": a gap in classes of the narrowest width, to
  // the nearest half.
  function inClasses(gap, step) {
    var halves = Math.round(2 * gap / step);
    var wholeClasses = Math.floor(halves / 2), half = halves % 2 === 1;
    if (wholeClasses === 0) return half ? 'half a class' : 'less than a class';
    var count = wholeClasses === 1 ? 'a class' : wholeClasses + ' classes';
    return count + (half ? ' and a half' : '');
  }

  // The gap rule as a line: in full up to full_gap, down in a straight line
  // to min_weight at min_gap, flat after.
  function gapPanel(rules, ladder) {
    var panel = el('section', 'panel rating-narrow');
    panel.appendChild(el('h2', '', 'The gap rule'));
    var LEFT = 40, WIDTH = 300, BASE = 120, TALL = 100;
    var end = rules.min_gap + rules.full_gap;
    var x = function (gap) { return LEFT + WIDTH * gap / end; };
    var y = function (weight) { return BASE - TALL * weight; };
    var chart = svg('svg', { viewBox: '0 0 360 170', 'class': 'rating-gap', role: 'img',
      'aria-label': 'A game counts in full up to a gap of ' + whole(rules.full_gap) + ' rating points, then falls in a straight line to ' +
        share(rules.min_weight).words + ' at ' + whole(rules.min_gap) + ' points and stays there.' });
    svg('line', { 'class': 'rating-gap-grid', x1: LEFT, y1: y(1), x2: LEFT + WIDTH, y2: y(1) }, chart);
    svg('line', { 'class': 'rating-gap-grid', x1: LEFT, y1: y(rules.min_weight), x2: LEFT + WIDTH, y2: y(rules.min_weight) }, chart);
    svg('line', { 'class': 'rating-gap-axis', x1: LEFT, y1: BASE, x2: LEFT + WIDTH, y2: BASE }, chart);
    svg('line', { 'class': 'rating-gap-grid dashed', x1: x(rules.full_gap), y1: y(1), x2: x(rules.full_gap), y2: BASE }, chart);
    svg('line', { 'class': 'rating-gap-grid dashed', x1: x(rules.min_gap), y1: y(rules.min_weight), x2: x(rules.min_gap), y2: BASE }, chart);
    svg('path', { 'class': 'rating-gap-line', d: 'M' + x(0) + ' ' + y(1) + ' L' + x(rules.full_gap) + ' ' + y(1) + ' L' + x(rules.min_gap) +
      ' ' + y(rules.min_weight) + ' L' + x(end) + ' ' + y(rules.min_weight) }, chart);
    var text = function (words, at, top, anchor) {
      svg('text', { 'class': 'rating-gap-text', x: at, y: top, 'text-anchor': anchor }, chart).textContent = words;
    };
    text(share(1).mark, LEFT - 8, y(1) + 4, 'end');
    text(share(rules.min_weight).mark, LEFT - 8, y(rules.min_weight) + 4, 'end');
    text('0', x(0), BASE + 18, 'middle');
    text(whole(rules.full_gap), x(rules.full_gap), BASE + 18, 'middle');
    text(whole(rules.min_gap), x(rules.min_gap), BASE + 18, 'middle');
    text('Rating points between the two players', LEFT + WIDTH / 2, BASE + 40, 'middle');
    panel.appendChild(chart);
    panel.appendChild(el('p', 'note rating-after', whole(rules.full_gap) + ' points is about ' + inClasses(rules.full_gap, ladder.step) +
      '. A Grand Master who loses to a D player gives up a few points, not a class.'));
    return panel;
  }

  // A change to the rating as a signed whole number, or a tenth when that
  // would read as nothing: "+14", "−0.4".
  function change(value) {
    var shown = Math.round(value) === 0 && value !== 0 ? Math.abs(value).toFixed(1) : String(Math.abs(Math.round(value)));
    return (value < 0 ? '−' : '+') + shown;
  }
  function counts(example) {
    var win = example.win.weight, loss = example.loss.weight;
    return win === loss ? capital(share(win).words) : capital(share(win).words) + ' if you win, ' + share(loss).words + ' if you lose';
  }
  function opponent(example, rating, rules) {
    if (example.band === 0) return 'A new player with no class yet';
    var gap = example.opponent - rating.rating;
    var where = Math.abs(gap) < rules.full_gap ? 'close to you' : whole(Math.abs(gap)) + (gap > 0 ? ' above you' : ' below you');
    return capital(classWord(example.band)) + ', rated ' + whole(example.opponent) + ', ' + where;
  }

  function examplesPanel(rating, rules) {
    var panel = el('section', 'panel');
    panel.appendChild(el('h2', '', 'What your next game would move'));
    panel.appendChild(el('p', 'note', 'Worked out from your numbers today, against ' + (COUNTS[rating.examples.length] || rating.examples.length) +
      (rating.examples.length === 1 ? ' kind' : ' kinds') + ' of opponent.'));
    panel.appendChild(table('What your next game would move', [['Opponent'], ['Counts'], ['If you win', 'num'], ['If you lose', 'num']],
      rating.examples.map(function (example) {
        return [cell(opponent(example, rating, rules)), cell(counts(example)),
          cell(change(example.win.change), 'num pts rating-gain'), cell(change(example.loss.change), 'num pts rating-loss')];
      }), 'rating-next'));
    panel.appendChild(closing());
    return panel;
  }

  function closing() {
    var note = el('p', 'note rating-after', 'Only public room games with a winner count. Draws, cancelled games and disconnects never do. The rating is Glicko-2, updated after every game. ');
    var link = el('a', '', 'How classes work');
    link.href = '/rankings#how';
    note.appendChild(link);
    return note;
  }

  function ownNote() {
    var note = el('p', 'rating-own');
    note.appendChild(blocks.lock(18));
    note.appendChild(el('span', '', 'Only you can see this page. Other players see your class, never these numbers, and nobody’s numbers are listed anywhere.'));
    return note;
  }

  function notice(title, text) {
    var panel = el('section', 'panel rule rating-notice');
    panel.appendChild(el('h2', '', title));
    panel.appendChild(el('p', 'note', text));
    return panel;
  }

  // The details into target, in place of what it held, from answers.rating's model.
  function draw(target, told) {
    var rating = told.rating, rules = told.rules, ladder = ladderOf(rules);
    while (target.firstChild) target.removeChild(target.firstChild);
    target.appendChild(ownNote());
    if (!rating) {
      target.appendChild(notice('No rated games yet', 'Your rating starts at ' + whole(rules.start_rating) + ' with a deviation of ' +
        whole(rules.start_deviation) + ' and moves with your first rated public room game. The rules below are how every game counts.'));
    } else {
      if (!rating.takes_part) {
        target.appendChild(notice('You do not take part in rankings', 'Your rating is kept as it was, and games move nothing until you take part again. ' +
          'Choose Take part in Ember under Online play, Rankings.'));
      }
      target.appendChild(threeNumbers(rating, rules, ladder));
      target.appendChild(ladderPanel(rating, rules, ladder));
    }
    var cols = el('div', 'rating-cols');
    cols.appendChild(rulesPanel(rules));
    cols.appendChild(gapPanel(rules, ladder));
    target.appendChild(cols);
    if (rating && rating.examples.length) target.appendChild(examplesPanel(rating, rules));
    else target.appendChild(closing());
  }

  // For the page to call once the details in target are on screen: where the
  // ladder scrolls in its box (a phone), it starts at the player's rating.
  function shown(target) {
    var mark = target.querySelector('.rating-mark');
    var box = mark && mark.closest('.data-scroll');
    if (box) box.scrollLeft = mark.offsetLeft - box.clientWidth / 2;
  }

  window.EmberRating = { draw: draw, shown: shown };
}());
