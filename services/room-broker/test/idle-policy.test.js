/**
 * Room idle-policy regression tests.
 * Run: node --test services/room-broker/test/*.test.js
 */

const { describe, it } = require("node:test");
const assert = require("node:assert/strict");

delete process.env.ROOM_OCCUPIED_IDLE_MS;
delete process.env.ROOM_IDLE_MS;

const {
  ROOM_LOBBY_IDLE_MS,
  ROOM_OCCUPIED_IDLE_MS,
  isRoomOccupied,
  roomIdleLimitMs,
  shouldPruneRoom,
} = require("../server");

function room(overrides = {}) {
  return {
    lastSeenAt: 1_000,
    startedAt: null,
    endpoints: { host: null, guest: null },
    ...overrides,
  };
}

describe("room idle policy", () => {
  it("keeps the five-minute cleanup for abandoned host-only lobbies", () => {
    const candidate = room();
    assert.equal(ROOM_LOBBY_IDLE_MS, 5 * 60 * 1000);
    assert.equal(isRoomOccupied(candidate), false);
    assert.equal(roomIdleLimitMs(candidate), ROOM_LOBBY_IDLE_MS);
    assert.equal(shouldPruneRoom(candidate, candidate.lastSeenAt + ROOM_LOBBY_IDLE_MS), false);
    assert.equal(shouldPruneRoom(candidate, candidate.lastSeenAt + ROOM_LOBBY_IDLE_MS + 1), true);
  });

  it("does not age out a started room by default", () => {
    const candidate = room({ startedAt: 2_000 });
    assert.equal(ROOM_OCCUPIED_IDLE_MS, 0);
    assert.equal(isRoomOccupied(candidate), true);
    assert.equal(roomIdleLimitMs(candidate), 0);
    assert.equal(shouldPruneRoom(candidate, candidate.lastSeenAt + 24 * 60 * 60 * 1000), false);
  });

  it("does not age out a room after both endpoints register", () => {
    const candidate = room({
      endpoints: { host: { ggpoPort: 23457 }, guest: { ggpoPort: 23457 } },
    });
    assert.equal(isRoomOccupied(candidate), true);
    assert.equal(shouldPruneRoom(candidate, Number.MAX_SAFE_INTEGER), false);
  });
});
