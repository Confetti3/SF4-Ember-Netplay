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
  "match.expired",
  "match.corrected",
  "provider.delivery.succeeded",
  "provider.delivery.failed",
  "provider.delivery.ambiguous",
  "lobby.created",
  "lobby.queue.changed",
  "lobby.set.started",
  "lobby.set.completed",
  "lobby.closed",
  "tournament.created",
  "tournament.entrants.changed",
  "tournament.started",
  "tournament.match.started",
  "tournament.match.completed",
  "tournament.match.reopened",
  "tournament.completed",
  "tournament.cancelled",
  "room.created",
  "room.opened",
  "room.changed",
  "room.closed",
] as const;

export type EventName = (typeof EVENT_TYPES)[number];

/** `io.ember.tournament.<name>.v1`. */
export function eventType<Name extends EventName>(name: Name): `io.ember.tournament.${Name}.v1` {
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

/** One open room, as the room routes list it. */
export interface RoomSummary {
  room_id: string;
  name: string;
  build_id: string;
  members: number;
  capacity: number;
  tables_playing: number;
  /** The host's relay region code, such as `use1`. */
  region: string;
  created_at: number;
}

/** `waiting` until the creator is in, then `open`, then `closed`. */
export type RoomState = "waiting" | "open" | "closed";

/** A room as the connection that created it sees it. */
export interface ConnectionRoom {
  room: RoomSummary;
  state: RoomState;
  creator_ember_id: string;
  /** The page that opens the room in Ember, or offers to install Ember first. */
  join_url: string;
}

/** Why a room closed: your `closeRoom`, or it ended on its own (it emptied, its creator never came, or its host stopped). */
export type RoomCloseReason = "closed_by_connection" | "ended";

const ROOM_STATES: readonly string[] = ["waiting", "open", "closed"];
const ROOM_CLOSE_REASONS: readonly string[] = ["closed_by_connection", "ended"];

/** `room.created`, `room.opened` and `room.changed` carry the room as it stands; `room.closed` adds why. */
export type RoomEvent =
  | (BridgeEvent & { type: `io.ember.tournament.room.${"created" | "opened" | "changed"}.v1`; data: ConnectionRoom })
  | (BridgeEvent & { type: "io.ember.tournament.room.closed.v1"; data: ConnectionRoom & { reason: RoomCloseReason } });

function isRoomSummary(value: unknown): value is RoomSummary {
  const room = value as { [key: string]: unknown } | null;
  return (
    typeof room === "object" &&
    room !== null &&
    typeof room.room_id === "string" &&
    typeof room.name === "string" &&
    typeof room.build_id === "string" &&
    typeof room.members === "number" &&
    typeof room.capacity === "number" &&
    typeof room.tables_playing === "number" &&
    typeof room.region === "string" &&
    typeof room.created_at === "number"
  );
}

/**
 * Whether `event` is one of the room events (for rooms your connection
 * created) with the data its type promises, so its `data` can be read as a
 * `ConnectionRoom`. Other events, and room events with unexpected data, are
 * not.
 */
export function isRoomEvent(event: BridgeEvent): event is RoomEvent {
  const kind = event.type;
  const room = event.data;
  const known =
    kind === eventType("room.created") || kind === eventType("room.opened") || kind === eventType("room.changed") || kind === eventType("room.closed");
  return (
    known &&
    isRoomSummary(room.room) &&
    typeof room.state === "string" &&
    ROOM_STATES.includes(room.state) &&
    typeof room.creator_ember_id === "string" &&
    typeof room.join_url === "string" &&
    (kind !== eventType("room.closed") || (typeof room.reason === "string" && ROOM_CLOSE_REASONS.includes(room.reason)))
  );
}
