// Replays from the report (docs/design/BRACKET_SITE.md, "Replays from the
// report"): a set's pack and games from the bridge, which keeps them, and
// clips, which are only where to look: the game cuts them.
import type { BracketSet } from "./bracket.ts";
import type { Tournament } from "./tournament.ts";

export interface Clip {
  id: string;
  set: string;
  crc: string;
  from_frame: string;
  to_frame: string;
  title: string;
}

/** What the bridge's pack header says of each game. */
interface Header {
  games: { crc: string; saved_at: number; size: number }[];
}

/** A link the game opens to play `crc` of the match from one frame to another. */
export function gameLink(bridge: string, match: string, crc: string, from?: string, to?: string): string {
  const query = new URLSearchParams({ bridge, match, crc, ...(from && to ? { from, to } : {}) });
  return `ember://replay?${query}`;
}

/**
 * A set's replays: the pack, each game with its file and its rounds, each
 * round a clip to open in the game. Rounds come from the games' facts,
 * matched to the replays by order only when both count the same games
 * (a drawn game is replayed, and has a replay but no place in the score).
 */
export async function setReplays(bridge: string, set: BracketSet, fetcher: typeof fetch = fetch): Promise<object | undefined> {
  if (!set.ember_match) return undefined;
  const base = `${bridge}/v1/public/matches/${encodeURIComponent(set.ember_match)}`;
  const answer = await fetcher(`${base}/replays`, { signal: AbortSignal.timeout(10_000) });
  if (answer.status === 404) return undefined;
  if (!answer.ok) throw new Error(`the bridge answered ${answer.status}`);
  const header = (await answer.json()) as Header;
  const facts = set.facts && set.facts.length === header.games.length ? set.facts : [];
  return {
    set: set.id,
    pack: `${base}/replays.pack`,
    games: header.games.map((game, index) => ({
      crc: game.crc,
      file: `${base}/replays/${game.crc}`,
      open: gameLink(bridge, set.ember_match!, game.crc),
      facts: facts[index] ?? null,
      rounds: (facts[index]?.rounds ?? []).map((round, n) => ({
        round: n + 1,
        ...round,
        open: gameLink(bridge, set.ember_match!, game.crc, round.from_frame, round.to_frame),
      })),
    })),
  };
}

/** A clip as the game is handed it. */
export function manifest(bridge: string, tournament: Tournament, clip: Clip): object {
  const set = tournament.sets.find((each) => each.id === clip.set)!;
  return { ...clip, match: set.ember_match, open: gameLink(bridge, set.ember_match!, clip.crc, clip.from_frame, clip.to_frame) };
}
