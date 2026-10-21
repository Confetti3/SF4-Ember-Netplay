// The site's routes over HTTP, with a stand-in for the bridge's Discord
// lookup. The connector's side sends what ember-brackets sends
// (src/service.rs): its headers, `/sets?state=open`, `/underway` with `{}`,
// and reports with an Idempotency-Key, repeated as they are on a lost answer.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";

import { BridgeError } from "../../../sdk/typescript/src/index.ts";
import { bracketSite } from "../src/server.ts";
import { Store } from "../src/store.ts";

const ORGANIZER = "organizer-test-token";
const CONNECTOR = "connector-test-token";
const emberId = (n: number) => `emb1_${"abcd"[n - 1]!.repeat(51)}a`;
/** Discord IDs 1000...0001 to 0004 are connected in Ember; 0009 is not; 0013 makes the bridge fail. */
const discordId = (n: number) => `10000000000000000${String(n).padStart(2, "0")}`;
const asked: string[][] = [];
const bridge = {
  async lookupPlayers(ids: string[]) {
    asked.push(ids);
    if (ids[0] === discordId(13)) throw new BridgeError(503, "unavailable", "down", true);
    const n = Number(ids[0]!.slice(-2));
    return n <= 4 ? [{ discord_user_id: ids[0]!, ember_id: emberId(n), participant_id: `prt_${n}` }] : [];
  },
};

let dir: string;
let store: Store;
let base: string;
let close: () => void;

before(async () => {
  dir = mkdtempSync(join(tmpdir(), "bracket-site-"));
  store = new Store(join(dir, "site.sqlite3"));
  const server = bracketSite({ store, organizerToken: ORGANIZER, connectorToken: CONNECTOR, bridge });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  close = () => server.close();
});

after(() => {
  close();
  store.close();
  rmSync(dir, { recursive: true, force: true });
});

async function call(method: string, path: string, options: { token?: string; body?: unknown; headers?: Record<string, string> } = {}) {
  const response = await fetch(`${base}${path}`, {
    method,
    headers: { ...(options.token ? { authorization: `Bearer ${options.token}` } : {}), ...options.headers },
    ...(options.body === undefined ? {} : { body: JSON.stringify(options.body) }),
  });
  const text = await response.text();
  return { status: response.status, body: text && response.headers.get("content-type") === "application/json" ? JSON.parse(text) : text };
}

const organizer = (method: string, path: string, body?: unknown) => call(method, path, { token: ORGANIZER, body });
/** A request as ember-brackets makes it. */
const connector = (method: string, path: string, body?: unknown, key?: string) =>
  call(method, `/tournaments/weekly/ember${path}`, {
    token: CONNECTOR,
    body,
    headers: { accept: "application/json", "ember-brackets-contract": "1", ...(key ? { "idempotency-key": key } : {}) },
  });

test("an organizer runs a three player bracket that the connector plays and reports", async () => {
  assert.equal((await call("POST", "/tournaments", { body: { id: "weekly", title: "Weekly" } })).status, 401);
  assert.equal((await call("POST", "/tournaments", { token: CONNECTOR, body: { id: "weekly", title: "Weekly" } })).status, 401);
  assert.equal((await organizer("POST", "/tournaments", { id: "a/b", title: "Weekly" })).status, 400);
  assert.equal((await organizer("POST", "/tournaments", { id: "a b", title: "Weekly" })).status, 400);
  assert.equal((await organizer("POST", "/tournaments", { id: "weekly", title: "Weekly", games_to_win: 11 })).status, 400);
  const created = await organizer("POST", "/tournaments", { id: "weekly", title: "Weekly <1>", games_to_win: 2 });
  assert.equal(created.status, 201);
  assert.equal((await organizer("POST", "/tournaments", { id: "weekly", title: "Again" })).status, 409);

  // Players are entered by their Discord accounts, and known by their Ember IDs.
  for (const n of [1, 2, 3]) {
    const joined = await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(n), name: `Player ${n}` });
    assert.equal(joined.status, 201);
    assert.deepEqual(joined.body, { id: emberId(n), discord: discordId(n), name: `Player ${n}` });
  }
  assert.deepEqual(asked, [[discordId(1)], [discordId(2)], [discordId(3)]]);
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(2) })).status, 409);
  assert.match((await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(9) })).body.error, /not connected/);
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(13) })).status, 502);
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: "kate" })).status, 400);
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(4) })).status, 201);
  assert.equal((await organizer("DELETE", `/tournaments/weekly/entrants/${emberId(4)}`)).status, 204);

  // Nothing to play before the start.
  assert.deepEqual((await connector("GET", "/sets?state=open")).body, { sets: [] });
  assert.deepEqual((await connector("GET", "/standing")).body, { standing: "running" });
  assert.equal((await organizer("POST", "/tournaments/weekly/start", { seeding: "best" })).status, 400);
  const started = await organizer("POST", "/tournaments/weekly/start", { seeding: "join" });
  assert.equal(started.status, 200);
  assert.equal((await organizer("POST", "/tournaments/weekly/start", {})).status, 409);
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(4) })).status, 409);

  // Player 1 has a bye; Players 2 and 3 play the semifinal.
  const semifinal = {
    id: "r1-2",
    round: 1,
    round_label: "Semifinals",
    status: "open",
    entries: [
      { id: emberId(2), accounts: [{ discord: discordId(2) }] },
      { id: emberId(3), accounts: [{ discord: discordId(3) }] },
    ],
    underway: false,
    winner: null,
  };
  assert.deepEqual((await connector("GET", "/sets?state=open")).body, { sets: [semifinal] });
  assert.deepEqual((await connector("GET", "/sets/r2-1")).body.set.entries, null);
  assert.equal((await connector("POST", "/sets/r1-2/underway", {})).status, 204);
  assert.equal((await connector("GET", "/sets/r1-2")).body.set.underway, true);

  // The connector's report: refused without its key, then taken, then taken
  // again as a repeat; another result is a conflict.
  const report = {
    winner: emberId(3),
    scores: [
      { entry: emberId(2), games: 1 },
      { entry: emberId(3), games: 2 },
    ],
    games: [emberId(3), emberId(2), emberId(3)],
  };
  assert.equal((await connector("POST", "/sets/r1-2/report", report)).status, 400);
  assert.equal((await connector("POST", "/sets/r1-2/report", { ...report, winner: emberId(1) }, "emt_semi")).status, 422);
  assert.equal((await connector("POST", "/sets/r1-2/report", report, "emt_semi")).status, 200);
  assert.equal((await connector("POST", "/sets/r1-2/report", report, "emt_semi")).status, 200);
  assert.equal((await connector("POST", "/sets/r1-2/report", report, "emt_other")).status, 409);
  const decided = (await connector("GET", "/sets/r1-2")).body.set;
  assert.equal(decided.status, "decided");
  assert.equal(decided.winner, emberId(3));

  // The final, entered by the organizer instead.
  const open = (await connector("GET", "/sets?state=open")).body.sets;
  assert.deepEqual(
    open.map((set: { id: string; round_label: string; entries: { id: string }[] }) => [set.id, set.round_label, set.entries.map((entry) => entry.id)]),
    [["r2-1", "Final", [emberId(1), emberId(3)]]],
  );
  assert.equal((await organizer("POST", "/tournaments/weekly/sets/r2-1/result", { winner: emberId(1), scores: { [emberId(1)]: 2, [emberId(3)]: 0 } })).status, 200);
  assert.deepEqual((await connector("GET", "/sets?state=open")).body, { sets: [] });
  assert.deepEqual((await connector("GET", "/standing")).body, { standing: "finished" });

  const shown = (await call("GET", "/tournaments/weekly")).body;
  assert.equal(shown.finished, true);
  assert.ok(shown.sets.every((set: object) => !("key" in set)), "keys are not shown");
  const html = await call("GET", "/tournaments/weekly/bracket");
  assert.equal(html.status, 200);
  assert.match(html.body, /<title>Weekly &#60;1&#62;<\/title>/);
  assert.match(html.body, /<tr class="won"><td>Player 1<\/td><td>2<\/td><\/tr>/);
});

test("the connector's token, contract version and gone sets", async () => {
  assert.equal((await call("GET", "/tournaments/weekly/ember/sets?state=open", { token: ORGANIZER })).status, 401);
  const old = await call("GET", "/tournaments/weekly/ember/sets?state=open", { token: CONNECTOR, headers: { "ember-brackets-contract": "2" } });
  assert.deepEqual([old.status, old.body], [426, { contract: "1" }]);
  // A set the tournament never had, or a deleted tournament's, is gone.
  assert.deepEqual((await connector("GET", "/sets/r7-1")).body, { set: null });
  assert.equal((await organizer("DELETE", "/tournaments/weekly")).status, 204);
  assert.deepEqual((await connector("GET", "/sets/r1-2")).body, { set: null });
  assert.deepEqual((await connector("GET", "/standing")).body, { standing: "finished" });
  assert.equal((await call("GET", "/tournaments/weekly")).status, 404);
});
