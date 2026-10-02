// Runs the whole provider flow against a bridge with two stand-in players:
// link both, create a first-to-2 match, record its games, and read the events.
//
//   EMBER_ORIGIN=https://bridge.embernetplay.link \
//   EMBER_CONNECTION=<your connection id> \
//   EMBER_PROVIDER_TOKEN=... EMBER_ORGANIZER_TOKEN=... \
//   node tools/walkthrough.ts
//
// Each run uses fresh test identities and subjects, so it can be repeated.
import { randomUUID } from "node:crypto";

import { BridgeClient, TestPlayer, type Participant } from "../src/index.ts";

function env(name: string): string {
  const value = process.env[name];
  if (!value) {
    console.error(`Set ${name}.`);
    process.exit(2);
  }
  return value;
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
    metadata: { round_label: "Walkthrough" },
  },
  `walkthrough-${run}`,
);
console.log(`match ${match.match_id} ${match.state}`);

// 3. The organizer records games; the second win for slot 0 completes it.
let revision = match.revision;
for (const winnerSlot of [0, 1, 0] as const) {
  const after = await organizer.recordGame(match.match_id, { winnerSlot }, revision, "walkthrough");
  revision = after.revision as string;
  console.log(`game to slot ${winnerSlot}: ${after.state}`);
}
const finished = await provider.getMatch(match.match_id);
const scores = finished.scores as { wins: number }[];
console.log(`final: ${finished.state}, ${scores.map((score) => score.wins).join("-")}`);

// 4. Everything above is an event. Webhooks and SSE carry the same ones.
const page = await provider.listEvents(cursor, 100);
for (const event of page.events) console.log(`event ${event.type}`);
const record = await provider.getPlayerRecord(participants[0]!.ember_id);
console.log(`record for slot 0: ${JSON.stringify(record.sets)}`);
