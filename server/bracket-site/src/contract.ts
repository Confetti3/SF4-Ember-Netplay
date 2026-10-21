// The bracket connector's contract (server/ember/brackets/ADAPTERS.md, "The
// http adapter") under /tournaments/<id>/ember.
import type { IncomingMessage } from "node:http";

import { Conflict, Invalid, report, roundLabel, status, type BracketSet, type Result } from "./bracket.ts";
import { fail, Failure, readJson, type Json, type Route } from "./http.ts";
import type { Store } from "./store.ts";
import { beforeLive, settle, type Tournament } from "./tournament.ts";

export const CONTRACT = "1";

/**
 * The connector's routes. `authorize` refuses a caller that may not play
 * the tournament (which may be gone).
 */
export function contractRoutes(store: Store, authorize: (request: IncomingMessage, tournament: Tournament | undefined) => void): Route[] {
  /** The tournament as the connector sees it: a cancelled or deleted one is gone, with every set. */
  const played = (request: IncomingMessage, id: string) => {
    const tournament = store.get(id);
    authorize(request, tournament);
    const version = request.headers["ember-brackets-contract"];
    if (version !== undefined && version !== CONTRACT) throw new Failure(426, "contract version", { contract: CONTRACT });
    return tournament?.state === "cancelled" ? undefined : tournament;
  };
  return [
    [
      "GET",
      /^\/tournaments\/([^/]+)\/ember\/sets$/,
      (request, [id], url) => {
        const tournament = played(request, id!);
        const state = url.searchParams.get("state");
        const sets = (tournament?.sets ?? []).filter((set) => !state || status(set) === state);
        return [200, { sets: sets.map((set) => contractSet(tournament!, set)) }];
      },
    ],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/ember\/sets\/([^/]+)$/,
      (request, [id, setId]) => {
        const tournament = played(request, id!);
        const set = tournament?.sets.find((each) => each.id === setId);
        return [200, { set: set ? contractSet(tournament!, set) : null }];
      },
    ],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/ember\/standing$/,
      (request, [id]) => {
        const tournament = played(request, id!);
        const running = tournament && (tournament.state === "live" || beforeLive(tournament));
        return [200, { standing: running ? "running" : "finished" }];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/ember\/sets\/([^/]+)\/underway$/,
      async (request, [id, setId]) => {
        const tournament = played(request, id!) ?? fail(404, `no tournament ${id}`);
        await readJson(request);
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
        const tournament = played(request, id!) ?? fail(404, `no tournament ${id}`);
        const key = request.headers["idempotency-key"];
        if (typeof key !== "string" || !key) fail(400, "Idempotency-Key is required");
        const body = await readJson(request);
        const scores = Array.isArray(body.scores) ? (body.scores as { entry?: unknown; games?: unknown }[]) : [];
        if (typeof body.winner !== "string" || scores.length !== 2) fail(400, "not a report");
        const byEntry = Object.fromEntries(scores.map((score) => [String(score.entry), score.games as number]));
        record(store, tournament, setId!, { winner: body.winner, scores: byEntry, key });
        return [200, {}];
      },
    ],
  ];
}

/** Records a set's result, from the connector or an organizer, and finishes the tournament on its final. */
export function record(store: Store, tournament: Tournament, setId: string, result: Result): void {
  if (tournament.state !== "live" && tournament.state !== "finished") fail(409, `the tournament is ${tournament.state}`);
  try {
    if (report(tournament.sets, setId, result) === "repeat") return;
  } catch (error) {
    if (error instanceof Conflict) fail(409, error.message);
    if (error instanceof Invalid) fail(422, error.message);
    throw error;
  }
  settle(tournament, new Date().toISOString());
  store.save(tournament);
}

/** A set as the connector reads it: each entry is found by its Discord account. */
function contractSet(tournament: Tournament, set: BracketSet): Json {
  const [one, two] = set.entries;
  const entry = (id: string) => ({ id, accounts: [{ discord: tournament.entrants.find((each) => each.ember_id === id)!.discord }] });
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
