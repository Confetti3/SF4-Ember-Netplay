// The site's routes over HTTP, with a stand-in for the bridge's Discord
// lookup. The connector's side sends what ember-brackets sends
// (src/service.rs): its headers, `/sets?state=open`, `/underway` with `{}`,
// and reports with an Idempotency-Key, repeated as they are on a lost answer.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";

import { BridgeError } from "../../../sdk/typescript/src/index.ts";
import { report } from "../src/bracket.ts";
import { bracketSite, type Actor } from "../src/server.ts";
import { Store } from "../src/store.ts";
import { enter, move, type Tournament } from "../src/tournament.ts";

const OPERATOR = "operator-test-token";
const CONNECTOR = "connector-test-token";
const BOT = "bot-test-token";
const emberId = (n: number) => `emb1_${"abcdefg"[n - 1]!.repeat(51)}a`;
const discordId = (n: number) => `10000000000000000${String(n).padStart(2, "0")}`;
/** An organizer the bridge let sign in (Discord 01), and a player's token (02). */
const TOKENS: Record<string, Actor> = {
  "organizer-1": { discord: discordId(1), operator: false, organizer: true },
  "player-2": { discord: discordId(2), operator: false, organizer: false },
};
/** Discord accounts 01 to 04 are connected in Ember; 05 once `linkFive`; asking for 13 alone makes the bridge fail. */
let linkFive = false;
const asked: string[][] = [];
const bridge = {
  async lookupPlayers(ids: string[]) {
    asked.push(ids);
    if (ids.length === 1 && ids[0] === discordId(13)) throw new BridgeError(503, "unavailable", "down", true);
    return ids
      .map((id) => Number(id.slice(-2)))
      .filter((n) => n <= 4 || (n === 5 && linkFive))
      .map((n) => ({ discord_user_id: discordId(n), ember_id: emberId(n), participant_id: `prt_${n}` }));
  },
};

let dir: string;
let store: Store;
let base: string;
let close: () => void;

before(async () => {
  dir = mkdtempSync(join(tmpdir(), "bracket-site-"));
  store = new Store(join(dir, "site.sqlite3"));
  const server = bracketSite({ store, operatorToken: OPERATOR, connectorToken: CONNECTOR, botToken: BOT, tokens: (token) => TOKENS[token], bridge });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  close = () => server.close();
});

after(() => {
  close();
  store.close();
  rmSync(dir, { recursive: true, force: true });
});

async function call(method: string, path: string, options: { token?: string; body?: unknown; headers?: Record<string, string> } = {}) {
  const response = await fetch(`${base}${path}`, {
    method,
    headers: { ...(options.token ? { authorization: `Bearer ${options.token}` } : {}), ...options.headers },
    ...(options.body === undefined ? {} : { body: JSON.stringify(options.body) }),
  });
  const text = await response.text();
  return { status: response.status, body: text && response.headers.get("content-type") === "application/json" ? JSON.parse(text) : text };
}

const as = (token: string) => (method: string, path: string, body?: unknown) => call(method, path, { token, body });
const organizer = as("organizer-1");
const operator = as(OPERATOR);
/** The Discord bot, for the player who pressed its button. */
const bot = (n: number, method: string, path: string, body?: unknown) => call(method, path, { token: BOT, body, headers: { "x-discord-user": discordId(n) } });
/** A request as ember-brackets makes it. */
const connector = (method: string, path: string, body?: unknown, key?: string, token = CONNECTOR) =>
  call(method, `/tournaments/weekly/ember${path}`, {
    token,
    body,
    headers: { accept: "application/json", "ember-brackets-contract": "1", ...(key ? { "idempotency-key": key } : {}) },
  });
const state = (action: string, body: object = {}) => organizer("PUT", "/tournaments/weekly/state", { action, ...body });

test("only organizers create tournaments, and only theirs they manage", async () => {
  assert.equal((await call("POST", "/tournaments", { body: { title: "Weekly" } })).status, 401);
  assert.equal((await as("player-2")("POST", "/tournaments", { id: "x", title: "Weekly" })).status, 403);
  assert.equal((await bot(2, "POST", "/tournaments", { id: "x", title: "Weekly" })).status, 403);
  assert.equal((await organizer("POST", "/tournaments", { id: "a b", title: "Weekly" })).status, 400);
  assert.equal((await organizer("POST", "/tournaments", { id: "weekly" })).status, 400);
  assert.equal((await organizer("POST", "/tournaments", { id: "weekly", title: "W", games_to_win: 11 })).status, 400);
  const created = await organizer("POST", "/tournaments", { id: "weekly", title: "Weekly <1>", games_to_win: 2, starts_at: "2026-10-30T19:00:00Z" });
  assert.equal(created.status, 201);
  assert.deepEqual([created.body.state, created.body.organizers, created.body.actions], ["draft", [discordId(1)], ["plan", "cancel"]]);
  assert.equal(created.body.starts_at, "2026-10-30T19:00:00.000Z");
  assert.equal((await organizer("POST", "/tournaments", { id: "weekly", title: "Again" })).status, 409);
  // Someone else's: the operator made it.
  assert.equal((await operator("POST", "/tournaments", { id: "other", title: "Other", organizers: [discordId(3)] })).status, 201);
  assert.equal((await organizer("PUT", "/tournaments/other/state", { action: "plan" })).status, 403);
  assert.deepEqual((await organizer("GET", "/tournaments")).body.tournaments.map((t: { id: string }) => t.id), ["weekly"]);
  assert.deepEqual((await operator("GET", "/tournaments")).body.tournaments.map((t: { id: string }) => t.id), ["other", "weekly"]);
  assert.equal((await organizer("PATCH", "/tournaments/weekly", { organizers: [discordId(4)] })).status, 400, "cannot leave it");
  assert.equal((await organizer("PATCH", "/tournaments/weekly", { check_in: true, cap: 8 })).status, 200);
  assert.equal((await organizer("DELETE", "/tournaments/other")).status, 403);
  assert.equal((await operator("DELETE", "/tournaments/other")).status, 204);
});

test("players sign up from the bot or the site, linked or not yet, check in, and the unlinked are dropped at the start", async () => {
  assert.equal((await bot(2, "POST", "/tournaments/weekly/entrants", {})).status, 409, "not open yet");
  assert.equal((await state("plan")).status, 200);
  assert.equal((await state("open_sign_ups")).status, 200);
  const two = await bot(2, "POST", "/tournaments/weekly/entrants", { name: "Player 2" });
  assert.deepEqual([two.status, two.body], [201, { discord: discordId(2), name: "Player 2", ember_id: emberId(2), checked_in: false }]);
  assert.equal((await bot(2, "POST", "/tournaments/weekly/entrants", {})).status, 409, "once");
  assert.equal((await as("player-2")("POST", "/tournaments/weekly/entrants", { discord: discordId(3) })).status, 403, "not for someone else");
  assert.equal((await call("POST", "/tournaments/weekly/entrants", { token: BOT, body: {} })).status, 400, "the bot names the player");
  // An organizer adds a player, as the bridge does for a sign-up through Ember.
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(3), name: "Player 3" })).status, 201);
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: "kate" })).status, 400);
  // Not linked in Ember yet, or the bridge is down: they wait for the link.
  for (const n of [5, 13]) {
    const waiting = await bot(n, "POST", "/tournaments/weekly/entrants", { name: `Player ${n}` });
    assert.deepEqual([waiting.status, waiting.body.ember_id], [201, null]);
  }
  assert.equal((await bot(6, "POST", "/tournaments/weekly/entrants", {})).status, 201);
  assert.equal((await bot(6, "DELETE", `/tournaments/weekly/entrants/${discordId(6)}`)).status, 204, "a player leaves");
  assert.equal((await bot(6, "DELETE", `/tournaments/weekly/entrants/${discordId(2)}`)).status, 403);
  assert.equal((await organizer("POST", "/tournaments/weekly/entrants", { discord: discordId(1), name: "Player 1" })).status, 201);

  assert.equal((await state("start")).status, 409, "check-in first");
  assert.equal((await state("open_check_in")).status, 200);
  assert.equal((await bot(4, "POST", "/tournaments/weekly/entrants", {})).status, 409, "too late");
  for (const n of [1, 2, 3, 5, 13]) assert.equal((await bot(n, "POST", "/tournaments/weekly/check-in")).status, 200);
  assert.equal((await bot(4, "POST", "/tournaments/weekly/check-in")).status, 422);

  // Player 5 connects Discord in Ember just before the start; 13's lookup fails.
  linkFive = true;
  asked.length = 0;
  assert.equal((await state("start", { seeding: "best" })).status, 400);
  const started = await state("start");
  assert.equal(started.status, 200);
  assert.deepEqual(asked, [[discordId(5), discordId(13)]], "only the unlinked are asked again");
  assert.deepEqual(started.body.entrants.map((each: { name: string }) => each.name), ["Player 2", "Player 3", "Player 5", "Player 1"]);
  assert.equal(started.body.log.at(-1).note, "dropped Player 13");
  assert.equal(started.body.state, "live");
});

test("the connector plays the live bracket and reports; the site finishes it", async () => {
  assert.equal((await connector("GET", "/sets?state=open", undefined, undefined, OPERATOR)).status, 200, "the operator may");
  assert.equal((await connector("GET", "/sets?state=open", undefined, undefined, "player-2")).status, 401);
  // A desk event's connector comes with the organizer's token.
  const open = (await connector("GET", "/sets?state=open", undefined, undefined, "organizer-1")).body.sets;
  assert.deepEqual(
    open.map((set: { id: string; round_label: string; entries: { id: string; accounts: object[] }[] }) => [set.id, set.round_label, set.entries.map((entry) => [entry.id, entry.accounts])]),
    [
      ["r1-1", "Semifinals", [[emberId(2), [{ discord: discordId(2) }]], [emberId(1), [{ discord: discordId(1) }]]]],
      ["r1-2", "Semifinals", [[emberId(3), [{ discord: discordId(3) }]], [emberId(5), [{ discord: discordId(5) }]]]],
    ],
  );
  const old = await call("GET", "/tournaments/weekly/ember/sets?state=open", { token: CONNECTOR, headers: { "ember-brackets-contract": "2" } });
  assert.deepEqual([old.status, old.body], [426, { contract: "1" }]);
  assert.equal((await connector("POST", "/sets/r1-1/underway", {})).status, 204);
  assert.equal((await connector("GET", "/sets/r1-1")).body.set.underway, true);
  assert.deepEqual((await connector("GET", "/sets/r9-1")).body, { set: null });

  const report = (winner: number, loser: number, key?: string) =>
    connector("POST", "/sets/r1-1/report", {
        winner: emberId(winner),
        scores: [{ entry: emberId(winner), games: 2 }, { entry: emberId(loser), games: 1 }],
        games: [emberId(winner), emberId(loser), emberId(winner)],
        ember_match: "emt_semi-1",
        facts: [null, null, { fighters: [{ character: "RYU", costume: 0, color: 0 }, { character: "BLR", costume: 0, color: 1 }], rounds: [] }],
      }, key);
  assert.equal((await report(1, 2)).status, 400, "a key is required");
  assert.equal((await report(4, 2, "emt_a")).status, 422);
  assert.equal((await report(1, 2, "emt_a")).status, 200);
  assert.equal((await report(1, 2, "emt_a")).status, 200, "a repeat");
  assert.equal((await report(1, 2, "emt_b")).status, 409);
  assert.equal((await state("reset")).status, 409, "a set is decided");
  // What the report said is kept, and counts in the stats and the feed.
  const semi = (await call("GET", "/tournaments/weekly")).body.sets.find((set: { id: string }) => set.id === "r1-1");
  assert.deepEqual([semi.ember_match, semi.games.length, semi.facts[2].fighters[1].character], ["emt_semi-1", 3, "BLR"]);
  assert.deepEqual((await call("GET", "/tournaments/weekly/stats")).body.characters, { RYU: 1, BLR: 1 });
  const feed = (await call("GET", "/tournaments/weekly/feed?after=0")).body.items;
  assert.deepEqual(feed.slice(-3).map((item: { kind: string }) => item.kind), ["set_live", "set_done", "upset"], "seed 4 beat seed 1");
  assert.deepEqual((await call("GET", `/tournaments/weekly/feed?after=${feed.at(-1).n}`)).body.items, []);

  const hand = (set: string, winner: number, loser: number) => organizer("POST", `/tournaments/weekly/sets/${set}/result`, { winner: emberId(winner), scores: { [emberId(winner)]: 2, [emberId(loser)]: 0 } });
  assert.equal((await as("player-2")("POST", "/tournaments/weekly/sets/r1-2/result", {})).status, 403);
  assert.equal((await hand("r1-2", 3, 5)).status, 200);
  assert.deepEqual((await connector("GET", "/standing")).body, { standing: "running" });
  const final = await hand("r2-1", 1, 3);
  assert.equal(final.body.state, "finished");
  assert.deepEqual((await connector("GET", "/standing")).body, { standing: "finished" });
  assert.ok(final.body.sets.every((set: object) => !("key" in set)), "keys are not shown");
  assert.equal((await call("GET", "/t/weekly")).body, (await call("GET", "/tournaments/weekly/bracket")).body);
  const html = await call("GET", "/tournaments/weekly/bracket");
  assert.match(html.body, /<title>Weekly &#60;1&#62;<\/title>/);
  assert.match(html.body, /<tr class="won"><td>Player 1<\/td><td>2<\/td><\/tr>/);
  const after = (await call("GET", "/tournaments/weekly/report")).body;
  assert.deepEqual(after.podium.map((each: { name: string }) => each.name), ["Player 1", "Player 3", "Player 2", "Player 5"], "both semifinal losers are third");
  const shared = await call("GET", "/t/weekly/report");
  assert.match(shared.body, /<meta property="og:description" content="1st Player 1 · 2nd Player 3 · 3rd/);
  // Awards on the players' profiles, announced in the feed.
  const champion = (await call("GET", `/players/${discordId(1)}`)).body;
  assert.deepEqual([champion.name, champion.ember_id, champion.awards[0].kind], ["Player 1", emberId(1), "champion"]);
  assert.ok((await call("GET", "/tournaments/weekly/feed")).body.items.some((item: { kind: string; text: string }) => item.kind === "award" && item.text.startsWith("Player 1: Champion")));
  assert.equal((await call("GET", "/players/99999999999999999")).status, 404);
  // Over within the hour: still listed for followers, with its last item.
  const live = (await call("GET", "/live")).body.tournaments;
  assert.deepEqual(live.map((each: { id: string; state: string }) => [each.id, each.state]), [["weekly", "finished"]]);
  assert.ok(live[0].last > 0);
  assert.equal((await state("archive")).status, 200);

  // The public vote: a player suggests, the organizer accepts, players vote once each.
  assert.equal((await bot(2, "POST", "/tournaments/weekly/vote", {})).status, 403);
  assert.equal((await organizer("POST", "/tournaments/weekly/vote", { hours: 24 })).status, 201);
  const suggested = await bot(2, "POST", "/tournaments/weekly/vote/nominations", { category: "best_player", player: discordId(1), title: "Player 1's run" });
  assert.deepEqual([suggested.status, suggested.body.accepted], [201, false]);
  assert.equal((await bot(3, "POST", "/tournaments/weekly/votes/best_player", { nomination: "n1" })).status, 422, "not accepted yet");
  assert.equal((await organizer("POST", "/tournaments/weekly/vote/nominations/n1/accept")).status, 200);
  for (const n of [1, 2, 3]) assert.equal((await bot(n, "POST", "/tournaments/weekly/votes/best_player", { nomination: "n1" })).status, 204);
  const running = await bot(3, "GET", "/tournaments/weekly/vote");
  assert.deepEqual([running.body.results, running.body.voters.best_player, running.body.mine.best_player], [null, 3, "n1"]);
  assert.equal((await organizer("DELETE", `/tournaments/weekly/votes/best_player/${discordId(3)}`)).status, 204);
  const closed = await organizer("POST", "/tournaments/weekly/vote/close");
  assert.deepEqual(closed.body.results.best_player, [{ nomination: "n1", votes: 2 }]);
  assert.equal((await bot(1, "POST", "/tournaments/weekly/votes/best_player", { nomination: "n1" })).status, 409);
  const kinds = (await call("GET", `/players/${discordId(1)}`)).body.awards.map((award: { kind: string }) => award.kind);
  assert.ok(kinds.includes("best_player"), String(kinds));
  assert.deepEqual((await call("GET", "/tournaments/weekly/report")).body.votes.best_player, [{ nomination: "n1", votes: 2 }]);
  assert.ok(!("vote" in (await call("GET", "/tournaments/weekly")).body), "ballots stay out of the tournament");
});

test("a cancelled or deleted tournament's sets are gone for the connector", async () => {
  await operator("POST", "/tournaments", { id: "gone", title: "Gone", organizers: [discordId(1)] });
  for (const action of ["plan", "open_sign_ups"]) await organizer("PUT", "/tournaments/gone/state", { action });
  for (const n of [1, 2]) await bot(n, "POST", "/tournaments/gone/entrants", {});
  await organizer("PUT", "/tournaments/gone/state", { action: "start" });
  const read = (path: string) => call("GET", `/tournaments/gone/ember${path}`, { token: CONNECTOR });
  assert.equal((await read("/sets/r1-1")).body.set.status, "open");
  assert.equal((await organizer("PUT", "/tournaments/gone/state", { action: "cancel" })).status, 200);
  assert.deepEqual((await read("/sets/r1-1")).body, { set: null });
  assert.deepEqual((await read("/standing")).body, { standing: "finished" });
  assert.equal((await organizer("DELETE", "/tournaments/gone")).status, 204);
  assert.deepEqual((await read("/sets?state=open")).body, { sets: [] });
  assert.equal((await call("GET", "/tournaments/gone")).status, 404);
  assert.equal((await call("GET", "/t/gone")).status, 404, "the page of none, answered");
});

test("organizers make leagues and seasons, and a tournament counts for a season that exists", async () => {
  assert.equal((await as("player-2")("POST", "/leagues", { id: "ember", title: "Ember League" })).status, 403);
  assert.equal((await organizer("POST", "/leagues", { id: "ember", title: "Ember League" })).status, 201);
  assert.equal((await organizer("POST", "/leagues", { id: "ember", title: "Again" })).status, 409);
  assert.equal((await operator("POST", "/leagues", { id: "theirs", title: "Theirs" })).status, 201);
  assert.equal((await organizer("POST", "/leagues/theirs/seasons", { id: "x", title: "X" })).status, 403);
  assert.equal((await organizer("POST", "/leagues/ember/seasons", { id: "s1", title: "Season 1", points: { first: 3 } })).status, 400);
  const season = await organizer("POST", "/leagues/ember/seasons", { id: "s1", title: "Season 1", best_of: 4 });
  assert.deepEqual([season.status, season.body.points["1"], season.body.best_of], [201, 100, 4]);
  assert.equal((await operator("POST", "/leagues/theirs/seasons", { id: "s1", title: "Taken" })).status, 409, "a season ID is the site's");

  assert.equal((await organizer("POST", "/tournaments", { id: "cup", title: "Cup", season: "s9" })).status, 400);
  assert.equal((await organizer("POST", "/tournaments", { id: "cup", title: "Cup", season: "s1" })).status, 201);
  assert.equal((await organizer("PATCH", "/tournaments/cup", { season: "nope" })).status, 422);
  const standings = await call("GET", "/leagues/ember/seasons/s1/standings");
  assert.deepEqual([standings.status, standings.body.standings], [200, []]);
  assert.equal((await call("GET", "/leagues/ember")).body.seasons.length, 1);
  assert.equal((await as("player-2")("POST", "/leagues/ember/seasons/s1/close")).status, 403);
  const closed = await organizer("POST", "/leagues/ember/seasons/s1/close");
  assert.deepEqual([closed.status, closed.body.awards], [200, []], "no finished event yet: nobody to award");
  assert.equal((await organizer("POST", "/leagues/ember/seasons/s1/close")).status, 409);
});

test("a closed season's awards are news its league's followers read", async () => {
  await organizer("POST", "/leagues/ember/seasons", { id: "s2", title: "Season 2" });
  // A finished event of the season, played out without the site's routes.
  const t: Tournament = { id: "s2cup", title: "S2 Cup", games_to_win: 2, check_in: false, cap: null, starts_at: null, season: "s2", organizers: [], state: "sign_ups", entrants: [], rounds: 0, sets: [], log: [] };
  for (const n of [1, 2]) enter(t, { discord: discordId(n), name: `Player ${n}`, ember_id: emberId(n), checked_in: false }, false);
  move(t, "start", "o", "2026-10-21T20:00:00.000Z");
  report(t.sets, "r1-1", { winner: emberId(1), scores: { [emberId(1)]: 2, [emberId(2)]: 0 }, key: null });
  t.state = "finished";
  store.add(t);
  const closed = await organizer("POST", "/leagues/ember/seasons/s2/close");
  assert.deepEqual(closed.body.awards.map((award: { name: string; kind: string }) => [award.name, award.kind]), [["Player 1", "season_champion"], ["Player 2", "season_top_3"]]);
  const listed = (await call("GET", "/live")).body.tournaments.find((each: { id: string }) => each.id === "league:ember");
  assert.deepEqual([listed.feed, listed.last], ["/leagues/ember/feed", 2]);
  const news = (await call("GET", "/leagues/ember/feed?after=0")).body.items;
  assert.equal(news[0].text, "Player 1: Season champion of Ember League, Season 2 (100 points).");
  assert.ok(!("feed" in (await call("GET", "/leagues/ember")).body));
});
