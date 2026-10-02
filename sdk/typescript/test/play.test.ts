// Two stand-in players play an ember-room-v1 set against the real bridge:
// claim, host, bind, prepare each game and report it.
import assert from "node:assert/strict";
import { test } from "node:test";

import { BridgeClient, eventType, TestPlayer, type Binding, type Permit } from "../src/index.ts";
import { missing, startBridge } from "./bridge-process.ts";

test("two test players play a set reported by the game", { skip: missing, timeout: 60_000 }, async () => {
  const bridge = await startBridge();
  try {
    const provider = new BridgeClient({ origin: bridge.origin, credential: bridge.providerToken });
    const [host, guest] = [new TestPlayer({ origin: bridge.origin }), new TestPlayer({ origin: bridge.origin })];
    const participants = [];
    for (const [slot, player] of [host, guest].entries()) {
      const subject = `play-${slot}`;
      const intent = await provider.createLinkIntent(subject);
      const claim = await player.claimLink(intent.code, bridge.connection);
      const link = await provider.approveLinkClaim(intent.intent_id, { claimId: claim.claim_id!, emberId: player.emberId, subject });
      participants.push({ participant_id: link.participant_id as string, ember_id: player.emberId, slot: slot as 0 | 1 });
    }
    const match = await provider.createMatch({
      external_match_id: "sdk-played-set",
      participants: participants as never,
      games_to_win: 2,
      required_build_id: "sdk",
      native_rules_profile: "ember-room-v1",
    });

    // The first claim hosts; the room it publishes reaches the other player with the binding.
    const lease = await host.claimMatch(match.match_id);
    assert.equal(lease.role, "host");
    assert.equal((await guest.claimMatch(match.match_id)).role, "wait");
    if (lease.role !== "host") return;
    await host.publishRoom(match.match_id, { leaseId: lease.lease_id, fence: lease.fence });
    const joined = await guest.claimMatch(match.match_id);
    assert.equal(joined.role, "room");
    if (joined.role !== "room" || !joined.binding) throw new Error("no binding");
    const binding: Binding = joined.binding.binding;
    assert.equal(binding.fighters[0].endpoint_id, host.endpointId);
    assert.equal(binding.fighters[1].endpoint_id, guest.endpointId);

    let generation = 0;
    for (const result of ["p1_win", "draw", "p2_win", "p1_win"] as const) {
      generation += 1;
      assert.equal((await host.prepareGame(match.match_id, binding, generation)).state, "pending");
      const answer = await guest.prepareGame(match.match_id, binding, generation);
      if (answer.state !== "permitted") throw new Error("no permit");
      const permit: Permit = answer.permit.permit;
      await host.submitReport(match.match_id, host.report(permit, result));
      // The host forwards the guest's report unchanged.
      const receipt = await host.submitReport(match.match_id, guest.report(permit, result));
      assert.equal(receipt.attempt_state, "accepted");
    }
    const finished = await provider.getMatch(match.match_id);
    assert.equal(finished.state, "completed");
    const scores = finished.scores as { wins: number }[];
    assert.deepEqual(scores.map((score) => score.wins), [2, 1]);
    const types = (await provider.listEvents("0", 200)).events.map((event) => event.type);
    for (const kind of ["match.room.ready", "match.attempt.authorized", "match.game.reported", "match.game.confirmed", "match.completed"] as const) {
      assert.ok(types.includes(eventType(kind)), kind);
    }
  } finally {
    await bridge.stop();
  }
});
