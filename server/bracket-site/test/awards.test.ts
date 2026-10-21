import assert from "node:assert/strict";
import { test } from "node:test";

import { seasonAwards, tournamentAwards } from "../src/awards.ts";
import { report } from "../src/bracket.ts";
import type { GameFacts } from "../src/live.ts";
import { enter, move, settle, type Tournament } from "../src/tournament.ts";

const AT = "2026-10-21T20:00:00.000Z";

const game = (p1: string, p2: string, perfect: boolean): GameFacts => ({
  fighters: [{ character: p1, costume: 0, color: 0 }, { character: p2, costume: 0, color: 0 }],
  rounds: [{ winner: 0, finish: "ko", perfect, from_frame: "1", to_frame: "900" }, { winner: 0, finish: "ko", perfect: false, from_frame: "1000", to_frame: "1900" }],
});

test("placements, stats and characters give their awards; nothing is given without what it needs", () => {
  const t: Tournament = { id: "cup", title: "Cup", games_to_win: 2, check_in: false, cap: null, starts_at: null, season: "s1", organizers: [], state: "sign_ups", entrants: [], rounds: 0, sets: [], log: [] };
  for (const n of [1, 2, 3, 4]) enter(t, { discord: `d${n}`, name: `P${n}`, ember_id: `e${n}`, checked_in: false }, false);
  move(t, "start", "o", AT);
  // r1-1: seed 1 vs 4, seed 4 wins 2-1 (an upset of 3 seeds); r1-2: seed 2 beats 3 2-0 with facts; final: P4 beats P2 2-0.
  const play = (id: string, winner: string, loser: string, scores: [number, number], facts?: (GameFacts | null)[]) => {
    report(t.sets, id, { winner, scores: { [winner]: scores[0], [loser]: scores[1] }, key: id });
    if (facts) Object.assign(t.sets.find((set) => set.id === id)!, { facts });
  };
  play("r1-1", "e4", "e1", [2, 1]);
  play("r1-2", "e2", "e3", [2, 0], [game("RYU", "KEN", true), game("SAG", "KEN", false)]);
  play("r2-1", "e4", "e2", [2, 0], [game("BLR", "AKU", false), game("BLR", "AKU", false)]);
  settle(t, AT);
  const given = tournamentAwards(t, t.report!).map((award) => [award.name, award.kind, award.detail]);
  assert.deepEqual(given, [
    ["P4", "champion", "1st place"],
    ["P2", "runner_up", "2nd place"],
    ["P1", "top_3", "3rd place"],
    ["P3", "top_3", "3rd place"],
    ["P4", "giant_killer", "beat P1, 3 seeds higher"],
    ["P2", "perfectionist", "1 perfect round"],
    ["P4", "iron", "5 games"],
    ["P1", "marathon", "a 3-game set"],
    ["P4", "marathon", "a 3-game set"],
    ["P2", "mixer", "3 characters"],
  ]);
  // No top 8 in a bracket of four, and no one-trick under four games (P4 played BLR twice).
  assert.ok(given.every(([, kind]) => !["top_8", "one_trick"].includes(kind!)));
  assert.equal(t.report!.players.find((player) => player.name === "P4")!.characters!.BLR, 2);

  const rows = [
    { discord: "d4", name: "P4", points: 100, events: 1, won: 1 },
    { discord: "d2", name: "P2", points: 70, events: 1, won: 0 },
    { discord: "d1", name: "P1", points: 50, events: 1, won: 0 },
    { discord: "d3", name: "P3", points: 50, events: 1, won: 0 },
  ];
  const season = seasonAwards({ id: "s1", title: "Season 1", points: {}, best_of: null }, rows, AT);
  assert.deepEqual(season.map((award) => [award.name, award.kind, award.of]), [["P4", "season_champion", "Season 1"], ["P2", "season_top_3", "Season 1"], ["P1", "season_top_3", "Season 1"]]);
});
