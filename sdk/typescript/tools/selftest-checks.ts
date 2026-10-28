// What the tournament self-test's coordinator (tools/selftest-bridge.ts)
// decides from what the games and the bridge say, apart from any I/O, so it
// can be tested without either. docs/design/TOURNAMENT_SELFTEST.md.
import { isEmberId, type MatchResult } from "../src/index.ts";
import { type Body, isBody } from "./body.ts";

/** A game's `ember-id` file: its Ember ID and the build it runs. */
export interface GameIdentity {
  emberId: string;
  build: string;
}

/** The game's `ember-id` line, "<ember id> <build>", or null until it is whole. */
export function parseIdentityLine(line: string): GameIdentity | null {
  const [emberId, build, ...rest] = line.trim().split(" ");
  if (!emberId || !build || rest.length > 0 || !isEmberId(emberId) || !/^[0-9a-f]{64}$/.test(build)) return null;
  return { emberId, build };
}

/**
 * The claim to approve on a link intent: the one pending claim, by the Ember
 * ID that game reported, with the fingerprint the game showed. Anything else
 * is somebody else's claim, which a platform's user would reject.
 */
export function claimToApprove(intent: Body, game: GameIdentity, fingerprint: string): string {
  const claim = intent.claim;
  if (!isBody(claim)) throw new Error("the link intent has no claim");
  if (claim.state !== "pending") throw new Error(`the claim is ${String(claim.state)}, not pending`);
  if (claim.ember_id !== game.emberId) throw new Error(`the claim is by ${String(claim.ember_id)}, not the game's ${game.emberId}`);
  if (claim.fingerprint !== fingerprint) throw new Error(`the claim's fingerprint ${String(claim.fingerprint)} is not the game's ${fingerprint}`);
  if (typeof claim.claim_id !== "string" || claim.claim_id === "") throw new Error("the claim has no ID");
  return claim.claim_id;
}

/** The games' winner as the bridge holds it: its Ember ID and its participant ID in the match. */
export interface Winner {
  participantId: string;
  emberId: string;
}

/**
 * What is wrong with a match the bridge should hold as finished, empty when
 * nothing is: it completed with `winner` ahead.
 */
export function completedProblems(match: Body, winner: { emberId: string }): string[] {
  const problems: string[] = [];
  if (match.state !== "completed") problems.push(`the match is ${String(match.state)}, not completed`);
  const scores = (Array.isArray(match.scores) ? match.scores : []).filter(isBody);
  const won = scores.find((score) => score.ember_id === winner.emberId)?.wins;
  const lost = scores.find((score) => score.ember_id !== winner.emberId)?.wins;
  if (scores.length !== 2 || typeof won !== "number" || typeof lost !== "number" || won <= lost) {
    problems.push(`the scores ${JSON.stringify(match.scores)} do not put ${winner.emberId} ahead`);
  }
  return problems;
}

/**
 * What is wrong with a finished match, empty when nothing is: the bridge
 * says it completed with `winner` ahead, and the signed result it delivered
 * says the same.
 */
export function finishedProblems(match: Body, result: MatchResult | null, winner: Winner): string[] {
  const problems = completedProblems(match, winner);
  if (!result) {
    problems.push("no signed result was delivered");
  } else if (result.match_id !== match.match_id) {
    problems.push(`the signed result is for ${result.match_id}, not ${String(match.match_id)}`);
  } else if (result.outcome !== "completed") {
    problems.push(`the signed result says ${result.outcome}, not completed`);
  } else if (result.winner_participant_id !== winner.participantId) {
    problems.push(`the signed result's winner is ${result.winner_participant_id}, not ${winner.participantId}`);
  }
  return problems;
}

/**
 * What is wrong with the match a bracket connector made for the two games,
 * empty when nothing is: it is on the connector's set (`external_match_id`
 * starts with `prefix`), and has exactly these two Ember IDs.
 */
export function connectorMatchProblems(match: Body, prefix: string, emberIds: [string, string]): string[] {
  const problems: string[] = [];
  const external = match.external_match_id;
  if (typeof external !== "string" || !external.startsWith(prefix)) problems.push(`the match's external ID ${JSON.stringify(external)} is not for ${prefix}`);
  const participants = (Array.isArray(match.participants) ? match.participants : []).filter(isBody);
  const present = participants.map((participant) => String(participant.ember_id)).sort();
  if (participants.length !== 2 || present.join(" ") !== [...emberIds].sort().join(" ")) {
    problems.push("the match is not between the two games' Ember IDs");
  }
  return problems;
}

/**
 * Whether the bridge delivered the signed result to the connection's
 * results URL and was answered: the connector verifies the signature before
 * it answers, so `delivered` means a verified result arrived.
 */
export function deliveryProblems(match: Body): string[] {
  return match.provider_delivery_state === "delivered" ? [] : [`the result's delivery is ${String(match.provider_delivery_state)}, not delivered`];
}

/** A game's `site-account` file: the account its sign-in connected. */
export interface SiteAccount {
  userId: string;
  name: string;
}

/** The game's `site-account` line, "<user ID> <name>", or null until it is whole. */
export function parseAccountLine(line: string): SiteAccount | null {
  const match = /^(\S+) (\S.*)$/.exec(line.trim());
  return match ? { userId: match[1]!, name: match[2]! } : null;
}

/** What is wrong with the two games' accounts: one account on both PCs links one player twice. */
export function accountProblems(host: SiteAccount, guest: SiteAccount): string[] {
  return host.userId === guest.userId ? ["both games connected the same account; each PC signs in as a different player"] : [];
}
