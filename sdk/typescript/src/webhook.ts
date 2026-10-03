// Standard Webhooks verification for bridge deliveries (spec 20.3).
import { createHmac, timingSafeEqual } from "node:crypto";

export const TOLERANCE_SECONDS = 300;
const MAX_BODY = 64 * 1024;
const MAX_SIGNATURES = 8;

export class WebhookError extends Error {}

export interface WebhookHeaders {
  "webhook-id": string;
  "webhook-timestamp": string;
  "webhook-signature": string;
}

function secretBytes(secret: string): Buffer {
  const encoded = secret.startsWith("whsec_") ? secret.slice(6) : secret;
  const bytes = Buffer.from(encoded, "base64");
  if (bytes.length !== 32 || bytes.toString("base64") !== encoded) throw new WebhookError("invalid subscription secret");
  return bytes;
}

export function header(headers: WebhookHeaders | Headers | Record<string, string | string[] | undefined>, name: string): string {
  if (typeof (headers as Headers).get === "function") return (headers as Headers).get(name) ?? "";
  const value = (headers as Record<string, string | string[] | undefined>)[name];
  return Array.isArray(value) ? (value[0] ?? "") : (value ?? "");
}

/**
 * Verifies a delivery before its body is parsed. `rawBody` must be the exact
 * bytes received. `secrets` may hold several during a rotation overlap.
 */
export function verifyWebhook(
  rawBody: Uint8Array | string,
  headers: WebhookHeaders | Headers | Record<string, string | string[] | undefined>,
  secrets: string[],
  now: number = Math.floor(Date.now() / 1000),
): void {
  const body = typeof rawBody === "string" ? Buffer.from(rawBody, "utf8") : Buffer.from(rawBody);
  if (body.length > MAX_BODY) throw new WebhookError("body too large");
  const id = header(headers, "webhook-id");
  const timestamp = header(headers, "webhook-timestamp");
  const signature = header(headers, "webhook-signature");
  if (!/^[\x21-\x7e]{1,256}$/.test(id)) throw new WebhookError("invalid webhook-id");
  if (!/^[0-9]{1,16}$/.test(timestamp)) throw new WebhookError("invalid webhook-timestamp");
  if (Math.abs(now - Number(timestamp)) > TOLERANCE_SECONDS) throw new WebhookError("stale delivery");
  const candidates = signature.split(/\s+/).filter(Boolean);
  if (candidates.length === 0 || candidates.length > MAX_SIGNATURES) throw new WebhookError("invalid webhook-signature");
  const signed = Buffer.concat([Buffer.from(`${id}.${timestamp}.`, "ascii"), body]);
  for (const secret of secrets) {
    const expected = createHmac("sha256", secretBytes(secret)).update(signed).digest();
    for (const candidate of candidates) {
      const [version, encoded] = candidate.split(",", 2);
      if (version !== "v1" || !encoded) continue;
      const given = Buffer.from(encoded, "base64");
      if (given.length === expected.length && timingSafeEqual(given, expected)) return;
    }
  }
  throw new WebhookError("invalid signature");
}

/** Signs a delivery, for tests and for relaying. */
export function signWebhook(rawBody: Uint8Array | string, id: string, timestamp: number, secret: string): WebhookHeaders {
  const body = typeof rawBody === "string" ? Buffer.from(rawBody, "utf8") : Buffer.from(rawBody);
  const mac = createHmac("sha256", secretBytes(secret))
    .update(Buffer.concat([Buffer.from(`${id}.${timestamp}.`, "ascii"), body]))
    .digest("base64");
  return { "webhook-id": id, "webhook-timestamp": String(timestamp), "webhook-signature": `v1,${mac}` };
}
