import assert from "node:assert/strict";
import { test } from "node:test";

import { Conflict, report } from "../src/bracket.ts";
import { actions, checkIn, enter, move, seed, settle, type Entrant, type Tournament } from "../src/tournament.ts";

const AT = "2026-10-21T20:00:00.000Z";

function tournament(change: Partial<Tournament> = {}): Tournament {
  return { id: "t", title: "T", games_to_win: 2, check_in: false, cap: null, starts_at: null, season: null, organizers: ["1"], state: "draft", entrants: [], rounds: 0, sets: [], log: [], ...change };
}

const player = (n: number, linked = true): Entrant => ({ discord: `${n}`.repeat(18), name: `P${n}`, ember_id: linked ? `emb${n}` : null, checked_in: false });

test("a tournament goes from draft to archived, one allowed action at a time", () => {
  const t = tournament();
  assert.deepEqual(actions(t), ["plan", "cancel"]);
  assert.throws(() => move(t, "start", "1", AT), Conflict);
  move(t, "plan", "1", AT);
  move(t, "open_sign_ups", "1", AT);
  assert.deepEqual(actions(t), ["close_sign_ups", "start", "cancel"], "no check-in without check_in");
  for (const n of [1, 2, 3]) enter(t, player(n), false);
  move(t, "start", "1", AT);
  assert.equal(t.state, "live");
  assert.equal(t.sets.length, 2);
  assert.deepEqual(actions(t), ["reset", "cancel"]);
  report(t.sets, "r1-2", { winner: "emb3", scores: { emb2: 0, emb3: 2 }, key: null });
  assert.deepEqual(actions(t), ["cancel"], "no reset once a set is decided");
  report(t.sets, "r2-1", { winner: "emb1", scores: { emb1: 2, emb3: 1 }, key: null });
  settle(t, AT);
  assert.equal(t.state, "finished");
  move(t, "archive", "1", AT);
  assert.deepEqual(actions(t), []);
  assert.deepEqual(
    t.log.map((change) => [change.by, change.from, change.to]),
    [
      ["1", "draft", "planned"],
      ["1", "planned", "sign_ups"],
      ["1", "sign_ups", "live"],
      ["site", "live", "finished"],
      ["1", "finished", "archived"],
    ],
  );
});

test("with check-in, only those checked in and linked play; the rest are dropped at the start", () => {
  const t = tournament({ state: "sign_ups", check_in: true });
  assert.ok(!actions(t).includes("start"), "check-in first");
  for (const entrant of [player(1), player(2), player(3, false), player(4)]) enter(t, entrant, false);
  assert.throws(() => checkIn(t, player(1).discord), Conflict);
  move(t, "open_check_in", "1", AT);
  assert.throws(() => enter(t, player(5), false), Conflict, "sign-ups closed for players");
  for (const n of [1, 3, 4]) checkIn(t, player(n).discord);
  assert.deepEqual(seed(t).playing.map((each) => each.name), ["P1", "P4"]);
  move(t, "start", "1", AT);
  assert.deepEqual(t.entrants.map((each) => each.name), ["P1", "P4"]);
  assert.equal(t.log.at(-1)!.note, "dropped P2, P3");
  // Back to check-in while nothing is decided: the bracket is gone.
  move(t, "reset", "1", AT);
  assert.equal(t.state, "check_in");
  assert.deepEqual(t.sets, []);
});

test("sign-ups: open to players only in sign_ups, to organizers before live, once each, up to the cap", () => {
  const t = tournament({ state: "planned", cap: 2 });
  assert.throws(() => enter(t, player(1), false), Conflict);
  enter(t, player(1), true);
  assert.throws(() => enter(t, player(1), true), /already entered/);
  t.state = "sign_ups";
  enter(t, player(2), false);
  assert.throws(() => enter(t, player(3), false), /full/);
  // Two Discord accounts on one Ember ID play once.
  const twice = tournament({ state: "sign_ups", entrants: [player(1), { ...player(2), ember_id: "emb1" }, player(3)] });
  assert.deepEqual(seed(twice).playing.map((each) => each.name), ["P1", "P3"]);
  // Too few who can play: the start is refused.
  const few = tournament({ state: "sign_ups", entrants: [player(1), player(2, false)] });
  assert.throws(() => move(few, "start", "1", AT), /at least two/);
  assert.equal(few.state, "sign_ups");
});
