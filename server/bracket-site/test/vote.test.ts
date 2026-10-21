import assert from "node:assert/strict";
import { test } from "node:test";

import { Conflict, Invalid } from "../src/bracket.ts";
import type { Tournament } from "../src/tournament.ts";
import { accept, accountCreated, cast, close, due, nominate, open, shown, voidVote } from "../src/vote.ts";

const NOW = new Date("2026-10-21T20:00:00.000Z");
const OLD = ["274220342558756145", "80351110224678912", "175928847299117063"];
/** An account made a day before NOW. */
const YOUNG = String(BigInt(NOW.getTime() - 86400_000 - 1420070400000) << 22n);

function finished(): Tournament {
  const entrant = (discord: string, name: string) => ({ discord, name, ember_id: `e-${name}`, checked_in: false });
  return { id: "cup", title: "Cup", games_to_win: 2, check_in: false, cap: null, starts_at: null, season: null, organizers: [], state: "finished", entrants: [entrant(OLD[0]!, "Kate"), entrant(OLD[1]!, "Max")], rounds: 1, sets: [], log: [] };
}

test("a Discord ID says when its account was made", () => {
  assert.equal(new Date(accountCreated("175928847299117063")).toISOString(), "2016-04-30T11:18:25.796Z");
  assert.ok(accountCreated(YOUNG) > NOW.getTime() - 2 * 86400_000);
});

test("nominations, one vote per account per category, voids and the close", () => {
  const t = finished();
  assert.throws(() => cast(t, "best_player", OLD[0]!, "n1", NOW), Invalid, "no vote yet");
  open(t, NOW, 48);
  assert.throws(() => open(t, NOW, 48), Conflict);
  const kate = nominate(t, { category: "best_player", player: OLD[0]!, title: "Kate", ref: null, by: "o" }, true);
  const max = nominate(t, { category: "best_player", player: OLD[1]!, title: "Max", ref: null, by: OLD[2]! }, false);
  assert.equal(max.accepted, false);
  assert.throws(() => nominate(t, { category: "best_combo", player: OLD[0]!, title: "No clip", ref: null, by: "o" }, true), Invalid);
  assert.throws(() => nominate(t, { category: "best_player", player: "1", title: "Nobody", ref: null, by: "o" }, true), Invalid);
  assert.throws(() => nominate(t, { category: "best_player", player: OLD[0]!, title: "Again", ref: null, by: "o" }, true), Conflict);
  assert.throws(() => cast(t, "best_player", OLD[2]!, max.id, NOW), Invalid, "not accepted yet");
  accept(t, max.id);
  const combo = nominate(t, { category: "best_combo", player: OLD[1]!, title: "The Ultra", ref: "c1", by: "o" }, true);

  cast(t, "best_player", OLD[0]!, kate.id, NOW);
  cast(t, "best_player", OLD[1]!, max.id, NOW);
  cast(t, "best_player", OLD[2]!, kate.id, NOW);
  cast(t, "best_player", OLD[2]!, max.id, NOW);
  cast(t, "best_combo", OLD[0]!, combo.id, NOW);
  assert.throws(() => cast(t, "best_player", YOUNG, kate.id, NOW), /too new/);
  assert.throws(() => cast(t, "best_combo", OLD[0]!, kate.id, NOW), Invalid, "Kate is no combo");
  // Counts are hidden while it runs; a voter sees their own picks.
  const view = shown(t.vote!, OLD[2]!) as { results: unknown; voters: Record<string, number>; mine: Record<string, string | null> };
  assert.deepEqual([view.results, view.voters.best_player, view.mine.best_player, view.mine.best_moment], [null, 3, max.id, null]);
  assert.ok(!("ballots" in view));

  voidVote(t, "best_player", OLD[1]!);
  assert.equal(due(t, NOW), false);
  assert.equal(due(t, new Date(NOW.getTime() + 48 * 3600_000)), true);
  assert.throws(() => cast(t, "best_player", OLD[1]!, max.id, new Date(NOW.getTime() + 49 * 3600_000)), /closed/);
  const awards = close(t, new Date(NOW.getTime() + 48 * 3600_000));
  // Kate 1, Max 1 after the void: a tie gives both the award.
  assert.deepEqual(t.vote!.results!.best_player.map((row) => row.votes), [1, 1]);
  assert.deepEqual(awards.map((award) => [award.name, award.kind]), [["Kate", "best_player"], ["Max", "best_player"], ["Max", "best_combo"]]);
  assert.deepEqual(close(t, NOW), [], "closed once");
});
