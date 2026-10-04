// A finished match's result, as a connection with a `results_url` receives it
// (docs/design/INTEGRATION_PATHS.md, "Result secret and result body").
import { parseStrict, type Json } from "./canonical.ts";
import { isEmberId } from "./identity.ts";
import { counter, prefixedId } from "./signed.ts";
import { header, verifyWebhook, WebhookError, type WebhookHeaders } from "./webhook.ts";

/** The `type` of a result body. */
export const RESULT_TYPE = "io.ember.tournament.match.result.v1";

/**
 * `completed`, `restart` for a cancelled or failed match (play it again), or
 * `expired` for a match nobody played before it expired. `expired` is not a
 * request to restart: whether to schedule the match again is yours to decide.
 */
export type ResultOutcome = "completed" | "restart" | "expired";

export interface ResultParticipant {
  participant_id: string;
  ember_id: string;
  slot: 0 | 1;
  /** Games won that count. */
  score: number;
}

interface ResultFields {
  type: typeof RESULT_TYPE;
  bridge_id: string;
  connection_id: string;
  match_id: string;
  external_match_id: string;
  /** The match revision, as a decimal string. */
  revision: string;
  participants: [ResultParticipant, ResultParticipant];
}

export interface CompletedResult extends ResultFields {
  outcome: "completed";
  /** One of the two participants. */
  winner_participant_id: string;
}

export interface RestartResult extends ResultFields {
  outcome: "restart";
  winner_participant_id?: never;
}

export interface ExpiredResult extends ResultFields {
  outcome: "expired";
  winner_participant_id?: never;
}

/** Narrow on `outcome`: only a completed result has a winner. */
export type MatchResult = CompletedResult | RestartResult | ExpiredResult;

export class ResultError extends Error {}

const FIELDS = ["type", "bridge_id", "connection_id", "match_id", "external_match_id", "outcome", "revision", "participants"];
const PARTICIPANT_FIELDS = ["participant_id", "ember_id", "slot", "score"];

function isObject(value: unknown): value is { [key: string]: unknown } {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function hasFields(value: { [key: string]: unknown }, fields: string[], optional: string[] = []): boolean {
  const keys = Object.keys(value);
  return fields.every((field) => keys.includes(field)) && keys.every((key) => fields.includes(key) || optional.includes(key));
}

function parseParticipant(value: unknown): ResultParticipant {
  if (!isObject(value) || !hasFields(value, PARTICIPANT_FIELDS)) throw new ResultError("invalid result participant");
  const { participant_id, ember_id, slot, score } = value;
  const valid =
    typeof participant_id === "string" &&
    participant_id !== "" &&
    typeof ember_id === "string" &&
    isEmberId(ember_id) &&
    (slot === 0 || slot === 1) &&
    typeof score === "number" &&
    Number.isInteger(score) &&
    score >= 0;
  if (!valid) throw new ResultError("invalid result participant");
  return { participant_id, ember_id, slot, score };
}

/**
 * Parses a match result body and checks its shape: the type, the fields, two
 * participants in slots 0 and 1, and a winner exactly when it completed. It
 * does not authenticate anything; use `verifyResult` on a delivery.
 */
export function parseResult(payload: string | Uint8Array | Json): MatchResult {
  const value =
    typeof payload === "string" ? parseStrict(payload) : payload instanceof Uint8Array ? parseStrict(Buffer.from(payload).toString("utf8")) : payload;
  if (!isObject(value)) throw new ResultError("a result is an object");
  if (value.type !== RESULT_TYPE) throw new ResultError("not a match result");
  if (!hasFields(value, FIELDS, ["winner_participant_id"])) throw new ResultError("unexpected result fields");
  const { bridge_id, connection_id, match_id, external_match_id, outcome, revision, participants, winner_participant_id } = value;
  const text = (field: unknown): field is string => typeof field === "string" && field !== "";
  const id = (field: unknown, prefix: string): field is string => typeof field === "string" && prefixedId(field, prefix);
  if (
    !(
      id(bridge_id, "brg") &&
      text(connection_id) &&
      id(match_id, "emt") &&
      text(external_match_id) &&
      typeof revision === "string" &&
      counter(revision) !== null &&
      Array.isArray(participants) &&
      participants.length === 2
    )
  ) {
    throw new ResultError("invalid result");
  }
  const first = parseParticipant(participants[0]);
  const second = parseParticipant(participants[1]);
  if (first.slot !== 0 || second.slot !== 1) throw new ResultError("participants are in slots 0 and 1");
  const fields: ResultFields = {
    type: RESULT_TYPE,
    bridge_id,
    connection_id,
    match_id,
    external_match_id,
    revision,
    participants: [first, second],
  };
  if (outcome === "completed") {
    if (!text(winner_participant_id) || ![first, second].some((p) => p.participant_id === winner_participant_id)) {
      throw new ResultError("a completed result names one of its participants as the winner");
    }
    return { ...fields, outcome, winner_participant_id };
  }
  if (outcome !== "restart" && outcome !== "expired") throw new ResultError("invalid result");
  if (winner_participant_id !== undefined) throw new ResultError(`${outcome === "restart" ? "a restart" : "an expired match"} has no winner`);
  return { ...fields, outcome };
}

/**
 * Verifies a delivery to your `results_url` with the connection's result
 * secret, then parses it. It is `verifyWebhook` (a result is signed the same
 * way) plus the two checks that tie the delivery to its body: the type, and
 * `webhook-id` being `res_` and the match ID. Answer 2xx once the result is
 * stored, or 409 if you already have it: the bridge may send it again.
 */
export function verifyResult(
  rawBody: Uint8Array | string,
  headers: WebhookHeaders | Headers | Record<string, string | string[] | undefined>,
  secrets: string[],
  now?: number,
): MatchResult {
  verifyWebhook(rawBody, headers, secrets, now);
  const result = parseResult(rawBody);
  if (header(headers, "webhook-id") !== `res_${result.match_id}`) throw new WebhookError("webhook-id is not this result's");
  return result;
}
