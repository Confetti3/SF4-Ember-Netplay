// needs: site
// The blocks the card, rating and points pages build from (site.css): tables
// of players or fighters in a box that scrolls sideways, lists of facts, the
// lock beside what only the player sees, and a month as a card says it.
(function () {
  'use strict';
  var site = window.EmberSite;
  var el = site.el;

  // A box that scrolls sideways when what it holds is wider than the window:
  // a stop for the keyboard, named for screen readers.
  function scrollBox(label) {
    var box = el('div', 'data-scroll');
    box.tabIndex = 0;
    box.setAttribute('role', 'region');
    box.setAttribute('aria-label', label);
    return box;
  }

  // The lock beside what only the player sees.
  function lock(size) {
    var icon = site.svg('svg', { width: size, height: size, viewBox: '0 0 24 24', fill: 'none', stroke: 'currentColor',
      'stroke-width': 2, 'stroke-linecap': 'round', 'stroke-linejoin': 'round', 'aria-hidden': 'true', focusable: 'false' });
    site.svg('rect', { x: 5, y: 11, width: 14, height: 10, rx: 2 }, icon);
    site.svg('path', { d: 'M8 11V7a4 4 0 0 1 8 0v4' }, icon);
    return icon;
  }
  function ownMark(text) {
    var mark = el('span', 'own-mark');
    mark.appendChild(lock(14));
    mark.appendChild(document.createTextNode(text));
    return mark;
  }

  // A table in its scrolling box. columns: [heading, class, own], own marking
  // what only the player sees; rows: each row's cells, from cell() and rowHead().
  function table(caption, columns, rows, className) {
    var box = scrollBox(caption);
    var node = box.appendChild(el('table', 'data-table ' + className));
    node.appendChild(el('caption', 'offscreen', caption));
    var headRow = node.appendChild(el('thead')).appendChild(el('tr'));
    columns.forEach(function (column) {
      var head = headRow.appendChild(el('th', column[1] || ''));
      head.scope = 'col';
      if (column[2]) head.appendChild(ownMark(column[0]));
      else head.textContent = column[0];
    });
    var body = node.appendChild(el('tbody'));
    rows.forEach(function (cells) {
      var row = body.appendChild(el('tr'));
      cells.forEach(function (one) { row.appendChild(one); });
    });
    return box;
  }
  // A cell of text, or of the nodes given.
  function cell(content, className) {
    var node = el('td', className || '');
    if (typeof content === 'string') node.textContent = content;
    else [].concat(content).forEach(function (part) { node.appendChild(part); });
    return node;
  }
  // The cell that names a row (a player or a fighter), its parts in a line.
  function rowHead(parts, className) {
    var head = el('th');
    head.scope = 'row';
    var line = head.appendChild(el('span', 'data-who' + (className ? ' ' + className : '')));
    parts.forEach(function (part) { line.appendChild(part); });
    return head;
  }

  // One fact of a facts list (dl.card-facts): a term and its value, text or a node.
  function fact(list, term, value) {
    var item = list.appendChild(el('div'));
    item.appendChild(el('dt', '', term));
    var description = item.appendChild(el('dd'));
    if (typeof value === 'string') description.textContent = value;
    else description.appendChild(value);
  }

  // "October 2026": the month a player came to Ember, which an answer gives as its first second.
  function month(seconds) {
    return new Date(seconds * 1000).toLocaleDateString('en-GB', { month: 'long', year: 'numeric', timeZone: 'UTC' });
  }

  window.EmberBlocks = {
    scrollBox: scrollBox, lock: lock, ownMark: ownMark, table: table, cell: cell, rowHead: rowHead, fact: fact, month: month
  };
}());
