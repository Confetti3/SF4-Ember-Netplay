// Starts the real ember-bridge binary (built by `cargo build -p ember-bridge`
// in server/ember) on a free loopback port with a fresh mock configuration.
import { execFileSync, spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createServer, type IncomingHttpHeaders } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import type { BridgeClient, Json, TestPlayer } from "../src/index.ts";

export const binary = fileURLToPath(new URL(`../../../server/ember/target/debug/ember-bridge${process.platform === "win32" ? ".exe" : ""}`, import.meta.url));
export const missing = !existsSync(binary) && "build ember-bridge first";

export interface RunningBridge {
  origin: string;
  connection: string;
  providerToken: string;
  organizerToken: string;
  stop(): Promise<void>;
}

async function freePort(): Promise<number> {
  const server = createServer();
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = (server.address() as { port: number }).port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

/** What a test changes in the bridge `init` writes. */
export interface BridgeOptions {
  /** Top-level settings, such as `discord` or `rooms`. */
  config?: Record<string, Json>;
  /** Settings of the `mock-local` connection: `discord_lookup`, `results_url`, `rooms`. */
  connection?: Record<string, Json>;
  /** The integration secrets file: `discord_client_secret`, `rooms_supervisor_secret`, `result_secrets`. */
  secrets?: Record<string, Json>;
}

export async function startBridge(options: BridgeOptions = {}): Promise<RunningBridge> {
  const dir = mkdtempSync(join(tmpdir(), "ember-sdk-"));
  execFileSync(binary, ["init", dir]);
  const configPath = join(dir, "bridge.json");
  const config = JSON.parse(readFileSync(configPath, "utf8")) as Record<string, Json>;
  const port = await freePort();
  config.listen = `127.0.0.1:${port}`;
  config.origin = `http://127.0.0.1:${port}`;
  Object.assign(config, options.config);
  const tenants = config.tenants as { connections: Record<string, Json>[] }[];
  Object.assign(tenants[0]!.connections[0]!, options.connection);
  if (options.secrets) {
    config.integration_secrets = "integration-secrets.json";
    writeFileSync(join(dir, "integration-secrets.json"), JSON.stringify({ format: "ember-bridge-integrations", version: 1, ...options.secrets }));
  }
  writeFileSync(configPath, JSON.stringify(config));
  const origin = config.origin as string;
  const connection = "mock-local";
  const providerToken = execFileSync(binary, ["credential", configPath, "provider", connection, "sdk test"], { encoding: "utf8" }).trim();
  const organizerToken = execFileSync(binary, ["credential", configPath, "organizer", "local", "sdk test"], { encoding: "utf8" }).trim();
  const child = spawn(binary, ["serve", configPath], { stdio: "ignore" });
  for (let i = 0; i < 100; i++) {
    try {
      if ((await fetch(`${origin}/.well-known/ember-bridge.json`)).ok) break;
    } catch {
      // Not listening yet.
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  return {
    origin,
    connection,
    providerToken,
    organizerToken,
    async stop() {
      child.kill();
      await new Promise((resolve) => setTimeout(resolve, 200));
      rmSync(dir, { recursive: true, force: true });
    },
  };
}

/** Links a stand-in player to the connection, the way a platform's user would. */
export async function linkPlayer(provider: BridgeClient, connection: string, player: TestPlayer, subject: string): Promise<{ participant_id: string; ember_id: string }> {
  const intent = await provider.createLinkIntent(subject);
  const claim = await player.claimLink(intent.code, connection);
  const link = await provider.approveLinkClaim(intent.intent_id, { claimId: claim.claim_id!, emberId: player.emberId, subject });
  return { participant_id: link.participant_id as string, ember_id: player.emberId };
}

export interface Delivery {
  headers: IncomingHttpHeaders;
  body: Buffer;
}

/** A local HTTP receiver that stores each POST and answers 204. */
export async function startReceiver(): Promise<{ url: string; deliveries: Delivery[]; close(): void }> {
  const deliveries: Delivery[] = [];
  const server = createServer((request, response) => {
    const chunks: Buffer[] = [];
    request.on("data", (chunk: Buffer) => chunks.push(chunk));
    request.on("end", () => {
      deliveries.push({ headers: request.headers, body: Buffer.concat(chunks) });
      response.writeHead(204).end();
    });
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  return { url: `http://127.0.0.1:${(server.address() as { port: number }).port}/hook`, deliveries, close: () => server.close() };
}

/** Polls until `done` holds, or fails after `seconds`. */
export async function until(done: () => boolean | Promise<boolean>, what: string, seconds = 20): Promise<void> {
  for (let i = 0; i < seconds * 10; i++) {
    if (await done()) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`timed out waiting for ${what}`);
}
