// A typed client for provider and organizer services (spec 19, 21.1).
import { randomUUID } from "node:crypto";

import { parseStrict, type Json } from "./canonical.ts";
import { parseEvent, type BridgeEvent, type ConnectionRoom } from "./events.ts";
import { isBridgeOrigin } from "./signed.ts";

export interface BridgeClientOptions {
  /** The bridge origin, e.g. `https://bridge.example`. */
  origin: string;
  /** A provider or organizer credential (`emk_...`). */
  credential: string;
  /**
   * Per-request timeout, `createRoom` included. Default 15 seconds, except
   * that `createRoom` waits 45: the bridge gives a room's host up to 30
   * seconds to start.
   */
  timeoutMs?: number;
  fetch?: typeof fetch;
}

/** The bridge's error contract (spec 19.5). */
export class BridgeError extends Error {
  readonly status: number;
  readonly code: string;
  readonly retryable: boolean;
  readonly requestId: string | undefined;
  readonly details: { [key: string]: Json };

  constructor(status: number, code: string, message: string, retryable: boolean, requestId?: string, details: { [key: string]: Json } = {}) {
    super(message);
    this.status = status;
    this.code = code;
    this.retryable = retryable;
    this.requestId = requestId;
    this.details = details;
  }
}

export interface Participant {
  participant_id: string;
  ember_id: string;
  slot: 0 | 1;
}

/**
 * Who decides a match's games. `organizer-reported-v1`: your organizer
 * credential records each game. `ember-room-v1`: the players play in an Ember
 * room and both players' Ember reports each game; agreeing reports score it.
 */
export type RulesProfile = "organizer-reported-v1" | "ember-room-v1";

/** A set of first to N games: 1 to 10, so best of 1 to best of 19. */
export type GamesToWin = 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10;

export interface MatchSpec {
  external_match_id: string;
  participants: [Participant, Participant];
  games_to_win: GamesToWin;
  required_build_id: string;
  /** Defaults to `organizer-reported-v1`. */
  native_rules_profile?: RulesProfile;
  metadata?: { [key: string]: string };
}

/** Who gives up the seat when a lobby set ends. Winner stays is king of the hill. */
export type Rotation = "winner_stays" | "loser_stays" | "both_rotate";

export interface LobbySpec {
  external_lobby_id: string;
  games_to_win: GamesToWin;
  rotation: Rotation;
  required_build_id: string;
  metadata?: { [key: string]: string };
}

export interface LobbyPlayer {
  participant_id: string;
  ember_id: string;
  slot?: 0 | 1;
}

export interface Lobby {
  lobby_id: string;
  external_lobby_id: string;
  state: "open" | "closed";
  revision: string;
  games_to_win: number;
  rotation: Rotation;
  current_match_id: string | null;
  sets_started: number;
  sets_completed: number;
  seated: LobbyPlayer[];
  queue: LobbyPlayer[];
  streak: { ember_id: string; sets: number } | null;
  /** Everyone who has finished a set: sets won, then fewest lost, then game difference, then best streak. */
  standings: LobbyStanding[];
  metadata: { [key: string]: string };
  event_cursor: string;
}

export interface LobbyStanding {
  ember_id: string;
  participant_id: string;
  sets_won: number;
  sets_lost: number;
  games_won: number;
  games_lost: number;
  best_streak: number;
}

/** A player's finished matches as one viewer may see them. */
export interface PlayerRecord {
  ember_id: string;
  sets: { played: number; won: number; lost: number };
  games: { won: number; lost: number; drawn: number };
  cancelled: number;
  recent: RecentMatch[];
  event_cursor: string;
}

export interface RecentMatch {
  match_id: string;
  result: "won" | "lost" | "cancelled";
  wins: number;
  opponent_wins: number;
  games_to_win: number;
  /** `participant_id` is present for provider and organizer credentials only. */
  opponent: { ember_id: string; participant_id?: string } | null;
  lobby_id: string | null;
  tournament_id: string | null;
  round_label: string | null;
  finished_at: number;
}

/** Single elimination, double elimination (with an optional grand final reset) or round robin. */
export type TournamentFormat = "single_elimination" | "double_elimination" | "round_robin";

export interface TournamentSpec {
  external_tournament_id: string;
  format: TournamentFormat;
  games_to_win: GamesToWin;
  /** The final (single elimination) or grand final and reset (double elimination). Defaults to `games_to_win`. */
  finals_games_to_win?: GamesToWin;
  /** Double elimination: a second grand final when the losers-bracket player wins the first. Default true. */
  grand_final_reset?: boolean;
  required_build_id: string;
  metadata?: { [key: string]: string };
}

export interface TournamentEntrant {
  ember_id: string;
  participant_id: string;
  state: "registered" | "withdrawn";
  /** 1 is the top seed; set when the tournament starts. */
  seed: number | null;
  placement: number | null;
}

/** One set of the bracket. A `players` entry is null while undecided, or `{ bye: true }` for nobody. */
export interface TournamentSet {
  node: number;
  side: "winners" | "losers" | "grand_final" | "round_robin";
  round: number;
  label: string;
  status: "pending" | "ready" | "playing" | "played" | "bye" | "walkover" | "skipped";
  players: [TournamentSlot, TournamentSlot];
  winner_slot: 0 | 1 | null;
  match_id: string | null;
  games_to_win: number;
  wins?: [number, number];
}

export type TournamentSlot = { ember_id: string; participant_id: string } | { bye: true } | null;

export interface Tournament {
  tournament_id: string;
  external_tournament_id: string;
  state: "registration" | "running" | "completed" | "cancelled";
  revision: string;
  format: TournamentFormat;
  games_to_win: number;
  finals_games_to_win: number;
  grand_final_reset: boolean;
  required_build_id: string;
  metadata: { [key: string]: string };
  entrants: TournamentEntrant[];
  sets: TournamentSet[];
  /** Round robin only: sets won, then game difference, then games won, then head-to-head, then seed. */
  standings: { ember_id: string; participant_id: string; sets_won: number; sets_lost: number; games_won: number; games_lost: number }[];
  event_cursor: string;
}

export interface ResolvedPlayer {
  subject: string;
  linked: boolean;
  participant_id?: string;
  ember_id?: string;
}

/** A player found by Discord account and linked on your connection. */
export interface FoundPlayer {
  discord_user_id: string;
  ember_id: string;
  participant_id: string;
}

/**
 * A sign-in site the bridge offers (`sign_in_sites` in its capabilities):
 * players connect their accounts there, and connections whose
 * `lookup_sites` name it find players by those accounts.
 */
export interface SignInSite {
  /** The site ID lookups and routes name, such as `challonge` or `startgg`. */
  id: string;
  display_name: string;
  /** Whether players can connect accounts now; when not, connected ones still answer lookups. */
  sign_in: boolean;
  /** What a lookup's `id` is on this site: a username or a user ID. */
  lookup_by: "username" | "user_id";
  /** Where the site's sign-in page is, which Ember checks before opening it. */
  authorize_origin: string;
  authorize_path: string;
}

/** An account on a sign-in site, as a lookup names it. */
export interface SiteAccount {
  site: string;
  /** A username or a user ID, as the site's `lookup_by` says. */
  id: string;
}

/** A player found by an account on a sign-in site and linked on your connection. */
export interface FoundAccount {
  site: string;
  /** The account as you asked for it. */
  id: string;
  /** The account's user ID on the site. */
  user_id: string;
  ember_id: string;
  participant_id: string;
  /**
   * When the account connected (Unix seconds), the last time the bridge saw
   * its username. A site may let accounts rename without telling the
   * bridge, so you may refuse a username connected too long ago.
   */
  connected_at: number;
}

/** A public room to open for a player linked on your connection. */
export interface RoomSpec {
  /** 1 to 64 characters on one line. */
  name: string;
  /** Seats in the room. */
  capacity: number;
  buildId: string;
  /** Goes in first and moderates the room; must be linked on your connection. */
  creator: { participantId: string; emberId: string };
}

/** Who may watch the matches of a connection, as its organizer set it. */
export interface WatchPolicy {
  connection_id: string;
  /** Whether anyone who holds a match link may watch it, up to `link_holder_cap`. */
  link_holders: boolean;
  link_holder_cap: number;
  /** The most watchers one match has, staff included. */
  ceiling: number;
  /** Ember IDs approved to watch every match of the connection. */
  staff: { ember_id: string; added_at: number }[];
}

/** Someone who asked to watch a match. */
export interface MatchWatcher {
  ember_id: string;
  /** What lets them in now: `staff`, `link_holders`, `denied` or `none`. */
  via: "staff" | "link_holders" | "denied" | "none";
  /** Whether the fighters' binding names them, so the room admits them. */
  bound: boolean;
  first_seen_at: number;
  last_seen_at: number;
}

export interface MatchWatchers {
  match_id: string;
  link_holders: boolean;
  watchers: MatchWatcher[];
  /** Ember IDs denied this match, whatever the policy says. */
  denied: string[];
}

type Body = { [key: string]: Json };

const REQUEST_TIMEOUT_MS = 15_000;
const CREATE_ROOM_TIMEOUT_MS = 45_000;

export class BridgeClient {
  readonly origin: string;
  readonly #credential: string;
  readonly #timeoutMs: number;
  readonly #createRoomTimeoutMs: number;
  readonly #fetch: typeof fetch;

  constructor(options: BridgeClientOptions) {
    if (!isBridgeOrigin(options.origin)) {
      throw new TypeError("origin must be a bare https origin, or http on loopback for local testing");
    }
    this.origin = options.origin;
    this.#credential = options.credential;
    this.#timeoutMs = options.timeoutMs ?? REQUEST_TIMEOUT_MS;
    this.#createRoomTimeoutMs = options.timeoutMs ?? CREATE_ROOM_TIMEOUT_MS;
    this.#fetch = options.fetch ?? fetch;
  }

  async #request(
    method: string,
    path: string,
    body?: Body,
    headers: Record<string, string> = {},
    timeoutMs: number = this.#timeoutMs,
  ): Promise<{ status: number; text: string }> {
    const response = await this.#fetch(`${this.origin}${path}`, {
      method,
      redirect: "error",
      signal: AbortSignal.timeout(timeoutMs),
      headers: {
        authorization: `Bearer ${this.#credential}`,
        ...(body ? { "content-type": "application/json" } : {}),
        ...headers,
      },
      ...(body ? { body: JSON.stringify(body) } : {}),
    });
    const text = await response.text();
    if (!response.ok) {
      let error: { code?: string; message?: string; retryable?: boolean; request_id?: string; details?: Body } = {};
      try {
        error = (parseStrict(text) as { error?: typeof error }).error ?? {};
      } catch {
        // Not the bridge's error shape.
      }
      throw new BridgeError(
        response.status,
        error.code ?? `http_${response.status}`,
        error.message ?? response.statusText,
        error.retryable ?? response.status >= 500,
        error.request_id,
        error.details ?? {},
      );
    }
    return { status: response.status, text };
  }

  async #json<T>(method: string, path: string, body?: Body, headers?: Record<string, string>, timeoutMs?: number): Promise<T> {
    const { text } = await this.#request(method, path, body, headers, timeoutMs);
    return parseStrict(text) as T;
  }

  getCapabilities(): Promise<Body> {
    return this.#json("GET", "/v1/capabilities");
  }

  /** The sign-in sites the bridge offers, from its capabilities. */
  async getSignInSites(): Promise<SignInSite[]> {
    const capabilities = await this.#json<{ sign_in_sites?: SignInSite[] }>("GET", "/v1/capabilities");
    return capabilities.sign_in_sites ?? [];
  }

  /** This connection's own subjects only; unknown subjects are unlinked. */
  async resolvePlayers(subjects: string[]): Promise<ResolvedPlayer[]> {
    const result = await this.#json<{ players: ResolvedPlayer[] }>("POST", "/v1/players/resolve", { subjects });
    return result.players;
  }

  /**
   * Finds players by Discord user ID (at most 32), for a connection that may
   * (`discord_lookup`). Each one found is linked on your connection, in the
   * order asked; an ID with no connected account is left out.
   */
  async lookupPlayers(discordIds: string[]): Promise<FoundPlayer[]> {
    const result = await this.#json<{ players: FoundPlayer[] }>("POST", "/v1/players/lookup", { discord: discordIds });
    return result.players;
  }

  /**
   * Finds players by their accounts on sign-in sites, as a tournament entry
   * shows them, for a connection whose `lookup_sites` name each site asked.
   * Each `id` is matched as the site's `lookup_by` and username rule say
   * (Challonge usernames without case, start.gg user IDs exactly); one the
   * site's accounts cannot have finds no one. Linked and answered like
   * `lookupPlayers`, once per site and player; at most 32 accounts.
   */
  async lookupAccounts(accounts: SiteAccount[]): Promise<FoundAccount[]> {
    const asked = accounts.map(({ site, id }) => ({ site, id }));
    const result = await this.#json<{ accounts?: FoundAccount[] }>("POST", "/v1/players/lookup", { accounts: asked });
    return result.accounts ?? [];
  }

  /**
   * Sets and games won and lost, and the most recent finished matches. A
   * provider sees its connection's matches, an organizer its tenant's, and a
   * player only their own record.
   */
  getPlayerRecord(emberId: string, recent?: number): Promise<PlayerRecord> {
    const query = recent === undefined ? "" : `?limit=${recent}`;
    return this.#json("GET", `/v1/players/${encodeURIComponent(emberId)}/record${query}`);
  }

  /** Starts linking for a subject your service has verified. The code is returned once. */
  createLinkIntent(subject: string, displayLabel?: string): Promise<{ intent_id: string; code: string; expires_at: number; connection_id: string }> {
    return this.#json("POST", "/v1/link-intents", { subject, ...(displayLabel ? { display_label: displayLabel } : {}) });
  }

  getLinkIntent(intentId: string): Promise<Body> {
    return this.#json("GET", `/v1/link-intents/${encodeURIComponent(intentId)}`);
  }

  /**
   * Approves the exact pending claim after the verified account holder
   * confirmed the fingerprint shown in Ember.
   */
  approveLinkClaim(intentId: string, approval: { claimId: string; emberId: string; subject: string; replace?: boolean }): Promise<Body> {
    return this.#json("POST", `/v1/link-intents/${encodeURIComponent(intentId)}/approve`, {
      claim_id: approval.claimId,
      ember_id: approval.emberId,
      subject: approval.subject,
      replace: approval.replace ?? false,
    });
  }

  rejectLinkClaim(intentId: string, claimId: string): Promise<Body> {
    return this.#json("POST", `/v1/link-intents/${encodeURIComponent(intentId)}/reject`, { claim_id: claimId });
  }

  /**
   * Creates a logical match. Reuse the same `idempotencyKey` when retrying,
   * so a lost response never schedules a second match.
   */
  /**
   * `play_url` is the match's Play link for both players: a page that opens
   * the match in Ember, or offers to install Ember first. It is safe to show
   * in messages, since only the two assigned Ember IDs can claim the match.
   */
  createMatch(spec: MatchSpec, idempotencyKey: string = randomUUID()): Promise<{ match_id: string; state: string; revision: string; match_url: string; play_url: string }> {
    return this.#json(
      "POST",
      "/v1/matches",
      {
        external_match_id: spec.external_match_id,
        game: "usf4",
        participants: spec.participants as unknown as Json[],
        rules: {
          games_to_win: spec.games_to_win,
          draw_policy: "replay_no_score",
          native_rules_profile: spec.native_rules_profile ?? "organizer-reported-v1",
          edition_policy: "ultra_only",
          character_policy: "unrestricted_between_games",
          stage_policy: "p1_selects",
          input_delay_policy: "ember_existing_ready_policy",
        },
        observer_policy: "authorized_only",
        result_policy: "two_player_agreement_or_review",
        required_build_id: spec.required_build_id,
        metadata: spec.metadata ?? {},
      },
      { "idempotency-key": idempotencyKey },
    );
  }

  getMatch(matchId: string): Promise<Body> {
    return this.#json("GET", `/v1/matches/${encodeURIComponent(matchId)}`);
  }

  /** `expectedRevision` is the match's `revision`, a decimal string. */
  cancelMatch(matchId: string, expectedRevision: string, reason: string, idempotencyKey: string = randomUUID()): Promise<Body> {
    return this.#json(
      "POST",
      `/v1/matches/${encodeURIComponent(matchId)}/cancel`,
      { reason, expected_revision: expectedRevision },
      { "idempotency-key": idempotencyKey },
    );
  }

  /** Organizer credentials only. */
  recordGame(
    matchId: string,
    result: { winnerSlot: 0 | 1 } | { draw: true },
    expectedRevision: string,
    reason: string,
    idempotencyKey: string = randomUUID(),
  ): Promise<Body> {
    return this.#json(
      "POST",
      `/v1/matches/${encodeURIComponent(matchId)}/adjudications`,
      {
        kind: "game_result",
        ...("draw" in result ? { draw: true } : { winner_slot: result.winnerSlot }),
        reason,
        expected_revision: expectedRevision,
      },
      { "idempotency-key": idempotencyKey },
    );
  }

  /** Organizer credentials only. On a completed match this is a correction. */
  voidGame(matchId: string, attemptId: string, expectedRevision: string, reason: string, idempotencyKey: string = randomUUID()): Promise<Body> {
    return this.#json(
      "POST",
      `/v1/matches/${encodeURIComponent(matchId)}/adjudications`,
      { kind: "void_game", attempt_id: attemptId, reason, expected_revision: expectedRevision },
      { "idempotency-key": idempotencyKey },
    );
  }

  /**
   * Opens a lobby: a queue that plays first-to-N sets one after another and
   * rotates its seats when a set ends. Each set is an ordinary match.
   */
  createLobby(spec: LobbySpec, idempotencyKey: string = randomUUID()): Promise<Lobby> {
    return this.#json(
      "POST",
      "/v1/lobbies",
      {
        external_lobby_id: spec.external_lobby_id,
        game: "usf4",
        games_to_win: spec.games_to_win,
        rotation: spec.rotation,
        required_build_id: spec.required_build_id,
        metadata: spec.metadata ?? {},
      },
      { "idempotency-key": idempotencyKey },
    );
  }

  getLobby(lobbyId: string): Promise<Lobby> {
    return this.#json("GET", `/v1/lobbies/${encodeURIComponent(lobbyId)}`);
  }

  /** Queues a linked player who asked to play. Nobody is queued without asking. */
  joinLobby(lobbyId: string, player: { participantId: string; emberId: string }, idempotencyKey: string = randomUUID()): Promise<Lobby> {
    return this.#json(
      "POST",
      `/v1/lobbies/${encodeURIComponent(lobbyId)}/queue`,
      { participant_id: player.participantId, ember_id: player.emberId },
      { "idempotency-key": idempotencyKey },
    );
  }

  /** Takes a player out of the queue or their seat. Leaving a seat cancels the running set. */
  leaveLobby(lobbyId: string, participantId: string, idempotencyKey: string = randomUUID()): Promise<Lobby> {
    return this.#json(
      "POST",
      `/v1/lobbies/${encodeURIComponent(lobbyId)}/queue/${encodeURIComponent(participantId)}/leave`,
      {},
      { "idempotency-key": idempotencyKey },
    );
  }

  /** `expectedRevision` is the lobby's `revision`. A running set is cancelled. */
  closeLobby(lobbyId: string, expectedRevision: string, reason: string, idempotencyKey: string = randomUUID()): Promise<Lobby> {
    return this.#json(
      "POST",
      `/v1/lobbies/${encodeURIComponent(lobbyId)}/close`,
      { reason, expected_revision: expectedRevision },
      { "idempotency-key": idempotencyKey },
    );
  }

  /** Opens registration for a tournament on your connection. */
  createTournament(spec: TournamentSpec, idempotencyKey: string = randomUUID()): Promise<Tournament> {
    return this.#json("POST", "/v1/tournaments", { game: "usf4", metadata: {}, ...spec }, { "idempotency-key": idempotencyKey });
  }

  getTournament(tournamentId: string): Promise<Tournament> {
    return this.#json("GET", `/v1/tournaments/${encodeURIComponent(tournamentId)}`);
  }

  /** Registers a linked player who asked to enter, before the start. */
  registerEntrant(tournamentId: string, player: { participantId: string; emberId: string }, idempotencyKey: string = randomUUID()): Promise<Tournament> {
    return this.#json(
      "POST",
      `/v1/tournaments/${encodeURIComponent(tournamentId)}/entrants`,
      { participant_id: player.participantId, ember_id: player.emberId },
      { "idempotency-key": idempotencyKey },
    );
  }

  /** Before the start this removes the player; once running, every set they have left goes to their opponent. */
  withdrawEntrant(tournamentId: string, participantId: string, idempotencyKey: string = randomUUID()): Promise<Tournament> {
    return this.#json(
      "POST",
      `/v1/tournaments/${encodeURIComponent(tournamentId)}/entrants/${encodeURIComponent(participantId)}/withdraw`,
      {},
      { "idempotency-key": idempotencyKey },
    );
  }

  /** Seeds the entrants (`seeding` lists every registered participant_id, top seed first; registration order otherwise) and starts the first sets. */
  startTournament(tournamentId: string, expectedRevision: string, seeding?: string[], idempotencyKey: string = randomUUID()): Promise<Tournament> {
    return this.#json(
      "POST",
      `/v1/tournaments/${encodeURIComponent(tournamentId)}/start`,
      seeding ? { expected_revision: expectedRevision, seeding } : { expected_revision: expectedRevision },
      { "idempotency-key": idempotencyKey },
    );
  }

  cancelTournament(tournamentId: string, expectedRevision: string, reason: string, idempotencyKey: string = randomUUID()): Promise<Tournament> {
    return this.#json(
      "POST",
      `/v1/tournaments/${encodeURIComponent(tournamentId)}/cancel`,
      { reason, expected_revision: expectedRevision },
      { "idempotency-key": idempotencyKey },
    );
  }

  /**
   * Opens a public room for a linked player, on a connection with `rooms`.
   * Refusals carry `error.details.reason`: `not_linked`, `room_limit`,
   * `unsupported_build` or `invalid_name`. When the creator already has a room
   * this connection opened, `room_limit` has its ID in `error.details.room_id`.
   * Starting the room's host can take up to 30 seconds, so this request waits
   * 45 unless the client's `timeoutMs` says otherwise.
   */
  createRoom(spec: RoomSpec): Promise<ConnectionRoom> {
    return this.#json(
      "POST",
      "/v1/rooms",
      {
        name: spec.name,
        capacity: spec.capacity,
        build_id: spec.buildId,
        creator: { participant_id: spec.creator.participantId, ember_id: spec.creator.emberId },
      },
      undefined,
      this.#createRoomTimeoutMs,
    );
  }

  /** The rooms your connection opened that are not closed, newest first. */
  async listRooms(buildId?: string): Promise<ConnectionRoom[]> {
    const query = buildId === undefined ? "" : `?build_id=${encodeURIComponent(buildId)}`;
    const result = await this.#json<{ rooms: ConnectionRoom[] }>("GET", `/v1/rooms${query}`);
    return result.rooms;
  }

  /** One of your connection's rooms, closed ones included for a day. */
  getRoom(roomId: string): Promise<ConnectionRoom> {
    return this.#json("GET", `/v1/rooms/${encodeURIComponent(roomId)}`);
  }

  /** Closes a room your connection opened (`reason` is 1 to 256 characters). Closing a closed room answers it again. */
  closeRoom(roomId: string, reason: string): Promise<ConnectionRoom> {
    return this.#json("POST", `/v1/rooms/${encodeURIComponent(roomId)}/close`, { reason });
  }

  /** Who may watch the matches of your connection. Provider and organizer credentials read it. */
  watchPolicy(connectionId: string): Promise<WatchPolicy> {
    return this.#json("GET", `/v1/connections/${encodeURIComponent(connectionId)}/watch`);
  }

  /**
   * Organizer credentials only. Lets anyone who holds a match link watch it,
   * up to `link_holder_cap` of them, or stops that. It is off by default.
   *
   * A watcher joins the fighters' room, and the room's members can learn each
   * other's public IP addresses when Iroh finds a direct path. With this on,
   * anyone with a match link and a free Ember ID can learn a competing
   * fighter's address in the middle of your event. Staff, whom you approve by
   * name, have the same reach; turn this on only if you accept that. A
   * connection whose platform fixes its settings (BluMint's) refuses it with
   * `error.details.reason` `connection_fixed`.
   */
  setWatchLinkHolders(connectionId: string, linkHolders: boolean): Promise<WatchPolicy> {
    return this.#json("PUT", `/v1/connections/${encodeURIComponent(connectionId)}/watch`, { link_holders: linkHolders });
  }

  /** Organizer credentials only. Approves an Ember ID to watch every match of the connection, such as a caster or a TO. */
  addWatchStaff(connectionId: string, emberId: string): Promise<{ ember_id: string; added_at: number }> {
    return this.#json("PUT", `/v1/connections/${encodeURIComponent(connectionId)}/watch/staff/${encodeURIComponent(emberId)}`, {});
  }

  /** Organizer credentials only. Takes the approval back; the watcher leaves a room within about a claim interval (10 seconds). */
  async removeWatchStaff(connectionId: string, emberId: string): Promise<void> {
    await this.#request("DELETE", `/v1/connections/${encodeURIComponent(connectionId)}/watch/staff/${encodeURIComponent(emberId)}`);
  }

  /** Organizer credentials only. Removes an Ember ID from one match's watchers, staff or not, and keeps it out. */
  denyWatcher(matchId: string, emberId: string): Promise<{ ember_id: string; set_at: number }> {
    return this.#json("PUT", `/v1/matches/${encodeURIComponent(matchId)}/watch/denials/${encodeURIComponent(emberId)}`);
  }

  /** Organizer credentials only. Lifts a denial. */
  async clearWatchDenial(matchId: string, emberId: string): Promise<void> {
    await this.#request("DELETE", `/v1/matches/${encodeURIComponent(matchId)}/watch/denials/${encodeURIComponent(emberId)}`);
  }

  /** Everyone who asked to watch a match, whether they are let in and whether the room admits them, and the denials. */
  matchWatchers(matchId: string): Promise<MatchWatchers> {
    return this.#json("GET", `/v1/matches/${encodeURIComponent(matchId)}/watchers`);
  }

  /** Events after `after`, oldest first, with the cursor to continue from. */
  async listEvents(after = "0", limit = 100): Promise<{ events: BridgeEvent[]; nextCursor: string }> {
    const page = await this.#json<{ events: Json[]; next_cursor: string }>("GET", `/v1/events?after=${encodeURIComponent(after)}&limit=${limit}`);
    return { events: page.events.map((event) => parseEvent(event)), nextCursor: page.next_cursor };
  }

  /** The secret is in this response only. Store it before anything else. */
  createWebhookSubscription(url: string, eventTypes: string[], idempotencyKey: string = randomUUID()): Promise<{ subscription_id: string; secret: string | null }> {
    return this.#json("POST", "/v1/webhook-subscriptions", { url, event_types: eventTypes }, { "idempotency-key": idempotencyKey });
  }

  rotateWebhookSecret(subscriptionId: string, overlapSeconds?: number): Promise<{ secret: string; previous_valid_until: number }> {
    return this.#json(
      "POST",
      `/v1/webhook-subscriptions/${encodeURIComponent(subscriptionId)}/rotate-secret`,
      overlapSeconds === undefined ? {} : { overlap_seconds: overlapSeconds },
    );
  }

  async deleteWebhookSubscription(subscriptionId: string): Promise<void> {
    await this.#request("DELETE", `/v1/webhook-subscriptions/${encodeURIComponent(subscriptionId)}`);
  }
}
