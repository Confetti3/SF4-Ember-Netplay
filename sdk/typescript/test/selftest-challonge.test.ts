// The Challonge mode of the tournament self-test (tools/selftest-challonge.ts):
// its run configuration, the bridge site and connector configurations it
// writes, and what it expects of the set before and after the run.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";

import {
  CONNECTOR_BUDGET,
  connectorConfig,
  openProblems,
  parseChallongeConfig,
  parseMatch,
  readSecret,
  reportedProblems,
  signInSite,
} from "../tools/selftest-challonge.ts";

const CONFIG = {
  clientId: "ab".repeat(32),
  clientSecretFile: "secrets/client-secret.txt",
  apiKeyFile: "secrets/organizer.key",
  tournament: "ember_selftest_1",
  match: "1234567",
  redirectUri: "http://127.0.0.1:18787/v1/sites/challonge/callback",
};

test("the run configuration names the app, the tournament, the match and the secret files, and nothing else", () => {
  assert.deepEqual(parseChallongeConfig(JSON.stringify(CONFIG)), CONFIG);
  for (const [change, message] of [
    [{ clientId: "" }, /clientId/],
    [{ clientId: "has space" }, /clientId/],
    [{ clientSecretFile: 7 }, /clientSecretFile/],
    [{ apiKeyFile: undefined }, /apiKeyFile/],
    [{ tournament: "not a slug" }, /tournament/],
    [{ tournament: "../x" }, /tournament/],
    [{ match: "12a" }, /match/],
    [{ redirectUri: "https://127.0.0.1:18787/v1/sites/challonge/callback" }, /redirectUri/],
    [{ redirectUri: "http://example.com:18787/v1/sites/challonge/callback" }, /redirectUri/],
    [{ extra: "field" }, /unknown field, extra/],
  ] as const) {
    assert.throws(() => parseChallongeConfig(JSON.stringify({ ...CONFIG, ...change })), message, JSON.stringify(change));
  }
  assert.throws(() => parseChallongeConfig("{"), /not JSON/);
  assert.throws(() => parseChallongeConfig("[]"), /not an object/);
});

test("a secret is the first line of its file, and a failure names the file and not the secret", () => {
  const folder = mkdtempSync(join(tmpdir(), "selftest-challonge-"));
  try {
    const file = join(folder, "secret.txt");
    writeFileSync(file, "  the-secret \r\nsecond line\r\n");
    assert.equal(readSecret(file), "the-secret");
    writeFileSync(file, "\n");
    assert.throws(() => readSecret(file), (error: Error) => error.message === `${file} is empty`);
    assert.throws(() => readSecret(join(folder, "missing.txt")), /cannot read .*missing\.txt$/);
  } finally {
    rmSync(folder, { recursive: true, force: true });
  }
});

test("the bridge's Challonge site is the shipped definition with the app's client ID", () => {
  const shipped = { id: "challonge", display_name: "Challonge", client_id: "your-challonge-client-id", scope: "me" };
  const site = signInSite(shipped, CONFIG.clientId);
  assert.deepEqual(site, { ...shipped, client_id: CONFIG.clientId });
  assert.equal(shipped.client_id, "your-challonge-client-id", "the template is not changed");
  assert.throws(() => signInSite({ ...shipped, id: "startgg" }, CONFIG.clientId), /not Challonge's/);
  assert.throws(() => signInSite(null, CONFIG.clientId), /not Challonge's/);
});

test("the connector plays one Challonge tournament first to 1, on a request budget for one run", () => {
  const configuration = connectorConfig({
    bridgeOrigin: "http://127.0.0.1:18787",
    connectionId: "brackets",
    providerTokenFile: "/work/provider.token",
    buildId: "cd".repeat(32),
    resultsPort: 18788,
    resultSecretFile: "/work/result.secret",
    database: "/work/brackets.sqlite3",
    tournament: CONFIG.tournament,
    apiKeyFile: "/keys/organizer.key",
  });
  assert.deepEqual(configuration, {
    bridge: { api: "http://127.0.0.1:18787", credential_file: "/work/provider.token", connection_id: "brackets", build_id: "cd".repeat(32) },
    results: { listen: "127.0.0.1:18788", path: "/results", secret_file: "/work/result.secret" },
    database: "/work/brackets.sqlite3",
    monthly_request_budget: CONNECTOR_BUDGET,
    tournaments: [
      { adapter: "challonge", tournament: "ember_selftest_1", credential: { kind: "v1_key", file: "/keys/organizer.key" }, rounds: { games_to_win: 1 } },
    ],
  });
  assert.ok(CONNECTOR_BUDGET <= 50, "a run must leave the free tier's month alone");
});

const document = (attributes: Record<string, unknown>, extra: Record<string, unknown> = {}) =>
  ({ data: { id: "1234567", type: "match", attributes, ...extra } }) as never;

const players = (one: unknown, two: unknown) => ({
  player1: { data: { id: one, type: "participant" } },
  player2: { data: { id: two, type: "participant" } },
});

test("a match is read wherever Challonge puts its players, with ids as text or numbers", () => {
  const inside = parseMatch(document({ state: "open", winner_id: null, relationships: players("11", 22) }));
  assert.deepEqual(inside, { id: "1234567", state: "open", winner: null, player1: "11", player2: "22", scores: null });
  const beside = parseMatch(document({ state: "complete", winner_id: 22, scores: "0 - 1" }, { relationships: players(11, "22") }));
  assert.deepEqual(beside, { id: "1234567", state: "complete", winner: "22", player1: "11", player2: "22", scores: "0 - 1" });
  assert.equal(parseMatch(document({ state: "pending" })).player1, null);
  assert.throws(() => parseMatch(document({})), /not a match/);
  assert.throws(() => parseMatch({ errors: [] } as never), /not a match/);
});

test("a run starts only on an open match with both players", () => {
  const open = parseMatch(document({ state: "open", relationships: players("11", "22") }));
  assert.deepEqual(openProblems(open, "1234567"), []);
  assert.match(openProblems({ ...open, state: "complete" }, "1234567").join("\n"), /is complete, not open[\s\S]*reopen/);
  assert.match(openProblems({ ...open, player2: null }, "1234567").join("\n"), /both players/);
  assert.match(openProblems(open, "7654321").join("\n"), /answered match 1234567, not 7654321/);
});

test("the reported match is complete, between the same players, with the winner ahead 1 to 0", () => {
  const reported = (winner: string, scores: string | null, change: Record<string, unknown> = {}) =>
    ({ id: "1234567", state: "complete", winner, player1: "11", player2: "22", scores, ...change }) as never;
  assert.deepEqual(reportedProblems(reported("11", "1 - 0"), "11", "22"), []);
  assert.deepEqual(reportedProblems(reported("22", "0 - 1"), "22", "11"), []);
  assert.deepEqual(reportedProblems(reported("22", "0-1"), "22", "11"), []);
  assert.match(reportedProblems(reported("11", "1 - 0", { state: "open" }), "11", "22").join("\n"), /is open, not complete/);
  assert.match(reportedProblems(reported("22", "0 - 1"), "11", "22").join("\n"), /winner is 22, not participant 11[\s\S]*score reads "0 - 1"/);
  assert.match(reportedProblems(reported("11", "0 - 1"), "11", "22").join("\n"), /score reads "0 - 1", not 1 - 0/);
  assert.match(reportedProblems(reported("11", null), "11", "22").join("\n"), /score reads null/);
  assert.match(reportedProblems(reported("11", "1 - 0", { player2: "33" }), "11", "22").join("\n"), /players are 11 and 33/);
});

test("the live answer names a match's players in points_by_participant, first player first", () => {
  const live = document({
    state: "open",
    scores: "0 - 0",
    points_by_participant: [
      { participant_id: 555001, scores: [] },
      { participant_id: 555002, scores: [] },
    ],
    winner_id: null,
  });
  assert.deepEqual(parseMatch(live), { id: "1234567", state: "open", winner: null, player1: "555001", player2: "555002", scores: "0 - 0" });
  const notFed = parseMatch(document({ state: "pending", points_by_participant: [{ participant_id: 555001 }, { participant_id: null }] }));
  assert.equal(notFed.player1, "555001");
  assert.equal(notFed.player2, null);
  assert.equal(parseMatch(document({ state: "pending", points_by_participant: [] })).player1, null);
});
