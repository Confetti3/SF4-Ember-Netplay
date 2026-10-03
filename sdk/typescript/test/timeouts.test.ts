// Request deadlines: createRoom waits longer than other requests, and an
// explicit `timeoutMs` applies to every request. The deadline in use is read
// from the `AbortSignal.timeout` call, so nothing here waits out a real one.
import assert from "node:assert/strict";
import { test } from "node:test";

import { BridgeClient, BridgeError, TestPlayer } from "../src/index.ts";

const ORIGIN = "http://127.0.0.1:1";
const creator = { participantId: "par_kate", emberId: new TestPlayer({ origin: ORIGIN }).emberId };
const spec = { name: "Fight Night", capacity: 4, buildId: "sdk-build", creator };
const ROOM = {
  room: { room_id: "0123456789abcdef0123456789abcdef", name: "Fight Night", build_id: "sdk-build", members: 0, capacity: 4, tables_playing: 0, region: "use1", created_at: 1 },
  state: "waiting",
  creator_ember_id: creator.emberId,
  join_url: "https://embernetplay.link/r#brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11/0123456789abcdef0123456789abcdef",
};

/** A stand-in bridge that answers every request with the room after `delayMs`, or fails the request when its deadline passes first. */
function slowBridge(delayMs: number): typeof fetch {
  return ((_url: string, init: RequestInit) =>
    new Promise<Response>((resolve, reject) => {
      const timer = setTimeout(() => resolve(Response.json(ROOM)), delayMs);
      init.signal?.addEventListener("abort", () => {
        clearTimeout(timer);
        reject(init.signal?.reason);
      });
    })) as typeof fetch;
}

/** The deadline each request in `run` asked for. */
async function deadlines(run: () => Promise<unknown>): Promise<number[]> {
  const original = AbortSignal.timeout;
  const asked: number[] = [];
  AbortSignal.timeout = (ms: number) => {
    asked.push(ms);
    return original.call(AbortSignal, ms);
  };
  try {
    await run();
  } finally {
    AbortSignal.timeout = original;
  }
  return asked;
}

test("createRoom waits 45 seconds by default and other requests 15", async () => {
  const client = new BridgeClient({ origin: ORIGIN, credential: "emk_test", fetch: slowBridge(0) });
  assert.deepEqual(await deadlines(() => client.createRoom(spec)), [45_000]);
  assert.deepEqual(await deadlines(() => client.getRoom(ROOM.room.room_id)), [15_000]);
  assert.deepEqual(await deadlines(() => client.listRooms()), [15_000]);
});

test("an explicit timeoutMs applies to every request, createRoom included", async () => {
  const client = new BridgeClient({ origin: ORIGIN, credential: "emk_test", timeoutMs: 5_000, fetch: slowBridge(0) });
  assert.deepEqual(await deadlines(() => client.createRoom(spec)), [5_000]);
  assert.deepEqual(await deadlines(() => client.getRoom(ROOM.room.room_id)), [5_000]);
});

test("a room that takes a moment to start is still returned, and a short override gives up", async () => {
  const patient = new BridgeClient({ origin: ORIGIN, credential: "emk_test", fetch: slowBridge(150) });
  assert.equal((await patient.createRoom(spec)).room.room_id, ROOM.room.room_id);

  const impatient = new BridgeClient({ origin: ORIGIN, credential: "emk_test", timeoutMs: 20, fetch: slowBridge(150) });
  await assert.rejects(impatient.createRoom(spec), (error: Error) => error.name === "TimeoutError" && !(error instanceof BridgeError));
});
