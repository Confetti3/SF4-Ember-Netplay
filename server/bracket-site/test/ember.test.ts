// Signing in from Ember: a challenge the site issues, the game's proof, the
// bridge's word on who signed it (a stand-in that checks the signature as
// the bridge does), and the site's own organizers.
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
import { mkdtempSync, rmSync } from "node:fs";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";

import {
  commandDigest,
  decodeBase64url,
  emberIdFromPublicKey,
  publicKeyFromSeed,
  signChallenge,
  verifyProof,
  type Challenge,
  type Proof,
} from "../../../sdk/typescript/src/index.ts";
import { EmberSignIn } from "../src/ember.ts";
import { bracketSite } from "../src/server.ts";
import { Store } from "../src/store.ts";

const ORIGIN = "https://brackets.example";
const BRIDGE = "brg_00000000-0000-4000-8000-000000000001";
const OPERATOR = "operator-test-token";
const DISCORD = "100000000000000001";
const seeds = { kate: randomBytes(32), sam: randomBytes(32) };
const idOf = (seed: Uint8Array) => emberIdFromPublicKey(decodeBase64url(publicKeyFromSeed(seed), 32));
const keys = new Map(Object.values(seeds).map((seed) => [idOf(seed), publicKeyFromSeed(seed)]));
/** Kate connected Discord in Ember; Sam did not. */
const bridge = {
  async prover(challenge: Challenge, proof: Proof) {
    verifyProof(challenge, proof, keys.get(challenge.ember_id)!, { bridgeId: BRIDGE, audience: ORIGIN, emberId: challenge.ember_id, action: challenge.action, method: challenge.method, path: challenge.path, command: {} }, Math.floor(Date.now() / 1000));
    const kate = challenge.ember_id === idOf(seeds.kate);
    return { ember_id: challenge.ember_id, ...(kate ? { player: { discord_user_id: DISCORD, ember_id: challenge.ember_id, participant_id: "prt_1" } } : {}) };
  },
};

let dir: string;
let store: Store;
let base: string;
let close: () => void;

before(async () => {
  dir = mkdtempSync(join(tmpdir(), "bracket-site-ember-"));
  store = new Store(join(dir, "site.sqlite3"));
  const ember = new EmberSignIn({ db: store.db, origin: ORIGIN, bridgeId: BRIDGE, bridge });
  const server = bracketSite({ store, operatorToken: OPERATOR, connectorToken: "connector", ember, bridge: { lookupPlayers: async () => [] } });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  close = () => server.close();
});

after(() => {
  close();
  store.close();
  rmSync(dir, { recursive: true, force: true });
});

async function call(method: string, path: string, body?: object, token?: string): Promise<[number, Record<string, unknown>]> {
  const response = await fetch(base + path, {
    method,
    headers: { "content-type": "application/json", ...(token ? { authorization: `Bearer ${token}` } : {}) },
    ...(body ? { body: JSON.stringify(body) } : {}),
  });
  const text = await response.text();
  return [response.status, text ? (JSON.parse(text) as Record<string, unknown>) : {}];
}

/** What the game does: ask for a challenge, sign it, send the proof. */
async function signIn(seed: Uint8Array, change?: (challenge: Challenge) => void): Promise<[number, Record<string, unknown>]> {
  const [status, issued] = await call("POST", "/v1/challenges", { ember_id: idOf(seed), request_digest: commandDigest({}) });
  assert.equal(status, 201);
  const challenge = issued as unknown as Challenge;
  const proof = signChallenge(seed, challenge, { bridgeId: BRIDGE, audience: ORIGIN, emberId: idOf(seed), action: "tournament.act", method: "POST", path: "/v1/sessions", command: {} }, Math.floor(Date.now() / 1000));
  change?.(challenge);
  return call("POST", "/v1/sessions", { command: {}, proof });
}

test("the game signs in with a proof, as its Discord account", async () => {
  const [status, session] = await signIn(seeds.kate);
  assert.equal(status, 201, JSON.stringify(session));
  assert.deepEqual([session.discord, session.organizer], [DISCORD, false]);
  // The token works on the site's own routes; making a tournament needs an organizer.
  const [mine] = await call("GET", "/tournaments", undefined, session.access_token as string);
  assert.equal(mine, 200);
  const [made] = await call("POST", "/tournaments", { title: "Thursday" }, session.access_token as string);
  assert.equal(made, 403);
});

test("a challenge answers once, and a player without Discord is told to connect it", async () => {
  const [status, issued] = await call("POST", "/v1/challenges", { ember_id: idOf(seeds.kate), request_digest: commandDigest({}) });
  assert.equal(status, 201);
  const challenge = issued as unknown as Challenge;
  const proof = signChallenge(seeds.kate, challenge, { bridgeId: BRIDGE, audience: ORIGIN, emberId: idOf(seeds.kate), action: "tournament.act", method: "POST", path: "/v1/sessions", command: {} }, Math.floor(Date.now() / 1000));
  assert.equal((await call("POST", "/v1/sessions", { command: {}, proof }))[0], 201);
  assert.equal((await call("POST", "/v1/sessions", { command: {}, proof }))[0], 401);
  // Another command than the one signed for.
  const [, again] = await call("POST", "/v1/challenges", { ember_id: idOf(seeds.kate), request_digest: commandDigest({}) });
  const second = signChallenge(seeds.kate, again as unknown as Challenge, { bridgeId: BRIDGE, audience: ORIGIN, emberId: idOf(seeds.kate), action: "tournament.act", method: "POST", path: "/v1/sessions", command: {} }, Math.floor(Date.now() / 1000));
  assert.equal((await call("POST", "/v1/sessions", { command: { other: true }, proof: second }))[0], 401);
  const [sam, answer] = await signIn(seeds.sam);
  assert.deepEqual([sam, answer.error], [409, "connect Discord in Ember first"]);
});

test("the operator's invite makes an organizer, once", async () => {
  assert.equal((await call("POST", "/organizer-invites", {}, "nobody"))[0], 401);
  const [, invite] = await call("POST", "/organizer-invites", {}, OPERATOR);
  const [, session] = await signIn(seeds.kate);
  const token = session.access_token as string;
  assert.equal((await call("POST", "/organizer-invites/redeem", { code: invite.code }, token))[0], 200);
  assert.equal((await call("POST", "/organizer-invites/redeem", { code: invite.code }, token))[0], 404);
  const [made, tournament] = await call("POST", "/tournaments", { title: "Thursday" }, token);
  assert.deepEqual([made, tournament.organizers], [201, [DISCORD]]);
  const [, later] = await signIn(seeds.kate);
  assert.equal(later.organizer, true);
});
