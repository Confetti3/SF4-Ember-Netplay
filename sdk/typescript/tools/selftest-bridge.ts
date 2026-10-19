// The bridge side of the two-PC tournament self-test
// (docs/design/TOURNAMENT_SELFTEST.md), started by
// scripts/run-tournament-selftest.ps1:
//
//   node tools/selftest-bridge.ts --bridge <ember-bridge.exe> --work <empty folder>
//     --host <exchange> --guest <exchange> [--port 18787] [--minutes 25]
//
// An exchange is a local folder, or `ssh:<host>:<folder>` on the other PC,
// which then reaches the bridge through an `ssh -R` tunnel this tool holds.
// It runs a fresh local bridge whose mock connection sends results to a
// receiver here, links each game the way a platform does (a link intent, the
// code to the game, the claim checked against the fingerprint the game shows
// and approved), creates a first-to-1 match with the host in slot 0, and
// waits for the bridge to complete it with the host as the winner and deliver
// its signed result. Prints PASS and FAIL lines; exits 0 when all passed.
// The runner writes `stop` in the work folder once both games have closed:
// nothing more will come from them, so whatever is still awaited has failed.
// Test identities and subjects only; everything it starts is stopped and the
// work folder deleted at the end.
import { execFile, execFileSync, spawn, type ChildProcess } from "node:child_process";
import { randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readdirSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { createServer, type Server } from "node:http";
import { join } from "node:path";
import { parseArgs, promisify } from "node:util";

import { BridgeClient, verifyResult, type Json, type MatchResult } from "../src/index.ts";
import { claimToApprove, finishedProblems, parseIdentityLine, type GameIdentity } from "./selftest-checks.ts";

const run = promisify(execFile);
const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const CONNECTION = "mock-local";

const { values: options } = parseArgs({
  options: {
    bridge: { type: "string" },
    work: { type: "string" },
    host: { type: "string" },
    guest: { type: "string" },
    port: { type: "string", default: "18787" },
    minutes: { type: "string", default: "25" },
  },
});
if (!options.bridge || !options.work || !options.host || !options.guest) {
  console.error("usage: node tools/selftest-bridge.ts --bridge <ember-bridge.exe> --work <folder> --host <exchange> --guest <exchange> [--port N] [--minutes N]");
  process.exit(2);
}
const port = Number(options.port);
const deadline = Date.now() + Number(options.minutes) * 60_000;
const stopFile = join(options.work, "stop");
// The games have closed, or the time is up.
const over = () => existsSync(stopFile) || Date.now() > deadline;

let failed = false;
function say(pass: boolean, step: string, detail = ""): void {
  console.log(`${pass ? "PASS" : "FAIL"} ${step}${detail ? `: ${detail}` : ""}`);
  failed ||= !pass;
}

// The files a game and this tool pass each other (sf4e__SelfTest__Tournament.cxx).
interface Exchange {
  name: string;
  read(file: string): Promise<string>;
  write(file: string, line: string): Promise<void>;
}

function localExchange(name: string, folder: string): Exchange {
  return {
    name,
    async read(file) {
      const path = join(folder, file);
      return existsSync(path) ? (readFileSync(path, "utf8").split(/\r?\n/)[0] ?? "").trim() : "";
    },
    async write(file, line) {
      writeFileSync(join(folder, `${file}.tmp`), `${line}\n`);
      renameSync(join(folder, `${file}.tmp`), join(folder, file));
    },
  };
}

// The other PC's folder, through ssh to its default shell (Windows PowerShell).
// Only plain IDs and codes are ever written, so nothing needs quoting but the path.
function remoteExchange(name: string, host: string, folder: string): Exchange {
  if (folder.includes("'")) throw new Error(`the exchange folder ${folder} has a quote in it`);
  const path = (file: string) => `'${folder}\\${file}'`;
  return {
    name,
    async read(file) {
      const { stdout } = await run("ssh", [host, `if (Test-Path -LiteralPath ${path(file)}) { Get-Content -LiteralPath ${path(file)} -TotalCount 1 }`], { timeout: 30_000 });
      return stdout.trim();
    },
    async write(file, line) {
      if (!/^[A-Za-z0-9_.:-]+$/.test(line)) throw new Error(`refusing to write ${JSON.stringify(line)}`);
      await run("ssh", [host, `Set-Content -LiteralPath ${path(`${file}.tmp`)} -Value '${line}'; Move-Item -Force -LiteralPath ${path(`${file}.tmp`)} -Destination ${path(file)}`], { timeout: 30_000 });
    },
  };
}

function exchangeOf(name: string, spec: string): { exchange: Exchange; sshHost?: string } {
  const remote = /^ssh:([^:]+):(.+)$/.exec(spec);
  return remote ? { exchange: remoteExchange(name, remote[1]!, remote[2]!), sshHost: remote[1]! } : { exchange: localExchange(name, spec) };
}

/** Reads `file` until the game has written it, or fails at the deadline. */
async function awaitFile(exchange: Exchange, file: string, what: string): Promise<string> {
  for (;;) {
    const line = await exchange.read(file).catch(() => "");
    if (line) return line;
    if (over()) throw new Error(`${exchange.name}: ${what} never arrived`);
    await sleep(2_000);
  }
}

// Everything started here, stopped in reverse order at the end.
const stops: (() => Promise<void> | void)[] = [];
function own(child: ChildProcess, what: string): ChildProcess {
  stops.push(() => {
    if (child.exitCode === null) child.kill();
  });
  child.on("exit", (code) => {
    if (code !== null && code !== 0) console.log(`${what} exited with ${code}`);
  });
  return child;
}

/** A fresh bridge on `port` whose mock connection sends signed results to `resultsUrl`. */
async function startBridge(resultsUrl: string, secret: string): Promise<{ origin: string; bridgeId: string; providerToken: string }> {
  const binary = options.bridge!;
  const dir = join(options.work!, "bridge");
  if (existsSync(dir) && readdirSync(dir).length > 0) throw new Error(`${dir} is not empty`);
  mkdirSync(dir, { recursive: true });
  stops.push(() => rmSync(options.work!, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 }));
  execFileSync(binary, ["init", dir], { stdio: "ignore" });
  const configPath = join(dir, "bridge.json");
  const config = JSON.parse(readFileSync(configPath, "utf8")) as { [key: string]: Json };
  config.listen = `127.0.0.1:${port}`;
  config.origin = `http://127.0.0.1:${port}`;
  config.integration_secrets = join(dir, "integration-secrets.json");
  const tenants = config.tenants as { connections: { [key: string]: Json }[] }[];
  tenants[0]!.connections[0]!.results_url = resultsUrl;
  writeFileSync(config.integration_secrets, JSON.stringify({ format: "ember-bridge-integrations", version: 1, result_secrets: { [CONNECTION]: secret } }));
  writeFileSync(configPath, JSON.stringify(config));
  const providerToken = execFileSync(binary, ["credential", configPath, "provider", CONNECTION, "self-test runner"], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
  const bridge = own(spawn(binary, ["serve", configPath], { stdio: ["ignore", "ignore", "pipe"] }), "the bridge");
  let log = "";
  bridge.stderr!.on("data", (chunk: Buffer) => (log = (log + chunk.toString()).slice(-4000)));
  const origin = config.origin as string;
  for (let i = 0; i < 100; i++) {
    const profile = await fetch(`${origin}/.well-known/ember-bridge.json`).then((response) => (response.ok ? response.json() : null)).catch(() => null);
    if (profile) return { origin, bridgeId: (profile as { bridge_id: string }).bridge_id, providerToken };
    await sleep(100);
  }
  throw new Error(`the bridge did not start: ${log}`);
}

/** The receiver for the connection's `results_url`: keeps the result whose signature checks out. */
async function startReceiver(secret: string): Promise<{ url: string; result: () => MatchResult | null }> {
  let verified: MatchResult | null = null;
  const server: Server = createServer((request, response) => {
    const chunks: Buffer[] = [];
    request.on("data", (chunk: Buffer) => chunks.push(chunk));
    request.on("end", () => {
      try {
        verified = verifyResult(Buffer.concat(chunks), request.headers, [secret]);
        response.writeHead(204).end();
      } catch (error) {
        console.log(`a result delivery was refused: ${(error as Error).message}`);
        response.writeHead(400).end();
      }
    });
  });
  await new Promise<void>((resolve, reject) => server.once("error", reject).listen(port + 1, "127.0.0.1", resolve));
  stops.push(() => new Promise<void>((resolve) => server.close(() => resolve())));
  return { url: `http://127.0.0.1:${port + 1}/results`, result: () => verified };
}

/** Holds `-R port` to `host` so its loopback reaches the bridge, and checks it does. */
async function startTunnel(host: string, origin: string): Promise<void> {
  own(spawn("ssh", ["-N", "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=15", "-R", `${port}:127.0.0.1:${port}`, host], { stdio: "ignore" }), "the tunnel");
  for (let i = 0; i < 15; i++) {
    await sleep(2_000);
    const { stdout } = await run("ssh", [host, `curl.exe -s ${origin}/.well-known/ember-bridge.json`], { timeout: 30_000 }).catch(() => ({ stdout: "" }));
    if (stdout.includes("bridge_id")) return;
  }
  throw new Error(`${host} cannot reach the bridge through the tunnel`);
}

interface Linked extends GameIdentity {
  participantId: string;
}

/** Links one game the way a platform does, with a code shown to its user only. */
async function link(provider: BridgeClient, exchange: Exchange, subject: string): Promise<Linked> {
  const game = parseIdentityLine(await awaitFile(exchange, "ember-id", "its Ember ID"));
  if (!game) throw new Error(`${exchange.name}: its ember-id file is not "<Ember ID> <build>"`);
  const intent = await provider.createLinkIntent(subject, subject === "player1" ? "Player 1" : "Player 2");
  await exchange.write("link-code", intent.code);
  const fingerprint = await awaitFile(exchange, "claim", "its claim");
  const claimId = claimToApprove(await provider.getLinkIntent(intent.intent_id), game, fingerprint);
  const approved = await provider.approveLinkClaim(intent.intent_id, { claimId, emberId: game.emberId, subject });
  say(true, `${exchange.name} linked`, `${subject} ${game.emberId} (${fingerprint})`);
  return { ...game, participantId: approved.participant_id as string };
}

async function main(): Promise<void> {
  const host = exchangeOf("host", options.host!);
  const guest = exchangeOf("guest", options.guest!);
  const secret = `whsec_${randomBytes(32).toString("base64")}`;
  const receiver = await startReceiver(secret);
  const bridge = await startBridge(receiver.url, secret);
  say(true, "bridge", `${bridge.bridgeId} at ${bridge.origin}`);
  for (const sshHost of new Set([host.sshHost, guest.sshHost].filter((name) => name !== undefined))) {
    await startTunnel(sshHost, bridge.origin);
    say(true, "tunnel", `${sshHost} reaches the bridge`);
  }
  // The runner starts the games once this line is out.
  console.log("READY");

  const provider = new BridgeClient({ origin: bridge.origin, credential: bridge.providerToken });
  const [p1, p2] = await Promise.all([link(provider, host.exchange, "player1"), link(provider, guest.exchange, "player2")]);
  if (p1.build !== p2.build) throw new Error(`the two games run different builds (${p1.build}, ${p2.build}); the bridge would refuse the second claim`);
  const match = await provider.createMatch(
    {
      external_match_id: `selftest-${randomBytes(4).toString("hex")}`,
      participants: [
        { participant_id: p1.participantId, ember_id: p1.emberId, slot: 0 },
        { participant_id: p2.participantId, ember_id: p2.emberId, slot: 1 },
      ],
      games_to_win: 1,
      required_build_id: p1.build,
      native_rules_profile: "ember-room-v1",
      metadata: { round_label: "Self-test" },
    },
    `selftest-${randomBytes(8).toString("hex")}`,
  );
  await Promise.all([host.exchange.write("match", match.match_id), guest.exchange.write("match", match.match_id)]);
  say(true, "match created", match.match_id);

  // Done once the bridge says so and its signed result has arrived. After
  // the games close, the result has a few more seconds to arrive.
  const finished = async () => {
    const current = await provider.getMatch(match.match_id);
    return { current, done: current.state === "completed" && receiver.result() !== null };
  };
  let { current, done } = await finished();
  while (!done && !over()) {
    await sleep(3_000);
    ({ current, done } = await finished());
  }
  if (!done) {
    await sleep(10_000);
    ({ current } = await finished());
  }
  const problems = finishedProblems(current, receiver.result(), { participantId: p1.participantId, emberId: p1.emberId });
  for (const problem of problems) say(false, "match result", problem);
  if (problems.length === 0) say(true, "match result", `completed, ${p1.emberId} won, signed result verified`);
}

try {
  await main();
} catch (error) {
  say(false, "bridge side", (error as Error).message);
} finally {
  for (const stop of stops.reverse()) await stop();
}
console.log(failed ? "RESULT fail" : "RESULT pass");
process.exit(failed ? 1 : 0);
