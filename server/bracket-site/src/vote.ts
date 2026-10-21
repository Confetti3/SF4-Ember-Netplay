// A finished tournament's public vote (docs/design/BRACKET_SITE.md,
// "Public vote"): best player, best combo, best moment, one vote per
// Discord account per category, the counts hidden until it closes.
import { Conflict, Invalid } from "./bracket.ts";
import type { Award } from "./awards.ts";
import { TITLES } from "./awards.ts";
import type { Tournament } from "./tournament.ts";

export const CATEGORIES = ["best_player", "best_combo", "best_moment"] as const;
export type Category = (typeof CATEGORIES)[number];

/** A Discord account must be older than this to vote. */
const MIN_ACCOUNT_AGE_MS = 7 * 86400_000;
const DISCORD_EPOCH_MS = 1420070400000;
/** Choices a category can hold: Discord's select menus take 25. */
const MAX_NOMINEES = 25;

export interface Nomination {
  id: string;
  category: Category;
  /** The player it is for: who gets the award if it wins. */
  player: string;
  title: string;
  /** What it points at: a clip (`c1`), a game (`<set>:<crc>`), or nothing for a player. */
  ref: string | null;
  /** Who suggested it; accepted once an organizer says so. */
  by: string;
  accepted: boolean;
}

export interface Vote {
  opened_at: string;
  closes_at: string;
  closed_at: string | null;
  nominations: Nomination[];
  /** By category, each voter's nomination. */
  ballots: Record<Category, Record<string, string>>;
  /** Once closed: each category's counts, winners first. */
  results: Record<Category, { nomination: string; votes: number }[]> | null;
}

/** When the Discord account was made, from its ID (a snowflake). */
export function accountCreated(discord: string): number {
  return Number(BigInt(discord) >> 22n) + DISCORD_EPOCH_MS;
}

export function open(tournament: Tournament, now: Date, hours: number): Vote {
  if (tournament.state !== "finished" && tournament.state !== "archived") throw new Conflict("a vote opens once the tournament is finished");
  if (tournament.vote) throw new Conflict("its vote is already open or done");
  tournament.vote = {
    opened_at: now.toISOString(),
    closes_at: new Date(now.getTime() + hours * 3600_000).toISOString(),
    closed_at: null,
    nominations: [],
    ballots: { best_player: {}, best_combo: {}, best_moment: {} },
    results: null,
  };
  return tournament.vote;
}

/** A nomination; an organizer's is accepted at once, anyone else's waits for one. */
export function nominate(tournament: Tournament, entry: Omit<Nomination, "id" | "accepted">, organizer: boolean): Nomination {
  const vote = running(tournament);
  if (!CATEGORIES.includes(entry.category)) throw new Invalid(`category is one of ${CATEGORIES.join(", ")}`);
  if (!tournament.entrants.some((entrant) => entrant.discord === entry.player)) throw new Invalid("player is one of its entrants");
  if (entry.category !== "best_player" && !entry.ref) throw new Invalid("a combo or a moment names a clip or a game");
  const same = vote.nominations.find((each) => each.category === entry.category && each.player === entry.player && each.ref === entry.ref);
  if (same) throw new Conflict("already nominated");
  if (vote.nominations.filter((each) => each.category === entry.category).length >= MAX_NOMINEES) throw new Conflict("this category is full");
  const nomination = { ...entry, id: `n${vote.nominations.length + 1}`, accepted: organizer };
  vote.nominations.push(nomination);
  return nomination;
}

export function accept(tournament: Tournament, id: string): void {
  const nomination = running(tournament).nominations.find((each) => each.id === id) ?? fail(`no nomination ${id}`);
  nomination.accepted = true;
}

/** `voter`'s vote in `category`; a later one replaces it until the vote closes. */
export function cast(tournament: Tournament, category: Category, voter: string, nomination: string, now: Date): void {
  const vote = running(tournament, now);
  if (!CATEGORIES.includes(category)) throw new Invalid(`category is one of ${CATEGORIES.join(", ")}`);
  if (accountCreated(voter) > now.getTime() - MIN_ACCOUNT_AGE_MS) throw new Conflict("your Discord account is too new to vote");
  const choice = vote.nominations.find((each) => each.id === nomination && each.category === category && each.accepted) ?? fail(`no nomination ${nomination} in ${category}`);
  vote.ballots[category][voter] = choice.id;
}

/** An organizer takes a vote out. */
export function voidVote(tournament: Tournament, category: Category, voter: string): void {
  const vote = running(tournament);
  if (!(voter in (vote.ballots[category] ?? {}))) throw new Invalid(`${voter} has no vote in ${category}`);
  delete vote.ballots[category][voter];
}

/**
 * Closes the vote, once its time is up or when an organizer says so: the
 * counts, and an award for each category's winner (all of them on a tie).
 */
export function close(tournament: Tournament, now: Date): Award[] {
  const vote = tournament.vote;
  if (!vote || vote.closed_at) return [];
  vote.closed_at = now.toISOString();
  vote.results = { best_player: [], best_combo: [], best_moment: [] };
  const awards: Award[] = [];
  for (const category of CATEGORIES) {
    const counts = new Map<string, number>();
    for (const choice of Object.values(vote.ballots[category])) counts.set(choice, (counts.get(choice) ?? 0) + 1);
    const ranked = [...counts].map(([nomination, votes]) => ({ nomination, votes })).sort((a, b) => b.votes - a.votes);
    vote.results[category] = ranked;
    for (const winner of ranked.filter((each) => each.votes === ranked[0]?.votes)) {
      const nomination = vote.nominations.find((each) => each.id === winner.nomination)!;
      const name = tournament.entrants.find((entrant) => entrant.discord === nomination.player)?.name ?? nomination.player;
      awards.push({ discord: nomination.player, name, kind: category, title: TITLES[category], tournament: tournament.id, season: tournament.season, of: tournament.title, given_at: vote.closed_at, detail: `${nomination.title}, ${winner.votes} votes` });
    }
  }
  return awards;
}

/** Whether its time is up and it is still open. */
export function due(tournament: Tournament, now: Date): boolean {
  return !!tournament.vote && !tournament.vote.closed_at && Date.parse(tournament.vote.closes_at) <= now.getTime();
}

/** The vote as anyone may see it: no ballots, and counts only once closed. */
export function shown(vote: Vote, voter: string | null): object {
  const { ballots, ...rest } = vote;
  const mine = voter ? Object.fromEntries(CATEGORIES.map((category) => [category, ballots[category][voter] ?? null])) : null;
  return { ...rest, nominations: vote.nominations, voters: Object.fromEntries(CATEGORIES.map((category) => [category, Object.keys(ballots[category]).length])), mine };
}

function running(tournament: Tournament, now?: Date): Vote {
  const vote = tournament.vote ?? fail("no vote is open");
  if (vote.closed_at || (now && Date.parse(vote.closes_at) <= now.getTime())) throw new Conflict("the vote is closed");
  return vote;
}

function fail(message: string): never {
  throw new Invalid(message);
}
