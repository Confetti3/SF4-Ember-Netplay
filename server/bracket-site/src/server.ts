// The HTTP routes: an organizer's API, a public view, and the bracket
// connector's contract (server/ember/brackets/ADAPTERS.md, "The http adapter")
// under /tournaments/<id>/ember.
import { createHash, randomInt, timingSafeEqual } from "node:crypto";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";

import type { BridgeClient } from "../../../sdk/typescript/src/index.ts";
import { BridgeError } from "../../../sdk/typescript/src/index.ts";
import { Conflict, Invalid, finished, generate, report, roundLabel, status, type BracketSet, type Tournament } from "./bracket.ts";
import { page } from "./page.ts";
import type { Store } from "./store.ts";

export const CONTRACT = "1";
const MAX_BODY = 64 * 1024;
const SLUG = /^[A-Za-z0-9_-]{1,64}$/;
const DISCORD_ID = /^\d{17,20}$/;

export interface Options {
  store: Store;
  /** For the organizer's routes. */
  organizerToken: string;
  /** For the connector's routes. */
  connectorToken: string;
  /** A bridge connection with `discord_lookup`, which finds each player's Ember ID. */
  bridge: Pick<BridgeClient, "lookupPlayers">;
}

class Failure extends Error {
  readonly status: number;
  readonly body: object;

  constructor(status: number, message: string, body: object = { error: message }) {
    super(message);
    this.status = status;
    this.body = body;
  }
}

type Json = Record<string, unknown>;
type Handler = (request: IncomingMessage, params: string[], url: URL) => Promise<[number, object?]> | [number, object?];

export function bracketSite(options: Options): Server {
  const { store } = options;
  const organizer = (request: IncomingMessage) => authorize(request, options.organizerToken);
  const connector = (request: IncomingMessage) => {
    authorize(request, options.connectorToken);
    const version = request.headers["ember-brackets-contract"];
    if (version !== undefined && version !== CONTRACT) throw new Failure(426, "contract version", { contract: CONTRACT });
  };
  const found = (id: string) => store.get(id) ?? fail(404, `no tournament ${id}`);

  const routes: [string, RegExp, Handler][] = [
    [
      "POST",
      /^\/tournaments$/,
      async (request) => {
        organizer(request);
        const body = await readJson(request);
        const id = text(body.id, SLUG, "id is 1 to 64 letters, digits, _ or -");
        const title = text(body.title, /^.{1,100}$/u, "title is 1 to 100 characters");
        const gamesToWin = body.games_to_win ?? 2;
        if (!Number.isInteger(gamesToWin) || (gamesToWin as number) < 1 || (gamesToWin as number) > 10) fail(400, "games_to_win is 1 to 10");
        const tournament: Tournament = { id, title, games_to_win: gamesToWin as number, state: "open", entrants: [], rounds: 0, sets: [] };
        if (!store.add(tournament)) fail(409, `tournament ${id} exists`);
        return [201, view(tournament)];
      },
    ],
    ["GET", /^\/tournaments\/([^/]+)$/, (_, [id]) => [200, view(found(id!))]],
    [
      "DELETE",
      /^\/tournaments\/([^/]+)$/,
      (request, [id]) => {
        organizer(request);
        return store.delete(id!) ? [204] : fail(404, `no tournament ${id}`);
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/entrants$/,
      async (request, [id]) => {
        organizer(request);
        const body = await readJson(request);
        const discord = text(body.discord, DISCORD_ID, "discord is a Discord user ID");
        const name = body.name === undefined ? discord : text(body.name, /^.{1,64}$/u, "name is 1 to 64 characters");
        open(found(id!));
        let players;
        try {
          players = await options.bridge.lookupPlayers([discord]);
        } catch (error) {
          if (error instanceof BridgeError) fail(502, `the bridge answered ${error.status} (${error.code})`);
          throw error;
        }
        const player = players[0] ?? fail(422, "that Discord account is not connected to an Ember ID; connect it in Ember first");
        // Read again: the tournament may have changed while the bridge answered.
        const tournament = open(found(id!));
        if (tournament.entrants.some((entrant) => entrant.id === player.ember_id || entrant.discord === discord)) fail(409, "already entered");
        const entrant = { id: player.ember_id, discord, name };
        tournament.entrants.push(entrant);
        store.save(tournament);
        return [201, entrant];
      },
    ],
    [
      "DELETE",
      /^\/tournaments\/([^/]+)\/entrants\/([^/]+)$/,
      (request, [id, entrant]) => {
        organizer(request);
        const tournament = open(found(id!));
        const before = tournament.entrants.length;
        tournament.entrants = tournament.entrants.filter((each) => each.id !== entrant);
        if (tournament.entrants.length === before) fail(404, `no entrant ${entrant}`);
        store.save(tournament);
        return [204];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/start$/,
      async (request, [id]) => {
        organizer(request);
        const body = await readJson(request);
        const seeding = body.seeding ?? "join";
        if (seeding !== "join" && seeding !== "random") fail(400, 'seeding is "join" or "random"');
        const tournament = open(found(id!));
        if (tournament.entrants.length < 2) fail(409, "a bracket needs at least two entrants");
        if (seeding === "random") shuffle(tournament.entrants);
        Object.assign(tournament, generate(tournament.entrants.map((entrant) => entrant.id)), { state: "started" });
        store.save(tournament);
        return [200, view(tournament)];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/sets\/([^/]+)\/result$/,
      async (request, [id, setId]) => {
        organizer(request);
        const body = await readJson(request);
        const scores = body.scores as Record<string, number>;
        if (typeof body.winner !== "string" || typeof scores !== "object" || scores === null) fail(400, "a result is { winner, scores: { <entrant>: <games> } }");
        return record(found(id!), setId!, { winner: body.winner as string, scores, key: null });
      },
    ],
    // The connector's contract.
    [
      "GET",
      /^\/tournaments\/([^/]+)\/ember\/sets$/,
      (request, [id], url) => {
        connector(request);
        const state = url.searchParams.get("state");
        const tournament = store.get(id!);
        const sets = (tournament?.sets ?? []).filter((set) => !state || status(set) === state);
        return [200, { sets: sets.map((set) => contractSet(tournament!, set)) }];
      },
    ],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/ember\/sets\/([^/]+)$/,
      (request, [id, setId]) => {
        connector(request);
        // A tournament deleted, or a set it never had, is gone: its match is cancelled.
        const tournament = store.get(id!);
        const set = tournament?.sets.find((each) => each.id === setId);
        return [200, { set: set ? contractSet(tournament!, set) : null }];
      },
    ],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/ember\/standing$/,
      (request, [id]) => {
        connector(request);
        const tournament = store.get(id!);
        return [200, { standing: !tournament || finished(tournament) ? "finished" : "running" }];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/ember\/sets\/([^/]+)\/underway$/,
      async (request, [id, setId]) => {
        connector(request);
        await readJson(request);
        const tournament = found(id!);
        const set = tournament.sets.find((each) => each.id === setId) ?? fail(404, `no set ${setId}`);
        set.underway = true;
        store.save(tournament);
        return [204];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/ember\/sets\/([^/]+)\/report$/,
      async (request, [id, setId]) => {
        connector(request);
        const key = request.headers["idempotency-key"];
        if (typeof key !== "string" || !key) fail(400, "Idempotency-Key is required");
        const body = await readJson(request);
        const scores = Array.isArray(body.scores) ? (body.scores as { entry?: unknown; games?: unknown }[]) : [];
        if (typeof body.winner !== "string" || scores.length !== 2) fail(400, "not a report");
        const byEntry = Object.fromEntries(scores.map((score) => [String(score.entry), score.games as number]));
        return record(found(id!), setId!, { winner: body.winner as string, scores: byEntry, key: key as string });
      },
    ],
  ];

  function record(tournament: Tournament, setId: string, result: Parameters<typeof report>[2]): [number, object] {
    if (tournament.state !== "started") fail(409, "the tournament has not started");
    try {
      report(tournament, setId, result);
    } catch (error) {
      if (error instanceof Conflict) fail(409, error.message);
      if (error instanceof Invalid) fail(422, error.message);
      throw error;
    }
    store.save(tournament);
    return [200, view(tournament)];
  }

  return createServer(async (request, response) => {
    const url = new URL(request.url ?? "/", "http://localhost");
    try {
      const html = /^\/tournaments\/([^/]+)\/bracket$/.exec(url.pathname);
      if (html && request.method === "GET") {
        response.writeHead(200, { "content-type": "text/html; charset=utf-8" }).end(page(found(decodeURIComponent(html[1]!))));
        return;
      }
      for (const [method, pattern, handler] of routes) {
        const match = pattern.exec(url.pathname);
        if (!match || request.method !== method) continue;
        const [code, body] = await handler(request, match.slice(1).map(decodeURIComponent), url);
        send(response, code, body);
        return;
      }
      fail(404, "not found");
    } catch (error) {
      if (error instanceof Failure) send(response, error.status, error.body);
      else {
        console.error(error);
        send(response, 500, { error: "internal error" });
      }
    }
  });
}

function send(response: ServerResponse, status: number, body?: object): void {
  if (body === undefined) response.writeHead(status).end();
  else response.writeHead(status, { "content-type": "application/json" }).end(JSON.stringify(body));
}

function fail(status: number, message: string): never {
  throw new Failure(status, message);
}

function authorize(request: IncomingMessage, token: string): void {
  const given = /^Bearer (.+)$/.exec(request.headers.authorization ?? "")?.[1] ?? "";
  const digest = (value: string) => createHash("sha256").update(value).digest();
  if (!timingSafeEqual(digest(given), digest(token))) fail(401, "wrong or missing token");
}

function open(tournament: Tournament): Tournament {
  if (tournament.state !== "open") fail(409, "the tournament has started");
  return tournament;
}

function text(value: unknown, pattern: RegExp, message: string): string {
  if (typeof value !== "string" || !pattern.test(value)) fail(400, message);
  return value;
}

async function readJson(request: IncomingMessage): Promise<Json> {
  let size = 0;
  const chunks: Buffer[] = [];
  for await (const chunk of request as AsyncIterable<Buffer>) {
    size += chunk.length;
    if (size > MAX_BODY) fail(413, "body too large");
    chunks.push(chunk);
  }
  const raw = Buffer.concat(chunks).toString("utf8");
  if (!raw) return {};
  try {
    const body = JSON.parse(raw) as unknown;
    if (typeof body === "object" && body !== null && !Array.isArray(body)) return body as Json;
  } catch {
    // Answered below.
  }
  return fail(400, "the body is not a JSON object");
}

function shuffle<T>(items: T[]): void {
  for (let i = items.length - 1; i > 0; i--) {
    const j = randomInt(i + 1);
    [items[i], items[j]] = [items[j]!, items[i]!];
  }
}

/** A set as the connector reads it: each entry is found by its Discord account. */
function contractSet(tournament: Tournament, set: BracketSet): Json {
  const [one, two] = set.entries;
  const entry = (id: string) => ({ id, accounts: [{ discord: tournament.entrants.find((each) => each.id === id)!.discord }] });
  return {
    id: set.id,
    round: set.round,
    round_label: roundLabel(set.round, tournament.rounds),
    status: status(set),
    entries: one && two ? [entry(one), entry(two)] : null,
    underway: set.underway,
    winner: set.winner,
  };
}

/** The tournament as anyone may see it. */
function view(tournament: Tournament): Json {
  return {
    ...tournament,
    finished: finished(tournament),
    sets: tournament.sets.map(({ key: _, ...set }) => ({ ...set, status: status(set), round_label: roundLabel(set.round, tournament.rounds) })),
  };
}
