import assert from "node:assert/strict";
import { test } from "node:test";

import { report } from "../src/bracket.ts";
import { setDone, setLive, stats, type GameFacts } from "../src/live.ts";
import { enter, move, settle, type Tournament } from "../src/tournament.ts";

const AT = "2026-10-21T20:00:00.000Z";

function game(p1: string, p2: string, winners: (0 | 1)[], perfect = false): GameFacts {
  return {
    fighters: [{ character: p1, costume: 0, color: 0 }, { character: p2, costume: 0, color: 0 }],
    rounds: winners.map((winner, i) => ({ winner, finish: "ko", perfect: perfect && i === 0, from_frame: String(i * 1000 + 1), to_frame: String(i * 1000 + 900) })),
  };
}

test("the feed tells each move, set and upset, and the stats count games, characters and perfects", () => {
  const t: Tournament = { id: "t", title: "Weekly", games_to_win: 2, check_in: false, cap: null, starts_at: null, season: null, organizers: [], state: "sign_ups", entrants: [], rounds: 0, sets: [], log: [] };
  for (const n of [1, 2, 3]) enter(t, { discord: `d${n}`, name: `P${n}`, ember_id: `e${n}`, checked_in: false }, false);
  move(t, "start", "o", AT);
  // Seed 3 beats seed 2 in the semifinal, 2-1, with facts for two games of three.
  const semi = t.sets.find((set) => set.id === "r1-2")!;
  setLive(t, semi, AT);
  report(t.sets, "r1-2", { winner: "e3", scores: { e2: 1, e3: 2 }, key: "k" });
  Object.assign(semi, { games: ["e3", "e2", "e3"], facts: [game("RYU", "KEN", [1, 1], true), null, game("RYU", "BLR", [1, 0, 1])] });
  setDone(t, semi, AT);
  const final = t.sets.find((set) => set.id === "r2-1")!;
  report(t.sets, "r2-1", { winner: "e1", scores: { e1: 2, e3: 0 }, key: "k2" });
  setDone(t, final, AT);
  settle(t, AT);
  assert.deepEqual(
    t.feed!.map((item) => [item.kind, item.text]),
    [
      ["state", "Weekly: The bracket has started."],
      ["set_live", "Semifinals: P2 vs P3 is live."],
      ["set_done", "Semifinals: P3 beats P2 2-1."],
      ["upset", "Upset: seed 3 P3 knocks out seed 2."],
      ["set_done", "Final: P1 beats P3 2-0."],
      ["state", "Weekly: The tournament is over."],
    ],
  );
  assert.deepEqual(t.feed!.map((item) => item.n), [1, 2, 3, 4, 5, 6]);

  const numbers = stats(t);
  assert.deepEqual(numbers.sets, { live: 0, done: 2, total: 2 });
  assert.deepEqual(numbers.longest_set, { set: "r1-2", games: 3 });
  assert.deepEqual([numbers.games, numbers.games_with_facts], [5, 2]);
  assert.deepEqual(numbers.characters, { RYU: 2, KEN: 1, BLR: 1 });
  assert.deepEqual(numbers.upsets, [{ set: "r1-2", winner: "P3", loser: "P2", gap: 1 }]);
  const [first, second, third] = numbers.players;
  assert.deepEqual([first!.name, first!.place, second!.name, second!.place, third!.name, third!.place], ["P1", 1, "P3", 2, "P2", 3]);
  assert.deepEqual([second!.games_won, second!.games_lost, second!.rounds_won, second!.perfects], [2, 3, 4, 1]);
  assert.deepEqual([third!.characters, third!.rounds_won, third!.perfects], [{ RYU: 2 }, 1, 0]);
  assert.deepEqual(second!.characters, { KEN: 1, BLR: 1 });

  // The after-report, made once at the finish.
  const after = t.report!;
  assert.deepEqual(after.podium.map((each) => [each.place, each.name]), [[1, "P1"], [2, "P3"], [3, "P2"]]);
  const p3 = after.players.find((player) => player.name === "P3")!;
  assert.deepEqual(
    p3.path.map((step) => [step.round, step.opponent, step.won, step.score, step.games]),
    [
      ["Semifinals", "P2", true, [2, 1], ["P3", "P2", "P3"]],
      ["Final", "P1", false, [0, 2], null],
    ],
  );
  assert.equal(p3.comebacks, 0, "P3 won the semifinal's first game");
  const p1 = after.players.find((player) => player.name === "P1")!;
  assert.deepEqual([p1.characters, p1.perfects, p1.comebacks], [null, null, null], "unknown, not none");
  assert.deepEqual([after.perfects, after.time_outs, after.shortest_set, after.final!.opponent], [1, 0, { set: "r2-1", games: 2 }, "P3"]);
  assert.deepEqual(Object.keys(after.characters!), ["RYU", "KEN", "BLR"], "most played first");
});
