// A desk event's sets, shared by the web desk (desk.js) and an event's public
// page (public.js): set cards, the filters, and the bracket by phase, side
// and round. Nothing from the bridge is ever written as HTML.
"use strict";

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

// The stages a longer set length can start from, widest first.
const STAGES = [["top_32", "Top 32"], ["top_24", "Top 24"], ["top_16", "Top 16"], ["top_12", "Top 12"], ["top_8", "Top 8"],
  ["quarter_finals", "Quarter-finals"], ["top_6", "Top 6"], ["top_4", "Top 4"], ["semi_finals", "Semi-finals"], ["finals", "Finals"],
  ["grand_final", "Grand final"]];
const stageName = (from) => (STAGES.find(([id]) => id === from) || [from, from])[1];
const LINK_ICONS = { bracket: "🏆", "sign up": "📝", signup: "📝", stream: "📺", twitch: "📺", matcherino: "💰", rules: "📜", discord: "💬" };
// Challonge refuses a longer tournament name; the bridge holds titles to it too.
const TITLE_MAX = 60;

/** Why a title can't be saved, or "" when it can. */
function titleError(title) {
  const length = [...title.trim()].length;
  if (!length) return "Give the tournament a title.";
  if (length > TITLE_MAX) return "Keep the title to " + TITLE_MAX + " characters: Challonge refuses a longer name.";
  return "";
}

/** Tiers once per stage, widest stage first. */
function sortTiers(tiers) {
  const order = (from) => STAGES.findIndex(([id]) => id === from);
  return tiers
    .filter((tier, index, all) => all.findIndex((other) => other.from === tier.from) === index)
    .sort((a, b) => order(a.from) - order(b.from));
}

/** The Discord post for an event: one message, lines without a value left out. */
function discordPost(event, siteName, origin) {
  const rounds = (event.settings && event.settings.rounds) || {};
  const tiers = (rounds.tiers || []).map((tier) => stageName(tier.from) + " FT" + tier.games_to_win);
  const ft = rounds.games_to_win
    ? "🎮 " + [siteName, "FT" + rounds.games_to_win, ...tiers].filter(Boolean).join(" · ")
    : siteName ? "🎮 " + siteName : "";
  const when = event.starts_at ? "📅 <t:" + event.starts_at + ":F> (<t:" + event.starts_at + ":R>)" : "";
  const links = (event.links || []).map((link) => (LINK_ICONS[link.label.toLowerCase()] || "🔗") + " " + link.label + ": " + link.url);
  const page = event.public ? origin + "/t/" + event.id : "";
  const follow = page ? (event.sign_ups ? "📝 Sign up and follow it live: " : "👀 Follow it live: ") + page : "";
  return ["**" + event.title + "** — USF4 on Ember", when, ft, ...links, follow, "Play on Ember: https://embernetplay.link"]
    .filter(Boolean)
    .join("\n");
}

/** A desk error as the open step shows it. */
function stepErrorText(error) {
  return error.reason === "needs_game_proof" ? error.message + " Then open the web desk from there." : error.message;
}

/** A sign-in code as shown: "123 456". */
function codeText(code) {
  return code ? code.slice(0, 3) + " " + code.slice(3) : "";
}

/** What each desk state reads as, and the tone it shows in. */
const STATES = {
  needs_review: ["Needs review", "bad"],
  needs_organizer: ["Needs you", "bad"],
  playing: ["Live", "live"],
  called: ["Called", "wait"],
  reporting: ["Reporting", "wait"],
  held: ["Held", "wait"],
  waiting: ["Waiting", ""],
  not_ready: ["Not ready", ""],
  done: ["Done", "ok"],
};

const NEEDS_ACTION = ["needs_review", "needs_organizer"];
const LIVE = ["playing", "called", "reporting"];

/** The filters over a set list, by name. */
const FILTERS = {
  All: () => true,
  Winners: (set) => set.side === "winners",
  Losers: (set) => set.side === "losers",
  "Needs action": (set) => NEEDS_ACTION.includes(set.desk_state),
  Live: (set) => LIVE.includes(set.desk_state),
};

/** Counts for the desk's tiles. */
function tally(sets) {
  return {
    action: sets.filter(FILTERS["Needs action"]).length,
    live: sets.filter(FILTERS.Live).length,
    done: sets.filter((set) => set.desk_state === "done").length,
    total: sets.length,
  };
}

function players(set) {
  const names = set.players || [];
  return [names[0] || "TBD", names[1] || "TBD"];
}

function roundName(set) {
  return set.round_label || (set.round ? "Round " + set.round : "Set");
}

/** One set as a card: round, players, score and state, then any actions. */
function setCard(set, ...actions) {
  const [state, tone] = STATES[set.desk_state] || [set.desk_state, ""];
  const [p1, p2] = players(set);
  const score = set.score || [];
  return el("div", { class: "wd-card set" + (NEEDS_ACTION.includes(set.desk_state) ? " act" : "") },
    el("span", { class: "small" }, [roundName(set), set.phase].filter(Boolean).join(" · ")),
    el("div", { class: "set-row" }, el("span", { class: "name" }, p1), el("b", {}, score.length ? String(score[0]) : "")),
    el("div", { class: "set-row" }, el("span", { class: "name" }, p2), el("b", {}, score.length ? String(score[1]) : "")),
    set.draws ? el("span", { class: "small" }, set.draws === 1 ? "1 draw, replayed" : set.draws + " draws, replayed") : null,
    el("span", { class: "status " + tone }, state),
    actions.length ? el("div", { class: "wd-row" }, actions) : null);
}

/** Sets grouped by phase, then side (winners, losers, grand final), then round. */
function bracketOf(sets) {
  const order = { winners: 0, losers: 1, grand_final: 2 };
  const phases = new Map();
  for (const set of sets) {
    const phase = set.phase || "Bracket";
    if (!phases.has(phase)) phases.set(phase, new Map());
    const key = set.side + "\u0000" + roundName(set);
    const rounds = phases.get(phase);
    if (!rounds.has(key)) rounds.set(key, { side: set.side, name: roundName(set), round: Math.abs(set.round || 0), sets: [] });
    rounds.get(key).sets.push(set);
  }
  return [...phases].map(([phase, rounds]) => ({
    phase,
    rounds: [...rounds.values()].sort((a, b) => order[a.side] - order[b.side] || a.round - b.round),
  }));
}

/** The bracket view: tabs per phase, and each round a section that folds. */
function bracketView(sets, picked) {
  const phases = bracketOf(sets);
  if (!phases.length) return el("p", { class: "note" }, "No sets yet.");
  const current = phases.find((phase) => phase.phase === picked.phase) || phases[0];
  const wide = window.innerWidth >= 900;
  const tabs = phases.length > 1
    ? el("div", { class: "chips", role: "tablist" }, phases.map((phase) =>
      el("button", { type: "button", class: "chip", role: "tab", "aria-selected": String(phase === current), onclick: () => picked.redraw(phase.phase) }, phase.phase)))
    : null;
  const sides = { winners: "Winners", losers: "Losers", grand_final: "Grand final" };
  return el("div", { class: "bracket" }, tabs,
    el("div", { class: "rounds" }, current.rounds.map((round) => {
      const open = wide || round.sets.some((set) => set.desk_state !== "done" && set.desk_state !== "not_ready");
      return el("details", { class: "round " + round.side, open },
        el("summary", {}, el("span", { class: "side" }, sides[round.side] || ""), " ", round.name),
        round.sets.map((set) => setCard(set)));
    })));
}

// Node's test runner loads this file too (desk/test); a browser has no module.
if (typeof module !== "undefined") {
  module.exports = { STAGES, stageName, TITLE_MAX, titleError, sortTiers, discordPost, stepErrorText, codeText, STATES, FILTERS, tally, players, roundName, bracketOf };
}
