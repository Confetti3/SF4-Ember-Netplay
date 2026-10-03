// Challenge proofs and signed game reports (spec 10.1, 16.2). A second
// implementation of the bridge's rules, checked against the same fixtures.
import { createHash, createPrivateKey, createPublicKey, sign } from "node:crypto";

import { canonicalize, type Json } from "./canonical.ts";
import { base64url, decodeBase64url, emberIdFromPublicKey, isEmberId, verifyEd25519 } from "./identity.ts";

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
const PREFIXED_UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const REPORT_FIELDS = [
  "version", "bridge_id", "match_id", "assignment_generation", "attempt_id", "permit_id", "observation_id",
  "room_id", "table_id", "match_generation", "reporter_id", "opponent_id", "p1_id", "p2_id", "rules_digest",
  "roster_digest", "build_id", "result", "capture_frame", "confirmed_input_frame", "observed_at", "helper_instance_id",
];
const OUTCOMES = ["p1_win", "p2_win", "draw", "abort", "cancel"];

/** Whether `value` is a string made of `<prefix>_` and a lowercase version 4 UUID. */
export function prefixedId(value: unknown, prefix: string): boolean {
  return typeof value === "string" && value.startsWith(`${prefix}_`) && PREFIXED_UUID.test(value.slice(prefix.length + 1));
}

/** A canonical counter as a bigint, or null when the value is not one. */
export function counter(value: unknown): bigint | null {
  if (typeof value !== "string" || !COUNTER.test(value)) return null;
  const parsed = BigInt(value);
  return parsed > 2n ** 64n - 1n ? null : parsed;
}

function digest(value: Json | undefined): boolean {
  if (typeof value !== "string") return false;
  try {
    decodeBase64url(value, 32);
    return true;
  } catch {
    return false;
  }
}

/** The frozen report rules (spec 16.2), the same checks as the Rust protocol crate. */
export function checkReport(report: { [key: string]: Json }): void {
  const keys = Object.keys(report);
  const unknown = keys.find((key) => !REPORT_FIELDS.includes(key));
  if (unknown !== undefined) throw new ProofError(`unknown report field ${unknown}`);
  // Every field is present; the two frames may be null. Rust signs an absent
  // frame as null, so a report that omits one would not verify there.
  const missing = REPORT_FIELDS.find((key) => !(key in report));
  if (missing !== undefined) throw new ProofError(`${missing} is missing`);
  const room = report.room_id;
  const assignment = counter(report.assignment_generation);
  const generation = counter(report.match_generation);
  const checks: [boolean, string][] = [
    [report.version === 1, "version"],
    [prefixedId(report.bridge_id, "brg"), "bridge_id"],
    [prefixedId(report.match_id, "emt"), "match_id"],
    [prefixedId(report.attempt_id, "ega"), "attempt_id"],
    [prefixedId(report.permit_id, "per"), "permit_id"],
    [prefixedId(report.observation_id, "obs"), "observation_id"],
    [prefixedId(report.helper_instance_id, "ins"), "helper_instance_id"],
    [typeof room === "string" && /^[0-9a-f]{32}$/.test(room), "room_id"],
    [Number.isInteger(report.table_id) && (report.table_id as number) >= 0 && (report.table_id as number) <= 3, "table_id"],
    [assignment !== null && assignment > 0n, "assignment_generation"],
    [generation !== null && generation > 0n, "match_generation"],
    [typeof report.build_id === "string" && report.build_id.length > 0 && Buffer.byteLength(report.build_id) <= 128, "build_id"],
    [Number.isSafeInteger(report.observed_at) && (report.observed_at as number) >= 0, "observed_at"],
    [OUTCOMES.includes(report.result as string), "result"],
    [digest(report.rules_digest), "rules_digest"],
    [digest(report.roster_digest), "roster_digest"],
    [["reporter_id", "opponent_id", "p1_id", "p2_id"].every((key) => typeof report[key] === "string" && isEmberId(report[key] as string)), "ember_id"],
  ];
  const failed = checks.find(([ok]) => !ok);
  if (failed) throw new ProofError(`${failed[1]} is invalid`);
  if (report.p1_id === report.p2_id) throw new ProofError("duplicate fighter");
  const fighters = new Set([report.p1_id, report.p2_id]);
  if (report.reporter_id === report.opponent_id || !fighters.has(report.reporter_id) || !fighters.has(report.opponent_id)) {
    throw new ProofError("reporter is not a fighter");
  }
  const frame = (key: string): bigint | null | undefined => {
    const value = report[key];
    if (value === null) return undefined;
    const parsed = counter(value);
    if (parsed === null) throw new ProofError(`${key} is not a canonical counter`);
    return parsed;
  };
  const capture = frame("capture_frame");
  const confirmed = frame("confirmed_input_frame");
  if (["p1_win", "p2_win", "draw"].includes(report.result as string)) {
    if (capture === undefined || confirmed === undefined) throw new ProofError("capture_frame is invalid");
    // Publication boundary: inputs confirmed through at least N - 1.
    if (capture === null || confirmed === null || capture <= 0n || confirmed < capture - 1n) {
      throw new ProofError("result inputs are not confirmed");
    }
  } else if (capture !== undefined && capture !== null && capture <= 0n) {
    throw new ProofError("capture_frame is invalid");
  }
}

/** Verifies a fighter's signed report and its internal consistency. */
export function verifyReport(envelope: SignedReport): { [key: string]: Json } {
  const report = envelope.report;
  const id = emberIdFromPublicKey(decodeBase64url(envelope.public_key, 32));
  if (report.reporter_id !== id) throw new ProofError("reporter does not match key");
  checkReport(report);
  if (!verifyEd25519(envelope.public_key, signingBytes(REPORT_DOMAIN, report), envelope.signature)) {
    throw new ProofError("invalid signature");
  }
  return report;
}

/** Signs a fighter's report with its key seed, after the same checks `verifyReport` makes. */
export function signReport(seed: Uint8Array, report: { [key: string]: Json }): SignedReport {
  checkReport(report);
  const publicKey = publicKeyFromSeed(seed);
  if (report.reporter_id !== emberIdFromPublicKey(decodeBase64url(publicKey, 32))) {
    throw new ProofError("reporter does not match key");
  }
  const key = createPrivateKey({ key: Buffer.concat([PKCS8_PREFIX, Buffer.from(seed)]), format: "der", type: "pkcs8" });
  return {
    report,
    public_key: publicKey,
    signature: base64url(sign(null, signingBytes(REPORT_DOMAIN, report), key)),
  };
}
