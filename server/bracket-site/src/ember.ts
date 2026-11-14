// Signing in from Ember: the game proves its Ember ID with a challenge this
// site issues (audience: the site's own origin), the bridge says who signed
// it and their Discord account, and the game gets an access token that works
// on every route as that Discord user would. Organizers are the site's:
// single-use invites from the operator, redeemed by a signed-in user.
import { createHash, randomBytes, randomUUID } from "node:crypto";
import type { IncomingMessage } from "node:http";
import type { DatabaseSync } from "node:sqlite";

import { checkChallenge, decodeBase64url, emberIdFromPublicKey, ProofError, type BridgeClient, type Challenge, type Json as SdkJson, type Proof } from "../../../sdk/typescript/src/index.ts";
import { fail, readJson, text, type Json, type Route } from "./http.ts";
import type { Actor } from "./server.ts";

export const ACTION = "tournament.act";
export const SESSIONS_PATH = "/v1/sessions";
const SECONDS = { challenge: 60, access: 3600 };
const DIGEST = /^[A-Za-z0-9_-]{43}$/;
/**
 * How many challenges are given out. Nobody is signed in when asking, so the
 * limits are what bounds the rows a stranger can make: `perEmberId` a
 * challenge's lifetime (a game asks one per sign-in), and `live` unanswered
 * challenges at once, whoever asked, which also holds against a flood of new
 * keys. The site sits behind a proxy and reads no client address, so there is
 * no limit by address: every request would share the proxy's.
 */
export interface ChallengeLimits {
  perEmberId: number;
  live: number;
}
const LIMITS: ChallengeLimits = { perEmberId: 10, live: 10_000 };

export interface EmberOptions {
  db: DatabaseSync;
  /** The site's public origin: every challenge's audience. */
  origin: string;
  /** The bridge's ID, which every challenge names. */
  bridgeId: string;
  bridge: Pick<BridgeClient, "prover">;
  now?: () => number;
  /** `LIMITS` unless a test sets its own. */
  limits?: ChallengeLimits;
}

export class EmberSignIn {
  readonly #options: EmberOptions;
  /** When each Ember ID asked for its challenges still alive, in seconds; kept in memory only. */
  readonly #asked = new Map<string, number[]>();

  constructor(options: EmberOptions) {
    this.#options = options;
    options.db.exec(`CREATE TABLE IF NOT EXISTS ember_challenges (id TEXT PRIMARY KEY, doc TEXT NOT NULL, expires_at INTEGER NOT NULL);
      CREATE TABLE IF NOT EXISTS ember_tokens (hash TEXT PRIMARY KEY, discord TEXT NOT NULL, expires_at INTEGER NOT NULL);
      CREATE TABLE IF NOT EXISTS organizers (discord TEXT PRIMARY KEY, since INTEGER NOT NULL);
      CREATE TABLE IF NOT EXISTS organizer_invites (hash TEXT PRIMARY KEY, made_at INTEGER NOT NULL)`);
  }

  #now(): number {
    return Math.floor((this.#options.now?.() ?? Date.now()) / 1000);
  }

  /** One more challenge for `emberId` at `now`, or 429 when it, or the site, holds as many as it may. */
  #admit(emberId: string, now: number): void {
    const limits = this.#options.limits ?? LIMITS;
    const alive = (at: number) => at + SECONDS.challenge > now;
    if (this.#asked.size > limits.live) {
      for (const [id, times] of this.#asked) if (!times.some(alive)) this.#asked.delete(id);
    }
    const asked = (this.#asked.get(emberId) ?? []).filter(alive);
    if (asked.length >= limits.perEmberId) fail(429, "too many sign-ins for this Ember ID; try again in a minute");
    const { live } = this.#options.db.prepare("SELECT COUNT(*) AS live FROM ember_challenges").get() as { live: number };
    if (live >= limits.live) fail(429, "too many sign-ins in progress; try again in a minute");
    asked.push(now);
    this.#asked.set(emberId, asked);
  }

  /** Who a token from `POST /v1/sessions` is. */
  actor(token: string): Actor | undefined {
    const row = this.#options.db.prepare("SELECT discord FROM ember_tokens WHERE hash = ? AND expires_at > ?").get(hash(token), this.#now()) as
      | { discord: string }
      | undefined;
    return row && { discord: row.discord, operator: false, organizer: this.isOrganizer(row.discord) };
  }

  isOrganizer(discord: string): boolean {
    return !!this.#options.db.prepare("SELECT 1 FROM organizers WHERE discord = ?").get(discord);
  }

  /** A single-use invite to organize, for the operator to hand out. */
  invite(): string {
    const code = randomBytes(18).toString("base64url");
    this.#options.db.prepare("INSERT INTO organizer_invites (hash, made_at) VALUES (?, ?)").run(hash(code), this.#now());
    return code;
  }

  /** `signedIn`: who is asking, for the invite's redeemer. */
  routes(signedIn: (request: IncomingMessage) => Actor, operator: (request: IncomingMessage) => void): Route[] {
    const { db, origin, bridgeId } = this.#options;
    return [
      [
        "POST",
        /^\/v1\/auth\/challenges$/,
        async (request) => {
          // The body the bridge's own challenges take, so the game asks both alike.
          const body = await readJson(request);
          if (body.action !== ACTION || body.method !== "POST" || body.path !== SESSIONS_PATH) fail(400, `only ${ACTION} at POST ${SESSIONS_PATH}`);
          const now = this.#now();
          const emberId = emberIdOf(body.public_key);
          const requestDigest = text(body.request_digest, DIGEST, "request_digest is a SHA-256 in base64url");
          db.prepare("DELETE FROM ember_challenges WHERE expires_at <= ?").run(now);
          this.#admit(emberId, now);
          const challenge: Challenge = {
            version: 1,
            bridge_id: bridgeId,
            audience: origin,
            challenge_id: `chl_${randomUUID()}`,
            ember_id: emberId,
            action: ACTION,
            method: "POST",
            path: SESSIONS_PATH,
            request_digest: requestDigest,
            nonce: randomBytes(32).toString("base64url"),
            issued_at: now,
            expires_at: now + SECONDS.challenge,
          };
          db.prepare("INSERT INTO ember_challenges (id, doc, expires_at) VALUES (?, ?, ?)").run(challenge.challenge_id, JSON.stringify(challenge), challenge.expires_at);
          return [201, challenge as unknown as Json];
        },
      ],
      [
        "POST",
        /^\/v1\/sessions$/,
        async (request) => {
          const body = await readJson(request);
          const proof = body.proof as Proof | undefined;
          const command = body.command;
          if (typeof proof?.challenge_id !== "string" || typeof proof.signature !== "string" || typeof command !== "object" || command === null || Array.isArray(command)) {
            fail(400, "the body is { command, proof }");
          }
          // Taken at once: a challenge answers one request, right or wrong.
          const row = db.prepare("DELETE FROM ember_challenges WHERE id = ? RETURNING doc").get(proof.challenge_id) as { doc: string } | undefined;
          const challenge = row ? (JSON.parse(row.doc) as Challenge) : fail(401, "no such challenge, or it was used");
          try {
            checkChallenge(
              challenge,
              { bridgeId, audience: origin, emberId: challenge.ember_id, action: ACTION, method: "POST", path: SESSIONS_PATH, command: command as SdkJson },
              this.#now(),
            );
          } catch (error) {
            if (error instanceof ProofError) fail(401, error.message);
            throw error;
          }
          // The bridge refuses a signature that is not the Ember ID's.
          const signer = await this.#options.bridge.prover(challenge, proof).catch(() => fail(401, "the bridge did not accept the proof"));
          if (signer.ember_id !== challenge.ember_id) fail(401, "the proof is another player's");
          const discord = signer.player?.discord_user_id ?? fail(409, "connect Discord in Ember first");
          const token = randomBytes(32).toString("base64url");
          db.prepare("DELETE FROM ember_tokens WHERE expires_at <= ?").run(this.#now());
          db.prepare("INSERT INTO ember_tokens (hash, discord, expires_at) VALUES (?, ?, ?)").run(hash(token), discord, this.#now() + SECONDS.access);
          return [201, { access_token: token, expires_in: SECONDS.access, discord, organizer: this.isOrganizer(discord) }];
        },
      ],
      [
        "POST",
        /^\/organizer-invites$/,
        (request) => {
          operator(request);
          return [201, { code: this.invite() }];
        },
      ],
      [
        "POST",
        /^\/organizer-invites\/redeem$/,
        async (request) => {
          const discord = signedIn(request).discord ?? fail(400, "who? (X-Discord-User)");
          const code = text((await readJson(request)).code, /^[A-Za-z0-9_-]{1,64}$/, "code is an invite code");
          if (db.prepare("DELETE FROM organizer_invites WHERE hash = ?").run(hash(code)).changes !== 1) fail(404, "no such invite, or it was used");
          db.prepare("INSERT OR IGNORE INTO organizers (discord, since) VALUES (?, ?)").run(discord, this.#now());
          return [200, { organizer: true }];
        },
      ],
    ];
  }
}

function emberIdOf(publicKey: unknown): string {
  try {
    if (typeof publicKey === "string") return emberIdFromPublicKey(decodeBase64url(publicKey, 32));
  } catch {
    // Answered below.
  }
  return fail(400, "public_key is an Ed25519 key in base64url");
}

function hash(value: string): string {
  return createHash("sha256").update(value).digest("base64url");
}
