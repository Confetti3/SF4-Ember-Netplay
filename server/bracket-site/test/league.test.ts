import assert from "node:assert/strict";
import { test } from "node:test";

import { report } from "../src/bracket.ts";
import { DEFAULT_POINTS, placements, standings, type Season } from "../src/league.ts";
import { enter, move, type Tournament } from "../src/tournament.ts";

const AT = "2026-10-21T20:00:00.000Z";

/** A finished tournament of `count` players in season `season`, the lower seed winning every set. */
function played(id: string, count: number, season = "s1", order = (n: number) => n): Tournament {
  const t: Tournament = { id, title: id, games_to_win: 2, check_in: false, cap: null, starts_at: null, season, organizers: [], state: "sign_ups", entrants: [], rounds: 0, sets: [], log: [] };
  for (let n = 1; n <= count; n++) enter(t, { discord: `d${order(n)}`, name: `P${order(n)}`, ember_id: `e${order(n)}`, checked_in: false }, false);
  move(t, "start", "o", AT);
  const seed = (ember: string) => t.entrants.findIndex((entrant) => entrant.ember_id === ember);
  for (let open = t.sets.filter((set) => set.entries[0] && set.entries[1] && !set.winner); open.length; open = t.sets.filter((set) => set.entries[0] && set.entries[1] && !set.winner)) {
    for (const set of open) {
      const [a, b] = set.entries as [string, string];
      const winner = seed(a) < seed(b) ? a : b;
      report(t.sets, set.id, { winner, scores: { [winner]: 2, [winner === a ? b : a]: 0 }, key: null });
    }
  }
  t.state = "finished";
  return t;
}

test("placements: 1, 2, then everyone out in a round together", () => {
  const places = placements(played("t", 6));
  assert.deepEqual(Object.fromEntries(places), { d1: 1, d2: 2, d3: 3, d4: 3, d5: 5, d6: 5 });
  assert.deepEqual(Object.fromEntries(placements(played("two", 2))), { d1: 1, d2: 2 });
});

test("standings add each finished event of the season, best of N, by points then wins", () => {
  const season: Season = { id: "s1", title: "Season 1", points: DEFAULT_POINTS, best_of: 2 };
  // P2 wins the second event (seeded first there), P1 the first and third.
  const events = [played("a", 4), played("b", 4, "s1", (n) => [2, 1, 3, 4][n - 1]!), played("c", 2), played("other", 4, "s2")];
  const unfinished = played("d", 2);
  unfinished.state = "live";
  const rows = standings(season, [...events, unfinished]);
  assert.deepEqual(
    rows.map((row) => [row.name, row.points, row.events, row.won]),
    [
      ["P1", 200, 2, 2],
      ["P2", 170, 2, 1],
      ["P3", 100, 2, 0],
      ["P4", 100, 2, 0],
    ],
  );
  assert.equal(standings({ ...season, best_of: null }, events)[0]!.points, 270);
  assert.equal(standings({ ...season, points: { "1": 3 } }, events).at(-1)!.points, 0, "no * gives nothing");
});
