// An event's public page, /t/{id}: what anyone following the tournament sees
// on a phone. It reads GET /v1/public/events/{id} every fifteen seconds and
// lets a signed-in player sign up while sign-ups are open. Set cards and
// the bracket come from sets.js.
"use strict";

const id = location.pathname.split("/").pop();
const page = { tab: "Live", phase: null, event: null, session: null };
const $ = (name) => document.getElementById(name);

function toast(text, good) {
  const box = $("toast");
  box.textContent = text;
  box.className = good ? "toast good" : "toast";
  box.hidden = !text;
}

async function read() {
  const response = await fetch("/v1/public/events/" + encodeURIComponent(id), { credentials: "same-origin" });
  if (!response.ok) {
    $("updated").textContent = response.status === 404 ? "This tournament is no longer public." : "Could not read the tournament just now.";
    return;
  }
  page.event = await response.json();
  draw();
}

function draw() {
  const event = page.event;
  $("title").textContent = event.title;
  $("when").textContent = event.starts_at
    ? new Date(event.starts_at * 1000).toLocaleString([], { dateStyle: "full", timeStyle: "short" })
    : "Date to be announced";
  fill($("links"), ...(event.links || []).map((link) =>
    el("a", { class: "wd-btn", href: link.url, target: "_blank", rel: "noopener" }, link.label)));
  signUp();
  const sets = event.sets || [];
  const count = tally(sets);
  fill($("counts"),
    el("div", { class: "wd-kpi live" }, el("b", {}, String(count.live)), el("span", { class: "small" }, "live now")),
    el("div", { class: "wd-kpi ok" }, el("b", {}, count.done + "/" + count.total), el("span", { class: "small" }, "sets done")),
  );
  fill($("tabs"), ...["Live", "Bracket"].map((tab) =>
    el("button", { type: "button", class: "chip", role: "tab", "aria-selected": String(page.tab === tab), onclick: () => { page.tab = tab; draw(); } }, tab)));
  if (page.tab === "Bracket") {
    fill($("body"), bracketView(sets, { phase: page.phase, redraw: (phase) => { page.phase = phase; draw(); } }));
  } else {
    const live = sets.filter((set) => FILTERS.Live(set) || FILTERS["Needs action"](set));
    fill($("body"), live.length
      ? el("div", { class: "wd-grid" }, live.map((set) => setCard(set)))
      : el("p", { class: "note" }, sets.length ? "No set is being played right now." : "The sets show here once the tournament starts."));
  }
  $("updated").textContent = event.updated_at ? "Bracket read " + new Date(event.updated_at * 1000).toLocaleTimeString([], { timeStyle: "short" }) : "";
}

// --- Signing up: a signed-in player in one tap; anyone else signs in with a
// linked account first and comes back here (desk.js sends them back).

async function signUp() {
  const box = $("sign-up");
  box.hidden = !page.event.sign_ups_open;
  if (box.hidden) return;
  if (page.session === null) {
    const response = await fetch("/web/session", { credentials: "same-origin" });
    page.session = response.ok ? await response.json() : false;
  }
  if (page.session) {
    fill(box, el("button", { type: "button", class: "wd-btn primary", onclick: join }, "Sign up"));
    return;
  }
  const capabilities = await fetch("/v1/capabilities").then((response) => response.json()).catch(() => ({}));
  fill(box,
    el("p", { class: "note" }, "Sign up with the account you linked in Ember:"),
    ...(capabilities.sign_in_sites || []).filter((site) => site.sign_in).map((site) =>
      el("a", { class: "wd-btn primary", href: "/web/sign-in/" + encodeURIComponent(site.id), onclick: () => remember() }, "Sign in with " + site.display_name)),
  );
}

function remember() {
  try {
    sessionStorage.setItem("ember.return", location.pathname);
  } catch {
    // Without storage the player lands on the desk and comes back by hand.
  }
}

async function join() {
  const response = await fetch("/v1/events/" + encodeURIComponent(id) + "/sign-up", {
    method: "POST",
    credentials: "same-origin",
    headers: { "Content-Type": "application/json", "X-Ember-CSRF": page.session.csrf },
    body: "{}",
  });
  const data = await response.json().catch(() => ({}));
  if (response.ok) toast("You're signed up. See you in the bracket!", true);
  else toast((data.error && data.error.message) || "Could not sign you up (" + response.status + ").");
}

read();
setInterval(() => {
  if (!document.hidden) read().catch(() => {});
}, 15000);
