// A stand-in for an Ember player, for testing a platform integration without
// the game. It holds a throwaway Ember key in memory and does what the Ember
// helper does: it claims link codes, and for `ember-room-v1` matches it claims
// the match, publishes a pretend room, prepares games and signs their reports.
// Never use it for real players: a real Ember ID lives in the player's game
// and its key never leaves it.
import { randomBytes, randomUUID } from "node:crypto";

import { parseStrict, type Json } from "./canonical.ts";
import { decodeBase64url, emberIdFromPublicKey, fingerprint } from "./identity.ts";
import { commandDigest, publicKeyFromSeed, signChallenge, signReport, type Challenge, type SignedReport } from "./signed.ts";
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

/** The bridge's room binding: which endpoint plays which slot in which room. */
export interface Binding {
  version: number;
  bridge_id: string;
  match_id: string;
  assignment_generation: string;
  binding_revision: string;
  room_id: string;
  build_id: string;
  games_to_win: number;
  rules_digest: string;
  roster_digest: string;
  fighters: [{ ember_id: string; endpoint_id: string }, { ember_id: string; endpoint_id: string }];
  issued_at: number;
}

/** One official game, signed by the bridge. */
export interface Permit {
  version: number;
  bridge_id: string;
  match_id: string;
  assignment_generation: string;
  attempt_id: string;
  permit_id: string;
  room_id: string;
  table_id: number;
  match_generation: string;
  p1_id: string;
  p2_id: string;
  rules_digest: string;
  roster_digest: string;
  build_id: string;
  issued_at: number;
  start_by: number;
}

export type ClaimAnswer =
  | { role: "host"; lease_id: string; fence: string; expires_at: number }
  | { role: "wait"; retry_after: number }
  | { role: "room"; room_id: string; invitation: string; binding: { binding: Binding; kid: string; signature: string } | null };

export type PrepareAnswer =
  | { state: "pending"; retry_after: number }
  | { state: "permitted"; permit: { permit: Permit; kid: string; signature: string } };

export type GameResult = "p1_win" | "p2_win" | "draw" | "abort" | "cancel";

export interface ReportReceipt {
  report_id: string;
  attempt_id: string;
  attempt_state: string;
  match_state: string;
}

export class TestPlayer {
  readonly origin: string;
  readonly seed: Uint8Array;
  readonly emberId: string;
  /** A pretend Iroh endpoint for this player's helper run. */
  readonly endpointId: string = randomBytes(32).toString("hex");
  readonly helperInstanceId: string = `ins_${randomUUID()}`;
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

  /**
   * Starts connecting a Discord account, as Ember's Connect Discord does. The
   * bridge answers with the Discord address the player's browser goes to; the
   * account is connected when Discord sends the browser back to the bridge.
   */
  async startDiscordConnect(): Promise<{ authorize_url: string; expires_at: number }> {
    const token = await this.#sessionToken();
    const path = "/v1/discord/start";
    const body = await this.#prove(token, "discord.connect", path, {});
    return (await this.#send(path, token, body, 201)) as unknown as { authorize_url: string; expires_at: number };
  }

  /** The player's own links, as Ember lists them. */
  async links(): Promise<Json> {
    const token = await this.#sessionToken();
    const response = await this.#fetch(`${this.origin}/v1/links`, { headers: { authorization: `Bearer ${token}` } });
    return this.#read(response, 200);
  }

  /**
   * Claims an `ember-room-v1` match. The first fighter gets the provisioning
   * lease (`host`), the other waits; once a room is published both get it,
   * with the bridge's binding when both have claimed. Claim again to renew.
   */
  async claimMatch(matchId: string, buildId = "test-build"): Promise<ClaimAnswer> {
    const command = { endpoint_id: this.endpointId, helper_instance_id: this.helperInstanceId, build_id: buildId };
    return (await this.#act("match.claim", `/v1/matches/${matchId}/claims`, command)) as unknown as ClaimAnswer;
  }

  /** Publishes the match's room: with the lease from a `host` claim for the
   * first room, or naming the room it replaces. */
  async publishRoom(
    matchId: string,
    room: { roomId?: string; leaseId?: string; fence?: string; replaces?: string },
  ): Promise<ClaimAnswer> {
    const roomId = room.roomId ?? randomBytes(16).toString("hex");
    const command = {
      room_id: roomId,
      invitation: `test-room:${roomId}`,
      lease_id: room.leaseId ?? null,
      fence: room.fence ?? null,
      replaces: room.replaces ?? null,
    };
    return (await this.#act("room.publish", `/v1/matches/${matchId}/room`, command)) as unknown as ClaimAnswer;
  }

  /** Describes the next game from the binding; the bridge answers with the
   * permit once both fighters have described the same game. */
  async prepareGame(matchId: string, binding: Binding, matchGeneration: number): Promise<PrepareAnswer> {
    const command = {
      assignment_generation: binding.assignment_generation,
      binding_revision: binding.binding_revision,
      room_id: binding.room_id,
      table_id: 0,
      match_generation: String(matchGeneration),
      rules_digest: binding.rules_digest,
      roster_digest: binding.roster_digest,
      build_id: binding.build_id,
      endpoint_id: this.endpointId,
    };
    return (await this.#act("attempt.prepare", `/v1/matches/${matchId}/attempts/prepare`, command)) as unknown as PrepareAnswer;
  }

  /** This player's signed report of a permitted game. */
  report(permit: Permit, result: GameResult): SignedReport {
    const native = result === "p1_win" || result === "p2_win" || result === "draw";
    const opponent = permit.p1_id === this.emberId ? permit.p2_id : permit.p1_id;
    return signReport(this.seed, {
      version: 1,
      bridge_id: permit.bridge_id,
      match_id: permit.match_id,
      assignment_generation: permit.assignment_generation,
      attempt_id: permit.attempt_id,
      permit_id: permit.permit_id,
      observation_id: `obs_${randomUUID()}`,
      room_id: permit.room_id,
      table_id: permit.table_id,
      match_generation: permit.match_generation,
      reporter_id: this.emberId,
      opponent_id: opponent,
      p1_id: permit.p1_id,
      p2_id: permit.p2_id,
      rules_digest: permit.rules_digest,
      roster_digest: permit.roster_digest,
      build_id: permit.build_id,
      result,
      capture_frame: native ? "900" : null,
      confirmed_input_frame: native ? "899" : null,
      observed_at: Math.floor(Date.now() / 1000),
      helper_instance_id: this.helperInstanceId,
    });
  }

  /** Sends a signed report: this player's own, or the other fighter's, unchanged. */
  async submitReport(matchId: string, signed: SignedReport): Promise<ReportReceipt> {
    const token = await this.#sessionToken();
    const response = await this.#fetch(`${this.origin}/v1/matches/${matchId}/reports`, {
      method: "POST",
      headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
      body: JSON.stringify(signed),
    });
    // An exact retry answers 200 with the original receipt.
    return (await this.#read(response, response.status === 200 ? 200 : 201)) as unknown as ReportReceipt;
  }

  async #act(action: string, path: string, command: Json): Promise<Json> {
    const token = await this.#sessionToken();
    return this.#send(path, token, await this.#prove(token, action, path, command), 200);
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
      const command = { requested_scopes: ["self:read", "tournament:participate"] };
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
