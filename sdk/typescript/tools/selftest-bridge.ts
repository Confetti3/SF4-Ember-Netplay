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
// and approved), and creates a first-to-1 match with the host in slot 0.
// The bridge serves until the runner writes `stop` in the work folder, once
// both games have closed: the games learn from it that the match is over.
// Then the match has to be completed with the host as the winner, and its
// signed result delivered. Prints PASS and FAIL lines; exits 0 when all passed.
// Test identities and subjects only; everything it starts is stopped and the
// work folder deleted at the end, also on Ctrl+C.
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

interface Args {
  bridge: string;
  work: string;
  host: string;
  guest: string;
  port: number;
  minutes: number;
}

function usage(problem: string): never {
  console.error(`${problem}\nusage: node tools/selftest-bridge.ts --bridge <ember-bridge.exe> --work <folder> --host <exchange> --guest <exchange> [--port N] [--minutes N]`);
  process.exit(2);
}

/** A whole number from `min` to `max`: a deadline that is NaN would never come. */
function whole(flag: string, text: string | undefined, min: number, max: number): number {
  const value = Number(text);
  if (text === undefined || !Number.isInteger(value) || value < min || value > max) usage(`${flag} must be a whole number from ${min} to ${max}`);
  return value;
}

function parseArguments(): Args {
  const { values } = parseArgs({
    options: {
      bridge: { type: "string" },
      work: { type: "string" },
      host: { type: "string" },
      guest: { type: "string" },
      port: { type: "string", default: "18787" },
      minutes: { type: "string", default: "25" },
    },
  });
  const { bridge, work, host, guest } = values;
  if (!bridge || !work || !host || !guest) usage("--bridge, --work, --host and --guest are needed");
  // The result receiver listens on the port after the bridge's.
  return { bridge, work, host, guest, port: whole("--port", values.port, 1024, 65534), minutes: whole("--minutes", values.minutes, 1, 24 * 60) };
}

const args = parseArguments();
const { port } = args;
const deadline = Date.now() + args.minutes * 60_000;
const stopFile = join(args.work, "stop");
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
let cleaning: Promise<void> | undefined;
/** Runs every stop once. What could not be cleaned up is said, and the rest still goes. */
function cleanUp(): Promise<void> {
  cleaning ??= (async () => {
    for (const stop of stops.reverse()) await Promise.resolve().then(stop).catch((error: Error) => console.log(`cleanup: ${error.message}`));
  })();
  return cleaning;
}
// Ctrl+C reaches this process with the runner's: the bridge, tunnel and work folder go first.
for (const signal of ["SIGINT", "SIGTERM"] as const) process.on(signal, () => void cleanUp().then(() => process.exit(130)));
// A child is stopped and waited for: its files stay open until it has exited.
function own(child: ChildProcess, what: string): ChildProcess {
  const exited = new Promise<void>((resolve) => child.once("exit", () => resolve()));
  stops.push(async () => {
    if (child.exitCode === null && child.signalCode === null) child.kill();
    await Promise.race([exited, sleep(10_000)]);
  });
  child.on("exit", (code) => {
    if (code !== null && code !== 0) console.log(`${what} exited with ${code}`);
  });
  return child;
}

/** The parts of the bridge's `bridge.json` this tool sets; the rest is written back as it was. */
type BridgeConfig = { [key: string]: Json } & {
  listen: string;
  origin: string;
  integration_secrets: string;
  tenants: { connections: { results_url?: string }[] }[];
};

function readConfig(path: string): BridgeConfig {
  const config = JSON.parse(readFileSync(path, "utf8")) as BridgeConfig;
  if (!config.tenants?.[0]?.connections?.[0]) throw new Error(`${path} has no tenant with a connection; \`ember-bridge init\` changed its shape`);
  return config;
}

/** What the bridge's profile document says of itself. */
function isBridgeProfile(value: unknown): value is { bridge_id: string } {
  return typeof value === "object" && value !== null && "bridge_id" in value && typeof value.bridge_id === "string";
}

/** A fresh bridge on `port` whose mock connection sends signed results to `resultsUrl`. */
async function startBridge(resultsUrl: string, secret: string): Promise<{ origin: string; bridgeId: string; providerToken: string }> {
  const dir = join(args.work, "bridge");
  if (existsSync(dir) && readdirSync(dir).length > 0) throw new Error(`${dir} is not empty`);
  mkdirSync(dir, { recursive: true });
  stops.push(() => rmSync(args.work, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 }));
  execFileSync(args.bridge, ["init", dir], { stdio: "ignore" });
  const configPath = join(dir, "bridge.json");
  const config = readConfig(configPath);
  config.listen = `127.0.0.1:${port}`;
  config.origin = `http://127.0.0.1:${port}`;
  config.integration_secrets = join(dir, "integration-secrets.json");
  config.tenants[0]!.connections[0]!.results_url = resultsUrl;
  writeFileSync(config.integration_secrets, JSON.stringify({ format: "ember-bridge-integrations", version: 1, result_secrets: { [CONNECTION]: secret } }));
  writeFileSync(configPath, JSON.stringify(config));
  const providerToken = execFileSync(args.bridge, ["credential", configPath, "provider", CONNECTION, "self-test runner"], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
  const bridge = own(spawn(args.bridge, ["serve", configPath], { stdio: ["ignore", "ignore", "pipe"] }), "the bridge");
  let log = "";
  bridge.stderr!.on("data", (chunk: Buffer) => (log = (log + chunk.toString()).slice(-4000)));
  for (let i = 0; i < 100; i++) {
    const profile: unknown = await fetch(`${config.origin}/.well-known/ember-bridge.json`).then((response) => (response.ok ? response.json() : null)).catch(() => null);
    if (isBridgeProfile(profile)) return { origin: config.origin, bridgeId: profile.bridge_id, providerToken };
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
  const participantId = approved.participant_id;
  if (typeof participantId !== "string") throw new Error(`${exchange.name}: the bridge approved the claim without a participant ID`);
  say(true, `${exchange.name} linked`, `${subject} ${game.emberId} (${fingerprint})`);
  return { ...game, participantId };
}

async function main(): Promise<void> {
  const host = exchangeOf("host", args.host);
  const guest = exchangeOf("guest", args.guest);
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

  // The games learn the match is over from the bridge, whose answer to their
  // next claim says it finished, so it serves until both have closed. The
  // match and its signed result, sent when it completed, are checked then.
  while (!over()) await sleep(2_000);
  const current = await provider.getMatch(match.match_id);
  const problems = finishedProblems(current, receiver.result(), { participantId: p1.participantId, emberId: p1.emberId });
  for (const problem of problems) say(false, "match result", problem);
  if (problems.length === 0) say(true, "match result", `completed, ${p1.emberId} won, signed result verified`);
}

try {
  await main();
} catch (error) {
  say(false, "bridge side", (error as Error).message);
} finally {
  await cleanUp();
}
console.log(failed ? "RESULT fail" : "RESULT pass");
process.exit(failed ? 1 : 0);
