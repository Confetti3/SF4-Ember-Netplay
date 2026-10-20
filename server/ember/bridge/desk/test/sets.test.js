// The desk's own logic, without a browser: `node --test server/ember/bridge/desk/test`.
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const desk = require("../sets.js");

test("a title is 1 to 60 characters, as Challonge takes them", () => {
  assert.equal(desk.titleError("  "), "Give the tournament a title.");
  assert.equal(desk.titleError("x".repeat(60)), "");
  assert.match(desk.titleError("x".repeat(61)), /60 characters/);
  // Characters, not bytes: 60 emoji fit.
  assert.equal(desk.titleError("🔥".repeat(60)), "");
});

test("tiers come once per stage, widest stage first", () => {
  const sorted = desk.sortTiers([
    { from: "grand_final", games_to_win: 5 },
    { from: "top_8", games_to_win: 3 },
    { from: "finals", games_to_win: 3 },
    { from: "top_8", games_to_win: 4 },
  ]);
  assert.deepEqual(sorted.map((tier) => tier.from), ["top_8", "finals", "grand_final"]);
  // The first tier picked for a stage is the one kept.
  assert.equal(sorted[0].games_to_win, 3);
  assert.equal(desk.stageName("quarter_finals"), "Quarter-finals");
  assert.equal(desk.stageName("unknown"), "unknown");
});

test("the Discord post carries what is set and leaves the rest out", () => {
  const event = {
    id: "oev_1",
    title: "Ember Weekly #13",
    starts_at: 1793041200,
    public: true,
    sign_ups: true,
    settings: { rounds: { games_to_win: 2, tiers: [{ from: "top_8", games_to_win: 3 }, { from: "grand_final", games_to_win: 5 }] } },
    links: [{ label: "Stream", url: "https://twitch.tv/x" }, { label: "Odd", url: "https://example.com" }],
  };
  const post = desk.discordPost(event, "Challonge", "https://bridge.test").split("\n");
  assert.deepEqual(post, [
    "**Ember Weekly #13** — USF4 on Ember",
    "📅 <t:1793041200:F> (<t:1793041200:R>)",
    "🎮 Challonge · FT2 · Top 8 FT3 · Grand final FT5",
    "📺 Stream: https://twitch.tv/x",
    "🔗 Odd: https://example.com",
    "📝 Sign up and follow it live: https://bridge.test/t/oev_1",
    "Play on Ember: https://embernetplay.link",
  ]);
  const bare = desk.discordPost({ id: "oev_2", title: "Draft" }, "", "https://bridge.test").split("\n");
  assert.deepEqual(bare, ["**Draft** — USF4 on Ember", "Play on Ember: https://embernetplay.link"]);
});

test("only a needs_game_proof refusal says to open the desk from Ember", () => {
  assert.match(desk.stepErrorText({ reason: "needs_game_proof", message: "Make this change in Ember." }), /open the web desk from there/);
  assert.equal(desk.stepErrorText({ message: "Challonge refused: name too long" }), "Challonge refused: name too long");
});

test("a code reads in two groups of three", () => {
  assert.equal(desk.codeText("123456"), "123 456");
  assert.equal(desk.codeText(""), "");
});

const sets = [
  { side: "winners", round: 1, round_label: "Winners Round 1", desk_state: "done", score: [2, 0], draws: 1 },
  { side: "winners", round: 2, round_label: "Winners Final", desk_state: "playing", score: [1, 1] },
  { side: "losers", round: -1, round_label: "Losers Round 1", desk_state: "needs_review" },
  { side: "grand_final", round: 3, round_label: "Grand Final", desk_state: "not_ready", phase: "Top 8" },
  { side: "losers", round: -2, desk_state: "called", players: ["A"] },
];

test("the desk's tiles count what needs the organizer, what is live and what is done", () => {
  assert.deepEqual(desk.tally(sets), { action: 1, live: 2, done: 1, total: 5 });
  assert.equal(sets.filter(desk.FILTERS.Losers).length, 2);
  assert.deepEqual(desk.players(sets[4]), ["A", "TBD"]);
  assert.equal(desk.roundName(sets[4]), "Round -2");
});

test("the bracket groups by phase, then winners, losers and grand final, by round", () => {
  const phases = desk.bracketOf(sets);
  assert.deepEqual(phases.map((phase) => phase.phase), ["Bracket", "Top 8"]);
  assert.deepEqual(phases[0].rounds.map((round) => round.name), ["Winners Round 1", "Winners Final", "Losers Round 1", "Round -2"]);
  assert.equal(phases[1].rounds[0].side, "grand_final");
});

test("the set card says who got the match link privately", () => {
  const set = { players: ["Ashgrove", "Minnow"], dms: ["delivered", "no_discord"] };
  assert.equal(desk.dmText(set), "Private message: Ashgrove: sent; Minnow: no Discord linked in Ember");
  assert.match(desk.dmText({ players: ["A", "B"], dms: ["failed", null] }), /^Private message: A: Discord refused it/);
  assert.equal(desk.dmText({ players: ["A", "B"] }), "");
});
