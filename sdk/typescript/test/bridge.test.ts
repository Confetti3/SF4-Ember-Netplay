// The SDK against the real ember-bridge binary (built by
// `cargo build -p ember-bridge` in rust/ember). Skipped when it is absent.
import assert from "node:assert/strict";
import { execFileSync, spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createServer, type IncomingHttpHeaders } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import {
  BridgeClient,
  BridgeError,
  commandDigest,
  eventType,
  parseEvent,
  parseStrict,
  publicKeyFromSeed,
  signChallenge,
  verifyWebhook,
  type Challenge,
  type Json,
} from "../src/index.ts";

const binary = fileURLToPath(new URL(`../../../rust/ember/target/debug/ember-bridge${process.platform === "win32" ? ".exe" : ""}`, import.meta.url));

async function freePort(): Promise<number> {
  const server = createServer();
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = (server.address() as { port: number }).port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

/** Acts as an Ember helper holding `seed`: opens a session and claims `code`. */
async function claimAsPlayer(origin: string, bridgeId: string, seed: Uint8Array, code: string, connectionId: string): Promise<Record<string, Json>> {
  const publicKey = publicKeyFromSeed(seed);
  const prove = async (token: string | undefined, action: string, path: string, command: Json) => {
    const response = await fetch(`${origin}/v1/auth/challenges`, {
      method: "POST",
      headers: { "content-type": "application/json", ...(token ? { authorization: `Bearer ${token}` } : {}) },
      body: JSON.stringify({ public_key: publicKey, action, method: "POST", path, request_digest: commandDigest(command) }),
    });
    assert.equal(response.status, 201);
    const challenge = parseStrict(await response.text()) as unknown as Challenge;
    const proof = signChallenge(
      seed,
      challenge,
      { bridgeId, audience: origin, emberId: challenge.ember_id, action, method: "POST", path, command },
      Math.floor(Date.now() / 1000),
    );
    return JSON.stringify({ command, proof });
  };
  const sessionBody = await prove(undefined, "session.create", "/v1/sessions", { requested_scopes: ["self:read"] });
  const session = (await (await fetch(`${origin}/v1/sessions`, { method: "POST", body: sessionBody })).json()) as { session_token: string };
  const command = { code, connection_id: connectionId, consent: true };
  const claimBody = await prove(session.session_token, "link.claim", "/v1/link-claims", command);
  const response = await fetch(`${origin}/v1/link-claims`, {
    method: "POST",
    headers: { authorization: `Bearer ${session.session_token}` },
    body: claimBody,
  });
  assert.equal(response.status, 201);
  return (await response.json()) as Record<string, Json>;
}

test("SDK drives a live bridge end to end", { skip: !existsSync(binary) && "build ember-bridge first", timeout: 60_000 }, async () => {
  const dir = mkdtempSync(join(tmpdir(), "ember-sdk-"));
  const child = { process: undefined as ReturnType<typeof spawn> | undefined };
  const receiver = createServer();
  try {
    execFileSync(binary, ["init", dir]);
    const configPath = join(dir, "bridge.json");
    const config = JSON.parse(readFileSync(configPath, "utf8")) as Record<string, Json>;
    const port = await freePort();
    config.listen = `127.0.0.1:${port}`;
    config.origin = `http://127.0.0.1:${port}`;
    writeFileSync(configPath, JSON.stringify(config));
    const origin = config.origin as string;
    const connection = "mock-local";
    const providerToken = execFileSync(binary, ["credential", configPath, "provider", connection, "sdk test"], { encoding: "utf8" }).trim();
    const organizerToken = execFileSync(binary, ["credential", configPath, "organizer", "local", "sdk test"], { encoding: "utf8" }).trim();
    child.process = spawn(binary, ["serve", configPath], { stdio: "ignore" });
    for (let i = 0; i < 100; i++) {
      try {
        if ((await fetch(`${origin}/.well-known/ember-bridge.json`)).ok) break;
      } catch {
        // Not listening yet.
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }

    // A local webhook receiver.
    const deliveries: { headers: IncomingHttpHeaders; body: Buffer }[] = [];
    receiver.on("request", (request, response) => {
      const chunks: Buffer[] = [];
      request.on("data", (chunk: Buffer) => chunks.push(chunk));
      request.on("end", () => {
        deliveries.push({ headers: request.headers, body: Buffer.concat(chunks) });
        response.writeHead(204).end();
      });
    });
    await new Promise<void>((resolve) => receiver.listen(0, "127.0.0.1", resolve));
    const hookUrl = `http://127.0.0.1:${(receiver.address() as { port: number }).port}/hook`;

    const provider = new BridgeClient({ origin, credential: providerToken });
    const organizer = new BridgeClient({ origin, credential: organizerToken });
    const capabilities = await provider.getCapabilities();
    assert.equal(capabilities.native_play, false);
    const profile = (await (await fetch(`${origin}/.well-known/ember-bridge.json`)).json()) as { bridge_id: string };

    const subscription = await organizer.createWebhookSubscription(hookUrl, [eventType("match.created"), eventType("match.completed")]);
    assert.ok(subscription.secret?.startsWith("whsec_"));

    const participants = [];
    for (const [slot, byte] of [[0, 51], [1, 52]] as const) {
      const subject = `sdk-player-${slot}`;
      const intent = await provider.createLinkIntent(subject, subject);
      const claim = await claimAsPlayer(origin, profile.bridge_id, Buffer.alloc(32, byte), intent.code, connection);
      const link = await provider.approveLinkClaim(intent.intent_id, { claimId: claim.claim_id as string, emberId: claim.ember_id as string, subject });
      participants.push({ participant_id: link.participant_id as string, ember_id: claim.ember_id as string, slot });
    }
    const resolved = await provider.resolvePlayers(["sdk-player-0", "nobody"]);
    assert.equal(resolved[0]!.linked, true);
    assert.equal(resolved[1]!.linked, false);

    const created = await provider.createMatch(
      { external_match_id: "sdk-set", participants: participants as never, games_to_win: 2, required_build_id: "sdk", metadata: { round_label: "SDK final" } },
      "sdk-create",
    );
    // A retry with the same key is the same match.
    const again = await provider.createMatch(
      { external_match_id: "sdk-set", participants: participants as never, games_to_win: 2, required_build_id: "sdk", metadata: { round_label: "SDK final" } },
      "sdk-create",
    );
    assert.equal(again.match_id, created.match_id);
    await assert.rejects(provider.recordGame(created.match_id, { winnerSlot: 0 }, "1", "providers cannot"), (error: BridgeError) => error.code === "forbidden");
    await organizer.recordGame(created.match_id, { winnerSlot: 0 }, created.revision as string, "VOD", "g1");
    await organizer.recordGame(created.match_id, { winnerSlot: 0 }, "2", "VOD", "g2");
    const snapshot = await organizer.getMatch(created.match_id);
    assert.equal(snapshot.state, "completed");

    const { events, nextCursor } = await organizer.listEvents();
    assert.ok(events.some((event) => event.type === eventType("match.completed")));
    assert.equal((await organizer.listEvents(nextCursor)).events.length, 0);

    for (let i = 0; i < 100 && deliveries.length < 2; i++) await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(deliveries.length, 2);
    for (const delivery of deliveries) {
      verifyWebhook(delivery.body, delivery.headers as Record<string, string>, [subscription.secret!]);
      parseEvent(delivery.body);
    }
    assert.equal(parseEvent(deliveries[1]!.body).type, eventType("match.completed"));

    // A king-of-the-hill lobby with nobody else waiting: the same two start a
    // new set when one ends.
    const lobby = await provider.createLobby({ external_lobby_id: "sdk-hill", games_to_win: 1, rotation: "winner_stays", required_build_id: "sdk" });
    assert.equal(lobby.state, "open");
    for (const player of participants) await provider.joinLobby(lobby.lobby_id, { participantId: player.participant_id, emberId: player.ember_id });
    const playing = await provider.getLobby(lobby.lobby_id);
    assert.equal(playing.seated.length, 2);
    const set = await organizer.getMatch(playing.current_match_id!);
    await organizer.recordGame(playing.current_match_id!, { winnerSlot: 1 }, set.revision as string, "Called on stream");
    const next = await provider.getLobby(lobby.lobby_id);
    assert.equal(next.sets_completed, 1);
    assert.notEqual(next.current_match_id, playing.current_match_id);
    assert.deepEqual(next.streak, { ember_id: participants[1]!.ember_id, sets: 1 });
    assert.equal(next.standings[0]!.ember_id, participants[1]!.ember_id);
    assert.equal(next.standings[0]!.sets_won, 1);

    // Records add up the first-to-2 set and the lobby set.
    const record = await provider.getPlayerRecord(participants[0]!.ember_id);
    assert.deepEqual(record.sets, { played: 2, won: 1, lost: 1 });
    assert.deepEqual(record.games, { won: 2, lost: 1, drawn: 0 });
    assert.equal(record.recent[0]!.result, "lost");
    assert.equal(record.recent[0]!.lobby_id, lobby.lobby_id);
    const lobbyEvents = (await provider.listEvents("0", 200)).events.map((event) => event.type);
    assert.ok(lobbyEvents.includes(eventType("lobby.set.completed")));
    const closed = await provider.closeLobby(lobby.lobby_id, next.revision, "End of stream");
    assert.equal(closed.state, "closed");
  } finally {
    child.process?.kill();
    receiver.close();
    await new Promise((resolve) => setTimeout(resolve, 200));
    rmSync(dir, { recursive: true, force: true });
  }
});
