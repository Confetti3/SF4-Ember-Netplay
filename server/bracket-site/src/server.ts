// The HTTP routes: who is asking, the tournaments' API for organizers and
// players, a public view, and the connector's contract (contract.ts).
import { createHash, randomBytes, randomInt, timingSafeEqual } from "node:crypto";
import { createServer, type IncomingMessage, type Server } from "node:http";

import type { BridgeClient } from "../../../sdk/typescript/src/index.ts";
import { Conflict, Invalid, roundLabel, status } from "./bracket.ts";
import { contractRoutes, record } from "./contract.ts";
import { fail, Failure, readJson, send, text, type Json, type Route } from "./http.ts";
import { seasonAwards } from "./awards.ts";
import { emblem, isKind } from "./emblems.ts";
import { DEFAULT_POINTS, standings, type League, type Season } from "./league.ts";
import { push, stats } from "./live.ts";
import { OAuth, OAuthError, Redirect } from "./oauth.ts";
import { page, reportPage } from "./page.ts";
import { manifest, setReplays, type Clip } from "./replays.ts";
import { accept, cast, close as closeVoteOf, due, nominate, open as openVote, shown as voteView, voidVote, type Category, type Nomination } from "./vote.ts";
import type { Store } from "./store.ts";
import { ACTIONS, actions, beforeLive, checkIn, enter, move, type Action, type Entrant, type Tournament } from "./tournament.ts";

const SLUG = /^[A-Za-z0-9_-]{1,64}$/;
export const DISCORD_ID = /^\d{17,20}$/;
/** The most Discord IDs one bridge lookup takes. */
const LOOKUP_BATCH = 32;

/** Who is asking. */
export interface Actor {
  /** Their Discord user ID; none for the operator acting as no one. */
  discord: string | null;
  /** The site's operator: may do anything. */
  operator: boolean;
  /** May create tournaments and manage those they organize: only organizers the bridge lets sign in as one. */
  organizer: boolean;
  /** Their Discord name, when signed in through Discord. */
  name?: string;
}

export interface Options {
  store: Store;
  /** The operator's token. With `X-Discord-User`, the operator acts as that user. */
  operatorToken: string;
  /** For the connector's configured tournaments. */
  connectorToken: string;
  /** For the Discord bot, which says in `X-Discord-User` who pressed its button. */
  botToken?: string;
  /** Other bearer tokens, for tests. */
  tokens?: (token: string) => Actor | undefined;
  /** Sign-in: the site's OAuth access tokens, browser sessions and their routes. */
  oauth?: OAuth;
  /** The site's public origin: a browser's session writes only from its own pages. */
  origin?: string;
  /** The bridge's public origin, which serves a public event's replays. */
  bridgeOrigin?: string;
  fetch?: typeof fetch;
  /** A bridge connection with `discord_lookup`, which finds each player's Ember ID. */
  bridge: Pick<BridgeClient, "lookupPlayers">;
  now?: () => Date;
}

export function bracketSite(options: Options): Server {
  const { store } = options;
  const now = () => (options.now?.() ?? new Date()).toISOString();
  const found = (id: string) => store.get(id) ?? fail(404, `no tournament ${id}`);

  function actor(request: IncomingMessage): Actor | undefined {
    const token = /^Bearer (.+)$/.exec(request.headers.authorization ?? "")?.[1];
    if (!token) {
      const session = options.oauth?.session(request);
      if (session && request.method !== "GET" && request.headers.origin !== options.origin) fail(403, "only from the site's own pages");
      return session;
    }
    const header = request.headers["x-discord-user"];
    const discord = typeof header === "string" && DISCORD_ID.test(header) ? header : null;
    if (same(token, options.operatorToken)) return { discord, operator: true, organizer: true };
    if (options.botToken && same(token, options.botToken)) return discord ? { discord, operator: false, organizer: false } : fail(400, "X-Discord-User is required");
    return options.tokens?.(token) ?? options.oauth?.actor(token);
  }
  const signedIn = (request: IncomingMessage) => actor(request) ?? fail(401, "wrong or missing token");
  const manages = (who: Actor | undefined, managed: { organizers: string[] }) =>
    !!who && (who.operator || (who.organizer && who.discord !== null && managed.organizers.includes(who.discord)));
  const manager = (request: IncomingMessage, tournament: Tournament) => {
    if (!manages(signedIn(request), tournament)) fail(403, "only its organizers may");
    return tournament;
  };
  const player = (request: IncomingMessage) => signedIn(request).discord ?? fail(400, "who? (X-Discord-User)");
  const by = (request: IncomingMessage) => signedIn(request).discord ?? "operator";

  /** The Ember IDs connected to these Discord accounts, as many as the bridge finds; none when it fails. */
  async function lookup(discords: string[]): Promise<Map<string, string>> {
    const found = new Map<string, string>();
    for (let i = 0; i < discords.length; i += LOOKUP_BATCH) {
      try {
        for (const player of await options.bridge.lookupPlayers(discords.slice(i, i + LOOKUP_BATCH))) found.set(player.discord_user_id, player.ember_id);
      } catch (error) {
        console.error(`the bridge's lookup failed: ${error instanceof Error ? error.message : error}`);
      }
    }
    return found;
  }

  /** Runs `change` on the tournament as it is now, saves it, and answers the 4xx its refusal means. */
  function change(id: string, request: IncomingMessage | null, change: (tournament: Tournament) => void): Tournament {
    const tournament = found(id);
    if (request) manager(request, tournament);
    try {
      change(tournament);
    } catch (error) {
      if (error instanceof Conflict) fail(409, error.message);
      if (error instanceof Invalid) fail(422, error.message);
      throw error;
    }
    store.save(tournament);
    return tournament;
  }

  const contract = contractRoutes(store, (request, tournament) => {
    const token = /^Bearer (.+)$/.exec(request.headers.authorization ?? "")?.[1] ?? "";
    if (same(token, options.connectorToken)) return;
    // A desk event's connector comes with the organizer's own token.
    if (!tournament || !manages(actor(request), tournament)) fail(401, "wrong or missing token");
  });

  const routes: Route[] = [
    [
      "POST",
      /^\/tournaments$/,
      async (request) => {
        const who = signedIn(request);
        if (!who.organizer) fail(403, "only organizers may create tournaments");
        const body = await readJson(request);
        const id = body.id === undefined ? `t${randomBytes(6).toString("hex")}` : text(body.id, SLUG, "id is 1 to 64 letters, digits, _ or -");
        const tournament: Tournament = {
          id,
          title: "",
          games_to_win: 2,
          check_in: false,
          cap: null,
          starts_at: null,
          season: null,
          organizers: who.discord ? [who.discord] : [],
          state: "draft",
          entrants: [],
          rounds: 0,
          sets: [],
          log: [],
        };
        settings(tournament, body, who.operator ? null : who.discord);
        if (!tournament.title) fail(400, "title is 1 to 100 characters");
        if (tournament.season && !store.leagueOf(tournament.season)) fail(400, `no season ${tournament.season}`);
        if (!store.add(tournament)) fail(409, `tournament ${id} exists`);
        return [201, view(tournament)];
      },
    ],
    [
      "GET",
      /^\/tournaments$/,
      (request) => {
        const who = signedIn(request);
        const mine = store.all().filter((tournament) => manages(who, tournament) && tournament.state !== "archived");
        return [200, { tournaments: mine.map(view) }];
      },
    ],
    ["GET", /^\/tournaments\/([^/]+)$/, (_, [id]) => [200, view(found(id!))]],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/feed$/,
      (_, [id], url) => {
        const after = Number(url.searchParams.get("after") ?? 0);
        return [200, { items: (found(id!).feed ?? []).filter((item) => item.n > after) }];
      },
    ],
    ["GET", /^\/tournaments\/([^/]+)\/stats$/, (_, [id]) => [200, stats(found(id!))]],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/replays\/([^/]+)$/,
      async (_, [id, setId]) => {
        const bridge = options.bridgeOrigin ?? fail(404, "this site links no replays");
        const set = found(id!).sets.find((each) => each.id === setId) ?? fail(404, `no set ${setId}`);
        let replays;
        try {
          replays = await setReplays(bridge, set, options.fetch);
        } catch {
          fail(502, "the bridge did not answer; try again");
        }
        return [200, replays ?? fail(404, "no public replays of this set")];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/clips$/,
      async (request, [id]) => {
        const body = await readJson(request);
        let clip: Clip | undefined;
        change(id!, request, (tournament) => {
          const set = tournament.sets.find((each) => each.id === body.set);
          if (!set?.ember_match) throw new Invalid("set is a set Ember played");
          const frame = (value: unknown) => typeof value === "string" && /^\d{1,12}$/.test(value);
          if (typeof body.crc !== "string" || !/^[0-9a-f]{8}$/.test(body.crc)) throw new Invalid("crc is a game's 8 hex digits");
          if (!frame(body.from_frame) || !frame(body.to_frame) || Number(body.to_frame) <= Number(body.from_frame)) throw new Invalid("from_frame and to_frame are frames, to after from");
          const title = text(body.title, /^.{1,100}$/u, "title is 1 to 100 characters");
          const clips = (tournament.clips ??= []);
          clip = { id: `c${clips.length + 1}`, set: set.id, crc: body.crc, from_frame: body.from_frame as string, to_frame: body.to_frame as string, title };
          clips.push(clip);
        });
        return [201, clip!];
      },
    ],
    ["GET", /^\/tournaments\/([^/]+)\/clips$/, (_, [id]) => [200, { clips: found(id!).clips ?? [] }]],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/clips\/([^/]+)$/,
      (_, [id, clipId]) => {
        const tournament = found(id!);
        const clip = tournament.clips?.find((each) => each.id === clipId) ?? fail(404, `no clip ${clipId}`);
        return [200, manifest(options.bridgeOrigin ?? "", tournament, clip)];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/vote$/,
      async (request, [id]) => {
        const body = await readJson(request);
        const hours = body.hours === undefined ? 48 : whole(body.hours, 1, 336, "hours is 1 to 336");
        return [201, voteView(change(id!, request, (tournament) => openVote(tournament, new Date(now()), hours)).vote!, null)];
      },
    ],
    [
      "GET",
      /^\/tournaments\/([^/]+)\/vote$/,
      (request, [id]) => {
        const tournament = closeIfDue(id!);
        return [200, voteView(tournament.vote ?? fail(404, "no vote"), actor(request)?.discord ?? options.oauth?.session(request)?.discord ?? null)];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/vote\/nominations$/,
      async (request, [id]) => {
        const by = player(request);
        const organizer = manages(signedIn(request), found(id!));
        const body = await readJson(request);
        const title = text(body.title, /^.{1,100}$/u, "title is 1 to 100 characters");
        const ref = body.ref === undefined || body.ref === null ? null : text(body.ref, /^[A-Za-z0-9_:-]{1,100}$/, "ref names a clip or a game");
        let made: Nomination | undefined;
        change(closeIfDue(id!).id, null, (tournament) => {
          made = nominate(tournament, { category: body.category as Category, player: String(body.player), title, ref, by }, organizer);
        });
        return [201, made!];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/vote\/nominations\/([^/]+)\/accept$/,
      (request, [id, nomination]) => [200, voteView(change(id!, request, (tournament) => accept(tournament, nomination!)).vote!, null)],
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/votes\/([^/]+)$/,
      async (request, [id, category]) => {
        const voter = player(request);
        const body = await readJson(request);
        change(closeIfDue(id!).id, null, (tournament) => cast(tournament, category as Category, voter, String(body.nomination), new Date(now())));
        return [204];
      },
    ],
    [
      "DELETE",
      /^\/tournaments\/([^/]+)\/votes\/([^/]+)\/(\d{17,20})$/,
      (request, [id, category, voter]) => {
        change(id!, request, (tournament) => voidVote(tournament, category as Category, voter!));
        return [204];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/vote\/close$/,
      (request, [id]) => {
        manager(request, found(id!));
        const tournament = closeVote(id!);
        return [200, voteView(tournament.vote ?? fail(404, "no vote"), null)];
      },
    ],
    ["GET", /^\/tournaments\/([^/]+)\/report$/, (_, [id]) => [200, found(id!).report ?? fail(404, "its report comes once it is finished")]],
    [
      "GET",
      /^\/live$/,
      () => {
        // Live, or over within the hour, so a follower sees how it ended.
        const recent = (tournament: Tournament) => Date.parse(tournament.log.at(-1)?.at ?? "") > Date.parse(now()) - 3600_000;
        const shown = store.all().filter((tournament) => tournament.state === "live" || (["finished", "cancelled"].includes(tournament.state) && recent(tournament)));
        // A league with news within the hour, such as a closed season's awards.
        const leagues = store.leagues().filter((league) => Date.parse(league.feed?.at(-1)?.at ?? "") > Date.parse(now()) - 3600_000);
        return [
          200,
          {
            tournaments: [
              ...shown.map((tournament) => ({ id: tournament.id, title: tournament.title, state: tournament.state, last: tournament.feed?.at(-1)?.n ?? 0 })),
              ...leagues.map((league) => ({ id: `league:${league.id}`, title: league.title, state: "league", last: league.feed!.at(-1)!.n, feed: `/leagues/${league.id}/feed` })),
            ],
          },
        ];
      },
    ],
    [
      "PATCH",
      /^\/tournaments\/([^/]+)$/,
      async (request, [id]) => {
        const body = await readJson(request);
        const who = signedIn(request);
        return [200, view(change(id!, request, (tournament) => {
          if (!beforeLive(tournament)) throw new Conflict(`its settings are fixed while ${tournament.state}`);
          settings(tournament, body, who.operator ? null : who.discord);
          if (tournament.season && !store.leagueOf(tournament.season)) throw new Invalid(`no season ${tournament.season}`);
        }))];
      },
    ],
    [
      "DELETE",
      /^\/tournaments\/([^/]+)$/,
      (request, [id]) => {
        manager(request, found(id!));
        store.delete(id!);
        return [204];
      },
    ],
    [
      "PUT",
      /^\/tournaments\/([^/]+)\/state$/,
      async (request, [id]) => {
        const body = await readJson(request);
        const action = body.action as Action;
        if (!ACTIONS.includes(action)) fail(400, `action is one of ${ACTIONS.join(", ")}`);
        const seeding = body.seeding ?? "join";
        if (seeding !== "join" && seeding !== "random" && seeding !== "points") fail(400, 'seeding is "join", "random" or "points"');
        manager(request, found(id!));
        // Entrants still waiting for their Discord link are looked up once more, and dropped at the start if still not linked.
        if (action === "start") {
          const linked = await lookup(found(id!).entrants.filter((entrant) => !entrant.ember_id).map((entrant) => entrant.discord));
          change(id!, null, (tournament) => {
            for (const entrant of tournament.entrants) entrant.ember_id ??= linked.get(entrant.discord) ?? null;
          });
        }
        return [200, view(change(id!, request, (tournament) => {
          if (action === "start" && seeding === "random") shuffle(tournament.entrants);
          if (action === "start" && seeding === "points") byPoints(tournament);
          move(tournament, action, by(request), now());
        }))];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/entrants$/,
      async (request, [id]) => {
        const who = signedIn(request);
        const body = await readJson(request);
        const discord = body.discord === undefined ? player(request) : text(body.discord, DISCORD_ID, "discord is a Discord user ID");
        const name = body.name !== undefined ? text(body.name, /^.{1,64}$/u, "name is 1 to 64 characters") : discord === who.discord ? (who.name ?? discord) : discord;
        const forSomeoneElse = discord !== who.discord;
        if (forSomeoneElse) manager(request, found(id!));
        const organizer = manages(who, found(id!));
        // Refused before the bridge is asked, and asked again after, as it may have changed meanwhile.
        const entrant: Entrant = { discord, name, ember_id: null, checked_in: false };
        enterOrFail(found(id!), entrant, organizer);
        entrant.ember_id = (await lookup([discord])).get(discord) ?? null;
        change(id!, null, (tournament) => enter(tournament, entrant, organizer));
        return [201, entrant];
      },
    ],
    [
      "DELETE",
      /^\/tournaments\/([^/]+)\/entrants\/([^/]+)$/,
      (request, [id, discord]) => {
        if (discord !== signedIn(request).discord) manager(request, found(id!));
        change(id!, null, (tournament) => {
          if (!beforeLive(tournament)) throw new Conflict(`entrants are fixed while ${tournament.state}`);
          const before = tournament.entrants.length;
          tournament.entrants = tournament.entrants.filter((each) => each.discord !== discord);
          if (tournament.entrants.length === before) throw new Invalid(`${discord} has not signed up`);
        });
        return [204];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/check-in$/,
      (request, [id]) => {
        const discord = player(request);
        let entrant: Entrant | undefined;
        change(id!, null, (tournament) => (entrant = checkIn(tournament, discord)));
        return [200, entrant!];
      },
    ],
    [
      "POST",
      /^\/tournaments\/([^/]+)\/sets\/([^/]+)\/result$/,
      async (request, [id, setId]) => {
        const tournament = manager(request, found(id!));
        const body = await readJson(request);
        const scores = body.scores as Record<string, number>;
        if (typeof body.winner !== "string" || typeof scores !== "object" || scores === null) fail(400, "a result is { winner, scores: { <ember id>: <games> } }");
        record(store, tournament, setId!, { winner: body.winner, scores, key: null });
        return [200, view(found(id!))];
      },
    ],
    [
      "POST",
      /^\/leagues$/,
      async (request) => {
        const who = signedIn(request);
        if (!who.organizer) fail(403, "only organizers may create leagues");
        const body = await readJson(request);
        const league: League = {
          id: text(body.id, SLUG, "id is 1 to 64 letters, digits, _ or -"),
          title: text(body.title, /^.{1,100}$/u, "title is 1 to 100 characters"),
          organizers: who.discord ? [who.discord] : [],
          seasons: [],
        };
        if (!store.putLeague(league, true)) fail(409, `league ${league.id} exists`);
        return [201, league];
      },
    ],
    [
      "GET",
      /^\/leagues\/([^/]+)$/,
      (_, [id]) => {
        const { feed: _feed, ...league } = store.league(id!) ?? fail(404, `no league ${id}`);
        return [200, league];
      },
    ],
    [
      "GET",
      /^\/leagues\/([^/]+)\/feed$/,
      (_, [id], url) => {
        const after = Number(url.searchParams.get("after") ?? 0);
        return [200, { items: ((store.league(id!) ?? fail(404, `no league ${id}`)).feed ?? []).filter((item) => item.n > after) }];
      },
    ],
    [
      "POST",
      /^\/leagues\/([^/]+)\/seasons$/,
      async (request, [id]) => {
        const league = store.league(id!) ?? fail(404, `no league ${id}`);
        if (!manages(signedIn(request), league)) fail(403, "only its organizers may");
        const body = await readJson(request);
        const season: Season = {
          id: text(body.id, SLUG, "id is 1 to 64 letters, digits, _ or -"),
          title: text(body.title, /^.{1,100}$/u, "title is 1 to 100 characters"),
          points: body.points === undefined ? DEFAULT_POINTS : pointsOf(body.points),
          best_of: body.best_of === undefined || body.best_of === null ? null : whole(body.best_of, 1, 100, "best_of is 1 to 100, or null"),
        };
        if (store.leagueOf(season.id)) fail(409, `season ${season.id} exists`);
        league.seasons.push(season);
        store.putLeague(league, false);
        return [201, season];
      },
    ],
    [
      "POST",
      /^\/leagues\/([^/]+)\/seasons\/([^/]+)\/close$/,
      (request, [id, seasonId]) => {
        const league = store.league(id!) ?? fail(404, `no league ${id}`);
        if (!manages(signedIn(request), league)) fail(403, "only its organizers may");
        const season = league.seasons.find((each) => each.id === seasonId) ?? fail(404, `no season ${seasonId}`);
        if (season.closed_at) fail(409, "the season is closed");
        season.closed_at = now();
        store.putLeague(league, false);
        const given = store.addAwards(seasonAwards(season, standings(season, store.all()), season.closed_at));
        for (const award of given) push(league, { at: season.closed_at, kind: "award", text: `${award.name}: ${award.title} of ${league.title}, ${award.of} (${award.detail}).` });
        store.putLeague(league, false);
        return [200, { season, awards: given }];
      },
    ],
    ["GET", /^\/awards$/, (_, __, url) => [200, { awards: store.awardsAfter(Number(url.searchParams.get("after") ?? 0) || 0) }]],
    [
      "GET",
      /^\/players\/(\d{17,20})$/,
      (_, [discord]) => {
        const awards = store.awardsOf(discord!);
        const entered = store.all().flatMap((tournament) => tournament.entrants.filter((entrant) => entrant.discord === discord));
        if (!awards.length && !entered.length) fail(404, "no such player here");
        const shown = awards.map((award) => ({ ...award, emblem: `/emblems/${award.kind}.svg` }));
        return [200, { discord, name: entered.at(-1)?.name ?? awards.at(-1)!.name, ember_id: entered.findLast((each) => each.ember_id)?.ember_id ?? null, awards: shown }];
      },
    ],
    [
      "GET",
      /^\/leagues\/([^/]+)\/seasons\/([^/]+)\/standings$/,
      (_, [id, seasonId]) => {
        const season = store.league(id!)?.seasons.find((each) => each.id === seasonId) ?? fail(404, `no season ${seasonId}`);
        return [200, { season, standings: standings(season, store.all()) }];
      },
    ],
    ...contract,
    ...(options.oauth?.routes() ?? []),
  ];

  /**
   * Seeds by the season's standings at the start: the most points first,
   * those without any after them in join order.
   */
  function byPoints(tournament: Tournament): void {
    const league = tournament.season ? store.leagueOf(tournament.season) : undefined;
    const season = league?.seasons.find((each) => each.id === tournament.season) ?? fail(409, "points seeding needs the tournament's season");
    const order = standings(season, store.all()).map((row) => row.discord);
    const rank = (discord: string) => (order.includes(discord) ? order.indexOf(discord) : order.length);
    // A stable sort keeps join order among those ranked the same.
    tournament.entrants.sort((a, b) => rank(a.discord) - rank(b.discord));
  }

  /** Closes the vote once its time is up; the tournament as it is now. */
  function closeIfDue(id: string): Tournament {
    return due(found(id), new Date(now())) ? closeVote(id) : found(id);
  }

  /** Closes the vote: its results into the report, its winners' awards, each announced. */
  function closeVote(id: string): Tournament {
    return change(id, null, (tournament) => {
      const at = now();
      const awards = store.addAwards(closeVoteOf(tournament, new Date(at)));
      if (tournament.report && tournament.vote?.results) tournament.report.votes = tournament.vote.results;
      for (const award of awards) push(tournament, { at, kind: "award", text: `${award.name}: ${award.title} of ${award.of} (${award.detail}).` });
    });
  }

  function enterOrFail(tournament: Tournament, entrant: Entrant, organizer: boolean): void {
    try {
      enter(structuredClone(tournament), entrant, organizer);
    } catch (error) {
      if (error instanceof Conflict) fail(409, error.message);
      throw error;
    }
  }

  return createServer(async (request, response) => {
    const url = new URL(request.url ?? "/", "http://localhost");
    try {
      const badge = /^\/emblems\/([a-z0-9_]+)\.svg$/.exec(url.pathname);
      if (badge && request.method === "GET") {
        const kind = isKind(badge[1]!) ? badge[1] : fail(404, "no such emblem");
        response.writeHead(200, { "content-type": "image/svg+xml", "cache-control": "public, max-age=86400" }).end(emblem(kind));
        return;
      }
      const reported = /^\/t\/([^/]+)\/report$/.exec(url.pathname);
      if (reported && request.method === "GET") {
        const tournament = found(decodeURIComponent(reported[1]!));
        const shown = reportPage(tournament.report ?? fail(404, "its report comes once it is finished"), tournament.id);
        response.writeHead(200, { "content-type": "text/html; charset=utf-8" }).end(shown);
        return;
      }
      // The page, also at /t/<id>, the short link the desk shows.
      const html = /^\/(?:t\/([^/]+)|tournaments\/([^/]+)\/bracket)$/.exec(url.pathname);
      if (html && request.method === "GET") {
        const viewer = options.oauth?.session(request)?.discord ?? null;
        const shown = page(found(decodeURIComponent(html[1] ?? html[2]!)), viewer, !!options.oauth);
        response.writeHead(200, { "content-type": "text/html; charset=utf-8" }).end(shown);
        return;
      }
      for (const [method, pattern, handler] of routes) {
        const match = pattern.exec(url.pathname);
        if (!match || request.method !== method) continue;
        const [code, body] = await handler(request, match.slice(1).map(decodeURIComponent), url);
        send(response, code, body);
        return;
      }
      fail(404, "not found");
    } catch (error) {
      if (error instanceof Failure) send(response, error.status, error.body);
      else if (error instanceof Redirect || error instanceof OAuthError) error.answer(response);
      else {
        console.error(error);
        send(response, 500, { error: "internal error" });
      }
    }
  });
}

function same(given: string, token: string): boolean {
  const digest = (value: string) => createHash("sha256").update(value).digest();
  return timingSafeEqual(digest(given), digest(token));
}

/** Reads the settings an organizer may change into `tournament`. `self`: the organizer making the change; null for the operator. */
function settings(tournament: Tournament, body: Json, self: string | null): void {
  if (body.title !== undefined) tournament.title = text(body.title, /^.{1,100}$/u, "title is 1 to 100 characters");
  if (body.games_to_win !== undefined) tournament.games_to_win = whole(body.games_to_win, 1, 10, "games_to_win is 1 to 10");
  if (body.check_in !== undefined) {
    if (typeof body.check_in !== "boolean") fail(400, "check_in is true or false");
    tournament.check_in = body.check_in;
  }
  if (body.cap !== undefined) tournament.cap = body.cap === null ? null : whole(body.cap, 2, 1024, "cap is 2 to 1024, or null");
  if (body.starts_at !== undefined) {
    if (body.starts_at !== null && (typeof body.starts_at !== "string" || Number.isNaN(Date.parse(body.starts_at)))) fail(400, "starts_at is a date and time, or null");
    tournament.starts_at = body.starts_at === null ? null : new Date(body.starts_at as string).toISOString();
  }
  if (body.season !== undefined) tournament.season = body.season === null ? null : text(body.season, SLUG, "season is 1 to 64 letters, digits, _ or -");
  if (body.organizers !== undefined) {
    const organizers = body.organizers;
    if (!Array.isArray(organizers) || !organizers.length || !organizers.every((each) => typeof each === "string" && DISCORD_ID.test(each))) {
      fail(400, "organizers is a list of Discord user IDs");
    }
    // An organizer may add co-organizers but not leave their own tournament without one.
    if (self !== null && !organizers.includes(self)) fail(400, "organizers must still name you");
    tournament.organizers = [...new Set(organizers as string[])];
  }
}

/** Points by placement (`"1"`, `"2"`, ... and `"*"`), each 0 to 10000. */
function pointsOf(value: unknown): Record<string, number> {
  const ok =
    typeof value === "object" &&
    value !== null &&
    !Array.isArray(value) &&
    Object.entries(value).length <= 32 &&
    Object.entries(value).every(([place, points]) => /^(\*|[1-9]\d{0,3})$/.test(place) && Number.isInteger(points) && points >= 0 && points <= 10000);
  if (!ok) fail(400, 'points maps placements ("1", "2", "3", "5", ... or "*") to whole points');
  return value as Record<string, number>;
}

function whole(value: unknown, min: number, max: number, message: string): number {
  if (!Number.isInteger(value) || (value as number) < min || (value as number) > max) fail(400, message);
  return value as number;
}

function shuffle<T>(items: T[]): void {
  for (let i = items.length - 1; i > 0; i--) {
    const j = randomInt(i + 1);
    [items[i], items[j]] = [items[j]!, items[i]!];
  }
}

/** The tournament as anyone may see it, with the actions its state allows. */
function view(tournament: Tournament): Json {
  // The feed, report, clips and vote have routes of their own; a vote's ballots are never shown.
  const { feed: _, report: __, clips: ___, vote: ____, ...shown } = tournament;
  return {
    ...shown,
    actions: actions(tournament),
    sets: tournament.sets.map(({ key: _, ...set }) => ({ ...set, status: status(set), round_label: roundLabel(set.round, tournament.rounds) })),
  };
}
