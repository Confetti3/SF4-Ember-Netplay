// Player lookup by Discord account and by Challonge username against the real
// ember-bridge (built by `cargo build -p ember-bridge` in server/ember), with
// stand-ins for Discord's and Challonge's sign-in endpoints. Requires
// EMBER_BRIDGE_TEST_BINARY from the tested build.
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { test } from "node:test";

import { BridgeClient, BridgeError, TestPlayer, type Json } from "../src/index.ts";
import { startBridge } from "./bridge-process.ts";

const CLIENT_ID = "1546980049692135514";
const DISCORD_USER = "274220342558756145";
const UNCONNECTED_USER = "80351110224678912";

/** A Discord that signs every code in as `DISCORD_USER`. */
async function startDiscord(): Promise<{ apiBase: string; close(): void }> {
  const server = createServer((request, response) => {
    request.resume();
    request.on("end", () => {
      const body = request.url === "/api/oauth2/token" ? { access_token: "token", token_type: "Bearer", scope: "identify" } : { id: DISCORD_USER, username: "kate" };
      response.writeHead(request.url === "/api/oauth2/token" || request.url === "/api/users/@me" ? 200 : 404, { "content-type": "application/json" }).end(JSON.stringify(body));
    });
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  return { apiBase: `http://127.0.0.1:${(server.address() as { port: number }).port}/api`, close: () => server.close() };
}

test("a connection without discord_lookup is refused", { timeout: 60_000 }, async () => {
  const bridge = await startBridge();
  try {
    const provider = new BridgeClient({ origin: bridge.origin, credential: bridge.providerToken });
    await assert.rejects(provider.lookupPlayers([DISCORD_USER]), (error: BridgeError) => error.status === 403 && error.code === "forbidden");
  } finally {
    await bridge.stop();
  }
});

test("a connection finds players by their connected Discord account", { timeout: 60_000 }, async () => {
  const discord = await startDiscord();
  const bridge = await startBridge({
    config: { discord: { client_id: CLIENT_ID, api_base: discord.apiBase } },
    connection: { discord_lookup: true },
    secrets: { discord_client_secret: "sdk-test-secret" },
  });
  try {
    const { origin } = bridge;
    const provider = new BridgeClient({ origin, credential: bridge.providerToken });
    assert.ok(((await provider.getCapabilities()).features as string[]).includes("players.lookup"));

    // Nobody has connected Discord yet.
    assert.deepEqual(await provider.lookupPlayers([DISCORD_USER]), []);

    // The player connects Discord from Ember: the bridge hands out the sign-in
    // address, and Discord sends the browser back with a code and that state.
    const player = new TestPlayer({ origin });
    const beginConfirmation = async () => {
      const started = await player.startDiscordConnect();
      const state = new URL(started.authorize_url).searchParams.get("state");
      assert.ok(state);
      const callback = await fetch(`${origin}/v1/discord/callback?code=sdk-code&state=${encodeURIComponent(state)}`);
      assert.equal(callback.status, 200);
      const page = await callback.text();
      const confirmation = /name="confirmation" value="([^"]+)"/.exec(page)?.[1];
      assert.ok(confirmation, "the callback requires explicit confirmation");
      assert.deepEqual(await provider.lookupPlayers([DISCORD_USER]), [], "callback must not link the account");
      const links = (await player.links()) as { links: unknown[] };
      assert.deepEqual(links.links, [], "callback must not link the provider");
      return confirmation;
    };
    const decide = (confirmation: string, choice: string) => fetch(`${origin}/v1/discord/callback`, {
      method: "POST", body: new URLSearchParams({ confirmation, choice }),
    });
    const refused = await beginConfirmation();
    assert.equal((await decide(refused, "refuse")).status, 200);
    assert.deepEqual(await provider.lookupPlayers([DISCORD_USER]), []);
    await decide(refused, "connect");
    assert.deepEqual(await provider.lookupPlayers([DISCORD_USER]), [], "a refused confirmation cannot be replayed");
    const confirmed = await decide(await beginConfirmation(), "connect");
    assert.equal(confirmed.status, 200);
    assert.match(await confirmed.text(), /Discord connected/);

    // Found, in the order asked; the ID with no account is left out.
    const found = await provider.lookupPlayers([UNCONNECTED_USER, DISCORD_USER]);
    assert.equal(found.length, 1);
    assert.equal(found[0]!.discord_user_id, DISCORD_USER);
    assert.equal(found[0]!.ember_id, player.emberId);
    assert.ok(found[0]!.participant_id);
    // The player is linked on the connection now, as the same participant.
    assert.deepEqual(await provider.lookupPlayers([DISCORD_USER]), found);
    const links = (await player.links()) as { links: { connection_id: string }[] };
    assert.ok(links.links.some((link) => link.connection_id === bridge.connection));

    // A lookup names at most 32 IDs.
    await assert.rejects(provider.lookupPlayers(Array.from({ length: 33 }, (_, i) => String(1000 + i))), (error: BridgeError) => error.status === 400);
  } finally {
    discord.close();
    await bridge.stop();
  }
});

const CHALLONGE_CLIENT = "sdk-challonge-client";
const DEVICE_CODE = "sdk-device-code-secret";
const ACCESS_TOKEN = "sdk-access-token-secret";

/** A Challonge that approves the device code on its first poll, as `Kate_99`. */
async function startChallonge(): Promise<{ apiBase: string; close(): void }> {
  const server = createServer((request, response) => {
    request.resume();
    request.on("end", () => {
      const path = new URL(request.url ?? "/", "http://challonge").pathname;
      const reply = (status: number, body: Json) => response.writeHead(status, { "content-type": "application/json" }).end(JSON.stringify(body));
      if (path === "/v2.1/oauth/authorize_device") {
        reply(200, {
          device_code: DEVICE_CODE, user_code: "SDKCODE", verification_uri: "https://auth.challonge.com/oauth/device",
          verification_uri_complete: "https://auth.challonge.com/oauth/device?user_code=SDKCODE", expires_in: 300, interval: 2,
        });
      } else if (path === "/oauth/token") {
        reply(200, { access_token: ACCESS_TOKEN, token_type: "Bearer", expires_in: 604800, scope: "me" });
      } else if (path === "/v2.1/me.json" && request.headers.authorization === `Bearer ${ACCESS_TOKEN}`) {
        reply(200, { data: { id: "1001", type: "user", attributes: { username: "Kate_99" } } });
      } else {
        reply(404, {});
      }
    });
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  return { apiBase: `http://127.0.0.1:${(server.address() as { port: number }).port}`, close: () => server.close() };
}

test("a connection without challonge_lookup is refused a Challonge lookup", { timeout: 60_000 }, async () => {
  const bridge = await startBridge({ connection: { discord_lookup: true } });
  try {
    const provider = new BridgeClient({ origin: bridge.origin, credential: bridge.providerToken });
    await assert.rejects(provider.lookupChallongePlayers(["Kate_99"]), (error: BridgeError) => error.status === 403 && error.code === "forbidden");
  } finally {
    await bridge.stop();
  }
});

test("a connection finds players by their connected Challonge account", { timeout: 60_000 }, async () => {
  const challonge = await startChallonge();
  const bridge = await startBridge({
    config: { challonge: { client_id: CHALLONGE_CLIENT, api_base: challonge.apiBase } },
    connection: { challonge_lookup: true },
  });
  try {
    const { origin } = bridge;
    const provider = new BridgeClient({ origin, credential: bridge.providerToken });
    assert.ok(((await provider.getCapabilities()).features as string[]).includes("challonge"));
    assert.deepEqual(await provider.lookupChallongePlayers(["Kate_99"]), []);

    // The player connects Challonge from Ember: the bridge hands out the code,
    // and connects the account once Challonge says it was approved.
    const player = new TestPlayer({ origin });
    const started = await player.startChallongeConnect();
    assert.equal(started.user_code, "SDKCODE");
    assert.equal(started.verification_uri, "https://auth.challonge.com/oauth/device");
    let connection = await player.challonge();
    for (let i = 0; i < 150 && !connection.account; i++) {
      await new Promise((resolve) => setTimeout(resolve, 100));
      connection = await player.challonge();
    }
    assert.equal(connection.account?.username, "Kate_99");
    assert.equal(connection.sign_in, null);

    // Found without case, as asked; a name nobody connected is left out.
    const found = await provider.lookupChallongePlayers(["nobody", "KATE_99"]);
    assert.deepEqual(found.map((player) => [player.username, player.challonge_user_id, player.ember_id]), [["KATE_99", "1001", player.emberId]]);
    assert.ok(found[0]!.participant_id);
    assert.deepEqual(await provider.lookupChallongePlayers(["kate_99"]), [{ ...found[0]!, username: "kate_99" }]);
    // Discord lookup is a setting of its own.
    await assert.rejects(provider.lookupPlayers(["274220342558756145"]), (error: BridgeError) => error.status === 403);
    await assert.rejects(provider.lookupChallongePlayers(["not a name"]), (error: BridgeError) => error.status === 400);

    // Neither the device code nor the token reached the bridge's output.
    const output = bridge.output();
    assert.ok(output.includes("POST /v1/challonge/start 201"), output);
    assert.ok(!output.includes(DEVICE_CODE) && !output.includes(ACCESS_TOKEN), output);
  } finally {
    challonge.close();
    await bridge.stop();
  }
});
