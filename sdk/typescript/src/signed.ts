// Challenge proofs and signed game reports (spec 10.1, 16.2). A second
// implementation of the bridge's rules, checked against the same fixtures.
import { createHash, createPrivateKey, createPublicKey, sign } from "node:crypto";

import { canonicalize, type Json } from "./canonical.ts";
import { base64url, decodeBase64url, emberIdFromPublicKey, verifyEd25519 } from "./identity.ts";

export const CHALLENGE_DOMAIN = "EMBER:CHALLENGE:1\n";
export const REPORT_DOMAIN = "EMBER:GAME-REPORT:1\n";
const PKCS8_PREFIX = Buffer.from("302e020100300506032b657004220420", "hex");

export interface Challenge {
  version: 1;
  bridge_id: string;
  audience: string;
  challenge_id: string;
  ember_id: string;
  action: string;
  method: "POST" | "DELETE";
  path: string;
  request_digest: string;
  nonce: string;
  issued_at: number;
  expires_at: number;
}

export interface Proof {
  challenge_id: string;
  signature: string;
}

export interface Expected {
  bridgeId: string;
  audience: string;
  emberId: string;
  action: string;
  method: "POST" | "DELETE";
  path: string;
  command: Json;
}

export class ProofError extends Error {}

/** `base64url(SHA-256(JCS(command)))`. */
export function commandDigest(command: Json): string {
  return base64url(createHash("sha256").update(canonicalize(command), "utf8").digest());
}

export function signingBytes(domain: string, value: Json): Buffer {
  return Buffer.from(domain + canonicalize(value), "utf8");
}

/** Checks a challenge against the operation it should authorize. */
export function checkChallenge(challenge: Challenge, expected: Expected, now: number): void {
  const mismatches: [boolean, string][] = [
    [challenge.version === 1, "version"],
    [challenge.bridge_id === expected.bridgeId, "bridge_id"],
    [challenge.audience === expected.audience, "audience"],
    [challenge.ember_id === expected.emberId, "ember_id"],
    [challenge.action === expected.action, "action"],
    [challenge.method === expected.method, "method"],
    [challenge.path === expected.path, "path"],
    [challenge.request_digest === commandDigest(expected.command), "request_digest"],
  ];
  const failed = mismatches.find(([ok]) => !ok);
  if (failed) throw new ProofError(`challenge ${failed[1]} does not match`);
  const lifetime = challenge.expires_at - challenge.issued_at;
  if (lifetime <= 0 || lifetime > 60) throw new ProofError("challenge lifetime is invalid");
  if (now < challenge.issued_at || now >= challenge.expires_at) throw new ProofError("challenge expired");
  decodeBase64url(challenge.nonce, 32);
}

/** Verifies a proof for `challenge` by the holder of `publicKey`. */
export function verifyProof(challenge: Challenge, proof: Proof, publicKey: string, expected: Expected, now: number): void {
  checkChallenge(challenge, expected, now);
  if (emberIdFromPublicKey(decodeBase64url(publicKey, 32)) !== challenge.ember_id) throw new ProofError("key does not match ID");
  if (proof.challenge_id !== challenge.challenge_id) throw new ProofError("proof names another challenge");
  if (!verifyEd25519(publicKey, signingBytes(CHALLENGE_DOMAIN, challenge as unknown as Json), proof.signature)) {
    throw new ProofError("invalid signature");
  }
}

/**
 * Signs a checked challenge with a 32-byte Ed25519 seed. For test clients
 * and bots that hold their own identity; Ember players sign in the helper.
 */
export function signChallenge(seed: Uint8Array, challenge: Challenge, expected: Expected, now: number): Proof {
  checkChallenge(challenge, expected, now);
  const key = createPrivateKey({ key: Buffer.concat([PKCS8_PREFIX, Buffer.from(seed)]), format: "der", type: "pkcs8" });
  return {
    challenge_id: challenge.challenge_id,
    signature: base64url(sign(null, signingBytes(CHALLENGE_DOMAIN, challenge as unknown as Json), key)),
  };
}

export function publicKeyFromSeed(seed: Uint8Array): string {
  const key = createPrivateKey({ key: Buffer.concat([PKCS8_PREFIX, Buffer.from(seed)]), format: "der", type: "pkcs8" });
  const spki = createPublicKey(key).export({ format: "der", type: "spki" });
  return base64url(spki.subarray(spki.length - 32));
}

export interface SignedReport {
  report: { [key: string]: Json };
  public_key: string;
  signature: string;
}

const COUNTER = /^(0|[1-9][0-9]{0,19})$/;

/** Verifies a fighter's signed report and its internal consistency. */
export function verifyReport(envelope: SignedReport): { [key: string]: Json } {
  const report = envelope.report;
  const id = emberIdFromPublicKey(decodeBase64url(envelope.public_key, 32));
  if (report.reporter_id !== id) throw new ProofError("reporter does not match key");
  if (report.p1_id === report.p2_id) throw new ProofError("duplicate fighter");
  const fighters = new Set([report.p1_id, report.p2_id]);
  if (report.reporter_id === report.opponent_id || !fighters.has(report.reporter_id ?? null) || !fighters.has(report.opponent_id ?? null)) {
    throw new ProofError("reporter is not a fighter");
  }
  for (const field of ["assignment_generation", "match_generation", "capture_frame", "confirmed_input_frame"]) {
    const value = report[field];
    if (value === null && field.endsWith("frame")) continue;
    if (typeof value !== "string" || !COUNTER.test(value) || BigInt(value) > 2n ** 64n - 1n) {
      throw new ProofError(`${field} is not a canonical counter`);
    }
  }
  if (["p1_win", "p2_win", "draw"].includes(report.result as string)) {
    const capture = BigInt(report.capture_frame as string);
    const confirmed = BigInt(report.confirmed_input_frame as string);
    if (confirmed < capture - 1n) throw new ProofError("result inputs are not confirmed");
  }
  if (!verifyEd25519(envelope.public_key, signingBytes(REPORT_DOMAIN, report), envelope.signature)) {
    throw new ProofError("invalid signature");
  }
  return report;
}
