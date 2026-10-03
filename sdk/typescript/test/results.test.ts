// Match results: the body's shape, and a result delivered by the real
// ember-bridge (built by `cargo build -p ember-bridge` in rust/ember) to a
// connection's `results_url`. The delivery test is skipped when it is absent.
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
import { test } from "node:test";

import { BridgeClient, parseResult, RESULT_TYPE, ResultError, signWebhook, TestPlayer, verifyResult, WebhookError } from "../src/index.ts";
import { linkPlayer, missing, startBridge, startReceiver, until } from "./bridge-process.ts";

const EMBER_A = new TestPlayer({ origin: "http://127.0.0.1:1", seed: Buffer.alloc(32, 1) }).emberId;
const EMBER_B = new TestPlayer({ origin: "http://127.0.0.1:1", seed: Buffer.alloc(32, 2) }).emberId;
const MATCH = "emt_0b9c1e3a-5d27-4f58-8a41-6c3e9d7f2b10";

function sample(): Record<string, unknown> {
  return {
    type: RESULT_TYPE,
    bridge_id: "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11",
    connection_id: "night-bot",
    match_id: MATCH,
    external_match_id: "round-3",
    outcome: "completed",
    revision: "9",
    participants: [
      { participant_id: "par_a", ember_id: EMBER_A, slot: 0, score: 2 },
      { participant_id: "par_b", ember_id: EMBER_B, slot: 1, score: 1 },
    ],
    winner_participant_id: "par_a",
  };
}

test("a result parses, and a malformed one is refused", () => {
  const result = parseResult(JSON.stringify(sample()));
  assert.equal(result.outcome, "completed");
  assert.equal(result.winner_participant_id, "par_a");
  assert.deepEqual(result.participants.map((p) => p.score), [2, 1]);

  // A restart has no winner.
  const { winner_participant_id: _winner, ...rest } = sample();
  assert.equal(parseResult(JSON.stringify({ ...rest, outcome: "restart" })).winner_participant_id, undefined);

  // The outcome narrows the result: a completed one has its winner as a string.
  const narrowed = parseResult(JSON.stringify(sample()));
  if (narrowed.outcome === "completed") {
    const winner: string = narrowed.winner_participant_id;
    assert.equal(winner, "par_a");
  } else {
    assert.fail("a completed result narrows to completed");
  }

  const refused = (change: Record<string, unknown>) => assert.throws(() => parseResult(JSON.stringify({ ...sample(), ...change })), ResultError);
  refused({ type: "io.ember.tournament.match.completed.v1" });
  refused({ extra: true });
  refused({ outcome: "forfeit" });
  refused({ revision: "09" });
  refused({ match_id: "emt_nope" });
  // Values of the wrong type are refused, not read as their text.
  refused({ revision: 9 });
  refused({ revision: ["9"] });
  refused({ bridge_id: [sample().bridge_id] });
  refused({ match_id: [MATCH] });
  refused({ connection_id: ["night-bot"] });
  refused({ external_match_id: 3 });
  // Prefixed lowercase version 4 UUIDs only.
  refused({ match_id: "emt_0b9c1e3a-5d27-1f58-8a41-6c3e9d7f2b10" });
  refused({ match_id: "emt_0b9c1e3a-5d27-4f58-ca41-6c3e9d7f2b10" });
  refused({ match_id: "emt_0B9C1E3A-5D27-4F58-8A41-6C3E9D7F2B10" });
  refused({ match_id: "emt_----------------------------------------" });
  refused({ bridge_id: "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11" });
  refused({ revision: "18446744073709551616" });
  // A completed result without a winner.
  assert.throws(() => parseResult(JSON.stringify(rest)), ResultError);
  refused({ winner_participant_id: null });
  refused({ winner_participant_id: ["par_a"] });
  refused({ winner_participant_id: "par_c" });
  refused({ outcome: "restart" });
  refused({ participants: [] });
  refused({ participants: [{ participant_id: "par_a", ember_id: EMBER_A, slot: 1, score: 2 }, { participant_id: "par_b", ember_id: EMBER_B, slot: 1, score: 1 }] });
  refused({ participants: [{ participant_id: "par_a", ember_id: "emb1_nope", slot: 0, score: 2 }, { participant_id: "par_b", ember_id: EMBER_B, slot: 1, score: 1 }] });
  assert.throws(() => parseResult("[]"), ResultError);
});

test("verifyResult checks the signature and that the delivery is for the match", () => {
  const secret = `whsec_${randomBytes(32).toString("base64")}`;
  const body = JSON.stringify(sample());
  const now = Math.floor(Date.now() / 1000);
  const headers = signWebhook(body, `res_${MATCH}`, now, secret);
  assert.equal(verifyResult(body, headers, [secret]).match_id, MATCH);

  assert.throws(() => verifyResult(body, headers, [`whsec_${randomBytes(32).toString("base64")}`]), WebhookError);
  assert.throws(() => verifyResult(`${body} `, headers, [secret]), WebhookError);
  // Signed correctly, but as another match's delivery.
  assert.throws(() => verifyResult(body, signWebhook(body, "res_emt_other", now, secret), [secret]), WebhookError);
  // Signed correctly, but not a result.
  const event = JSON.stringify({ ...sample(), type: "io.ember.tournament.match.completed.v1" });
  assert.throws(() => verifyResult(event, signWebhook(event, `res_${MATCH}`, now, secret), [secret]), ResultError);
});

test("a finished match is delivered to the connection's results_url, signed", { skip: missing, timeout: 60_000 }, async () => {
  const receiver = await startReceiver();
  const secret = `whsec_${randomBytes(32).toString("base64")}`;
  const bridge = await startBridge({ connection: { results_url: receiver.url }, secrets: { result_secrets: { "mock-local": secret } } });
  try {
    const { origin, connection } = bridge;
    const provider = new BridgeClient({ origin, credential: bridge.providerToken });
    const organizer = new BridgeClient({ origin, credential: bridge.organizerToken });
    assert.ok(((await provider.getCapabilities()).features as string[]).includes("results.signed"));

    const participants = [];
    for (const slot of [0, 1] as const) {
      participants.push({ ...(await linkPlayer(provider, connection, new TestPlayer({ origin }), `result-${slot}`)), slot });
    }
    const spec = { participants: participants as never, games_to_win: 2 as const, required_build_id: "sdk" };

    // A set the first player wins 2-0.
    const played = await provider.createMatch({ ...spec, external_match_id: "result-set" });
    await organizer.recordGame(played.match_id, { winnerSlot: 0 }, played.revision, "VOD", "g1");
    await organizer.recordGame(played.match_id, { winnerSlot: 0 }, "2", "VOD", "g2");
    // A set cancelled: the platform replays it.
    const dropped = await provider.createMatch({ ...spec, external_match_id: "result-restart" });
    await provider.cancelMatch(dropped.match_id, dropped.revision, "Player left");

    await until(() => receiver.deliveries.length >= 2, "both results");
    const results = receiver.deliveries.map((delivery) => verifyResult(delivery.body, delivery.headers as Record<string, string>, [secret]));
    const won = results.find((result) => result.match_id === played.match_id)!;
    assert.equal(won.outcome, "completed");
    assert.equal(won.external_match_id, "result-set");
    assert.equal(won.connection_id, connection);
    assert.equal(won.winner_participant_id, participants[0]!.participant_id);
    assert.deepEqual(won.participants.map((p) => [p.participant_id, p.ember_id, p.slot, p.score]), [
      [participants[0]!.participant_id, participants[0]!.ember_id, 0, 2],
      [participants[1]!.participant_id, participants[1]!.ember_id, 1, 0],
    ]);
    const replay = results.find((result) => result.match_id === dropped.match_id)!;
    assert.equal(replay.outcome, "restart");
    assert.equal(replay.winner_participant_id, undefined);
  } finally {
    receiver.close();
    await bridge.stop();
  }
});
