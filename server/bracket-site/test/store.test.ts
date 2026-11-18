// A write that spans several rows lands whole or not at all.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";

import type { Award } from "../src/awards.ts";
import { Store } from "../src/store.ts";

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
