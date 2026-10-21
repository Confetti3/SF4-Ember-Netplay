// A live tournament's feed and stats (docs/design/BRACKET_SITE.md, "Live
// stats"): what happened, in order, for the page and Discord, and the
// numbers worked out from the sets and the games' facts.
import { roundLabel, status, type BracketSet } from "./bracket.ts";
import { placements } from "./league.ts";
import type { State, Tournament } from "./tournament.ts";

/** What both fighters' games said of one game (`ember_protocol::facts`). */
export interface GameFacts {
  fighters: [{ character: string; costume: number; color: number }, { character: string; costume: number; color: number }];
  rounds: { winner: 0 | 1 | null; finish: "ko" | "super" | "ultra" | "time" | "double_ko"; perfect: boolean; from_frame: string; to_frame: string }[];
}

export interface FeedItem {
  /** From 1, in order: a reader asks for those after the last it saw. */
  n: number;
  at: string;
  kind: "state" | "set_live" | "set_done" | "upset" | "streak" | "award";
  /** The set it is about, if any. */
  set?: string;
  /** In words, names and all, as Discord and the page show it. */
  text: string;
}

/** The most items a tournament keeps; older ones fall off. */
const MAX_FEED = 500;

/** Adds an item to a tournament's feed, or a league's. */
export function push(owner: { feed?: FeedItem[] }, item: Omit<FeedItem, "n">): void {
  const feed = (owner.feed ??= []);
  feed.push({ n: (feed.at(-1)?.n ?? 0) + 1, ...item });
  if (feed.length > MAX_FEED) feed.splice(0, feed.length - MAX_FEED);
}

const STATE_TEXT: Partial<Record<State, string>> = {
  sign_ups: "Sign-ups are open.",
  check_in: "Check-in is open.",
  live: "The bracket has started.",
  finished: "The tournament is over.",
  cancelled: "The tournament was cancelled.",
};

export function stateChanged(tournament: Tournament, at: string): void {
  const text = STATE_TEXT[tournament.state];
  if (text) push(tournament, { at, kind: "state", text: `${tournament.title}: ${text}` });
}

const nameOf = (tournament: Tournament, ember: string | null) => tournament.entrants.find((each) => each.ember_id === ember)?.name ?? "?";
/** Seeds follow the entrants' order at the start, from 1. */
const seedOf = (tournament: Tournament, ember: string | null) => tournament.entrants.findIndex((each) => each.ember_id === ember) + 1;

export function setLive(tournament: Tournament, set: BracketSet, at: string): void {
  const [a, b] = set.entries;
  push(tournament, { at, kind: "set_live", set: set.id, text: `${label(tournament, set)}: ${nameOf(tournament, a)} vs ${nameOf(tournament, b)} is live.` });
}

/** A set's result, and the upset or streak it makes. */
export function setDone(tournament: Tournament, set: BracketSet, at: string): void {
  const loser = set.entries.find((entry) => entry !== set.winner) ?? null;
  const [won, lost] = set.entries[0] === set.winner ? set.scores! : [set.scores![1], set.scores![0]];
  push(tournament, {
    at,
    kind: "set_done",
    set: set.id,
    text: `${label(tournament, set)}: ${nameOf(tournament, set.winner)} beats ${nameOf(tournament, loser)} ${won}-${lost}.`,
  });
  const gap = seedOf(tournament, set.winner) - seedOf(tournament, loser);
  if (gap > 0) {
    push(tournament, { at, kind: "upset", set: set.id, text: `Upset: seed ${seedOf(tournament, set.winner)} ${nameOf(tournament, set.winner)} knocks out seed ${seedOf(tournament, loser)}.` });
  }
  const streak = tournament.sets.filter((each) => each.winner === set.winner).length;
  if (streak >= 3) push(tournament, { at, kind: "streak", set: set.id, text: `${nameOf(tournament, set.winner)} has won ${streak} sets in a row.` });
}

function label(tournament: Tournament, set: BracketSet): string {
  return roundLabel(set.round, tournament.rounds);
}

export interface PlayerStats {
  discord: string;
  name: string;
  seed: number;
  /** Their place once they are out (or have won); null while still in. */
  place: number | null;
  sets_won: number;
  sets_lost: number;
  games_won: number;
  games_lost: number;
  /** Games played with each character, where the games' facts say. */
  characters: Record<string, number>;
  rounds_won: number;
  perfects: number;
}

export interface Stats {
  sets: { live: number; done: number; total: number };
  players: PlayerStats[];
  upsets: { set: string; winner: string; loser: string; gap: number }[];
  /** The set with the most games so far. */
  longest_set: { set: string; games: number } | null;
  characters: Record<string, number>;
  /** Games whose facts were kept, of those played. */
  games_with_facts: number;
  games: number;
}

export function stats(tournament: Tournament): Stats {
  const places = placements(tournament);
  const players = new Map<string, PlayerStats>();
  tournament.entrants.forEach((entrant, index) => {
    if (!entrant.ember_id) return;
    players.set(entrant.ember_id, {
      discord: entrant.discord,
      name: entrant.name,
      seed: index + 1,
      place: places.get(entrant.discord) ?? null,
      sets_won: 0,
      sets_lost: 0,
      games_won: 0,
      games_lost: 0,
      characters: {},
      rounds_won: 0,
      perfects: 0,
    });
  });
  const result: Stats = { sets: { live: 0, done: 0, total: tournament.sets.length }, players: [], upsets: [], longest_set: null, characters: {}, games_with_facts: 0, games: 0 };
  for (const set of tournament.sets) {
    if (status(set) === "open" && set.underway) result.sets.live += 1;
    if (!set.winner || !set.scores) continue;
    result.sets.done += 1;
    set.entries.forEach((entry, slot) => {
      const player = entry ? players.get(entry) : undefined;
      if (!player) return;
      if (entry === set.winner) player.sets_won += 1;
      else player.sets_lost += 1;
      player.games_won += set.scores![slot]!;
      player.games_lost += set.scores![1 - slot]!;
    });
    const games = set.scores[0] + set.scores[1];
    result.games += games;
    if (!result.longest_set || games > result.longest_set.games) result.longest_set = { set: set.id, games };
    const loser = set.entries.find((entry) => entry !== set.winner) ?? null;
    const gap = seedOf(tournament, set.winner) - seedOf(tournament, loser);
    if (gap > 0) result.upsets.push({ set: set.id, winner: nameOf(tournament, set.winner), loser: nameOf(tournament, loser), gap });
    for (const facts of set.facts ?? []) {
      if (!facts) continue;
      result.games_with_facts += 1;
      facts.fighters.forEach((fighter, slot) => {
        const player = players.get(set.entries[slot]!);
        result.characters[fighter.character] = (result.characters[fighter.character] ?? 0) + 1;
        if (player) player.characters[fighter.character] = (player.characters[fighter.character] ?? 0) + 1;
      });
      for (const round of facts.rounds) {
        const player = round.winner === null ? undefined : players.get(set.entries[round.winner]!);
        if (!player) continue;
        player.rounds_won += 1;
        if (round.perfect) player.perfects += 1;
      }
    }
  }
  result.upsets.sort((a, b) => b.gap - a.gap);
  result.players = [...players.values()].sort((a, b) => (a.place ?? 0) - (b.place ?? 0) || b.sets_won - a.sets_won || a.seed - b.seed);
  return result;
}
