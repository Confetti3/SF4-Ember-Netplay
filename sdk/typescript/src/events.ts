// Bridge events: CloudEvents 1.0 with an `emberseq` cursor (spec 20.2).
import { parseStrict, type Json } from "./canonical.ts";

export const EVENT_TYPES = [
  "identity.link.pending",
  "identity.link.completed",
  "identity.link.removed",
  "match.created",
  "match.assignment.changed",
  "match.room.ready",
  "match.player.present",
  "match.player.ready",
  "match.started",
  "match.attempt.authorized",
  "match.game.reported",
  "match.game.confirmed",
  "match.score.changed",
  "match.needs_review",
  "match.completed",
  "match.cancelled",
  "match.failed",
  "match.corrected",
  "provider.delivery.succeeded",
  "provider.delivery.failed",
  "provider.delivery.ambiguous",
] as const;

export type EventName = (typeof EVENT_TYPES)[number];

/** `io.ember.tournament.<name>.v1`. */
export function eventType(name: EventName): string {
  return `io.ember.tournament.${name}.v1`;
}

export interface BridgeEvent {
  specversion: "1.0";
  id: string;
  source: string;
  type: string;
  subject: string;
  time: string;
  datacontenttype: "application/json";
  dataschema: string;
  emberseq: string;
  data: { [key: string]: Json };
}

export class EventError extends Error {}

const FIELDS = ["specversion", "id", "source", "type", "subject", "time", "datacontenttype", "dataschema", "emberseq", "data"];

/**
 * Parses a verified webhook body or one event from the replay API. Unknown
 * event types parse too, so a receiver can skip ones it does not handle.
 */
export function parseEvent(payload: string | Uint8Array | Json): BridgeEvent {
  const value =
    typeof payload === "string" ? parseStrict(payload) : payload instanceof Uint8Array ? parseStrict(Buffer.from(payload).toString("utf8")) : payload;
  if (value === null || typeof value !== "object" || Array.isArray(value)) throw new EventError("an event is an object");
  const keys = Object.keys(value);
  if (keys.length !== FIELDS.length || !FIELDS.every((field) => keys.includes(field))) throw new EventError("unexpected event fields");
  const event = value as unknown as BridgeEvent;
  const valid =
    event.specversion === "1.0" &&
    /^evt_[0-9a-f-]{36}$/.test(event.id) &&
    /^io\.ember\.[a-z_]+(\.[a-z_]+)+\.v1$/.test(event.type) &&
    /^(0|[1-9][0-9]{0,19})$/.test(event.emberseq) &&
    event.datacontenttype === "application/json" &&
    typeof event.data === "object" &&
    event.data !== null &&
    !Array.isArray(event.data);
  if (!valid) throw new EventError("invalid event envelope");
  return event;
}
