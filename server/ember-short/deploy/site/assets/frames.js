// Frame data: the list of fighters narrows to a name as it is typed
// (#fd-find), and a fighter's tables to the moves whose name or notation holds
// what is typed (#fd-filter). A table with no move left goes with its heading.
(function () {
  'use strict';

  var find = document.getElementById('fd-find');
  if (find) {
    var fighters = Array.prototype.slice.call(document.querySelectorAll('.fd-roster a'));
    find.addEventListener('input', function () {
      var text = find.value.trim().toLowerCase();
      fighters.forEach(function (link) { link.hidden = link.textContent.toLowerCase().indexOf(text) < 0; });
    });
    // Enter opens the fighter when one is left.
    find.addEventListener('keydown', function (event) {
      var left = fighters.filter(function (link) { return !link.hidden; });
      if (event.key === 'Enter' && left.length === 1) location.href = left[0].href;
    });
  }

  var filter = document.getElementById('fd-filter');
  if (!filter) return;
  var count = document.getElementById('fd-count');
  // The key at the top is a table too; only the move tables are filtered.
  var tables = Array.prototype.slice.call(document.querySelectorAll('#frames .table-wrap')).filter(function (wrap) {
    return !wrap.closest('details');
  }).map(function (wrap) {
    var heading = wrap.previousElementSibling;
    return {
      wrap: wrap,
      heading: heading && heading.tagName === 'H2' ? heading : null,
      rows: Array.prototype.slice.call(wrap.querySelectorAll('tbody tr')).map(function (row) {
        return { row: row, text: row.cells[0].textContent.toLowerCase() };
      })
    };
  });

  filter.addEventListener('input', function () {
    var words = filter.value.toLowerCase().split(/\s+/).filter(Boolean);
    var shown = 0;
    tables.forEach(function (table) {
      var left = 0;
      table.rows.forEach(function (move) {
        var keep = words.every(function (word) { return move.text.indexOf(word) >= 0; });
        move.row.hidden = !keep;
        if (keep) left += 1;
      });
      table.wrap.hidden = left === 0;
      if (table.heading) table.heading.hidden = left === 0;
      shown += left;
    });
    count.textContent = !words.length ? '' : shown === 0 ? 'No move matches.' : shown + (shown === 1 ? ' move matches.' : ' moves match.');
  });
}());
