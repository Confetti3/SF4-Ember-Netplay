// The web desk (docs/design/TOURNAMENT_DESK.md, 4a): an organizer signed in
// with a linked site account sets up a tournament, step by step, through the
// bridge's /web and /v1/organizer routes. Every change carries the session's
// CSRF token. Nothing from the bridge is ever written as HTML. Set cards and
// the bracket come from sets.js, loaded before this file.
"use strict";

const STEPS = ["Details", "Service and rules", "Discord and sign-ups", "Ready", "Live"];
const NEXT = ["Save", "Save", "Save", "Mark ready", "Your tournaments"];
// Words for a site's creation settings, by field and value.
const FIELD_NAMES = { format: "Format", grand_final: "Grand final", third_place_match: "Third place match", open_signup: "The site's own sign-up page",
  round_robin_match_tie: "Points for a drawn set", swiss_match_tie: "Points for a drawn set" };
const VALUE_NAMES = { "": "Normal, with a reset", "single match": "One match", skip: "No grand final", 3: "Yes", true: "Yes", false: "No",
  "0.0": "0 points", "0.5": "½ point", "1.0": "1 point" };
// A setting that only goes with one format, by its name's start.
const ONLY_WITH = { round_robin_: "round robin", swiss_: "swiss" };

const desk = { session: null, accounts: [], events: [], event: null, step: 1, picks: {} };
const $ = (id) => document.getElementById(id);

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
    error.reason = data.error && data.error.details && data.error.details.reason;
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

/** An error where the organizer is looking: at the top of the open step. */
function stepError(text) {
  const box = $("step-error");
  box.textContent = text || "";
  box.hidden = !text;
  if (text) box.scrollIntoView({ block: "nearest" });
}

function show(page) {
  for (const id of ["signed-out", "home", "wizard"]) $(id).hidden = id !== page;
  $("sign-out").hidden = page === "signed-out";
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

// Signing in on this device from Ember: the bridge gives this browser a
// six-digit code and a secret cookie. The code stays hidden until asked for,
// off stream. Once Ember has read the code, this page shows a number to pick
// there; once it is approved, this browser (and only it) is signed in.
const device = { timer: 0, until: 0 };

function deviceShows(code, number) {
  $("device-hidden").hidden = Boolean(code);
  $("device-code").hidden = !code || Boolean(number);
  $("device-code").textContent = codeText(code);
  $("device-pick").hidden = !number;
  $("device-number").textContent = number || "";
  $("device-new").hidden = !code;
}

async function newCode() {
  clearInterval(device.timer);
  $("device-error").hidden = true;
  try {
    const made = await api("POST", "/web/device");
    deviceShows(made.code, 0);
    device.until = made.expires_at;
    device.timer = setInterval(pollCode, 3000);
    pollCode();
  } catch (error) {
    $("device-error").textContent = error.status === 429 ? "Too many codes. Wait a few minutes." : error.message;
    $("device-error").hidden = false;
  }
}

async function pollCode() {
  const left = Math.max(0, device.until - Math.floor(Date.now() / 1000));
  $("device-left").textContent = left ? "Works for " + Math.floor(left / 60) + ":" + String(left % 60).padStart(2, "0") + " more." : "";
  if ($("signed-out").hidden || document.hidden) return;
  try {
    const read = await api("GET", "/web/device");
    if (read.signed_in) {
      clearInterval(device.timer);
      toast("Signed in.", true);
      start();
    } else if (read.pick) {
      deviceShows($("device-code").textContent.replace(" ", ""), read.pick);
    }
  } catch {
    clearInterval(device.timer);
    deviceShows("", 0);
    $("device-new").hidden = false;
    $("device-left").textContent = "That code expired or was turned down. Get a new one.";
  }
}

$("device-show").addEventListener("click", newCode);
$("device-new").addEventListener("click", newCode);

async function signedOut() {
  show("signed-out");
  clearInterval(device.timer);
  deviceShows("", 0);
  $("device-left").textContent = "";
  const list = $("sign-in-sites");
  fill(list);
  const capabilities = await api("GET", "/v1/capabilities").catch(() => ({}));
  for (const site of capabilities.sign_in_sites || []) {
    if (!site.sign_in) continue;
    list.append(el("a", { class: "wd-btn", href: "/web/sign-in/" + encodeURIComponent(site.id) }, "Sign in with " + site.display_name));
  }
  if (!list.children.length) list.append(el("p", { class: "note" }, "This Ember service offers no web sign-in."));
}

async function home() {
  show("home");
  $("brand-sub").textContent = "Tournament desk";
  accounts().catch((error) => toast(error.message));
  // An organizer not invited yet, on a bridge that takes organizers by
  // invitation, is told how to get in; a bridge without the route takes all.
  const access = await api("GET", "/v1/organizer/access").catch(() => ({ invite_only: false, allowed: true }));
  const invited = !access.invite_only || access.allowed;
  $("invite").hidden = invited;
  $("new-tournament").hidden = !invited;
  desk.events = (await api("GET", "/v1/organizer/events")).events;
  const list = $("events");
  fill(list,
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
  go(!event || event.status === "draft" ? 1 : 5);
}

async function refreshEvent() {
  if (!desk.event) return;
  desk.events = (await api("GET", "/v1/organizer/events")).events;
  desk.event = desk.events.find((event) => event.id === desk.event.id) || null;
}

// --- The steps.

function go(step) {
  clearInterval(view.timer);
  desk.step = Math.min(STEPS.length, Math.max(1, step));
  toast("");
  stepError("");
  for (const section of document.querySelectorAll("[data-step]")) section.hidden = Number(section.dataset.step) !== desk.step;
  fill($("step-now"), "Step " + desk.step + " of " + STEPS.length + " · ", el("b", {}, STEPS[desk.step - 1]));
  fill($("steps"),
    ...STEPS.map((label, index) =>
      el("button", {
        type: "button",
        class: "wd-step" + (index + 1 < desk.step ? " done" : ""),
        "aria-label": label,
        "aria-current": index + 1 === desk.step ? "step" : false,
        disabled: !desk.event && index > 0,
        onclick: () => go(index + 1),
      }, el("span", { class: "dot" }, String(index + 1)), el("span", { class: "label" }, label))),
  );
  $("brand-sub").textContent = desk.event ? "Tournament desk · " + desk.event.title : "Tournament desk · New tournament";
  $("next").textContent = desk.step === 4 && desk.event && desk.event.status !== "draft" ? "Continue" : NEXT[desk.step - 1];
  $("next").disabled = false;
  summary();
  [details, service, liveSettings, post, live][desk.step - 1]().catch((error) => stepError(error.message));
}

function summary() {
  const event = desk.event;
  fill($("summary"),
    el("span", { class: "name" }, event ? event.title : "New tournament"),
    el("span", { class: "small" }, event ? event.status + " · " + whenText(event.starts_at) : "Not saved yet"),
    el("span", { class: "small" }, event && event.site ? siteName(event.site) + (event.name ? " · " + event.name : "") : "No bracket service yet"),
    el("span", { class: "small" }, (event ? (event.links || []).length : 0) + " links"),
  );
}

async function accounts() {
  desk.accounts = (await api("GET", "/web/accounts")).sites;
  fill($("accounts"),
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
  if (document.visibilityState === "visible" && !$("home").hidden) accounts().catch(() => {});
});

async function details() {
  const event = desk.event;
  $("title").value = event ? event.title : "";
  [$("date").value, $("time").value] = localParts(event && event.starts_at);
  const kept = (event && event.links) || [];
  fill($("links"), ...(kept.length ? kept : [{ label: "Stream", url: "" }, { label: "Matcherino", url: "" }]).map(linkRow));
}

function startsAt() {
  const date = $("date").value;
  const time = $("time").value;
  if (!date) return null;
  if (!time) throw new Error("Give the start time too, or leave the date empty.");
  return Math.floor(new Date(date + "T" + time).getTime() / 1000);
}

async function saveDetails() {
  const title = $("title").value.trim();
  const wrong = titleError(title);
  if (wrong) throw new Error(wrong);
  const starts_at = startsAt();
  const links = linksFromForm();
  if (!desk.event) {
    const made = await api("POST", "/v1/organizer/drafts", { title });
    desk.event = { id: made.id };
    await api("POST", "/v1/organizer/events/" + made.id, { starts_at, links });
  } else {
    await api("POST", "/v1/organizer/events/" + desk.event.id, { title, starts_at, links });
  }
  await refreshEvent();
}

/** One tier: a stage and its first-to, mobile first: two selects and Remove. */
function tierRow(tier = { from: "top_8", games_to_win: 3 }) {
  const row = el("div", { class: "wd-row tier" },
    el("label", { class: "wd-field" }, "From",
      el("select", { class: "wd-input", "data-tier": "from" }, ...STAGES.map(([id, label]) => el("option", { value: id, selected: id === tier.from }, label)))),
    el("label", { class: "wd-field" }, "first to", el("select", { class: "wd-input", "data-tier": "games" }, ...gamesOptions(tier.games_to_win))),
  );
  row.append(el("button", { type: "button", class: "wd-btn", "aria-label": "Remove this tier", onclick: () => row.remove() }, "Remove"));
  return row;
}

$("add-tier").addEventListener("click", () => {
  const used = [...$("tiers").querySelectorAll('[data-tier="from"]')].map((select) => select.value);
  const next = ["top_8", "semi_finals", "finals", "grand_final", "top_16"].find((id) => !used.includes(id)) || "top_8";
  $("tiers").append(tierRow({ from: next, games_to_win: 3 }));
});

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
  fill($("services"),
    el("legend", {}, "Bracket service"),
    ...usable.map((site) =>
      el("label", { class: "choice" },
        el("input", { type: "radio", name: "service", value: site.id, checked: site.id === chosen, disabled: !!(event && event.tournament), onchange: () => { desk.picks.site = site.id; service(); } }),
        el("span", {}, el("span", { class: "name" }, site.display_name), el("br"),
          el("span", { class: "small" }, site.create ? "Ember creates it for you" : "Create it on " + site.display_name + ", then pick it here")))),
  );
  const site = usable.find((site) => site.id === chosen);
  // Only the chosen service's own settings: Challonge's creation fields,
  // or start.gg's event and stream holding.
  const detail = $("service-detail");
  fill(detail);
  if (event && event.tournament) {
    detail.append(el("span", { class: "name" }, "Linked to " + (event.name || event.tournament)), el("span", { class: "small" }, siteName(event.site)));
  } else if (site && site.create) {
    detail.append(el("span", { class: "small" }, "Settings for the new " + site.display_name + " tournament. Saving creates it there."));
    for (const [name, values] of Object.entries(site.create)) {
      const only = Object.entries(ONLY_WITH).find(([start]) => name.startsWith(start));
      // Draw points default to half a point, which Ember sends with the format.
      const first = only ? el("option", { value: "" }, "½ point") : el("option", { value: "" }, "As " + site.display_name + " sets it");
      const select = el("select", { class: "wd-input", "data-field": name }, first,
        ...values.filter((value) => !only || value !== "0.5").map((value) => el("option", { value: JSON.stringify(value) }, VALUE_NAMES[String(value)] || String(value).replace(/^./, (c) => c.toUpperCase()))));
      const label = el("label", { class: "wd-field", "data-only": only ? only[1] : "" }, FIELD_NAMES[name] || name.replace(/_/g, " "), select);
      detail.append(label);
    }
    // Draw points belong to round robin and swiss alone: shown with them.
    const format = detail.querySelector('[data-field="format"]');
    const showOnly = () => detail.querySelectorAll("[data-only]").forEach((label) => {
      const wanted = label.dataset.only;
      const shown = !wanted || (format && format.value === JSON.stringify(wanted));
      label.hidden = !shown;
      if (!shown) label.querySelector("select").value = "";
    });
    if (format) format.addEventListener("change", showOnly);
    showOnly();
  } else if (site) {
    const picker = el("select", { class: "wd-input", id: "pick" }, el("option", { value: "" }, "Loading your events…"));
    detail.append(
      el("a", { class: "wd-btn", href: site.home, target: "_blank", rel: "noopener" }, "Create the event on " + site.display_name),
      el("label", { class: "wd-field" }, "Your event", picker),
    );
    api("GET", "/v1/organizer/tournaments?site=" + encodeURIComponent(site.id))
      .then((listed) => fill(picker,
        el("option", { value: "" }, listed.tournaments.length ? "Pick an event" : "No events found: create one first"),
        ...listed.tournaments.map((found) => el("option", { value: found.id }, found.name))))
      .catch((error) => stepError(error.message));
  }
  $("next").textContent = site && site.create && !(event && event.tournament) ? "Create on " + site.display_name : "Save";
  const settings = (event && event.settings) || {};
  const rounds = settings.rounds || { games_to_win: 2 };
  fill($("games"), ...gamesOptions(rounds.games_to_win || 2));
  fill($("tiers"), ...(rounds.tiers || []).map(tierRow));
  // ponytail: holding sets on stream is start.gg's, the one service Ember
  // attaches to rather than creates on; a flag in /web/accounts if another such site comes.
  fill($("hold-row"), ...(site && !site.create
    ? [el("label", { class: "check" }, "Hold sets " + site.display_name + " puts on a stream", el("input", { type: "checkbox", id: "hold", checked: !!(settings.hold && settings.hold.streamed) }))]
    : []));
}

function settingsFromForm() {
  const kept = (desk.event && desk.event.settings && desk.event.settings.rounds) || {};
  const tiers = sortTiers([...$("tiers").children]
    .map((row) => ({ from: row.querySelector('[data-tier="from"]').value, games_to_win: Number(row.querySelector('[data-tier="games"]').value) })));
  const rounds = { games_to_win: Number($("games").value), rules: kept.rules || [], tiers };
  const settings = { rounds };
  if ($("hold") && $("hold").checked) settings.hold = { streamed: true };
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
  const row = el("div", { class: "link-row" },
    el("label", { class: "wd-field" }, "Label", el("input", { class: "wd-input", "data-link": "label", maxlength: 40, value: link.label, placeholder: "Stream" })),
    el("label", { class: "wd-field" }, "Address", el("input", { class: "wd-input", "data-link": "url", type: "url", inputmode: "url", value: link.url, placeholder: "https://" })),
  );
  row.append(el("button", { type: "button", class: "wd-btn", onclick: () => row.remove() }, "Remove"));
  return row;
}

$("add-link").addEventListener("click", () => $("links").append(linkRow()));

function linksFromForm() {
  const list = [...$("links").children]
    .map((row) => ({ label: row.querySelector('[data-link="label"]').value.trim(), url: row.querySelector('[data-link="url"]').value.trim() }))
    .filter((link) => link.url);
  for (const link of list) {
    if (!link.url.startsWith("https://")) throw new Error(link.url + " is not an https address.");
    if (!link.label) throw new Error("Give " + link.url + " a label.");
  }
  return list;
}

async function post() {
  const event = desk.event;
  $("post").value = discordPost(event, siteName(event.site), location.origin);
  const missing = [!event.starts_at && "a start date", !event.tournament && "its bracket event (step 2)"].filter(Boolean);
  $("ready-note").textContent = event.status !== "draft"
    ? event.status === "ready" ? "Ready: Ember runs it from " + whenText(event.starts_at) + "." : "Running now."
    : missing.length ? "To mark it ready it still needs " + missing.join(" and ") + "." : "Mark ready when everything is right: Ember runs it from its start time.";
  $("next").disabled = event.status === "draft" && missing.length > 0;
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

// --- Live: the sets, the bracket and the event's settings, read again every
// ten seconds while the step is open.

const view = { tab: "Sets", filter: "All", phase: null, sets: [], timer: 0 };

async function live() {
  clearInterval(view.timer);
  $("start-bracket").hidden = true;
  $("stop").textContent = desk.event.status === "draft" ? "Delete this draft" : "Stop this tournament";
  if (desk.event.status !== "running") {
    fill($("live"), el("div", { class: "wd-kpi" }, el("b", {}, desk.event.status), el("span", { class: "small" }, whenText(desk.event.starts_at))));
    fill($("live-tabs"));
    fill($("live-body"), el("p", { class: "note" }, "The sets show here once the tournament runs."));
    return;
  }
  await readSets();
  view.timer = setInterval(() => {
    if (desk.step === 5 && !$("wizard").hidden && !document.hidden) readSets().catch(() => {});
  }, 10000);
}

async function readSets() {
  const read = await api("GET", "/v1/organizer/events/" + desk.event.id + "/sets");
  view.sets = read.sets;
  // Until the site has the bracket's sets, the organizer can start it here.
  $("start-bracket").hidden = !desk.event.starts_on_site || view.sets.length > 0;
  $("start-bracket").textContent = "Start the bracket on " + siteName(desk.event.site);
  const count = tally(view.sets);
  fill($("live"),
    el("div", { class: "wd-kpi bad" }, el("b", {}, String(count.action)), el("span", { class: "small" }, "need you")),
    el("div", { class: "wd-kpi live" }, el("b", {}, String(count.live)), el("span", { class: "small" }, "live")),
    el("div", { class: "wd-kpi ok" }, el("b", {}, count.done + "/" + count.total), el("span", { class: "small" }, "done")),
  );
  drawLive();
}

function drawLive() {
  fill($("live-tabs"), ...["Sets", "Bracket"].map((tab) =>
    el("button", { type: "button", class: "chip", role: "tab", "aria-selected": String(view.tab === tab), onclick: () => { view.tab = tab; drawLive(); } }, tab)));
  if (view.tab === "Bracket") {
    const picked = { phase: view.phase, redraw: (phase) => { view.phase = phase; drawLive(); } };
    fill($("live-body"), bracketView(view.sets, picked));
    return;
  }
  const shown = view.sets.filter(FILTERS[view.filter]);
  fill($("live-body"),
    el("div", { class: "chips" }, Object.keys(FILTERS).map((name) =>
      el("button", { type: "button", class: "chip", "aria-pressed": String(view.filter === name), onclick: () => { view.filter = name; drawLive(); } }, name))),
    shown.length ? el("div", { class: "wd-grid" }, shown.map(liveCard)) : el("p", { class: "note" }, "No sets here."),
  );
}

function liveCard(set) {
  const actions = [];
  const done = set.desk_state === "done";
  if (!done) {
    const held = set.desk_state === "held";
    actions.push(el("button", { type: "button", class: "wd-btn", onclick: () => mark(set, { held: !held }, held ? "Let this set be played in Ember?" : "Hold this set? Ember stops creating its match.") }, held ? "Release" : "Hold"));
    actions.push(el("button", { type: "button", class: "wd-btn", onclick: () => mark(set, { call: true }, "Call this set again on Discord?") }, "Call again"));
  }
  if (set.match_id) actions.push(el("button", { type: "button", class: "wd-btn" + (NEEDS_ACTION.includes(set.desk_state) ? " primary" : ""), onclick: () => decide(set) }, "Games"));
  const card = setCard(set, ...actions);
  if (set.called_at || set.call) {
    const posted = set.call ? { delivered: "posted on Discord", pending: "posting to Discord", failed: "Discord refused it" }[set.call.posted] : "";
    card.insertBefore(el("span", { class: "small" }, "Called " + whenText(set.called_at || set.call.at) + (posted ? ", " + posted : "")), card.lastChild);
  }
  if (dmText(set)) card.insertBefore(el("span", { class: "small" }, dmText(set)), card.lastChild);
  return card;
}

async function mark(set, change, question) {
  if (!window.confirm(question)) return;
  try {
    await api("POST", "/v1/organizer/events/" + desk.event.id + "/sets/" + encodeURIComponent(set.id), change);
    await readSets();
  } catch (error) {
    toast(error.message);
  }
}

// --- Deciding a set's games: a played game waiting for a result gets a
// winner, an accepted one can be voided, and a missing one added.

const OUTCOMES = { p1_win: 0, p2_win: 1 };

function decide(set) {
  const box = $("decide");
  const [p1, p2] = players(set);
  const reason = el("input", { class: "wd-input", maxlength: 200, value: "Decided by the organizer", "aria-label": "Reason" });
  const send = async (command, question) => {
    if (!window.confirm(question)) return;
    try {
      await api("POST", "/v1/organizer/events/" + desk.event.id + "/matches/" + set.match_id + "/adjudications",
        { ...command, reason: reason.value.trim() || "Decided by the organizer", expected_revision: set.revision });
      box.close();
      await readSets();
    } catch (error) {
      toast(error.message);
      box.close();
    }
  };
  const games = (set.games || []).map((game, index) => {
    const label = "Game " + (index + 1);
    if (game.state !== "accepted") {
      return el("div", { class: "wd-card" }, el("span", { class: "name" }, label + ": waiting for a result"),
        el("div", { class: "wd-row" }, [p1, p2].map((name, slot) =>
          el("button", { type: "button", class: "wd-btn", onclick: () => send({ kind: "game_result", winner_slot: slot, attempt_id: game.attempt_id }, name + " wins " + label.toLowerCase() + "?") }, name + " wins")),
          el("button", { type: "button", class: "wd-btn", onclick: () => send({ kind: "game_result", draw: true, attempt_id: game.attempt_id }, label + " is a draw? It is replayed and scores for nobody.") }, "Draw")));
    }
    const winner = OUTCOMES[game.outcome] === 1 ? p2 : OUTCOMES[game.outcome] === 0 ? p1 : "Draw (replayed)";
    return el("div", { class: "wd-card" }, el("div", { class: "set-row" }, el("span", { class: "name" }, label), el("span", {}, winner)),
      el("button", { type: "button", class: "wd-btn danger", onclick: () => send({ kind: "void_game", attempt_id: game.attempt_id }, "Void " + label.toLowerCase() + "? It no longer counts.") }, "Void"));
  });
  const waiting = (set.games || []).some((game) => game.state !== "accepted");
  fill(box, [
    el("h2", { class: "wd-h" }, p1 + " vs " + p2),
    el("label", { class: "wd-field" }, "Reason (kept with the decision)", reason),
    ...games,
    waiting || set.desk_state === "done" ? null : el("div", { class: "wd-card" }, el("span", { class: "name" }, "Add a game"),
      el("div", { class: "wd-row" }, [p1, p2].map((name, slot) =>
        el("button", { type: "button", class: "wd-btn", onclick: () => send({ kind: "game_result", winner_slot: slot }, "Add a game won by " + name + "?") }, name + " won")))),
    el("button", { type: "button", class: "wd-btn", onclick: () => box.close() }, "Close"),
  ]);
  box.showModal();
}

// --- The event's settings: Discord, results, the public page and sign-ups.

async function liveSettings() {
  const event = desk.event;
  const page = location.origin + "/t/" + event.id;
  const hook = el("input", { class: "wd-input", type: "url", inputmode: "url", placeholder: event.discord_webhook ? "Set: " + event.discord_webhook : "https://discord.com/api/webhooks/…", "aria-label": "Discord webhook" });
  const check = (label, on) => {
    const box = el("input", { type: "checkbox", checked: on });
    return [el("label", { class: "check" }, label, box), box];
  };
  const [resultsRow, results] = check("Post each set's score on Discord", event.post_results);
  const [publicRow, open] = check("Public page anyone can follow", event.public);
  const [signRow, signs] = check("Players sign up through Ember (" + (event.signed_up || 0) + " signed up)", event.sign_ups);
  fill($("live-settings"),
    el("h2", { class: "name" }, "Settings"),
    el("label", { class: "wd-field" }, "Discord channel webhook (calls are posted there)", hook),
    event.discord_webhook ? el("button", { type: "button", class: "wd-btn", onclick: () => saveLive({ discord_webhook: null }) }, "Remove the webhook") : null,
    resultsRow, publicRow,
    event.public ? el("a", { href: page, target: "_blank", rel: "noopener", class: "small" }, page) : null,
    signRow,
    el("p", { class: "note" }, "Sign-ups put the public page on too, since players sign up there."),
  );
  desk.settingsForm = () => {
    const change = { post_results: results.checked, public: open.checked || signs.checked, sign_ups: signs.checked };
    if (hook.value.trim()) change.discord_webhook = hook.value.trim();
    return change;
  };
}

async function saveLive(change) {
  try {
    await api("POST", "/v1/organizer/events/" + desk.event.id, change);
    await refreshEvent();
    toast("Saved.", true);
    liveSettings();
  } catch (error) {
    stepError(error.message);
  }
}

async function saveSettings() {
  await api("POST", "/v1/organizer/events/" + desk.event.id, desk.settingsForm());
  await refreshEvent();
}

$("start-bracket").addEventListener("click", async () => {
  if (!window.confirm("Start the bracket on " + siteName(desk.event.site) + "? Its seeding is fixed from then on.")) return;
  try {
    await api("POST", "/v1/organizer/events/" + desk.event.id + "/start");
    toast("Bracket started. Its sets show here once Ember reads them.", true);
    await readSets();
  } catch (error) {
    toast(error.message);
  }
});

// Deleting a draft or stopping an event; one Ember created on its site
// can go there too, which the organizer chooses each time.
$("stop").addEventListener("click", () => {
  const event = desk.event;
  const box = $("decide");
  const draft = event.status === "draft";
  const site = siteName(event.site);
  const end = async (onSite) => {
    try {
      await api("DELETE", "/v1/organizer/events/" + event.id, onSite ? { on_site: true } : {});
    } catch (error) {
      return toast(error.message);
    }
    box.close();
    desk.event = null;
    home().catch((error) => toast(error.message));
  };
  const button = (label, onSite, danger) =>
    el("button", { type: "button", class: danger ? "wd-btn danger" : "wd-btn", onclick: () => end(onSite) }, label);
  fill(box, [
    el("h2", { class: "wd-h" }, draft ? "Delete this draft?" : "Stop this tournament?"),
    el("p", { class: "small" }, draft ? "Its settings and links go." : "Ember stops creating its sets."),
    event.created_on_site ? el("p", { class: "small" }, "Ember created it on " + site + ". Deleting it there too removes its bracket and results for everyone.") : null,
    button(draft ? "Delete the draft" : "Stop it", false, !event.created_on_site),
    event.created_on_site ? button((draft ? "Delete it" : "Stop it") + " and delete it on " + site, true, true) : null,
    el("button", { type: "button", class: "wd-btn", onclick: () => box.close() }, "Keep it"),
  ]);
  box.showModal();
});

// --- The bar.

const SAVE = [saveDetails, saveService, saveSettings, null, null];

$("next").addEventListener("click", async () => {
  const button = $("next");
  button.disabled = true;
  try {
    if (SAVE[desk.step - 1]) await SAVE[desk.step - 1]();
    if (desk.step === 4 && desk.event.status === "draft") {
      await api("POST", "/v1/organizer/events/" + desk.event.id + "/ready");
      await refreshEvent();
      return go(4);
    }
    if (desk.step === 5) return home();
    go(desk.step + 1);
  } catch (error) {
    stepError(stepErrorText(error));
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
  clearInterval(device.timer);
  try {
    desk.session = await api("GET", "/web/session");
  } catch (error) {
    if (error.status === 401) return signedOut();
    return toast(error.message);
  }
  // Signed in from an event's public page: back to it.
  const back = sessionStorage.getItem("ember.return");
  if (back && back.startsWith("/t/")) {
    sessionStorage.removeItem("ember.return");
    return location.replace(back);
  }
  desk.accounts = (await api("GET", "/web/accounts").catch(() => ({ sites: [] }))).sites;
  home().catch((error) => toast(error.message));
}

start();
