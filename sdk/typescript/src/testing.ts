// A stand-in for an Ember player, for testing a platform integration without
// the game. It holds a throwaway Ember key in memory and does what the Ember
// helper does when a player enters a link code. Never use it for real players:
// a real Ember ID lives in the player's game and its key never leaves it.
import { randomBytes } from "node:crypto";

import { parseStrict, type Json } from "./canonical.ts";
import { decodeBase64url, emberIdFromPublicKey, fingerprint } from "./identity.ts";
import { commandDigest, publicKeyFromSeed, signChallenge, type Challenge } from "./signed.ts";
import { BridgeError } from "./client.ts";

export interface TestPlayerOptions {
  /** The bridge origin, exactly as it publishes it. */
  origin: string;
  /** A 32-byte key seed to reuse an identity across runs; random when absent. */
  seed?: Uint8Array;
  fetch?: typeof fetch;
}

/** What the bridge answers a claim with. `claim_id` is null when this Ember ID
 * is already linked to the account the code was made for. */
export interface LinkClaim {
  claim_id: string | null;
  intent_id: string | null;
  state: string;
  ember_id: string;
  fingerprint: string;
  connection_id: string;
  provider: string;
  expires_at: number | null;
}

export class TestPlayer {
  readonly origin: string;
  readonly seed: Uint8Array;
  readonly emberId: string;
  readonly #fetch: typeof fetch;
  #bridgeId: string | undefined;
  #session: { token: string; expiresAt: number } | undefined;

  constructor(options: TestPlayerOptions) {
    this.origin = options.origin;
    this.seed = options.seed ?? randomBytes(32);
    if (this.seed.length !== 32) throw new TypeError("seed must be 32 bytes");
    this.emberId = emberIdFromPublicKey(decodeBase64url(publicKeyFromSeed(this.seed), 32));
    this.#fetch = options.fetch ?? fetch;
  }

  /** The short form Ember shows next to a link request. */
  get fingerprint(): string {
    return fingerprint(this.emberId);
  }

  /** Claims a link code from `connectionId`, as a player entering it in Ember. */
  async claimLink(code: string, connectionId: string): Promise<LinkClaim> {
    const token = await this.#sessionToken();
    const command = { code, connection_id: connectionId, consent: true };
    const body = await this.#prove(token, "link.claim", "/v1/link-claims", command);
    return (await this.#send("/v1/link-claims", token, body, 201)) as unknown as LinkClaim;
  }

  /** The player's own links, as Ember lists them. */
  async links(): Promise<Json> {
    const token = await this.#sessionToken();
    const response = await this.#fetch(`${this.origin}/v1/links`, { headers: { authorization: `Bearer ${token}` } });
    return this.#read(response, 200);
  }

  async #bridge(): Promise<string> {
    if (this.#bridgeId === undefined) {
      const response = await this.#fetch(`${this.origin}/.well-known/ember-bridge.json`);
      const profile = (await this.#read(response, 200)) as { bridge_id: string; origin: string };
      if (profile.origin !== this.origin) throw new TypeError(`the bridge publishes origin ${profile.origin}, not ${this.origin}`);
      this.#bridgeId = profile.bridge_id;
    }
    return this.#bridgeId;
  }

  async #sessionToken(): Promise<string> {
    const now = Math.floor(Date.now() / 1000);
    if (this.#session === undefined || this.#session.expiresAt - 60 <= now) {
      const command = { requested_scopes: ["self:read"] };
      const body = await this.#prove(undefined, "session.create", "/v1/sessions", command);
      const session = (await this.#send("/v1/sessions", undefined, body, 201)) as { session_token: string; expires_at: number };
      this.#session = { token: session.session_token, expiresAt: session.expires_at };
    }
    return this.#session.token;
  }

  async #prove(token: string | undefined, action: string, path: string, command: Json): Promise<string> {
    const bridgeId = await this.#bridge();
    const response = await this.#fetch(`${this.origin}/v1/auth/challenges`, {
      method: "POST",
      headers: { "content-type": "application/json", ...(token ? { authorization: `Bearer ${token}` } : {}) },
      body: JSON.stringify({ public_key: publicKeyFromSeed(this.seed), action, method: "POST", path, request_digest: commandDigest(command) }),
    });
    const challenge = (await this.#read(response, 201)) as unknown as Challenge;
    const proof = signChallenge(
      this.seed,
      challenge,
      { bridgeId, audience: this.origin, emberId: this.emberId, action, method: "POST", path, command },
      Math.floor(Date.now() / 1000),
    );
    return JSON.stringify({ command, proof });
  }

  async #send(path: string, token: string | undefined, body: string, expected: number): Promise<Json> {
    const response = await this.#fetch(`${this.origin}${path}`, {
      method: "POST",
      headers: { "content-type": "application/json", ...(token ? { authorization: `Bearer ${token}` } : {}) },
      body,
    });
    return this.#read(response, expected);
  }

  async #read(response: Response, expected: number): Promise<Json> {
    const text = await response.text();
    if (response.status !== expected) {
      let error: { code?: string; message?: string; retryable?: boolean; request_id?: string } = {};
      try {
        error = (parseStrict(text) as { error?: typeof error }).error ?? {};
      } catch {
        // Not the bridge's error shape.
      }
      throw new BridgeError(response.status, error.code ?? `http_${response.status}`, error.message ?? response.statusText, error.retryable ?? response.status >= 500, error.request_id);
    }
    return parseStrict(text);
  }
}
