// Ranks, titles and awards handed out at a tournament's finish and a
// season's close (docs/design/BRACKET_SITE.md, "Ranks, titles, emblems and
// awards"), kept on each player's Discord identity for good.
import type { Season, Standing } from "./league.ts";
import type { Report } from "./report.ts";
import type { Tournament } from "./tournament.ts";

export type Kind =
  | "champion"
  | "runner_up"
  | "top_3"
  | "top_8"
  | "giant_killer"
  | "perfectionist"
  | "marathon"
  | "iron"
  | "one_trick"
  | "mixer"
  | "season_champion"
  | "season_top_3"
  | "best_player"
  | "best_combo"
  | "best_moment";

export const TITLES: Record<Kind, string> = {
  champion: "Champion",
  runner_up: "Runner-up",
  top_3: "Top 3",
  top_8: "Top 8",
  giant_killer: "Giant killer",
  perfectionist: "Perfectionist",
  marathon: "Marathon",
  iron: "Iron",
  one_trick: "One-trick",
  mixer: "Mixer",
  season_champion: "Season champion",
  season_top_3: "Season top 3",
  best_player: "Best player (vote)",
  best_combo: "Best combo (vote)",
  best_moment: "Best moment (vote)",
};

export interface Award {
  discord: string;
  name: string;
  kind: Kind;
  title: string;
  /** What it was given for: a tournament, or a season. */
  tournament: string | null;
  season: string | null;
  /** The event's or season's title, for a profile to show. */
  of: string;
  given_at: string;
  detail: string;
}

/** The awards of a finished tournament, from its report. */
export function tournamentAwards(tournament: Tournament, report: Report): Award[] {
  const awards: Award[] = [];
  const give = (player: { discord: string; name: string }, kind: Kind, detail: string) =>
    awards.push({ discord: player.discord, name: player.name, kind, title: TITLES[kind], tournament: tournament.id, season: tournament.season, of: tournament.title, given_at: report.finished_at, detail });
  const big = tournament.entrants.length > 8;
  for (const player of report.players) {
    const place = player.place;
    if (place === 1) give(player, "champion", "1st place");
    else if (place === 2) give(player, "runner_up", "2nd place");
    else if (place === 3) give(player, "top_3", "3rd place");
    else if (place !== null && place <= 8 && big) give(player, "top_8", `${place}th place`);
  }
  const byName = (name: string) => report.players.find((player) => player.name === name);
  const upset = report.upsets[0];
  if (upset && upset.gap >= 2 && byName(upset.winner)) give(byName(upset.winner)!, "giant_killer", `beat ${upset.loser}, ${upset.gap} seeds higher`);
  const most = <T>(score: (player: Report["players"][number]) => T | null, at: (value: T) => number) => {
    const scored = report.players.filter((player) => score(player) !== null);
    const best = Math.max(0, ...scored.map((player) => at(score(player)!)));
    return { best, players: best > 0 ? scored.filter((player) => at(score(player)!) === best) : [] };
  };
  const perfects = most((player) => player.perfects, (value) => value);
  for (const player of perfects.players) give(player, "perfectionist", `${perfects.best} perfect round${perfects.best === 1 ? "" : "s"}`);
  const iron = most((player) => player.games_won + player.games_lost, (value) => value);
  for (const player of iron.players) give(player, "iron", `${iron.best} games`);
  if (report.longest_set && report.longest_set.games >= 2 * report.games_to_win - 1 && report.longest_set.games > 2) {
    const set = tournament.sets.find((each) => each.id === report.longest_set!.set)!;
    for (const entry of set.entries) {
      const player = report.players.find((each) => tournament.entrants.find((entrant) => entrant.ember_id === entry)?.discord === each.discord);
      if (player) give(player, "marathon", `a ${report.longest_set.games}-game set`);
    }
  }
  for (const player of report.players) {
    if (!player.characters) continue;
    const picks = Object.entries(player.characters);
    const games = picks.reduce((sum, [, count]) => sum + count, 0);
    if (picks.length === 1 && games >= 4) give(player, "one_trick", `${games} games as ${picks[0]![0]}`);
    if (picks.length >= 3) give(player, "mixer", `${picks.length} characters`);
  }
  return awards;
}

/** A closed season's champion and top 3, from its standings. */
export function seasonAwards(season: Season, rows: Standing[], at: string): Award[] {
  return rows.slice(0, 3).map((row, index) => {
    const kind: Kind = index === 0 ? "season_champion" : "season_top_3";
    return { discord: row.discord, name: row.name, kind, title: TITLES[kind], tournament: null, season: season.id, of: season.title, given_at: at, detail: `${row.points} points` };
  });
}
