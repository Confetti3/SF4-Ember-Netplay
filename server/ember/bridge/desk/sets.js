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
