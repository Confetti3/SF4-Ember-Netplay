// Signing in through Discord: the bridge as the site's OAuth client, and a
// browser on the site's own page, against a stand-in for Discord.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync } from "node:fs";
import { createServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";

import { OAuth } from "../src/oauth.ts";
import { bracketSite } from "../src/server.ts";
import { Store } from "../src/store.ts";

const ORGANIZER = "274220342558756145";
/** The site's public origin, which it is reached at through its proxy. */
const ORIGIN = "https://brackets.example";
const BRIDGE = { id: "ember-bridge", secret: "bridge-test-secret", redirect_uri: "https://bridge.example/v1/sites/ember-bracket-site/callback" };
/** Discord's codes: each signs in the user named in it. */
const DISCORD_USERS: Record<string, string> = { "code-organizer": ORGANIZER, "code-player": "80351110224678912" };

let dir: string;
let store: Store;
let site: string;
let discord: string;
const servers: Server[] = [];

async function listen(server: Server): Promise<string> {
  servers.push(server);
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  return `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
}

before(async () => {
  dir = mkdtempSync(join(tmpdir(), "bracket-site-oauth-"));
  store = new Store(join(dir, "site.sqlite3"));
  const tokens = new Map<string, string>();
  discord = await listen(
    createServer((request, response) => {
      let body = "";
      request.on("data", (chunk) => (body += chunk));
      request.on("end", () => {
        const json = (status: number, value: object) => response.writeHead(status, { "content-type": "application/json" }).end(JSON.stringify(value));
        if (request.url === "/api/oauth2/token") {
          const form = new URLSearchParams(body);
          const user = DISCORD_USERS[form.get("code") ?? ""];
          if (!user || form.get("client_secret") !== "discord-test-secret") return json(400, { error: "invalid_grant" });
          tokens.set(`at-${user}`, user);
          return json(200, { access_token: `at-${user}`, token_type: "Bearer" });
        }
        const user = tokens.get(request.headers.authorization?.replace("Bearer ", "") ?? "");
        if (request.url === "/api/users/@me" && user) return json(200, { id: user, username: `user${user.slice(-2)}`, global_name: user === ORGANIZER ? "Kate" : null });
        json(404, {});
      });
    }),
  );
  const oauth = new OAuth({ db: store.db, origin: ORIGIN, discord: { clientId: "1546980049692135514", clientSecret: "discord-test-secret", apiBase: `${discord}/api`, authorizeUrl: `${discord}/oauth2/authorize` }, clients: [BRIDGE] });
  const lookups = { lookupPlayers: async () => [] };
  site = await listen(bracketSite({ store, operatorToken: "operator", connectorToken: "connector", oauth, origin: ORIGIN, bridge: lookups }));
});

after(() => {
  for (const server of servers) server.close();
  store.close();
  rmSync(dir, { recursive: true, force: true });
});

const get = (url: string, headers: Record<string, string> = {}) => fetch(url, { redirect: "manual", headers });

/** Follows the site to Discord and back with `code`, as a browser does; the address Discord's answer leads to. */
async function throughDiscord(start: string, code: string): Promise<Response> {
  const toDiscord = await get(start);
  assert.equal(toDiscord.status, 302, await toDiscord.text());
  const asked = new URL(toDiscord.headers.get("location")!);
  assert.equal(asked.origin + asked.pathname, `${discord}/oauth2/authorize`);
  assert.equal(asked.searchParams.get("scope"), "identify");
  assert.equal(asked.searchParams.get("redirect_uri"), `${ORIGIN}/oauth/discord/callback`);
  const callback = new URL(`${site}/oauth/discord/callback`);
  callback.searchParams.set("state", asked.searchParams.get("state")!);
  if (code) callback.searchParams.set("code", code);
  return get(callback.href);
}

const authorize = (params: Record<string, string>) => `${site}/oauth/authorize?${new URLSearchParams({ response_type: "code", client_id: BRIDGE.id, redirect_uri: BRIDGE.redirect_uri, state: "bridge-state", ...params })}`;
const token = (fields: Record<string, string>) =>
  fetch(`${site}/oauth/token`, { method: "POST", headers: { "content-type": "application/x-www-form-urlencoded" }, body: new URLSearchParams({ client_id: BRIDGE.id, client_secret: BRIDGE.secret, ...fields }) });

test("the bridge signs an organizer in, and their token creates tournaments and refreshes once", async () => {
  const back = await throughDiscord(authorize({ scope: "organizer" }), "code-organizer");
  const location = new URL(back.headers.get("location")!);
  assert.equal(location.origin + location.pathname, BRIDGE.redirect_uri);
  assert.equal(location.searchParams.get("state"), "bridge-state");
  const code = location.searchParams.get("code")!;

  assert.equal((await token({ grant_type: "authorization_code", code, redirect_uri: BRIDGE.redirect_uri, client_secret: "wrong" })).status, 401);
  assert.equal((await token({ grant_type: "authorization_code", code, redirect_uri: "https://elsewhere.example/" })).status, 400);
  const granted = await token({ grant_type: "authorization_code", code, redirect_uri: BRIDGE.redirect_uri });
  // The wrong redirect used the code up.
  assert.equal(granted.status, 400);

  const again = new URL((await throughDiscord(authorize({ scope: "organizer" }), "code-organizer")).headers.get("location")!).searchParams.get("code")!;
  const tokens = (await (await token({ grant_type: "authorization_code", code: again, redirect_uri: BRIDGE.redirect_uri })).json()) as Record<string, string>;
  assert.deepEqual([tokens.token_type, tokens.scope, tokens.expires_in], ["Bearer", "organizer", 3600]);
  assert.equal((await token({ grant_type: "authorization_code", code: again, redirect_uri: BRIDGE.redirect_uri })).status, 400, "a code is used once");

  const me = await get(`${site}/oauth/me`, { authorization: `Bearer ${tokens.access_token}` });
  assert.deepEqual(await me.json(), { discord_id: ORGANIZER, username: "Kate" });
  const created = await fetch(`${site}/tournaments`, { method: "POST", headers: { authorization: `Bearer ${tokens.access_token}` }, body: JSON.stringify({ id: "weekly", title: "Weekly" }) });
  assert.equal(created.status, 201);
  assert.deepEqual(((await created.json()) as { organizers: string[] }).organizers, [ORGANIZER]);

  const refreshed = (await (await token({ grant_type: "refresh_token", refresh_token: tokens.refresh_token!, redirect_uri: BRIDGE.redirect_uri })).json()) as Record<string, string>;
  assert.ok(refreshed.access_token && refreshed.access_token !== tokens.access_token);
  assert.equal((await token({ grant_type: "refresh_token", refresh_token: tokens.refresh_token! })).status, 400, "a refresh token is used once");
  assert.equal((await get(`${site}/oauth/me`, { authorization: `Bearer ${refreshed.access_token}` })).status, 200);
  assert.equal((await get(`${site}/oauth/me`, { authorization: "Bearer nope" })).status, 401);
});

test("a player's token signs up but does not create tournaments; a refused sign-in goes back to the bridge", async () => {
  const code = new URL((await throughDiscord(authorize({}), "code-player")).headers.get("location")!).searchParams.get("code")!;
  const tokens = (await (await token({ grant_type: "authorization_code", code, redirect_uri: BRIDGE.redirect_uri })).json()) as Record<string, string>;
  assert.equal(tokens.scope, "player");
  const create = await fetch(`${site}/tournaments`, { method: "POST", headers: { authorization: `Bearer ${tokens.access_token}` }, body: JSON.stringify({ title: "Mine" }) });
  assert.equal(create.status, 403);

  const denied = new URL((await throughDiscord(authorize({}), "")).headers.get("location")!);
  assert.deepEqual([denied.searchParams.get("error"), denied.searchParams.get("state")], ["access_denied", "bridge-state"]);
  assert.equal((await get(authorize({ redirect_uri: "https://evil.example/cb" }))).status, 400);
  assert.equal((await get(authorize({ scope: "admin" }))).status, 400);
  assert.equal((await get(`${site}/oauth/discord/callback?state=unknown&code=x`)).status, 400);
});

test("a browser signs in on the site's page and signs up from it, only from the site's own pages", async () => {
  const organizer = { authorization: "Bearer operator", "x-discord-user": ORGANIZER };
  for (const action of ["plan", "open_sign_ups"]) await fetch(`${site}/tournaments/weekly/state`, { method: "PUT", headers: organizer, body: JSON.stringify({ action }) });
  const page = `${site}/tournaments/weekly/bracket`;
  assert.match(await (await get(page)).text(), /Sign in with Discord/);
  const signedIn = await throughDiscord(`${site}/signin?back=${encodeURIComponent("/tournaments/weekly/bracket")}`, "code-player");
  assert.equal(signedIn.headers.get("location"), "/tournaments/weekly/bracket");
  const cookie = signedIn.headers.get("set-cookie")!.split(";")[0]!;
  assert.match(signedIn.headers.get("set-cookie")!, /HttpOnly; Secure; SameSite=Lax/);
  assert.match(await (await get(page, { cookie })).text(), /data-path="\/tournaments\/weekly\/entrants">Sign up/);

  const signUp = (headers: Record<string, string>) => fetch(`${site}/tournaments/weekly/entrants`, { method: "POST", headers: { cookie, ...headers }, body: "{}" });
  assert.equal((await signUp({})).status, 403, "no Origin: not from the site's page");
  assert.equal((await signUp({ origin: "https://evil.example" })).status, 403);
  assert.equal((await signUp({ origin: ORIGIN })).status, 201);
  const shown = await (await get(page, { cookie })).text();
  assert.match(shown, />Leave</);
  assert.match(shown, /user12 \(waiting for Discord to be connected in Ember\)/);
  // A session is a player's only, and the back address stays on the site.
  assert.equal((await fetch(`${site}/tournaments`, { method: "POST", headers: { cookie, origin: ORIGIN }, body: JSON.stringify({ title: "Mine" }) })).status, 403);
  const away = await throughDiscord(`${site}/signin?back=${encodeURIComponent("//evil.example")}`, "code-player");
  assert.equal(away.headers.get("location"), "/");
});
