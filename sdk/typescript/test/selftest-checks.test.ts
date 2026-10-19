// The tournament self-test coordinator's decisions (tools/selftest-checks.ts).
import assert from "node:assert/strict";
import { test } from "node:test";

import { parseResult, RESULT_TYPE, TestPlayer } from "../src/index.ts";
import { claimToApprove, finishedProblems, parseIdentityLine } from "../tools/selftest-checks.ts";

const PLAYER1 = new TestPlayer({ origin: "http://127.0.0.1:1", seed: Buffer.alloc(32, 1) });
const PLAYER2 = new TestPlayer({ origin: "http://127.0.0.1:1", seed: Buffer.alloc(32, 2) });
const BUILD = "ab".repeat(32);
const MATCH = "emt_0b9c1e3a-5d27-4f58-8a41-6c3e9d7f2b10";
const OTHER = "emt_1d2c3b4a-5d27-4f58-8a41-6c3e9d7f2b10";

test("a game's ember-id line is its Ember ID and build, whole", () => {
  assert.deepEqual(parseIdentityLine(`${PLAYER1.emberId} ${BUILD}\r`), { emberId: PLAYER1.emberId, build: BUILD });
  assert.equal(parseIdentityLine(""), null);
  assert.equal(parseIdentityLine(PLAYER1.emberId), null);
  assert.equal(parseIdentityLine(`${PLAYER1.emberId} ${BUILD.slice(1)}`), null);
  assert.equal(parseIdentityLine(`${PLAYER1.emberId} ${BUILD.toUpperCase()}`), null);
  assert.equal(parseIdentityLine(`not-an-id ${BUILD}`), null);
  assert.equal(parseIdentityLine(`${PLAYER1.emberId} ${BUILD} extra`), null);
});

test("only the game's own pending claim, with the fingerprint it showed, is approved", () => {
  const game = { emberId: PLAYER1.emberId, build: BUILD };
  const claim = { claim_id: "clm_1", ember_id: PLAYER1.emberId, fingerprint: PLAYER1.fingerprint, state: "pending", consented_at: 1 };
  const intent = (change: Record<string, unknown> | null) => ({ intent_id: "lki_1", state: "claim_pending", claim: change === null ? null : { ...claim, ...change } }) as never;
  assert.equal(claimToApprove(intent({}), game, PLAYER1.fingerprint), "clm_1");
  assert.throws(() => claimToApprove(intent(null), game, PLAYER1.fingerprint), /no claim/);
  assert.throws(() => claimToApprove(intent({ state: "rejected" }), game, PLAYER1.fingerprint), /not pending/);
  assert.throws(() => claimToApprove(intent({ ember_id: PLAYER2.emberId }), game, PLAYER1.fingerprint), /not the game's/);
  assert.throws(() => claimToApprove(intent({}), game, PLAYER2.fingerprint), /fingerprint/);
  assert.throws(() => claimToApprove(intent({ claim_id: "" }), game, PLAYER1.fingerprint), /no ID/);
});

test("a finished match passes only when the bridge and its signed result both name the winner", () => {
  const winner = { participantId: "par_1", emberId: PLAYER1.emberId };
  const match = (state: string, wins: [number, number]) => ({
    match_id: MATCH,
    state,
    scores: [
      { ember_id: PLAYER1.emberId, wins: wins[0] },
      { ember_id: PLAYER2.emberId, wins: wins[1] },
    ],
  });
  const result = (outcome: "completed" | "restart", winnerId = "par_1", matchId = MATCH) =>
    parseResult({
      type: RESULT_TYPE,
      bridge_id: "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11",
      connection_id: "mock-local",
      match_id: matchId,
      external_match_id: "selftest-1",
      outcome,
      revision: "4",
      participants: [
        { participant_id: "par_1", ember_id: PLAYER1.emberId, slot: 0, score: outcome === "completed" ? 1 : 0 },
        { participant_id: "par_2", ember_id: PLAYER2.emberId, slot: 1, score: 0 },
      ],
      ...(outcome === "completed" ? { winner_participant_id: winnerId } : {}),
    });

  assert.deepEqual(finishedProblems(match("completed", [1, 0]), result("completed"), winner), []);
  assert.match(finishedProblems(match("in_progress", [0, 0]), null, winner).join("\n"), /not completed[\s\S]*ahead[\s\S]*no signed result/);
  assert.match(finishedProblems(match("completed", [0, 1]), result("completed", "par_2"), winner).join("\n"), /ahead[\s\S]*winner is par_2/);
  assert.match(finishedProblems(match("completed", [1, 0]), result("restart"), winner).join("\n"), /says restart/);
  assert.match(finishedProblems(match("completed", [1, 0]), result("completed", "par_1", OTHER), winner).join("\n"), /is for emt_1d2c/);
  assert.match(finishedProblems({ ...match("completed", [1, 0]), scores: [] }, result("completed"), winner).join("\n"), /ahead/);
});
