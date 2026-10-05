// Room events, and a connection opening rooms through the real ember-bridge
// (built by `cargo build -p ember-bridge` in server/ember) with a stand-in room
// supervisor (docs/design/PUBLIC_ROOMS.md, "Supervisor API"). The bridge asks
// the supervisor which rooms are alive every five seconds, so the live test
// waits on that. Skipped when the binary is absent.
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { test } from "node:test";

import { BridgeClient, BridgeError, eventType, isRoomEvent, parseEvent, TestPlayer, type BridgeEvent, type ConnectionRoom, type RoomEvent } from "../src/index.ts";
import { linkPlayer, missing, startBridge, until } from "./bridge-process.ts";

const SECRET = "sdk-supervisor-secret";
const BUILD = "sdk-build";

function roomData(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    room: { room_id: "0123456789abcdef0123456789abcdef", name: "Fight Night", build_id: BUILD, members: 1, capacity: 8, tables_playing: 0, region: "use1", created_at: 1 },
    state: "open",
    creator_ember_id: new TestPlayer({ origin: "http://127.0.0.1:1", seed: Buffer.alloc(32, 3) }).emberId,
    join_url: "https://embernetplay.link/r#brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11/0123456789abcdef0123456789abcdef",
    ...overrides,
  };
}

function envelope(name: Parameters<typeof eventType>[0], data: Record<string, unknown>): BridgeEvent {
  return parseEvent({
    specversion: "1.0",
    id: "evt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11",
    source: "http://127.0.0.1:8787",
    type: eventType(name),
    subject: "rooms/0123456789abcdef0123456789abcdef",
    time: "2026-10-02T12:00:00Z",
    datacontenttype: "application/json",
    dataschema: "https://example.test/schema",
    emberseq: "1",
    data: data as never,
  });
}

test("isRoomEvent narrows room events and refuses others", () => {
  for (const name of ["room.created", "room.opened", "room.changed"] as const) {
    const event = envelope(name, roomData());
    assert.ok(isRoomEvent(event));
    // Narrowed: the data reads as a ConnectionRoom.
    const room: ConnectionRoom = event.data;
    assert.equal(room.room.name, "Fight Night");
  }
  const closed = envelope("room.closed", roomData({ state: "closed", reason: "ended" }));
  assert.ok(isRoomEvent(closed));
  if (isRoomEvent(closed) && closed.type === eventType("room.closed")) assert.equal(closed.data.reason, "ended");

  // Not a room event, or not the data a room event carries.
  assert.ok(!isRoomEvent(envelope("match.created", roomData())));
  assert.ok(!isRoomEvent(envelope("room.closed", roomData())));
  assert.ok(!isRoomEvent(envelope("room.closed", roomData({ reason: "tired" }))));
  assert.ok(!isRoomEvent(envelope("room.opened", roomData({ state: "ajar" }))));
  assert.ok(!isRoomEvent(envelope("room.opened", { ...roomData(), room: null })));
});

interface Hosted {
  room_id: string;
  members: number;
  capacity: number;
  tables_playing: number;
  invitation: string;
  banned: string[];
  opened: boolean;
}

/** A stand-in room supervisor: hosts whatever it is asked for, and says what its test tells it to. */
async function startSupervisor(): Promise<{ url: string; created: Record<string, unknown>[]; hosted: Hosted[]; deleted: string[]; close(): void }> {
  const created: Record<string, unknown>[] = [];
  const hosted: Hosted[] = [];
  const deleted: string[] = [];
  const server = createServer((request, response) => {
    const chunks: Buffer[] = [];
    request.on("data", (chunk: Buffer) => chunks.push(chunk));
    request.on("end", () => {
      const answer = (status: number, body?: unknown) => response.writeHead(status, { "content-type": "application/json" }).end(body === undefined ? undefined : JSON.stringify(body));
      if (request.headers.authorization !== `Bearer ${SECRET}`) return answer(401, {});
      if (request.method === "POST" && request.url === "/rooms") {
        const body = JSON.parse(Buffer.concat(chunks).toString("utf8")) as { room_id: string; capacity: number };
        created.push(body);
        hosted.push({ room_id: body.room_id, members: 0, capacity: body.capacity, tables_playing: 0, invitation: `sf4e3:${body.room_id}`, banned: [], opened: false });
        return answer(201, { invitation: `sf4e3:${body.room_id}`, region: "use1" });
      }
      if (request.method === "GET" && request.url === "/rooms") return answer(200, hosted);
      if (request.method === "DELETE" && request.url?.startsWith("/rooms/")) {
        const id = request.url.slice("/rooms/".length);
        deleted.push(id);
        hosted.splice(0, hosted.length, ...hosted.filter((room) => room.room_id !== id));
        return answer(204);
      }
      return answer(404, {});
    });
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  return { url: `http://127.0.0.1:${(server.address() as { port: number }).port}`, created, hosted, deleted, close: () => server.close() };
}

function refusal(reason: string, status: number) {
  return (error: BridgeError) => error.status === status && error.details.reason === reason;
}

test("a connection opens, lists and closes rooms for linked players", { skip: missing, timeout: 90_000 }, async () => {
  const supervisor = await startSupervisor();
  const bridge = await startBridge({
    config: { rooms: { supervisor_url: supervisor.url } },
    connection: { rooms: { max_open: 2 } },
    secrets: { rooms_supervisor_secret: SECRET },
  });
  try {
    const { origin, connection } = bridge;
    const provider = new BridgeClient({ origin, credential: bridge.providerToken });
    assert.ok(((await provider.getCapabilities()).features as string[]).includes("rooms.connections"));

    const kate = await linkPlayer(provider, connection, new TestPlayer({ origin }), "room-kate");
    const sam = await linkPlayer(provider, connection, new TestPlayer({ origin }), "room-sam");
    const creator = (player: { participant_id: string; ember_id: string }) => ({ participantId: player.participant_id, emberId: player.ember_id });

    // The room is the creator's, hosted and waiting for them to come in.
    const room = await provider.createRoom({ name: "Fight Night", capacity: 4, buildId: BUILD, creator: creator(kate) });
    assert.equal(room.state, "waiting");
    assert.equal(room.creator_ember_id, kate.ember_id);
    assert.equal(room.room.name, "Fight Night");
    assert.equal(room.room.capacity, 4);
    assert.equal(room.room.members, 0);
    assert.equal(room.room.region, "use1");
    const { bridge_id: bridgeId } = (await (await fetch(`${origin}/.well-known/ember-bridge.json`)).json()) as { bridge_id: string };
    assert.equal(room.join_url, `https://embernetplay.link/r#${bridgeId}/${room.room.room_id}`);
    assert.equal(supervisor.created[0]!.room_id, room.room.room_id);
    assert.equal(supervisor.created[0]!.creator, kate.ember_id);

    // Refusals say why. One open room per creator, and the bot can hand out the one it has.
    await assert.rejects(provider.createRoom({ name: "Again", capacity: 4, buildId: BUILD, creator: creator(kate) }), (error: BridgeError) => {
      assert.equal(error.details.reason, "room_limit");
      assert.equal(error.details.room_id, room.room.room_id);
      return true;
    });
    const stranger = new TestPlayer({ origin });
    await assert.rejects(
      provider.createRoom({ name: "Not yours", capacity: 4, buildId: BUILD, creator: { participantId: "par_unknown", emberId: stranger.emberId } }),
      refusal("not_linked", 403),
    );
    await assert.rejects(provider.createRoom({ name: "", capacity: 4, buildId: BUILD, creator: creator(sam) }), refusal("invalid_name", 400));
    assert.equal(supervisor.created.length, 1);

    // Listing and reading.
    assert.deepEqual(await provider.listRooms(), [room]);
    assert.deepEqual(await provider.listRooms(BUILD), [room]);
    assert.deepEqual(await provider.listRooms("another-build"), []);
    assert.deepEqual(await provider.getRoom(room.room.room_id), room);
    await assert.rejects(provider.getRoom("f".repeat(32)), (error: BridgeError) => error.status === 404 && error.details.reason === "room_not_found");

    // The creator comes in: the next poll opens the room.
    const [hosted] = supervisor.hosted;
    Object.assign(hosted!, { members: 1, opened: true });
    await until(async () => (await provider.getRoom(room.room.room_id)).state === "open", "the room to open");
    assert.equal((await provider.getRoom(room.room.room_id)).room.members, 1);
    Object.assign(hosted!, { members: 2, tables_playing: 1 });
    await until(async () => (await provider.getRoom(room.room.room_id)).room.members === 2, "the room to change");

    // A room whose host stops ends on its own.
    const second = await provider.createRoom({ name: "Second", capacity: 2, buildId: BUILD, creator: creator(sam) });
    assert.deepEqual((await provider.listRooms()).map((listed) => listed.room.room_id), [second.room.room_id, room.room.room_id]);
    supervisor.hosted.splice(1, 1);
    await until(async () => (await provider.getRoom(second.room.room_id)).state === "closed", "the room to end");

    // Closing: the supervisor is told, the room is closed, and closing again answers the same.
    const closed = await provider.closeRoom(room.room.room_id, "Night over");
    assert.equal(closed.state, "closed");
    assert.deepEqual(supervisor.deleted, [room.room.room_id]);
    assert.deepEqual(await provider.closeRoom(room.room.room_id, "Night over"), closed);
    assert.deepEqual(await provider.listRooms(), []);
    assert.equal((await provider.getRoom(room.room.room_id)).state, "closed");

    // The stream told it all, in order, with why each room closed.
    const { events } = await provider.listEvents("0", 200);
    const roomEvents = events.filter(isRoomEvent);
    assert.deepEqual(
      roomEvents.map((event) => [event.type.split(".").slice(-2, -1)[0], event.subject, event.data.state]),
      [
        ["created", `rooms/${room.room.room_id}`, "waiting"],
        ["opened", `rooms/${room.room.room_id}`, "open"],
        ["changed", `rooms/${room.room.room_id}`, "open"],
        ["created", `rooms/${second.room.room_id}`, "waiting"],
        ["closed", `rooms/${second.room.room_id}`, "closed"],
        ["closed", `rooms/${room.room.room_id}`, "closed"],
      ],
    );
    const reasons = roomEvents.filter((event): event is Extract<RoomEvent, { type: "io.ember.tournament.room.closed.v1" }> => event.type === eventType("room.closed")).map((event) => event.data.reason);
    assert.deepEqual(reasons, ["ended", "closed_by_connection"]);
  } finally {
    supervisor.close();
    await bridge.stop();
  }
});

test("rooms are off for a connection without them, and for a bridge without a supervisor", { skip: missing, timeout: 60_000 }, async () => {
  const bridge = await startBridge();
  try {
    const provider = new BridgeClient({ origin: bridge.origin, credential: bridge.providerToken });
    assert.ok(!((await provider.getCapabilities()).features as string[]).includes("rooms.connections"));
    await assert.rejects(provider.listRooms(), (error: BridgeError) => error.status === 404);
    await assert.rejects(provider.closeRoom("f".repeat(32), "x"), (error: BridgeError) => error.status === 404);
  } finally {
    await bridge.stop();
  }

  // A supervisor without `rooms` on the connection: the routes exist, the connection may not use them.
  const supervisor = await startSupervisor();
  const withSupervisor = await startBridge({ config: { rooms: { supervisor_url: supervisor.url } }, secrets: { rooms_supervisor_secret: SECRET } });
  try {
    const provider = new BridgeClient({ origin: withSupervisor.origin, credential: withSupervisor.providerToken });
    await assert.rejects(provider.listRooms(), (error: BridgeError) => error.status === 403 && error.code === "forbidden");
    await assert.rejects(
      provider.createRoom({ name: "No", capacity: 4, buildId: BUILD, creator: { participantId: "par_x", emberId: new TestPlayer({ origin: withSupervisor.origin }).emberId } }),
      (error: BridgeError) => error.status === 403,
    );
  } finally {
    supervisor.close();
    await withSupervisor.stop();
  }
});
