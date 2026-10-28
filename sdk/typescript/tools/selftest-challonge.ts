// What the Challonge mode of the tournament self-test
// (docs/design/TOURNAMENT_SELFTEST.md, "Challonge mode") needs of Challonge
// and of the local run configuration, apart from the bridge side's own
// processes: the configuration file, the bridge's sign-in site and the
// bracket connector's configuration, the organizer's reads of the set, and
// what a set has to look like before and after the run. The decisions are
// pure functions with a test (test/selftest-challonge.test.ts).
//
// Run as a script it reopens the set a finished run reported, so the next
// run can play it again (the connector leaves a reported set alone):
//
//   node tools/selftest-challonge.ts reopen <run configuration>
//
// Every request counts against the free tier's 500 a month, so this tool
// makes few: one read before the run, one or a few after it, and for a reopen
// a read, the change and a read to confirm it.
// Secrets are read from the files the configuration names when they are used
// and appear in no message.
import { readFileSync } from "node:fs";

import type { Json } from "../src/index.ts";
import { type Body, isBody } from "./body.ts";

/**
 * The local run configuration: a JSON file kept outside the repository (it
 * names the organizer's tournament and where the secrets are), with
 *
 *   clientId          the Challonge developer app's client ID
 *   clientSecretFile  a file holding its client secret
 *   apiKeyFile        a file holding the organizer's v1 API key
 *   tournament        the tournament's ID or URL slug
 *   match             the one open match the run plays
 *   redirectUri       the redirect URI registered for the app, which has to be
 *                     `<bridge origin>/v1/sites/challonge/callback`
 */
export interface ChallongeConfig {
  clientId: string;
  clientSecretFile: string;
  apiKeyFile: string;
  tournament: string;
  match: string;
  redirectUri: string;
}

/** The configuration `text` holds, or why it is not one. */
export function parseChallongeConfig(text: string): ChallongeConfig {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    throw new Error("the run configuration is not JSON");
  }
  if (typeof value !== "object" || value === null || Array.isArray(value)) throw new Error("the run configuration is not an object");
  const fields = value as Record<string, unknown>;
  const need = (name: keyof ChallongeConfig, form: RegExp, what: string): string => {
    const field = fields[name];
    if (typeof field !== "string" || !form.test(field)) throw new Error(`the run configuration's ${name} is ${what}`);
    return field;
  };
  const allowed = new Set<string>(["clientId", "clientSecretFile", "apiKeyFile", "tournament", "match", "redirectUri"]);
  const unknown = Object.keys(fields).find((name) => !allowed.has(name));
  if (unknown !== undefined) throw new Error(`the run configuration has an unknown field, ${unknown}`);
  return {
    clientId: need("clientId", /^[\x21-\x7e]{1,256}$/, "1 to 256 visible characters"),
    clientSecretFile: need("clientSecretFile", /^.{1,1024}$/, "a file path"),
    apiKeyFile: need("apiKeyFile", /^.{1,1024}$/, "a file path"),
    tournament: need("tournament", /^[A-Za-z0-9_-]{1,64}$/, "a Challonge ID or URL slug"),
    match: need("match", /^[0-9]{1,20}$/, "a match ID (digits)"),
    redirectUri: need("redirectUri", /^http:\/\/127\.0\.0\.1:[0-9]{4,5}\/v1\/sites\/challonge\/callback$/, "the loopback callback URI registered for the app"),
  };
}

/** The first line of a secret file, without its line end. Errors name the file, never its contents. */
export function readSecret(path: string): string {
  let text: string;
  try {
    text = readFileSync(path, "utf8");
  } catch {
    throw new Error(`cannot read ${path}`);
  }
  const secret = text.split(/\r?\n/)[0]!.trim();
  if (!secret) throw new Error(`${path} is empty`);
  return secret;
}

/**
 * The bridge's Challonge sign-in site: the definition the repository ships
 * (`server/ember/bridge/deploy/sites/challonge.json`) with the app's client ID.
 */
export function signInSite(template: Json, clientId: string): { id: string; [key: string]: Json } {
  if (!isBody(template) || template.id !== "challonge") throw new Error("the shipped site definition is not Challonge's");
  return { ...template, id: "challonge", client_id: clientId };
}

/**
 * The most requests the connector may spend in one run. Only this per-run cap
 * is enforced: the connector's database is new each run, so it does not know
 * how many requests earlier runs spent this month.
 */
export const CONNECTOR_BUDGET = 40;

/** What the connector's configuration refers to, all as absolute paths or plain values. */
export interface ConnectorSetup {
  bridgeOrigin: string;
  connectionId: string;
  providerTokenFile: string;
  buildId: string;
  resultsPort: number;
  resultSecretFile: string;
  database: string;
  tournament: string;
  apiKeyFile: string;
}

/** The connector's `brackets.json` for the run: one Challonge tournament, every set first to 1. */
export function connectorConfig(setup: ConnectorSetup): Body {
  return {
    bridge: {
      api: setup.bridgeOrigin,
      credential_file: setup.providerTokenFile,
      connection_id: setup.connectionId,
      build_id: setup.buildId,
    },
    results: { listen: `127.0.0.1:${setup.resultsPort}`, path: "/results", secret_file: setup.resultSecretFile },
    database: setup.database,
    monthly_request_budget: CONNECTOR_BUDGET,
    tournaments: [
      {
        adapter: "challonge",
        tournament: setup.tournament,
        credential: { kind: "v1_key", file: setup.apiKeyFile },
        rounds: { games_to_win: 1 },
      },
    ],
  };
}

/** A match as Challonge's v2.1 API shows it: the parts the run reads. */
export interface ChallongeMatch {
  id: string;
  state: string;
  winner: string | null;
  player1: string | null;
  player2: string | null;
  /** The display text, such as "1 - 0". */
  scores: string | null;
}

const idText = (value: Json | undefined): string | null => (typeof value === "string" && value !== "" ? value : typeof value === "number" ? String(value) : null);

/** The player a `relationships` object names in `slot`, wherever the document puts it. */
function playerOf(relationships: Json | undefined, slot: "player1" | "player2"): string | null {
  const related = isBody(relationships) ? relationships[slot] : undefined;
  return isBody(related) && isBody(related.data) ? idText(related.data.id) : null;
}

/** The participant `points_by_participant` lists at `index`, which is where the live answer names a match's players. */
function listedPlayer(points: Json | undefined, index: 0 | 1): string | null {
  const entry = Array.isArray(points) && points.length === 2 ? points[index] : undefined;
  return isBody(entry) ? idText(entry.participant_id) : null;
}

/**
 * The match in a `GET .../matches/<id>.json` document. The live answer names
 * the players only in `points_by_participant`, first player first; the
 * documented `player1` and `player2` relationships are read too, in its
 * attributes or beside them.
 */
export function parseMatch(document: Json): ChallongeMatch {
  const data = isBody(document) ? document.data : undefined;
  const attributes = isBody(data) ? data.attributes : undefined;
  const id = isBody(data) ? idText(data.id) : null;
  if (!isBody(data) || !isBody(attributes) || id === null || typeof attributes.state !== "string") throw new Error("Challonge's answer is not a match");
  const player = (slot: "player1" | "player2", index: 0 | 1) =>
    playerOf(attributes.relationships, slot) ?? playerOf(data.relationships, slot) ?? listedPlayer(attributes.points_by_participant, index);
  return {
    id,
    state: attributes.state,
    winner: idText(attributes.winner_id),
    player1: player("player1", 0),
    player2: player("player2", 1),
    scores: typeof attributes.scores === "string" ? attributes.scores : null,
  };
}

/**
 * The usernames in a `GET .../participants.json` document, by participant ID
 * (lower case). A tournament of this test has two participants, so its first
 * page is all of them. A participant with no username is left out.
 */
export function parseParticipants(document: Json): Map<string, string> {
  const list = isBody(document) ? document.data : undefined;
  if (!Array.isArray(list)) throw new Error("Challonge's answer is not a list of participants");
  const names = new Map<string, string>();
  for (const participant of list) {
    const id = isBody(participant) ? idText(participant.id) : null;
    const attributes = isBody(participant) ? participant.attributes : undefined;
    const username = isBody(attributes) ? attributes.username : undefined;
    if (id !== null && typeof username === "string" && username !== "") names.set(id, username.toLowerCase());
  }
  return names;
}

/** The one participant ID whose username is `account` (Challonge compares usernames without case), or null. */
export function participantFor(names: Map<string, string>, account: string): string | null {
  const found = [...names].filter(([, username]) => username === account.toLowerCase());
  return found.length === 1 ? found[0]![0] : null;
}

/** What stops a run from starting: the match has to be open with both players in it. */
export function openProblems(match: ChallongeMatch, id: string): string[] {
  const problems: string[] = [];
  if (match.id !== id) problems.push(`Challonge answered match ${match.id}, not ${id}`);
  if (match.state !== "open") problems.push(`match ${id} is ${match.state}, not open (reopen it: node tools/selftest-challonge.ts reopen <run configuration>)`);
  if (match.player1 === null || match.player2 === null) problems.push(`match ${id} does not have both players yet`);
  else if (match.player1 === match.player2) problems.push(`match ${id} has the same player twice`);
  return problems;
}

/**
 * What is wrong with the match after the run, empty when nothing is: it is
 * complete, between the same two players, with `winner` ahead 1 to 0.
 */
export function reportedProblems(match: ChallongeMatch, winner: string, loser: string): string[] {
  const problems: string[] = [];
  if (match.state !== "complete") problems.push(`match ${match.id} is ${match.state}, not complete`);
  if (match.winner !== winner) problems.push(`its winner is ${String(match.winner)}, not participant ${winner}`);
  const players = [match.player1, match.player2].sort().join(" ");
  if (players !== [winner, loser].sort().join(" ")) problems.push(`its players are ${String(match.player1)} and ${String(match.player2)}, not ${winner} and ${loser}`);
  const expected = match.player1 === winner ? "1-0" : "0-1";
  const shown = (match.scores ?? "").replace(/\s+/g, "");
  if (shown !== expected) problems.push(`its score reads ${JSON.stringify(match.scores)}, not ${expected === "1-0" ? "1 - 0" : "0 - 1"} for the winner`);
  return problems;
}

const API = "https://api.challonge.com/v2.1";

/** The organizer's reads and the reopen, with the v1 key, counting what it sends. */
export class ChallongeApi {
  /** Requests sent so far. */
  requests = 0;
  readonly #base: string;
  readonly #headers: Record<string, string>;

  readonly tournament: string;

  constructor(tournament: string, apiKey: string) {
    this.tournament = tournament;
    this.#base = `${API}/tournaments/${encodeURIComponent(tournament)}`;
    this.#headers = {
      Accept: "application/json",
      "Content-Type": "application/vnd.api+json",
      "Authorization-Type": "v1",
      Authorization: apiKey,
    };
  }

  async #send(method: "GET" | "PUT", path: string, body?: Json): Promise<Json> {
    this.requests++;
    const response = await fetch(`${this.#base}${path}`, {
      method,
      headers: this.#headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(20_000),
      redirect: "error",
    });
    const text = await response.text();
    let document: Json = null;
    try {
      document = text === "" ? null : (JSON.parse(text) as Json);
    } catch {
      // A body that is no JSON is reported by its status below.
    }
    if (!response.ok) {
      const errors = isBody(document) && Array.isArray(document.errors) ? document.errors : [];
      const detail = errors.map((error) => (isBody(error) && typeof error.detail === "string" ? error.detail : "")).filter(Boolean).join("; ");
      throw new Error(`Challonge answered ${response.status}${detail ? `: ${detail.slice(0, 200)}` : ""}`);
    }
    return document;
  }

  async match(id: string): Promise<ChallongeMatch> {
    return parseMatch(await this.#send("GET", `/matches/${encodeURIComponent(id)}.json`));
  }

  /** The tournament's participants by ID, with their usernames. */
  async participants(): Promise<Map<string, string>> {
    return parseParticipants(await this.#send("GET", "/participants.json"));
  }

  /** Puts a decided or underway match back to open. */
  async reopen(id: string): Promise<ChallongeMatch> {
    return parseMatch(await this.#send("PUT", `/matches/${encodeURIComponent(id)}/change_state.json`, { data: { type: "MatchState", attributes: { state: "reopen" } } }));
  }
}

/** The API for a run configuration, with its key read now. */
export function apiFor(config: ChallongeConfig): ChallongeApi {
  return new ChallongeApi(config.tournament, readSecret(config.apiKeyFile));
}

/** What is wrong with a match read back after a reopen, empty when nothing is: it is open again, between two players. */
export function reopenedProblems(match: ChallongeMatch, id: string): string[] {
  const problems: string[] = [];
  if (match.id !== id) problems.push(`Challonge answered match ${match.id}, not ${id}`);
  if (match.state !== "open") problems.push(`match ${id} is ${match.state} after the reopen, not open`);
  if (match.player1 === null || match.player2 === null) problems.push(`match ${id} does not have both players after the reopen`);
  return problems;
}

/**
 * Puts the set back to open if it is not, and reads it again to confirm it
 * is: a refused or ignored reopen fails here, not later in a run. Says what
 * it found, including the score Challonge shows after the reopen.
 */
export async function reopenSet(api: Pick<ChallongeApi, "match" | "reopen">, id: string): Promise<string> {
  const before = await api.match(id);
  if (before.state === "open") return `match ${id} is open already; nothing changed`;
  await api.reopen(id);
  const after = await api.match(id);
  const problems = reopenedProblems(after, id);
  if (problems.length > 0) throw new Error(problems.join("; "));
  return `match ${id} was ${before.state}; it is open again, and its score reads ${JSON.stringify(after.scores)}`;
}

if (import.meta.main) {
  const [command, path, ...rest] = process.argv.slice(2);
  if (command !== "reopen" || !path || rest.length > 0) {
    console.error("usage: node tools/selftest-challonge.ts reopen <run configuration>");
    process.exit(2);
  }
  try {
    const config = parseChallongeConfig(readFileSync(path, "utf8"));
    const api = apiFor(config);
    const said = await reopenSet(api, config.match);
    console.log(`${said} (${api.requests} requests)`);
  } catch (error) {
    console.error((error as Error).message);
    process.exit(1);
  }
}
