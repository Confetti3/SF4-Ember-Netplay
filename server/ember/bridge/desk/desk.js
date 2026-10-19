// The web desk (docs/design/TOURNAMENT_DESK.md, 4a): an organizer signed in
// with a linked site account sets up a tournament, step by step, through the
// bridge's /web and /v1/organizer routes. Every change carries the session's
// CSRF token. Nothing from the bridge is ever written as HTML.
"use strict";

const STEPS = ["Accounts", "Tournament", "Bracket service", "Links", "Discord post", "Live"];
const LINK_ICONS = { bracket: "🏆", "sign up": "📝", signup: "📝", stream: "📺", twitch: "📺", matcherino: "💰", rules: "📜", discord: "💬" };
const NEXT = ["Continue", "Save", "Save", "Save links", "Mark ready", "Your tournaments"];

const desk = { session: null, accounts: [], events: [], event: null, step: 1, picks: {} };
const $ = (id) => document.getElementById(id);

/** An element with attributes and children (strings become text). */
function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attrs)) {
    if (value === false || value == null) continue;
    if (name.startsWith("on")) node.addEventListener(name.slice(2), value);
    else node.setAttribute(name, value === true ? "" : value);
  }
  for (const child of children.flat()) {
    if (child != null && child !== false) node.append(child);
  }
  return node;
}

async function api(method, path, body) {
  const init = { method, credentials: "same-origin", headers: {} };
  if (method !== "GET") {
    init.headers["Content-Type"] = "application/json";
    init.headers["X-Ember-CSRF"] = desk.session ? desk.session.csrf : "";
    init.body = JSON.stringify(body || {});
  }
  const response = await fetch(path, init);
  const text = await response.text();
  const data = text ? JSON.parse(text) : {};
  if (!response.ok) {
    const error = new Error((data.error && data.error.message) || "The desk could not do that (" + response.status + ").");
    error.status = response.status;
    throw error;
  }
  return data;
}

function toast(text, good) {
  const box = $("toast");
  box.textContent = text;
  box.className = good ? "toast good" : "toast";
  box.hidden = !text;
}

function show(page) {
  for (const id of ["signed-out", "home", "wizard"]) $(id).hidden = id !== page;
  $("sign-out").hidden = page === "signed-out";
}

/** The Discord post for an event: one message, lines without a value left out. */
function discordPost(event, siteName) {
  const rounds = (event.settings && event.settings.rounds) || {};
  const longest = Math.max(rounds.games_to_win || 0, ...(rounds.rules || []).map((rule) => rule.games_to_win || 0));
  const ft = rounds.games_to_win
    ? "🎮 " + [siteName, "FT" + rounds.games_to_win, longest > rounds.games_to_win ? "later rounds up to FT" + longest : ""].filter(Boolean).join(" · ")
    : siteName ? "🎮 " + siteName : "";
  const when = event.starts_at ? "📅 <t:" + event.starts_at + ":F> (<t:" + event.starts_at + ":R>)" : "";
  const links = (event.links || []).map((link) => (LINK_ICONS[link.label.toLowerCase()] || "🔗") + " " + link.label + ": " + link.url);
  return ["**" + event.title + "** — USF4 on Ember", when, ft, ...links, "Play on Ember: https://embernetplay.link"]
    .filter(Boolean)
    .join("\n");
}

function siteName(id) {
  const site = desk.accounts.find((site) => site.id === id);
  return site ? site.display_name : id || "";
}

function localParts(seconds) {
  if (!seconds) return ["", ""];
  const at = new Date(seconds * 1000);
  const pad = (n) => String(n).padStart(2, "0");
  return [at.getFullYear() + "-" + pad(at.getMonth() + 1) + "-" + pad(at.getDate()), pad(at.getHours()) + ":" + pad(at.getMinutes())];
}

function whenText(seconds) {
  return seconds ? new Date(seconds * 1000).toLocaleString([], { dateStyle: "medium", timeStyle: "short" }) : "No date yet";
}

// --- Signed out and the tournaments list.

async function signedOut() {
  show("signed-out");
  const list = $("sign-in-sites");
  list.replaceChildren();
  const capabilities = await api("GET", "/v1/capabilities").catch(() => ({}));
  for (const site of capabilities.sign_in_sites || []) {
    if (!site.sign_in) continue;
    list.append(el("a", { class: "wd-btn primary", href: "/web/sign-in/" + encodeURIComponent(site.id) }, "Sign in with " + site.display_name));
  }
  if (!list.children.length) list.append(el("p", { class: "note" }, "This Ember service offers no web sign-in."));
}

async function home() {
  show("home");
  $("brand-sub").textContent = "Tournament desk";
  desk.events = (await api("GET", "/v1/organizer/events")).events;
  const list = $("events");
  list.replaceChildren(
    ...desk.events.map((event) =>
      el("button", { type: "button", class: "wd-card", onclick: () => open(event) },
        el("span", { class: "name" }, event.title),
        el("span", { class: "small" }, whenText(event.starts_at) + (event.site ? " · " + siteName(event.site) : "")),
        el("span", { class: "status " + event.status }, event.status))),
  );
  $("no-events").hidden = desk.events.length > 0;
}

function open(event) {
  desk.event = event;
  desk.picks = {};
  show("wizard");
  go(!event ? 1 : event.status === "draft" ? 2 : 6);
}

async function refreshEvent() {
  if (!desk.event) return;
  desk.events = (await api("GET", "/v1/organizer/events")).events;
  desk.event = desk.events.find((event) => event.id === desk.event.id) || null;
}

// --- The steps.

function go(step) {
  desk.step = Math.min(6, Math.max(1, step));
  toast("");
  for (const section of document.querySelectorAll("[data-step]")) section.hidden = Number(section.dataset.step) !== desk.step;
  $("step-now").replaceChildren("Step " + desk.step + " of 6 · ", el("b", {}, STEPS[desk.step - 1]));
  $("steps").replaceChildren(
    ...STEPS.map((label, index) =>
      el("button", {
        type: "button",
        class: "wd-step" + (index + 1 < desk.step ? " done" : ""),
        "aria-label": label,
        "aria-current": index + 1 === desk.step ? "step" : false,
        disabled: !desk.event && index > 1,
        onclick: () => go(index + 1),
      }, el("span", { class: "dot" }, String(index + 1)), el("span", { class: "label" }, label))),
  );
  $("brand-sub").textContent = desk.event ? "Tournament desk · " + desk.event.title : "Tournament desk · New tournament";
  $("next").textContent = desk.step === 5 && desk.event && desk.event.status !== "draft" ? "Continue" : NEXT[desk.step - 1];
  summary();
  [accounts, tournament, service, links, post, live][desk.step - 1]().catch((error) => toast(error.message));
}

function summary() {
  const event = desk.event;
  $("summary").replaceChildren(
    el("span", { class: "name" }, event ? event.title : "New tournament"),
    el("span", { class: "small" }, event ? event.status + " · " + whenText(event.starts_at) : "Not saved yet"),
    el("span", { class: "small" }, event && event.site ? siteName(event.site) + (event.name ? " · " + event.name : "") : "No bracket service yet"),
    el("span", { class: "small" }, (event ? (event.links || []).length : 0) + " links"),
  );
}

async function accounts() {
  desk.accounts = (await api("GET", "/web/accounts")).sites;
  $("accounts").replaceChildren(
    ...desk.accounts.map((site) => {
      let state;
      let action = null;
      if (!site.account) {
        state = el("span", { class: "small bad" }, "Not linked: connect it in Ember first");
      } else if (site.organizer) {
        state = el("span", { class: "small ok" }, "Organizer access · " + site.account.username);
      } else if (site.organizer_offered) {
        state = el("span", { class: "small wait" }, "Linked, no organizer access");
        action = el("button", { type: "button", class: "wd-btn primary", onclick: () => allow(site) }, "Allow");
      } else {
        state = el("span", { class: "small ok" }, "Linked · " + site.account.username);
      }
      return el("div", { class: "wd-card" + (action ? " act" : "") },
        el("div", { class: "card-row" }, el("div", {}, el("span", { class: "name" }, site.display_name), state), action));
    }),
  );
}

async function allow(site) {
  const started = await api("POST", "/web/organize/" + encodeURIComponent(site.id)).catch((error) => toast(error.message));
  if (!started) return;
  // The site's consent page opens beside the desk; coming back reads the
  // accounts again.
  window.open(started.authorize_url, "_blank", "noopener");
  toast("Approve Ember on " + site.display_name + ", then come back here.", true);
}

document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible" && desk.step === 1 && !$("wizard").hidden) accounts().catch(() => {});
});

async function tournament() {
  const event = desk.event;
  $("title").value = event ? event.title : "";
  [$("date").value, $("time").value] = localParts(event && event.starts_at);
}

function startsAt() {
  const date = $("date").value;
  const time = $("time").value;
  if (!date) return null;
  if (!time) throw new Error("Give the start time too, or leave the date empty.");
  return Math.floor(new Date(date + "T" + time).getTime() / 1000);
}

async function saveTournament() {
  const title = $("title").value.trim();
  if (!title) throw new Error("Give the tournament a title.");
  const starts_at = startsAt();
  if (!desk.event) {
    const made = await api("POST", "/v1/organizer/drafts", { title });
    desk.event = { id: made.id };
    await api("POST", "/v1/organizer/events/" + made.id, { starts_at });
  } else {
    await api("POST", "/v1/organizer/events/" + desk.event.id, { title, starts_at });
  }
  await refreshEvent();
}

function gamesOptions(selected) {
  return [1, 2, 3, 4, 5, 7, 10].map((n) => el("option", { value: n, selected: n === selected }, "FT" + n));
}

async function service() {
  if (!desk.accounts.length) desk.accounts = (await api("GET", "/web/accounts")).sites;
  const event = desk.event;
  const usable = desk.accounts.filter((site) => site.organizer);
  $("no-service").hidden = usable.length > 0;
  const chosen = (event && event.site) || desk.picks.site || (usable[0] && usable[0].id);
  desk.picks.site = chosen;
  $("services").replaceChildren(
    el("legend", {}, "Service"),
    ...usable.map((site) =>
      el("label", { class: "choice" },
        el("input", { type: "radio", name: "service", value: site.id, checked: site.id === chosen, disabled: !!(event && event.tournament), onchange: () => { desk.picks.site = site.id; service(); } }),
        el("span", {}, el("span", { class: "name" }, site.display_name), el("br"),
          el("span", { class: "small" }, site.create ? "Ember creates it for you" : "Create it on " + site.display_name + ", then pick it here")))),
  );
  const site = usable.find((site) => site.id === chosen);
  const detail = $("service-detail");
  detail.replaceChildren();
  if (event && event.tournament) {
    detail.append(el("span", { class: "name" }, "Linked to " + (event.name || event.tournament)), el("span", { class: "small" }, siteName(event.site)));
  } else if (site && site.create) {
    detail.append(el("span", { class: "small" }, "Settings for the new " + site.display_name + " tournament"));
    for (const [name, values] of Object.entries(site.create)) {
      const label = name.replace(/_/g, " ").replace(/^./, (c) => c.toUpperCase());
      const select = el("select", { class: "wd-input", "data-field": name },
        el("option", { value: "" }, "Site default"),
        ...values.map((value) => el("option", { value: JSON.stringify(value) }, value === "" ? "Standard" : String(value))));
      detail.append(el("label", { class: "wd-field" }, label, select));
    }
    $("next").textContent = "Create on " + site.display_name;
  } else if (site) {
    const picker = el("select", { class: "wd-input", id: "pick" }, el("option", { value: "" }, "Loading your events…"));
    detail.append(
      el("a", { class: "wd-btn", href: site.home, target: "_blank", rel: "noopener" }, "Create the event on " + site.display_name),
      el("label", { class: "wd-field" }, "Your event", picker),
    );
    api("GET", "/v1/organizer/tournaments?site=" + encodeURIComponent(site.id))
      .then((listed) => picker.replaceChildren(
        el("option", { value: "" }, listed.tournaments.length ? "Pick an event" : "No events found: create one first"),
        ...listed.tournaments.map((found) => el("option", { value: found.id }, found.name))))
      .catch((error) => toast(error.message));
  }
  const rounds = (event && event.settings && event.settings.rounds) || { games_to_win: 2, rules: [] };
  $("games").replaceChildren(...gamesOptions(rounds.games_to_win || 2));
  $("rules").replaceChildren(...(rounds.rules || []).map(ruleRow));
  $("hold").checked = !!(event && event.settings && event.settings.hold && event.settings.hold.streamed);
}

function ruleRow(rule = {}) {
  const row = el("div", { class: "rule" },
    el("label", { class: "wd-field" }, "Side",
      el("select", { class: "wd-input", "data-rule": "side" },
        ...["any", "winners", "losers"].map((side) => el("option", { value: side, selected: (rule.side || "any") === side }, side)))),
    el("label", { class: "wd-field" }, "From round", el("input", { class: "wd-input", "data-rule": "from_round", type: "number", min: 1, value: rule.from_round || 1 })),
    el("label", { class: "wd-field" }, "Phase (start.gg)", el("input", { class: "wd-input", "data-rule": "phase", maxlength: 64, value: rule.phase || "", placeholder: "Top 8" })),
    el("label", { class: "wd-field" }, "Games to win", el("select", { class: "wd-input", "data-rule": "games_to_win" }, ...gamesOptions(rule.games_to_win || 3))),
  );
  row.append(el("button", { type: "button", class: "wd-btn", onclick: () => row.remove() }, "Remove rule"));
  return row;
}

$("add-rule").addEventListener("click", () => $("rules").append(ruleRow()));

function settingsFromForm() {
  const rules = [...$("rules").children].map((row) => {
    const value = (name) => row.querySelector('[data-rule="' + name + '"]').value;
    const rule = { side: value("side"), from_round: Number(value("from_round")) || 1, games_to_win: Number(value("games_to_win")) };
    if (value("phase").trim()) rule.phase = value("phase").trim();
    return rule;
  });
  const settings = { rounds: { games_to_win: Number($("games").value), rules } };
  if ($("hold").checked) settings.hold = { streamed: true };
  return settings;
}

async function saveService() {
  const event = desk.event;
  await api("POST", "/v1/organizer/events/" + event.id, { settings: settingsFromForm() });
  if (!event.tournament) {
    const site = desk.accounts.find((site) => site.id === desk.picks.site);
    if (!site) throw new Error("Allow organizer access on a bracket service first.");
    if (site.create) {
      const fields = {};
      for (const select of document.querySelectorAll("[data-field]")) {
        if (select.value !== "") fields[select.dataset.field] = JSON.parse(select.value);
      }
      await api("POST", "/v1/organizer/events/" + event.id + "/create", { site: site.id, fields });
    } else {
      const picked = $("pick") && $("pick").value;
      if (!picked) throw new Error("Pick the event you made on " + site.display_name + ".");
      await api("POST", "/v1/organizer/events/" + event.id, { site: site.id, tournament: picked });
    }
  }
  await refreshEvent();
}

function linkRow(link = { label: "", url: "" }) {
  const row = el("div", { class: "wd-card" },
    el("label", { class: "wd-field" }, "Label", el("input", { class: "wd-input", "data-link": "label", maxlength: 40, value: link.label, placeholder: "Stream" })),
    el("label", { class: "wd-field" }, "Address", el("input", { class: "wd-input", "data-link": "url", type: "url", inputmode: "url", value: link.url, placeholder: "https://" })),
  );
  row.append(el("button", { type: "button", class: "wd-btn", onclick: () => row.remove() }, "Remove"));
  return row;
}

async function links() {
  const kept = (desk.event && desk.event.links) || [];
  $("links").replaceChildren(...(kept.length ? kept : [{ label: "Stream", url: "" }, { label: "Matcherino", url: "" }]).map(linkRow));
}

$("add-link").addEventListener("click", () => $("links").append(linkRow()));

async function saveLinks() {
  const list = [...$("links").children]
    .map((row) => ({ label: row.querySelector('[data-link="label"]').value.trim(), url: row.querySelector('[data-link="url"]').value.trim() }))
    .filter((link) => link.url);
  for (const link of list) {
    if (!link.url.startsWith("https://")) throw new Error(link.url + " is not an https address.");
    if (!link.label) throw new Error("Give " + link.url + " a label.");
  }
  await api("POST", "/v1/organizer/events/" + desk.event.id, { links: list });
  await refreshEvent();
}

async function post() {
  const event = desk.event;
  $("post").value = discordPost(event, siteName(event.site));
  $("ready-note").textContent = event.status === "draft"
    ? "Mark ready when the date, service and links are right: Ember runs it from its start time."
    : event.status === "ready" ? "Ready: Ember runs it from " + whenText(event.starts_at) + "." : "Running now.";
}

$("copy").addEventListener("click", async () => {
  const box = $("post");
  try {
    await navigator.clipboard.writeText(box.value);
  } catch {
    box.select();
    document.execCommand("copy");
  }
  toast("Copied. Paste it in Discord.", true);
});

async function live() {
  const event = desk.event;
  $("live").replaceChildren(
    el("div", { class: "wd-kpi" }, el("b", {}, event.status), el("span", { class: "small" }, "status")),
    el("div", { class: "wd-kpi" }, el("b", {}, whenText(event.starts_at)), el("span", { class: "small" }, "start")),
    el("div", { class: "wd-kpi" }, el("b", {}, siteName(event.site) || "—"), el("span", { class: "small" }, event.name || "no event yet")),
  );
  $("stop").textContent = event.status === "draft" ? "Delete this draft" : "Stop this tournament";
}

$("stop").addEventListener("click", async () => {
  const draft = desk.event.status === "draft";
  if (!window.confirm(draft ? "Delete this draft?" : "Stop this tournament? Ember stops creating its sets.")) return;
  await api("DELETE", "/v1/organizer/events/" + desk.event.id).catch((error) => toast(error.message));
  desk.event = null;
  home().catch((error) => toast(error.message));
});

// --- The bar.

const SAVE = [null, saveTournament, saveService, saveLinks, null, null];

$("next").addEventListener("click", async () => {
  const button = $("next");
  button.disabled = true;
  try {
    if (SAVE[desk.step - 1]) await SAVE[desk.step - 1]();
    if (desk.step === 5 && desk.event.status === "draft") {
      await api("POST", "/v1/organizer/events/" + desk.event.id + "/ready");
      await refreshEvent();
      return go(5);
    }
    if (desk.step === 6) return home();
    go(desk.step + 1);
  } catch (error) {
    toast(error.message);
  } finally {
    button.disabled = false;
  }
});

$("back").addEventListener("click", () => (desk.step === 1 ? home().catch((error) => toast(error.message)) : go(desk.step - 1)));
$("new-tournament").addEventListener("click", () => open(null));
$("sign-out").addEventListener("click", async () => {
  await api("POST", "/web/sign-out").catch(() => {});
  desk.session = null;
  signedOut();
});

async function start() {
  try {
    desk.session = await api("GET", "/web/session");
  } catch (error) {
    if (error.status === 401) return signedOut();
    return toast(error.message);
  }
  desk.accounts = (await api("GET", "/web/accounts").catch(() => ({ sites: [] }))).sites;
  home().catch((error) => toast(error.message));
}

start();
