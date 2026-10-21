// A tournament's states, entrants and the moves between them
// (docs/design/BRACKET_SITE.md, "States"). Pure functions; the store keeps
// each tournament whole.
import { Conflict, decided, generate, Invalid, type BracketSet } from "./bracket.ts";
import { stateChanged, type FeedItem } from "./live.ts";
import { buildReport, type Report } from "./report.ts";

export type State = "draft" | "planned" | "sign_ups" | "check_in" | "live" | "finished" | "archived" | "cancelled";

export interface Entrant {
  /** A Discord user ID: who the entrant is. */
  discord: string;
  name: string;
  /** The Ember ID connected to that Discord account, once the bridge finds one. */
  ember_id: string | null;
  checked_in: boolean;
}

export interface Change {
  at: string;
  /** A Discord user ID, `operator`, or `site` for a move the site made itself. */
  by: string;
  from: State;
  to: State;
  note?: string;
}

export interface Tournament {
  id: string;
  title: string;
  games_to_win: number;
  /** Whether entrants check in before the bracket is made. */
  check_in: boolean;
  /** At most this many entrants, or any number. */
  cap: number | null;
  starts_at: string | null;
  /** The league season it counts for; kept for leagues, not used yet. */
  season: string | null;
  /** The Discord user IDs who may manage it. */
  organizers: string[];
  state: State;
  entrants: Entrant[];
  rounds: number;
  /** Each entry is an entrant's Ember ID. */
  sets: BracketSet[];
  log: Change[];
  /** What happened, for the page and Discord; absent in a tournament saved before it. */
  feed?: FeedItem[];
  /** The after-report, made once when it is finished. */
  report?: Report;
}

export type Action = "plan" | "open_sign_ups" | "close_sign_ups" | "open_check_in" | "reopen_sign_ups" | "start" | "reset" | "archive" | "cancel";

/** Where each action goes from, and to. */
const MOVES: Record<Action, [State[], State]> = {
  plan: [["draft"], "planned"],
  open_sign_ups: [["planned"], "sign_ups"],
  close_sign_ups: [["sign_ups"], "planned"],
  open_check_in: [["sign_ups"], "check_in"],
  reopen_sign_ups: [["check_in"], "sign_ups"],
  start: [["sign_ups", "check_in"], "live"],
  reset: [["live"], "sign_ups"],
  archive: [["finished"], "archived"],
  cancel: [["draft", "planned", "sign_ups", "check_in", "live"], "cancelled"],
};

export const ACTIONS = Object.keys(MOVES) as Action[];

/** Before the bracket is made: entrants and settings may change. */
export function beforeLive(tournament: Tournament): boolean {
  return ["draft", "planned", "sign_ups", "check_in"].includes(tournament.state);
}

/** The actions the tournament's state allows now. */
export function actions(tournament: Tournament): Action[] {
  return ACTIONS.filter((action) => allows(tournament, action));
}

function allows(tournament: Tournament, action: Action): boolean {
  if (!MOVES[action][0].includes(tournament.state)) return false;
  switch (action) {
    case "open_check_in":
      return tournament.check_in;
    // With check-in on, the bracket is made from those checked in.
    case "start":
      return (tournament.state === "check_in") === tournament.check_in;
    case "reset":
      return !tournament.sets.some((set) => set.winner);
    default:
      return true;
  }
}

/**
 * Moves the tournament by `action`. `start` makes the bracket of the
 * entrants who can play (§ seed), in their order: shuffle them first for a
 * random seeding.
 */
export function move(tournament: Tournament, action: Action, by: string, at: string): void {
  if (!allows(tournament, action)) throw new Conflict(`${action} is not allowed while ${tournament.state}`);
  const from = tournament.state;
  let to = MOVES[action][1];
  let note: string | undefined;
  if (action === "start") {
    const { playing, dropped } = seed(tournament);
    if (playing.length < 2) throw new Conflict("a bracket needs at least two entrants who can play");
    Object.assign(tournament, generate(playing.map((entrant) => entrant.ember_id!)));
    tournament.entrants = playing;
    if (dropped.length) note = `dropped ${dropped.map((entrant) => entrant.name).join(", ")}`;
  }
  if (action === "reset") {
    if (tournament.check_in) to = "check_in";
    tournament.rounds = 0;
    tournament.sets = [];
  }
  tournament.state = to;
  tournament.log.push({ at, by, from, to, ...(note ? { note } : {}) });
  stateChanged(tournament, at);
}

/**
 * Who plays: entrants with an Ember ID, checked in when check-in is on, one
 * entry per Ember ID. The rest are dropped from the tournament at the start.
 */
export function seed(tournament: Tournament): { playing: Entrant[]; dropped: Entrant[] } {
  const seen = new Set<string>();
  const playing: Entrant[] = [];
  const dropped: Entrant[] = [];
  for (const entrant of tournament.entrants) {
    const plays = entrant.ember_id !== null && !seen.has(entrant.ember_id) && (entrant.checked_in || !tournament.check_in);
    (plays ? playing : dropped).push(entrant);
    if (plays) seen.add(entrant.ember_id!);
  }
  return { playing, dropped };
}

/** Marks the tournament finished once its final is decided. */
export function settle(tournament: Tournament, at: string): void {
  if (tournament.state === "live" && decided(tournament.sets, tournament.rounds)) {
    tournament.state = "finished";
    tournament.log.push({ at, by: "site", from: "live", to: "finished" });
    stateChanged(tournament, at);
    tournament.report = buildReport(tournament, at);
  }
}

/** Adds an entrant, or says why not. `organizer`: added by an organizer, who may also before sign-ups open. */
export function enter(tournament: Tournament, entrant: Entrant, organizer: boolean): void {
  const open = organizer ? beforeLive(tournament) : tournament.state === "sign_ups";
  if (!open) throw new Conflict(`sign-ups are not open while ${tournament.state}`);
  if (tournament.entrants.some((each) => each.discord === entrant.discord)) throw new Conflict("already entered");
  if (tournament.cap !== null && tournament.entrants.length >= tournament.cap) throw new Conflict("the tournament is full");
  tournament.entrants.push(entrant);
}

export function checkIn(tournament: Tournament, discord: string): Entrant {
  if (tournament.state !== "check_in") throw new Conflict(`check-in is not open while ${tournament.state}`);
  const entrant = tournament.entrants.find((each) => each.discord === discord) ?? fail(`${discord} has not signed up`);
  entrant.checked_in = true;
  return entrant;
}

function fail(message: string): never {
  throw new Invalid(message);
}
