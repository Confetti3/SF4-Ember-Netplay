// The after-report (docs/design/BRACKET_SITE.md, "After-report"): built
// once when a tournament is finished and kept with it, so later changes to
// how it is worked out never rewrite an event's history.
import { roundLabel, type BracketSet } from "./bracket.ts";
import { stats, type PlayerStats, type Stats } from "./live.ts";
import type { Tournament } from "./tournament.ts";

export interface PathStep {
  set: string;
  round: string;
  opponent: string;
  won: boolean;
  /** Games won and lost, the player's first. */
  score: [number, number];
  /** Each game's winner, by name, when the connector said. */
  games: string[] | null;
  ember_match: string | null;
}

export interface ReportPlayer extends Omit<PlayerStats, "characters" | "rounds_won" | "perfects"> {
  path: PathStep[];
  /** Null when none of the player's games had facts: unknown, not none. */
  characters: Record<string, number> | null;
  rounds_won: number | null;
  perfects: number | null;
  /** Sets won after losing the first game. */
  comebacks: number | null;
}

export interface Report {
  title: string;
  finished_at: string;
  games_to_win: number;
  podium: { place: number; name: string; discord: string }[];
  players: ReportPlayer[];
  upsets: Stats["upsets"];
  longest_set: Stats["longest_set"];
  shortest_set: { set: string; games: number } | null;
  /** Most played first; null when no game had facts. */
  characters: Record<string, number> | null;
  perfects: number | null;
  time_outs: number | null;
  final: PathStep | null;
  games: number;
  games_with_facts: number;
}

export function buildReport(tournament: Tournament, at: string): Report {
  const numbers = stats(tournament);
  const name = (ember: string | null) => tournament.entrants.find((each) => each.ember_id === ember)?.name ?? "?";
  const ember = (discord: string) => tournament.entrants.find((each) => each.discord === discord)!.ember_id!;
  const decided = tournament.sets.filter((set) => set.winner && set.scores);
  const step = (set: BracketSet, me: string): PathStep => {
    const slot = set.entries[0] === me ? 0 : 1;
    return {
      set: set.id,
      round: roundLabel(set.round, tournament.rounds),
      opponent: name(set.entries[1 - slot]!),
      won: set.winner === me,
      score: [set.scores![slot]!, set.scores![1 - slot]!],
      games: set.games ? set.games.map(name) : null,
      ember_match: set.ember_match ?? null,
    };
  };
  const players = numbers.players.map((player): ReportPlayer => {
    const me = ember(player.discord);
    const mine = decided.filter((set) => set.entries.includes(me)).sort((a, b) => a.round - b.round);
    const facts = mine.some((set) => set.facts?.some(Boolean));
    const told = mine.filter((set) => set.games);
    return {
      ...player,
      path: mine.map((set) => step(set, me)),
      characters: facts ? player.characters : null,
      rounds_won: facts ? player.rounds_won : null,
      perfects: facts ? player.perfects : null,
      comebacks: told.length ? told.filter((set) => set.winner === me && set.games![0] !== me).length : null,
    };
  });
  const allFacts = decided.flatMap((set) => set.facts ?? []).filter((facts) => facts !== null);
  const rounds = allFacts.flatMap((facts) => facts.rounds);
  const shortest = decided.reduce<Report["shortest_set"]>((best, set) => {
    const games = set.scores![0] + set.scores![1];
    return !best || games < best.games ? { set: set.id, games } : best;
  }, null);
  const final = decided.find((set) => set.round === tournament.rounds);
  const known = allFacts.length > 0;
  return {
    title: tournament.title,
    finished_at: at,
    games_to_win: tournament.games_to_win,
    podium: players.filter((player) => player.place !== null && player.place <= 3).map((player) => ({ place: player.place!, name: player.name, discord: player.discord })),
    players,
    upsets: numbers.upsets,
    longest_set: numbers.longest_set,
    shortest_set: shortest,
    characters: known ? Object.fromEntries(Object.entries(numbers.characters).sort((a, b) => b[1] - a[1])) : null,
    perfects: known ? rounds.filter((round) => round.perfect).length : null,
    time_outs: known ? rounds.filter((round) => round.finish === "time").length : null,
    final: final ? step(final, final.winner!) : null,
    games: numbers.games,
    games_with_facts: numbers.games_with_facts,
  };
}
