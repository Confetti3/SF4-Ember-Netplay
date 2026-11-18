// A write that spans several rows lands whole or not at all.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";

import type { Award } from "../src/awards.ts";
import { record } from "../src/contract.ts";
import { Store } from "../src/store.ts";
import { enter, move, type Tournament } from "../src/tournament.ts";

const AT = "2026-10-21T20:00:00.000Z";
const award = (discord: string, given_for: string): Award => ({
  discord,
  name: "P",
  kind: "champion",
  title: "Champion",
  tournament: given_for,
  season: null,
  of: "Cup",
  given_at: AT,
  detail: "1st place",
});

const open = () => {
  const dir = mkdtempSync(join(tmpdir(), "bracket-site-store-"));
  return { dir, store: new Store(join(dir, "site.sqlite3")) };
};

test("a transaction keeps all of its writes, drops them all when it throws, and joins one already open", () => {
  const { dir, store } = open();
  try {
    store.transaction(() => store.addAwards([award("d1", "a")]));
    assert.throws(
      () =>
        store.transaction(() => {
          store.addAwards([award("d1", "b")]);
          throw new Error("crash");
        }),
      /crash/,
    );
    store.transaction(() => store.transaction(() => store.addAwards([award("d1", "c")])));
    assert.deepEqual(store.awardsOf("d1").map((each) => each.tournament), ["a", "c"]);
  } finally {
    store.close();
    rmSync(dir, { recursive: true, force: true });
  }
});

test("a set's result and its awards are saved together, so a redelivery announces them", () => {
  class Failing extends Store {
    fail = true;
    override save(tournament: Tournament): void {
      if (this.fail) throw new Error("crash");
      super.save(tournament);
    }
  }
  const dir = mkdtempSync(join(tmpdir(), "bracket-site-store-"));
  const store = new Failing(join(dir, "site.sqlite3"));
  try {
    const t: Tournament = { id: "cup", title: "Cup", games_to_win: 2, check_in: false, cap: null, starts_at: null, season: null, organizers: [], state: "sign_ups", entrants: [], rounds: 0, sets: [], log: [] };
    for (const n of [1, 2]) enter(t, { discord: `d${n}`, name: `P${n}`, ember_id: `e${n}`, checked_in: false }, false);
    move(t, "start", "o", AT);
    store.add(t);
    const result = { winner: "e1", scores: { e1: 2, e2: 0 }, key: null };
    const attempt = () => record(store, structuredClone(store.get("cup")!), "r1-1", result);
    assert.throws(attempt, /crash/);
    assert.deepEqual(store.awardsAfter(0), [], "a crash before the save leaves no awards behind");
    store.fail = false;
    attempt();
    assert.ok(store.awardsAfter(0).length > 0);
    assert.ok(store.get("cup")!.feed!.some((item) => item.kind === "award"), "the award lines were posted");
  } finally {
    store.close();
    rmSync(dir, { recursive: true, force: true });
  }
});
