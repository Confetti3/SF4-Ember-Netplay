// A single elimination bracket: seeding with byes, results and who moves on.
// Pure functions over a bracket's sets, each entry an entrant's Ember ID.
import type { GameFacts } from "./live.ts";

export interface BracketSet {
  /** `r<round>-<position>`, from 1. A round 1 set with a bye is never made. */
  id: string;
  round: number;
  /** Where in its round, from 0: its winner goes to `position / 2` of the next. */
  position: number;
  entries: [string | null, string | null];
  winner: string | null;
  /** Games won, in `entries` order. */
  scores: [number, number] | null;
  underway: boolean;
  /** The Idempotency-Key the result came with; null when an organizer entered it. */
  key: string | null;
  /** From the connector's report: each game's winning entry in order, its facts, and the Ember match. */
  games?: string[];
  facts?: (GameFacts | null)[];
  ember_match?: string;
}

export type Status = "pending" | "open" | "decided";

export function status(set: Pick<BracketSet, "winner" | "entries">): Status {
  if (set.winner) return "decided";
  return set.entries[0] && set.entries[1] ? "open" : "pending";
}

export function roundLabel(round: number, rounds: number): string {
  return ["Final", "Semifinals", "Quarterfinals"][rounds - round] ?? `Round ${round}`;
}

/** Seed numbers (from 1) in bracket order, so seed 1 and 2 meet last: 4 is 1, 4, 2, 3. */
export function seedOrder(size: number): number[] {
  let order = [1];
  while (order.length < size) order = order.flatMap((seed) => [seed, order.length * 2 + 1 - seed]);
  return order;
}

/** The sets of a bracket for `ids` in seed order. Byes go to the top seeds. */
export function generate(ids: string[]): { rounds: number; sets: BracketSet[] } {
  if (ids.length < 2) throw new Error("a bracket needs at least two entrants");
  const rounds = Math.ceil(Math.log2(ids.length));
  const sets: BracketSet[] = [];
  for (let round = 2; round <= rounds; round++) {
    for (let position = 0; position < 2 ** (rounds - round); position++) sets.push(blank(round, position, [null, null]));
  }
  const order = seedOrder(2 ** rounds).map((seed) => ids[seed - 1] ?? null);
  for (let position = 0; position < order.length / 2; position++) {
    const pair: [string | null, string | null] = [order[position * 2]!, order[position * 2 + 1]!];
    if (pair[0] && pair[1]) sets.push(blank(1, position, pair));
    else moveOn(sets, 1, position, pair[0] ?? pair[1]!);
  }
  sets.sort((a, b) => a.round - b.round || a.position - b.position);
  return { rounds, sets };
}

function blank(round: number, position: number, entries: [string | null, string | null]): BracketSet {
  return { id: `r${round}-${position + 1}`, round, position, entries, winner: null, scores: null, underway: false, key: null };
}

/** Puts the winner of `round`'s set at `position` into the next round's set. */
function moveOn(sets: BracketSet[], round: number, position: number, winner: string): void {
  const next = sets.find((set) => set.round === round + 1 && set.position === position >> 1);
  if (next) next.entries[position & 1] = winner;
}

export class Conflict extends Error {}
export class Invalid extends Error {}

export interface Result {
  winner: string;
  /** Games won by each entry, by entry ID. */
  scores: Record<string, number>;
  key: string | null;
}

/**
 * Records a set's result and moves its winner on. A repeat of the result
 * already recorded under the same key is `"repeat"` and changes nothing; any
 * other result for a decided or pending set is a Conflict.
 */
export function report(sets: BracketSet[], setId: string, result: Result): "recorded" | "repeat" {
  const set = sets.find((set) => set.id === setId);
  if (!set) throw new Invalid(`no set ${setId}`);
  const scores = set.entries.map((entry) => (entry ? result.scores[entry] : undefined));
  if (set.winner) {
    const same = result.key !== null && result.key === set.key && result.winner === set.winner && scores.every((games, i) => games === set.scores![i]);
    if (same) return "repeat";
    throw new Conflict(`set ${setId} already has a result`);
  }
  if (status(set) !== "open") throw new Conflict(`set ${setId} does not have both players yet`);
  if (!set.entries.includes(result.winner)) throw new Invalid(`${result.winner} is not in set ${setId}`);
  if (!scores.every((games) => Number.isInteger(games) && games! >= 0)) throw new Invalid("scores need a whole number of games for each entry");
  const [mine, theirs] = set.entries[0] === result.winner ? [scores[0]!, scores[1]!] : [scores[1]!, scores[0]!];
  if (mine <= theirs) throw new Invalid("the winner must have won more games");
  set.winner = result.winner;
  set.scores = [scores[0]!, scores[1]!];
  set.key = result.key;
  moveOn(sets, set.round, set.position, result.winner);
  return "recorded";
}

/** Whether the final is decided. */
export function decided(sets: BracketSet[], rounds: number): boolean {
  return sets.some((set) => set.round === rounds && set.winner);
}
