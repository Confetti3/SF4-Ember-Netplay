// Starts the real ember-bridge binary (built by `cargo build -p ember-bridge`
// in rust/ember) on a free loopback port with a fresh mock configuration.
import { execFileSync, spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import type { Json } from "../src/index.ts";

export const binary = fileURLToPath(new URL(`../../../rust/ember/target/debug/ember-bridge${process.platform === "win32" ? ".exe" : ""}`, import.meta.url));
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

export async function startBridge(): Promise<RunningBridge> {
  const dir = mkdtempSync(join(tmpdir(), "ember-sdk-"));
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
