// The docs pages: the guide list behind a button on a phone, search over the
// pages' titles and headings, and the contents list following the reader.
(function () {
  "use strict";
  var menu = document.querySelector(".docs-menu"), nav = document.getElementById("docs-nav");
  menu.addEventListener("click", function () {
    var open = nav.classList.toggle("open");
    menu.setAttribute("aria-expanded", String(open));
  });

  var input = document.getElementById("docs-search"), results = document.getElementById("docs-results"), index = null;
  document.addEventListener("keydown", function (event) {
    if (event.key === "/" && !/INPUT|TEXTAREA/.test(document.activeElement.tagName)) {
      event.preventDefault();
      input.focus();
    } else if (event.key === "Escape") {
      results.hidden = true;
    }
  });
  function escape(text) {
    return text.replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; });
  }
  function search() {
    var words = input.value.trim().toLowerCase().split(/\s+/).filter(Boolean);
    if (!words.length) { results.hidden = true; return; }
    var found = [];
    index.forEach(function (page) {
      var title = page.title.toLowerCase(), about = page.about.toLowerCase();
      if (words.every(function (w) { return title.indexOf(w) >= 0 || about.indexOf(w) >= 0; })) {
        found.push({ url: page.url, text: page.title, where: page.section });
      }
      page.headings.forEach(function (h) {
        var text = h.text.toLowerCase();
        if (words.every(function (w) { return text.indexOf(w) >= 0; })) {
          found.push({ url: page.url + "#" + h.id, text: h.text, where: page.title });
        }
      });
    });
    results.innerHTML = found.length
      ? found.slice(0, 12).map(function (f) {
          return '<a href="' + escape(f.url) + '">' + escape(f.text) + "<small>" + escape(f.where) + "</small></a>";
        }).join("")
      : "<p>Nothing found. Try another word, or ask in Discord.</p>";
    results.hidden = false;
  }
  input.addEventListener("input", function () {
    if (index) { search(); return; }
    fetch(input.getAttribute("data-index")).then(function (r) { return r.json(); }).then(function (data) {
      index = data;
      search();
    });
  });
  input.addEventListener("keydown", function (event) {
    if (event.key === "ArrowDown" && !results.hidden) { event.preventDefault(); var first = results.querySelector("a"); if (first) first.focus(); }
  });
  results.addEventListener("keydown", function (event) {
    var links = Array.prototype.slice.call(results.querySelectorAll("a")), at = links.indexOf(document.activeElement);
    if (event.key === "ArrowDown" && at < links.length - 1) { event.preventDefault(); links[at + 1].focus(); }
    if (event.key === "ArrowUp") { event.preventDefault(); (at > 0 ? links[at - 1] : input).focus(); }
  });
  document.addEventListener("click", function (event) {
    if (!results.contains(event.target) && event.target !== input) results.hidden = true;
  });

  var toc = document.querySelectorAll(".docs-toc a");
  if (toc.length && "IntersectionObserver" in window) {
    var byId = {}, current = null;
    toc.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (!entry.isIntersecting) return;
        if (current) current.removeAttribute("aria-current");
        current = byId[entry.target.id];
        if (current) current.setAttribute("aria-current", "true");
      });
    }, { rootMargin: "0px 0px -70% 0px" });
    Object.keys(byId).forEach(function (id) {
      var heading = document.getElementById(id);
      if (heading) observer.observe(heading);
    });
  }
})();
