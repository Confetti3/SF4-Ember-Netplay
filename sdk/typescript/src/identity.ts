// Ember IDs and the canonical encodings around them (spec 6.2).
import { createHash, createPublicKey, verify as verifySignature, type KeyObject } from "node:crypto";

export const ID_PREFIX = "emb1_";
const ID_DOMAIN = Buffer.from("ember-id-v1\u0000", "ascii");
const BASE32 = "abcdefghijklmnopqrstuvwxyz234567";
// DER prefix of an Ed25519 SubjectPublicKeyInfo; the raw key follows.
const SPKI_PREFIX = Buffer.from("302a300506032b6570032100", "hex");

export class EncodingError extends Error {}

export function base64url(bytes: Uint8Array): string {
  return Buffer.from(bytes).toString("base64url");
}

/** Decodes unpadded base64url of exactly `length` bytes, canonical form only. */
export function decodeBase64url(text: string, length: number): Buffer {
  if (!/^[A-Za-z0-9_-]*$/.test(text) || text.length !== Math.ceil((length * 4) / 3)) {
    throw new EncodingError("invalid base64url");
  }
  const bytes = Buffer.from(text, "base64url");
  if (bytes.length !== length || base64url(bytes) !== text) throw new EncodingError("noncanonical base64url");
  return bytes;
}

function base32(bytes: Uint8Array): string {
  let out = "";
  let buffer = 0;
  let bits = 0;
  for (const byte of bytes) {
    buffer = ((buffer << 8) | byte) & 0xfff;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out += BASE32[(buffer >> bits) & 31];
    }
  }
  if (bits > 0) out += BASE32[(buffer << (5 - bits)) & 31];
  return out;
}

/** `emb1_` + base32(SHA-256("ember-id-v1" || 0x00 || public key)). */
export function emberIdFromPublicKey(publicKey: Uint8Array): string {
  if (publicKey.length !== 32) throw new EncodingError("an Ed25519 public key is 32 bytes");
  const digest = createHash("sha256").update(ID_DOMAIN).update(publicKey).digest();
  return ID_PREFIX + base32(digest);
}

export function isEmberId(text: string): boolean {
  return /^emb1_[a-z2-7]{51}[aq]$/.test(text);
}

/** First and last eight symbols, for people to compare. Never for lookup. */
export function fingerprint(emberId: string): string {
  if (!isEmberId(emberId)) throw new EncodingError("invalid Ember ID");
  const encoded = emberId.slice(ID_PREFIX.length);
  return `${encoded.slice(0, 8)}-${encoded.slice(-8)}`;
}

export function publicKeyObject(publicKey: string | Uint8Array): KeyObject {
  const raw = typeof publicKey === "string" ? decodeBase64url(publicKey, 32) : Buffer.from(publicKey);
  return createPublicKey({ key: Buffer.concat([SPKI_PREFIX, raw]), format: "der", type: "spki" });
}

/** Verifies an Ed25519 signature over `message`. */
export function verifyEd25519(publicKey: string | Uint8Array, message: Uint8Array, signature: string): boolean {
  try {
    return verifySignature(null, message, publicKeyObject(publicKey), decodeBase64url(signature, 64));
  } catch {
    return false;
  }
}
