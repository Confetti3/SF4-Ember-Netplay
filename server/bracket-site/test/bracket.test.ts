import assert from "node:assert/strict";
import { test } from "node:test";

import { Conflict, Invalid, decided, generate, report, roundLabel, seedOrder, status, type BracketSet } from "../src/bracket.ts";

const ids = (count: number) => Array.from({ length: count }, (_, i) => `p${i + 1}`);

function tournament(count: number): { rounds: number; sets: BracketSet[] } {
  return generate(ids(count));
}

/** Plays every open set, the lower seed number winning 2-1, until none is left. */
function playOut(t: { sets: BracketSet[] }): void {
  for (let open = t.sets.filter((set) => status(set) === "open"); open.length; open = t.sets.filter((set) => status(set) === "open")) {
    for (const set of open) {
      const [a, b] = set.entries as [string, string];
      const winner = Number(a.slice(1)) < Number(b.slice(1)) ? a : b;
      report(t.sets, set.id, { winner, scores: { [winner]: 2, [winner === a ? b : a]: 1 }, key: `k-${set.id}` });
    }
  }
}

test("seed order keeps the top seeds apart until the end", () => {
  assert.deepEqual(seedOrder(2), [1, 2]);
  assert.deepEqual(seedOrder(4), [1, 4, 2, 3]);
  assert.deepEqual(seedOrder(8), [1, 8, 4, 5, 2, 7, 3, 6]);
});

test("every size from 2 to 17 gets byes for its top seeds, and its first seed wins", () => {
  for (let count = 2; count <= 17; count++) {
    const t = tournament(count);
    const size = 2 ** t.rounds;
    assert.ok(size >= count && size / 2 < count, `${count}`);
    const first = t.sets.filter((set) => set.round === 1);
    // One set for each pair of real entrants; the byes are not sets.
    assert.equal(first.length, count - size / 2, `${count}`);
    assert.equal(t.sets.length, count - 1, `${count}: one set less than entrants`);
    const byes = size - count;
    if (t.rounds > 1) {
      const waiting = t.sets.filter((set) => set.round === 2).flatMap((set) => set.entries).filter(Boolean);
      assert.deepEqual(waiting.sort(), ids(byes).sort(), `${count}`);
    }
    assert.ok(new Set(t.sets.map((set) => set.id)).size === t.sets.length);
    playOut(t);
    assert.ok(decided(t.sets, t.rounds), `${count}`);
    const final = t.sets.find((set) => set.round === t.rounds)!;
    assert.equal(final.winner, "p1");
    assert.deepEqual([...final.entries].sort(), ["p1", "p2"], `${count}: seeds 1 and 2 meet in the final`);
  }
});

test("three players: seed 1 waits for the winner of 2 against 3", () => {
  const t = tournament(3);
  assert.deepEqual(
    t.sets.map((set) => [set.id, set.entries, status(set)]),
    [
      ["r1-2", ["p2", "p3"], "open"],
      ["r2-1", ["p1", null], "pending"],
    ],
  );
  assert.equal(roundLabel(1, 2), "Semifinals");
  assert.equal(roundLabel(2, 2), "Final");
  assert.equal(roundLabel(1, 5), "Round 1");
  report(t.sets, "r1-2", { winner: "p3", scores: { p2: 1, p3: 2 }, key: "a" });
  assert.deepEqual(t.sets[1]!.entries, ["p1", "p3"]);
  assert.equal(status(t.sets[1]!), "open");
  assert.equal(decided(t.sets, t.rounds), false);
});

test("a result is recorded once; its repeat changes nothing, any other is a conflict", () => {
  const t = tournament(4);
  const result = { winner: "p1", scores: { p1: 2, p4: 0 }, key: "emt_1" };
  assert.equal(report(t.sets, "r1-1", result), "recorded");
  assert.equal(report(t.sets, "r1-1", result), "repeat");
  assert.deepEqual(t.sets.find((set) => set.id === "r1-1")!.scores, [2, 0]);
  assert.throws(() => report(t.sets, "r1-1", { ...result, key: "emt_2" }), Conflict);
  assert.throws(() => report(t.sets, "r1-1", { ...result, scores: { p1: 2, p4: 1 } }), Conflict);
  assert.throws(() => report(t.sets, "r1-1", { ...result, key: null }), Conflict);
  // The final has one player so far.
  assert.throws(() => report(t.sets, "r2-1", { winner: "p1", scores: { p1: 2 }, key: "x" }), Conflict);
  assert.throws(() => report(t.sets, "r9-1", result), Invalid);
});

test("a result names one of the set's players, who won more games", () => {
  const t = tournament(2);
  const bads: { winner: string; scores: Record<string, number> }[] = [
    { winner: "p3", scores: { p1: 2, p2: 0 } },
    { winner: "p1", scores: { p1: 1, p2: 2 } },
    { winner: "p1", scores: { p1: 2, p2: 2 } },
    { winner: "p1", scores: { p1: 2 } },
    { winner: "p1", scores: { p1: 2.5, p2: 0 } },
    { winner: "p1", scores: { p1: 2, p2: -1 } },
  ];
  for (const bad of bads) {
    assert.throws(() => report(t.sets, "r1-1", { ...bad, key: null }), Invalid, JSON.stringify(bad));
  }
  assert.equal(status(t.sets[0]!), "open");
  assert.throws(() => generate(["p1"]), /at least two/);
});
