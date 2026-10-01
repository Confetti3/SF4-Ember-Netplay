// The specification's public fixtures, checked by this second implementation
// (AUTH-01, AUTH-02, AUTH-05, WEB-01, WEB-02). All seeds and secrets are
// public test values.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import {
  canonicalize,
  commandDigest,
  emberIdFromPublicKey,
  fingerprint,
  parseEvent,
  parseStrict,
  publicKeyFromSeed,
  signChallenge,
  signingBytes,
  signWebhook,
  verifyProof,
  verifyReport,
  verifyWebhook,
  CHALLENGE_DOMAIN,
  type Challenge,
  type Json,
  type Proof,
  type SignedReport,
} from "../src/index.ts";

const examples = new URL("../../../docs/design/identity-bridge/examples/", import.meta.url);
const text = (name: string) => readFileSync(new URL(name, examples), "utf8");
const load = <T>(name: string) => parseStrict(text(name)) as T;

const vectors = load<{
  identities: { seed_hex: string; public_key_base64url: string; ember_id: string }[];
  challenge: { command: Json; canonical_utf8: string; signing_bytes_hex: string; signature_base64url: string };
}>("test-vectors.json");

test("identity derivation vectors", () => {
  for (const vector of vectors.identities) {
    const key = publicKeyFromSeed(Buffer.from(vector.seed_hex, "hex"));
    assert.equal(key, vector.public_key_base64url);
    assert.equal(emberIdFromPublicKey(Buffer.from(key, "base64url")), vector.ember_id);
    assert.equal(vector.ember_id.length, 57);
  }
  assert.equal(fingerprint(vectors.identities[0]!.ember_id), "j25zrhe6-pmdhvlja");
});

const challenge = load<Challenge>("auth-challenge.json");
const proof = load<Proof>("auth-proof.json");
const seed = Buffer.from(vectors.identities[0]!.seed_hex, "hex");
const publicKey = vectors.identities[0]!.public_key_base64url;
const expected = {
  bridgeId: challenge.bridge_id,
  audience: "https://bridge.ember.example",
  emberId: challenge.ember_id,
  action: "session.create",
  method: "POST" as const,
  path: "/v1/sessions",
  command: vectors.challenge.command,
};
const now = challenge.issued_at + 1;

test("challenge canonical and signing bytes", () => {
  assert.equal(canonicalize(challenge as unknown as Json), vectors.challenge.canonical_utf8);
  assert.equal(signingBytes(CHALLENGE_DOMAIN, challenge as unknown as Json).toString("hex"), vectors.challenge.signing_bytes_hex);
  assert.equal(commandDigest(vectors.challenge.command), challenge.request_digest);
});

test("challenge proof verifies and signing reproduces it", () => {
  verifyProof(challenge, proof, publicKey, expected, now);
  assert.deepEqual(signChallenge(seed, challenge, expected, now), proof);
});

test("challenge rejections", () => {
  const tampered = { ...challenge, nonce: Buffer.alloc(32).toString("base64url") };
  assert.throws(() => verifyProof(tampered, proof, publicKey, expected, now), /invalid signature/);
  assert.throws(() => verifyProof({ ...challenge, audience: "https://attacker.example" }, proof, publicKey, expected, now), /audience/);
  assert.throws(() => verifyProof(challenge, proof, publicKey, { ...expected, command: { requested_scopes: ["admin"] } }, now), /request_digest/);
  assert.throws(() => verifyProof(challenge, proof, publicKey, expected, challenge.expires_at), /expired/);
  assert.throws(() => verifyProof(challenge, proof, vectors.identities[1]!.public_key_base64url, expected, now), /key does not match/);
  assert.throws(() => signChallenge(seed, challenge, { ...expected, action: "link.claim" }, now), /action/);
});

test("reports verify, agree, and reject tampering", () => {
  const a = load<SignedReport>("game-report-p1.json");
  const b = load<SignedReport>("game-report-p2.json");
  const ra = verifyReport(a);
  const rb = verifyReport(b);
  assert.notEqual(ra.reporter_id, rb.reporter_id);
  for (const field of ["match_id", "attempt_id", "permit_id", "room_id", "p1_id", "p2_id", "result"]) assert.equal(ra[field], rb[field]);
  assert.throws(() => verifyReport({ ...a, report: { ...a.report, result: "p2_win" } }), /invalid signature/);
  assert.throws(() => verifyReport({ ...a, public_key: b.public_key }), /reporter/);
  assert.throws(() => verifyReport({ ...a, report: { ...a.report, confirmed_input_frame: "1" } }), /not confirmed/);
  assert.throws(() => verifyReport({ ...a, report: { ...a.report, match_generation: "03" } }), /canonical counter/);
  assert.throws(() => verifyReport({ ...a, report: { ...a.report, match_generation: "18446744073709551616" } }), /canonical counter/);
});

test("strict JSON", () => {
  assert.throws(() => parseStrict('{"a":1,"a":2}'), /duplicate/);
  for (const bad of ['{"n":1.5}', '{"n":1e3}', '{"n":9007199254740992}', "[1] x", '{"a":"\\ud800"}', "-0"]) {
    assert.throws(() => parseStrict(bad), bad);
  }
  const deep = "[".repeat(17) + "0" + "]".repeat(17);
  assert.throws(() => parseStrict(deep), /deep/);
  assert.doesNotThrow(() => parseStrict("[".repeat(16) + "0" + "]".repeat(16)));
  // RFC 8785 orders keys by UTF-16 code units.
  assert.equal(canonicalize(parseStrict('{"\\ufb33":1,"\\ud83d\\ude00":2,"\\u20ac":3}')), '{"€":3,"😀":2,"דּ":1}');
  assert.equal(parseStrict('{"__proto__":{"x":1}}')!.constructor, Object);
});

test("webhook fixture", () => {
  const fixture = load<{ secret_base64: string; verification_time_unix: number; headers: Record<string, string> }>("webhook-fixture.json");
  const body = readFileSync(new URL("match-completed-event.json", examples));
  const at = fixture.verification_time_unix;
  verifyWebhook(body, fixture.headers, [fixture.secret_base64], at);
  assert.throws(() => verifyWebhook(Buffer.concat([body, Buffer.from(" ")]), fixture.headers, [fixture.secret_base64], at), /signature/);
  assert.throws(() => verifyWebhook(body, fixture.headers, [fixture.secret_base64], at + 301), /stale/);
  assert.throws(() => verifyWebhook(body, fixture.headers, [Buffer.alloc(32).toString("base64")], at), /signature/);
  const signed = signWebhook(body, fixture.headers["webhook-id"]!, at, fixture.secret_base64);
  assert.equal(signed["webhook-signature"], fixture.headers["webhook-signature"]);
  const event = parseEvent(body);
  assert.equal(event.type, "io.ember.tournament.match.completed.v1");
  assert.equal(event.emberseq, "12");
});
