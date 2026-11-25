// The guides' search, over /docs/search.json (scripts/site/guides.py writes
// it: an entry for each guide and each of its headings, with the heading's
// address and the words under it). The index loads when the box is first used.
// Every word typed must be in the entry. A guide whose name holds the words
// comes first, then headings that do, then sections that only mention them,
// each shown with the words around the first one.
(function () {
  'use strict';
  var input = document.getElementById('doc-search');
  var list = document.getElementById('doc-results');
  if (!input || !list || !window.fetch) return;
  var MOST = 12;
  // Letters shown before and after the first word found in a section's text.
  var BEFORE = 40;
  var AFTER = 90;
  var index = null;

  function load() {
    if (index) return Promise.resolve(index);
    return fetch('/docs/search.json')
      .then(function (response) { return response.ok ? response.json() : []; })
      .then(function (entries) {
        index = entries.map(function (entry) {
          return { entry: entry, name: (entry.heading || entry.page).toLowerCase(), text: entry.text.toLowerCase() };
        });
        return index;
      });
  }

  function has(text, words) {
    return words.every(function (word) { return text.indexOf(word) >= 0; });
  }

  // 0 a guide by its name, 1 a heading by its words, 2 a section that mentions them; none otherwise.
  function rank(item, words) {
    if (has(item.name, words)) return item.entry.heading ? 1 : 0;
    return has(item.name + ' ' + item.text, words) ? 2 : -1;
  }

  function found(words) {
    var hits = [];
    index.forEach(function (item) {
      var at = rank(item, words);
      if (at >= 0) hits.push({ item: item, rank: at });
    });
    return hits.sort(function (a, b) { return a.rank - b.rank; }).slice(0, MOST);
  }

  // The section's words around the first word typed that it holds.
  function around(item, words) {
    var at = -1;
    words.forEach(function (word) {
      var here = item.text.indexOf(word);
      if (here >= 0 && (at < 0 || here < at)) at = here;
    });
    if (at < 0) return '';
    var start = Math.max(0, at - BEFORE);
    var text = item.entry.text;
    return (start ? '...' : '') + text.slice(start, at + AFTER).trim() + (at + AFTER < text.length ? '...' : '');
  }

  function line(className, text) {
    var node = document.createElement('span');
    node.className = className;
    node.textContent = text;
    return node;
  }

  function show() {
    var words = input.value.toLowerCase().split(/\s+/).filter(Boolean);
    list.textContent = '';
    if (!words.length) { list.hidden = true; return; }
    var hits = found(words);
    hits.forEach(function (hit) {
      var entry = hit.item.entry;
      var item = document.createElement('li');
      var link = document.createElement('a');
      link.href = entry.url;
      link.textContent = entry.heading || entry.page;
      if (entry.heading) link.appendChild(line('in', entry.page));
      if (hit.rank === 2) link.appendChild(line('words', around(hit.item, words)));
      item.appendChild(link);
      list.appendChild(item);
    });
    if (!hits.length) {
      var none = document.createElement('li');
      none.className = 'none';
      none.textContent = 'Nothing in the guides matches. Try another word, or ask on Discord.';
      list.appendChild(none);
    }
    list.hidden = false;
  }

  input.addEventListener('focus', function () { load().catch(function () {}); }, { once: true });
  input.addEventListener('input', function () { load().then(show).catch(function () {}); });
  input.addEventListener('keydown', function (event) {
    var first = list.querySelector('a');
    if (event.key === 'ArrowDown' && first) { event.preventDefault(); first.focus(); }
    if (event.key === 'Escape') { input.value = ''; show(); }
  });
  // Arrow keys walk the results; Up from the first goes back to the box.
  list.addEventListener('keydown', function (event) {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    var links = Array.prototype.slice.call(list.querySelectorAll('a'));
    var next = links[links.indexOf(document.activeElement) + (event.key === 'ArrowDown' ? 1 : -1)];
    event.preventDefault();
    (next || (event.key === 'ArrowUp' ? input : document.activeElement)).focus();
  });
}());
