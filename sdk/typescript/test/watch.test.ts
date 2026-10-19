// An organizer opens a match to watchers and a stand-in watcher joins its room,
// against the real ember-bridge (built by `cargo build -p ember-bridge` in
// server/ember). Requires EMBER_BRIDGE_TEST_BINARY from the tested build.
import assert from "node:assert/strict";
import { test } from "node:test";

import { BridgeClient, BridgeError, TestPlayer, type Binding } from "../src/index.ts";
import { linkPlayer, startBridge } from "./bridge-process.ts";

async function refusal(promise: Promise<unknown>): Promise<BridgeError> {
  try {
    await promise;
  } catch (error) {
    assert.ok(error instanceof BridgeError, String(error));
    return error;
  }
  throw new Error("expected a refusal");
}

test("watch policy: staff, link holders and denials decide who is named in the binding", { timeout: 60_000 }, async () => {
  const bridge = await startBridge();
  try {
    const provider = new BridgeClient({ origin: bridge.origin, credential: bridge.providerToken });
    const organizer = new BridgeClient({ origin: bridge.origin, credential: bridge.organizerToken });
    const [host, guest, caster, viewer] = [1, 2, 3, 4].map(() => new TestPlayer({ origin: bridge.origin }));
    const participants = [];
    for (const [slot, player] of [host!, guest!].entries()) {
      const link = await linkPlayer(provider, bridge.connection, player, `watch-${slot}`);
      participants.push({ ...link, slot: slot as 0 | 1 });
    }
    const match = await provider.createMatch({
      external_match_id: "sdk-watched-set",
      participants: participants as never,
      games_to_win: 2,
      required_build_id: "sdk",
      native_rules_profile: "ember-room-v1",
    });
    const id = match.match_id;
    const lease = await host!.claimMatch(id, undefined, { watch: true });
    await guest!.claimMatch(id, undefined, { watch: true });
    if (lease.role !== "host") throw new Error("no lease");
    await host!.publishRoom(id, { leaseId: lease.lease_id, fence: lease.fence });
    const named = async (fighter: TestPlayer, watch = true): Promise<string[]> => {
      const answer = await fighter.claimMatch(id, undefined, { watch });
      if (answer.role !== "room" || !answer.binding) throw new Error("no binding");
      const binding: Binding = answer.binding.binding;
      return (binding.watchers ?? []).map((watcher) => watcher.ember_id);
    };
    assert.deepEqual(await named(guest!), []);

    // A match is closed to watchers until its organizer opens it.
    const policy = await provider.watchPolicy(bridge.connection);
    assert.deepEqual(policy, { connection_id: bridge.connection, link_holders: false, link_holder_cap: 4, ceiling: 8, staff: [] });
    const closed = await refusal(caster!.watchMatch(id));
    assert.equal(closed.status, 403);
    assert.equal(closed.details.reason, "not_allowed");
    assert.equal((await refusal(host!.watchMatch(id))).details.reason, "fighter");
    // A provider credential reads the policy but does not write it.
    assert.equal((await refusal(provider.setWatchLinkHolders(bridge.connection, true))).status, 403);
    assert.equal((await refusal(provider.addWatchStaff(bridge.connection, caster!.emberId))).status, 403);

    // Staff are named in the fighters' binding, and the watcher gets the room.
    const staff = await organizer.addWatchStaff(bridge.connection, caster!.emberId);
    assert.equal(staff.ember_id, caster!.emberId);
    const placed = await caster!.watchMatch(id);
    if (placed.state !== "room") throw new Error(`not placed: ${JSON.stringify(placed)}`);
    assert.deepEqual(await named(guest!), [caster!.emberId]);
    const view = await provider.matchWatchers(id);
    assert.equal(view.link_holders, false);
    assert.deepEqual(view.watchers.map((w) => [w.ember_id, w.via, w.bound]), [[caster!.emberId, "staff", true]]);

    // Anyone with the match link, while the organizer allows it.
    assert.equal((await refusal(viewer!.watchMatch(id))).details.reason, "not_allowed");
    assert.equal((await organizer.setWatchLinkHolders(bridge.connection, true)).link_holders, true);
    assert.equal((await viewer!.watchMatch(id)).state, "room");
    assert.deepEqual((await named(host!)).sort(), [caster!.emberId, viewer!.emberId].sort());
    await organizer.setWatchLinkHolders(bridge.connection, false);
    assert.deepEqual(await named(host!), [caster!.emberId]);
    assert.equal((await refusal(viewer!.watchMatch(id))).details.reason, "not_allowed");

    // A denial removes a watcher from this match, staff or not, until it is lifted.
    await organizer.denyWatcher(id, caster!.emberId);
    assert.deepEqual(await named(guest!), []);
    assert.equal((await refusal(caster!.watchMatch(id))).details.reason, "revoked");
    assert.deepEqual((await organizer.matchWatchers(id)).denied, [caster!.emberId]);
    await organizer.clearWatchDenial(id, caster!.emberId);
    assert.equal((await caster!.watchMatch(id)).state, "room");

    // A fighter whose Ember does not say it reads watchers gets none named.
    assert.deepEqual(await named(host!), [caster!.emberId]);
    assert.deepEqual(await named(host!, false), []);
    assert.equal((await caster!.watchMatch(id)).state, "waiting");
    assert.deepEqual(await named(host!), [caster!.emberId]);

    // Stopping, and taking the approval back, both free the place.
    await caster!.stopWatching(id);
    assert.deepEqual(await named(host!), []);
    await caster!.watchMatch(id);
    await organizer.removeWatchStaff(bridge.connection, caster!.emberId);
    assert.deepEqual(await named(host!), []);
    assert.deepEqual((await organizer.watchPolicy(bridge.connection)).staff, []);
  } finally {
    await bridge.stop();
  }
});
