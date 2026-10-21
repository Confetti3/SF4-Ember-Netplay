// A set's replays and clips, with a stand-in for the bridge's public pack header.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";

import { generate, report } from "../src/bracket.ts";
import { bracketSite } from "../src/server.ts";
import { Store } from "../src/store.ts";
import type { Tournament } from "../src/tournament.ts";

const BRIDGE = "https://bridge.example";
const round = (from: string, to: string) => ({ winner: 0 as const, finish: "ko" as const, perfect: false, from_frame: from, to_frame: to });

test("a set's replays link its pack, each game and each round; organizers mark clips", async () => {
  const dir = mkdtempSync(join(tmpdir(), "bracket-site-replays-"));
  const store = new Store(join(dir, "site.sqlite3"));
  const t: Tournament = { id: "cup", title: "Cup", games_to_win: 2, check_in: false, cap: null, starts_at: null, season: null, organizers: [], state: "finished", entrants: [], log: [], ...generate(["e1", "e2"]) };
  report(t.sets, "r1-1", { winner: "e1", scores: { e1: 2, e2: 0 }, key: "emt_1" });
  const fighters = [{ character: "RYU", costume: 0, color: 0 }, { character: "KEN", costume: 0, color: 0 }] as [never, never];
  Object.assign(t.sets[0]!, { ember_match: "emt_1", games: ["e1", "e1"], facts: [{ fighters, rounds: [round("10", "900"), round("1000", "1900")] }, null] });
  store.add(t);
  const asked: string[] = [];
  const bridge = (async (url: string) => {
    asked.push(url);
    return new Response(JSON.stringify({ games: [{ crc: "0a0b0c0d", saved_at: 1, size: 9 }, { crc: "0e0f1011", saved_at: 2, size: 9 }] }), { status: 200 });
  }) as typeof fetch;
  const server = bracketSite({ store, operatorToken: "operator", connectorToken: "connector", bridge: { lookupPlayers: async () => [] }, bridgeOrigin: BRIDGE, fetch: bridge });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  const call = async (method: string, path: string, body?: unknown) => {
    const response = await fetch(`${base}${path}`, { method, headers: { authorization: "Bearer operator" }, ...(body ? { body: JSON.stringify(body) } : {}) });
    return { status: response.status, body: await response.json() };
  };
  try {
    const replays = (await call("GET", "/tournaments/cup/replays/r1-1")).body;
    assert.deepEqual(asked, [`${BRIDGE}/v1/public/matches/emt_1/replays`]);
    assert.equal(replays.pack, `${BRIDGE}/v1/public/matches/emt_1/replays.pack`);
    assert.equal(replays.games[1].file, `${BRIDGE}/v1/public/matches/emt_1/replays/0e0f1011`);
    assert.equal(replays.games[0].rounds[1].open, `ember://replay?bridge=https%3A%2F%2Fbridge.example&match=emt_1&crc=0a0b0c0d&from=1000&to=1900`);
    assert.deepEqual(replays.games[1].rounds, [], "a game without facts has no rounds to cut");

    assert.equal((await call("POST", "/tournaments/cup/clips", { set: "r1-1", crc: "0a0b0c0d", from_frame: "900", to_frame: "800", title: "Backwards" })).status, 422);
    const clip = await call("POST", "/tournaments/cup/clips", { set: "r1-1", crc: "0a0b0c0d", from_frame: "400", to_frame: "880", title: "The Ultra" });
    assert.deepEqual([clip.status, clip.body.id], [201, "c1"]);
    const shown = (await call("GET", "/tournaments/cup/clips/c1")).body;
    assert.deepEqual([shown.match, shown.title, shown.open], ["emt_1", "The Ultra", "ember://replay?bridge=https%3A%2F%2Fbridge.example&match=emt_1&crc=0a0b0c0d&from=400&to=880"]);
    assert.equal((await call("GET", "/tournaments/cup/clips")).body.clips.length, 1);
  } finally {
    server.close();
    store.close();
    rmSync(dir, { recursive: true, force: true });
  }
});
