// Runs the whole provider flow against a bridge with two stand-in players:
// link both, create a first-to-2 match, play or record its games, and read
// the events.
//
//   EMBER_ORIGIN=https://bridge.embernetplay.link \
//   EMBER_CONNECTION=<your connection id> \
//   EMBER_PROVIDER_TOKEN=... EMBER_ORGANIZER_TOKEN=... \
//   node tools/walkthrough.ts [players|organizer]
//
// `players` (the default) plays the match the way Ember does: the test
// players claim it, one hosts a pretend room, and both report every game.
// `organizer` records each game with the organizer credential instead.
// Each run uses fresh test identities and subjects, so it can be repeated.
import { randomUUID } from "node:crypto";

import { BridgeClient, TestPlayer, type Participant, type Permit } from "../src/index.ts";

function env(name: string): string {
  const value = process.env[name];
  if (!value) {
    console.error(`Set ${name}.`);
    process.exit(2);
  }
  return value;
}

const mode = process.argv[2] ?? "players";
if (mode !== "players" && mode !== "organizer") {
  console.error("usage: node tools/walkthrough.ts [players|organizer]");
  process.exit(2);
}
const origin = env("EMBER_ORIGIN");
const connection = env("EMBER_CONNECTION");
const provider = new BridgeClient({ origin, credential: env("EMBER_PROVIDER_TOKEN") });
const organizer = new BridgeClient({ origin, credential: env("EMBER_ORGANIZER_TOKEN") });
const run = randomUUID().slice(0, 8);

const profile = (await (await fetch(`${origin}/.well-known/ember-bridge.json`)).json()) as { bridge_id: string; display_name: string };
console.log(`bridge ${profile.bridge_id} (${profile.display_name})`);
// Skip to the end of the existing events, so only this run's are printed.
let cursor = "0";
for (;;) {
  const page = await provider.listEvents(cursor, 100);
  if (page.events.length === 0) break;
  cursor = page.nextCursor;
}

// 1. Link two players. Your service would show the code to its signed-in
// user, who types it into Ember; the test player stands in for the game.
const players: TestPlayer[] = [];
const participants: Participant[] = [];
for (const slot of [0, 1] as const) {
  const subject = `walkthrough-${run}-${slot}`;
  const intent = await provider.createLinkIntent(subject, `Walkthrough ${slot + 1}`);
  const player = new TestPlayer({ origin });
  const claim = await player.claimLink(intent.code, connection);
  // Show claim.fingerprint to the user next to the one Ember shows, then
  // approve that exact claim once they confirm.
  const pending = await provider.getLinkIntent(intent.intent_id);
  const link = await provider.approveLinkClaim(intent.intent_id, { claimId: claim.claim_id!, emberId: claim.ember_id, subject });
  console.log(`linked ${subject} -> ${claim.ember_id} (${claim.fingerprint}, intent was ${pending.state})`);
  players.push(player);
  participants.push({ participant_id: link.participant_id as string, ember_id: claim.ember_id, slot });
}
const resolved = await provider.resolvePlayers(participants.map((_, slot) => `walkthrough-${run}-${slot}`));
console.log(`resolve: ${resolved.map((player) => `${player.subject}=${player.linked ? player.ember_id : "unlinked"}`).join(", ")}`);

// 2. A first-to-2 set. Retrying with the same idempotency key, or the same
// external_match_id, returns the same match.
const match = await provider.createMatch(
  {
    external_match_id: `walkthrough-${run}`,
    participants: participants as [Participant, Participant],
    games_to_win: 2,
    required_build_id: "walkthrough",
    native_rules_profile: mode === "players" ? "ember-room-v1" : "organizer-reported-v1",
    metadata: { round_label: "Walkthrough" },
  },
  `walkthrough-${run}`,
);
console.log(`match ${match.match_id} ${match.state}: ${mode === "players" ? "the players report" : "the organizer records"} the games`);

// 3. The games: slot 0 wins, slot 1 wins, slot 0 wins the set.
if (mode === "players") {
  const [host, guest] = players as [TestPlayer, TestPlayer];
  const lease = await host.claimMatch(match.match_id);
  if (lease.role !== "host") throw new Error(`expected to host, got ${lease.role}`);
  await guest.claimMatch(match.match_id);
  await host.publishRoom(match.match_id, { leaseId: lease.lease_id, fence: lease.fence });
  const joined = await guest.claimMatch(match.match_id);
  if (joined.role !== "room" || !joined.binding) throw new Error("the room has no binding yet");
  const binding = joined.binding.binding;
  console.log(`room ${binding.room_id} bound (revision ${binding.binding_revision})`);
  let generation = 0;
  for (const result of ["p1_win", "p2_win", "p1_win"] as const) {
    generation += 1;
    await host.prepareGame(match.match_id, binding, generation);
    const answer = await guest.prepareGame(match.match_id, binding, generation);
    if (answer.state !== "permitted") throw new Error("no permit");
    const permit: Permit = answer.permit.permit;
    await host.submitReport(match.match_id, host.report(permit, result));
    const receipt = await guest.submitReport(match.match_id, guest.report(permit, result));
    console.log(`game ${generation} (${result}): ${receipt.attempt_state}, match ${receipt.match_state}`);
  }
} else {
  let revision = match.revision;
  for (const winnerSlot of [0, 1, 0] as const) {
    const after = await organizer.recordGame(match.match_id, { winnerSlot }, revision, "walkthrough");
    revision = after.revision as string;
    console.log(`game to slot ${winnerSlot}: ${after.state}`);
  }
}
const finished = await provider.getMatch(match.match_id);
const scores = finished.scores as { wins: number }[];
console.log(`final: ${finished.state}, ${scores.map((score) => score.wins).join("-")}`);

// 4. Everything above is an event. Webhooks and SSE carry the same ones.
const page = await provider.listEvents(cursor, 100);
for (const event of page.events) console.log(`event ${event.type}`);
const record = await provider.getPlayerRecord(participants[0]!.ember_id);
console.log(`record for slot 0: ${JSON.stringify(record.sets)}`);
