// Leagues: seasons of tournaments with standings across them
// (docs/design/BRACKET_SITE.md, "Leagues"). Pure functions; standings are
// worked out from the season's finished tournaments, never stored.
import type { FeedItem } from "./live.ts";
import type { Tournament } from "./tournament.ts";

export interface Season {
  /** Unique across leagues: a tournament names it in `season`. */
  id: string;
  title: string;
  /** Points by placement; `*` for any other finisher. */
  points: Record<string, number>;
  /** Only a player's best this many events count; null counts all. */
  best_of: number | null;
  /** When its organizers closed it and its awards were given. */
  closed_at?: string;
}

export interface League {
  id: string;
  title: string;
  /** The Discord user IDs who may manage it. */
  organizers: string[];
  seasons: Season[];
  /** Its news, such as a closed season's awards (`live.ts`). */
  feed?: FeedItem[];
}

export const DEFAULT_POINTS: Record<string, number> = { "1": 100, "2": 70, "3": 50, "5": 30, "9": 15, "*": 5 };

export interface Standing {
  discord: string;
  name: string;
  points: number;
  /** Events counted, and won. */
  events: number;
  won: number;
}

/**
 * Each entrant's placement in a finished single elimination bracket: the
 * winner 1, the final's loser 2, then one place for everyone out in the
 * same round (3, 5, 9, ...), by the round they lost.
 */
export function placements(tournament: Tournament): Map<string, number> {
  const places = new Map<string, number>();
  const discord = (ember: string) => tournament.entrants.find((entrant) => entrant.ember_id === ember)?.discord;
  for (const set of tournament.sets) {
    if (!set.winner) continue;
    const loser = set.entries.find((entry) => entry && entry !== set.winner);
    const who = loser && discord(loser);
    if (who) places.set(who, 2 ** (tournament.rounds - set.round) + 1);
    if (set.round === tournament.rounds) {
      const winner = discord(set.winner);
      if (winner) places.set(winner, 1);
    }
  }
  return places;
}

/** The season's standings: points, then events won, then name. */
export function standings(season: Season, tournaments: Tournament[]): Standing[] {
  const earned = new Map<string, { name: string; scores: number[]; won: number }>();
  for (const tournament of tournaments) {
    if (tournament.season !== season.id || !["finished", "archived"].includes(tournament.state)) continue;
    for (const [discord, place] of placements(tournament)) {
      const name = tournament.entrants.find((entrant) => entrant.discord === discord)!.name;
      const row = earned.get(discord) ?? { name, scores: [], won: 0 };
      row.name = name;
      row.scores.push(season.points[String(place)] ?? season.points["*"] ?? 0);
      if (place === 1) row.won += 1;
      earned.set(discord, row);
    }
  }
  return [...earned]
    .map(([discord, row]) => {
      const counted = row.scores.sort((a, b) => b - a).slice(0, season.best_of ?? undefined);
      return { discord, name: row.name, points: counted.reduce((sum, each) => sum + each, 0), events: counted.length, won: row.won };
    })
    .sort((a, b) => b.points - a.points || b.won - a.won || a.name.localeCompare(b.name));
}
