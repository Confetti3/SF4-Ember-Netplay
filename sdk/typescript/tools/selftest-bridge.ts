// The bridge side of the two-PC tournament self-test
// (docs/design/TOURNAMENT_SELFTEST.md), started by
// scripts/run-tournament-selftest.ps1:
//
//   node tools/selftest-bridge.ts --bridge <ember-bridge.exe> --work <missing or empty folder>
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
// Test identities and subjects only; everything it starts is stopped and what
// it created in the work folder deleted at the end, also on Ctrl+C. It refuses
// a work folder that has anything in it, and deletes only what it made: the
// folder itself when it created it, otherwise just its own subfolders and the
// `stop` file.
//
// With --site challonge (docs/design/TOURNAMENT_SELFTEST.md, "Challonge
// mode") the platform is a real Challonge tournament instead: the bridge
// offers Challonge's sign-in, each game connects its account there in the
// browser, and the match is made by the ember-brackets connector, started
// here once both accounts are connected, from the tournament's one open set.
// Needs --challonge <run configuration> and --connector <ember-brackets.exe>.
import { execFile, execFileSync, spawn, type ChildProcess } from "node:child_process";
import { randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { createServer, type Server } from "node:http";
import { join } from "node:path";
import { parseArgs, promisify } from "node:util";

import { BridgeClient, verifyResult, type Json, type MatchResult } from "../src/index.ts";
import {
  accountProblems,
  claimToApprove,
  completedProblems,
  connectorMatchProblems,
  deliveryProblems,
  finishedProblems,
  parseAccountLine,
  parseIdentityLine,
  type GameIdentity,
  type SiteAccount,
} from "./selftest-checks.ts";
import {
  apiFor,
  connectorConfig,
  openProblems,
  parseChallongeConfig,
  participantFor,
  readSecret,
  reportedProblems,
  signInSite,
  type ChallongeConfig,
} from "./selftest-challonge.ts";

const run = promisify(execFile);
const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const SITE = "challonge";

interface Args {
  bridge: string;
  work: string;
  host: string;
  guest: string;
  port: number;
  minutes: number;
  /** The Challonge mode, when --site challonge: the run configuration, the connector and the connection it works through. */
  challonge?: { config: string; connector: string; connection: string };
}

function usage(problem: string): never {
  console.error(`${problem}\nusage: node tools/selftest-bridge.ts --bridge <ember-bridge.exe> --work <folder> --host <exchange> --guest <exchange> [--port N] [--minutes N] [--site challonge --challonge <run configuration> --connector <ember-brackets.exe> [--connection brackets]]`);
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
      site: { type: "string", default: "mock" },
      challonge: { type: "string" },
      connector: { type: "string" },
      connection: { type: "string", default: "brackets" },
    },
  });
  const { bridge, work, host, guest } = values;
  if (!bridge || !work || !host || !guest) usage("--bridge, --work, --host and --guest are needed");
  if (values.site !== "mock" && values.site !== SITE) usage(`--site is mock or ${SITE}`);
  if (values.site === SITE && (!values.challonge || !values.connector)) usage(`--site ${SITE} needs --challonge and --connector`);
  if (values.site === "mock" && (values.challonge || values.connector)) usage("--challonge and --connector are for --site " + SITE);
  // The result receiver, or the connector's results listener, is on the port after the bridge's.
  return {
    bridge,
    work,
    host,
    guest,
    port: whole("--port", values.port, 1024, 65534),
    minutes: whole("--minutes", values.minutes, 1, 24 * 60),
    challonge: values.site === SITE ? { config: values.challonge!, connector: values.connector!, connection: values.connection! } : undefined,
  };
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
  sign_in_sites: Json[];
  tenants: { connections: { [key: string]: Json }[] }[];
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

/** What the two modes set differently in the bridge's configuration. */
interface BridgeSetup {
  /** Fields to set on the tenant's one connection, which `ember-bridge init` made a mock provider. */
  connection: { [key: string]: Json };
  /** Where the connection's signed results go, and the secret they are signed with. */
  resultsUrl: string;
  resultSecret: string;
  /** A sign-in site the bridge offers, and the secret of the OAuth app behind it. */
  site?: { definition: { id: string; [key: string]: Json }; clientSecret: string };
}

// The bridge's own messages (its stderr), the last few thousand characters.
let bridgeLog = "";

/** What this tool creates in the work folder besides the folder itself: the bridge's, the connector's, and the runner's `stop` file. */
const workContents = ["bridge", "connector", "stop"];

/**
 * Takes the work folder, which has to be missing or empty, and arranges for
 * cleanup to delete only what this tool made there: the folder when it made
 * it, otherwise just its own contents. Nothing is deleted when it refuses.
 */
function claimWork(): void {
  const created = !existsSync(args.work);
  if (!created && (!statSync(args.work).isDirectory() || readdirSync(args.work).length > 0)) {
    throw new Error(`${args.work} has to be a missing or empty folder: this tool deletes what it creates there when it ends`);
  }
  mkdirSync(args.work, { recursive: true });
  const remove = (path: string) => rmSync(path, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 });
  stops.push(() => (created ? remove(args.work) : workContents.forEach((name) => remove(join(args.work, name)))));
}

/** A fresh bridge on `port` with the connection `setup` describes. */
async function startBridge(setup: BridgeSetup): Promise<{ origin: string; bridgeId: string; providerToken: string; connection: string }> {
  claimWork();
  const dir = join(args.work, "bridge");
  mkdirSync(dir, { recursive: true });
  execFileSync(args.bridge, ["init", dir], { stdio: "ignore" });
  const configPath = join(dir, "bridge.json");
  const config = readConfig(configPath);
  config.listen = `127.0.0.1:${port}`;
  config.origin = `http://127.0.0.1:${port}`;
  config.integration_secrets = join(dir, "integration-secrets.json");
  const connection = Object.assign(config.tenants[0]!.connections[0]!, setup.connection, { results_url: setup.resultsUrl });
  const id = connection.id;
  if (typeof id !== "string") throw new Error("the connection has no ID");
  if (setup.site) config.sign_in_sites = [setup.site.definition];
  // Owner-only like the bridge's other secret files, and deleted with the work folder.
  const secrets = {
    format: "ember-bridge-integrations",
    version: 1,
    result_secrets: { [id]: setup.resultSecret },
    ...(setup.site ? { site_client_secrets: { [setup.site.definition.id]: setup.site.clientSecret } } : {}),
  };
  writeFileSync(config.integration_secrets, JSON.stringify(secrets), { mode: 0o600 });
  writeFileSync(configPath, JSON.stringify(config));
  const providerToken = execFileSync(args.bridge, ["credential", configPath, "provider", id, "self-test runner"], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
  const bridge = own(spawn(args.bridge, ["serve", configPath], { stdio: ["ignore", "ignore", "pipe"] }), "the bridge");
  bridge.stderr!.on("data", (chunk: Buffer) => (bridgeLog = (bridgeLog + chunk.toString()).slice(-4000)));
  for (let i = 0; i < 100; i++) {
    const profile: unknown = await fetch(`${config.origin}/.well-known/ember-bridge.json`).then((response) => (response.ok ? response.json() : null)).catch(() => null);
    if (isBridgeProfile(profile)) return { origin: config.origin, bridgeId: profile.bridge_id, providerToken, connection: id };
    await sleep(100);
  }
  throw new Error(`the bridge did not start: ${bridgeLog}`);
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

/** A secret the bridge signs a connection's results with, in the form it accepts. */
const newResultSecret = () => `whsec_${randomBytes(32).toString("base64")}`;

/**
 * What both modes begin with: the bridge up with the connection `setup`
 * describes, each remote PC tunnelled to it, and READY said (the runner
 * starts the games once that line is out).
 */
async function serveBridge(setup: BridgeSetup, offering = "") {
  const host = exchangeOf("host", args.host);
  const guest = exchangeOf("guest", args.guest);
  const bridge = await startBridge(setup);
  say(true, "bridge", `${bridge.bridgeId} at ${bridge.origin}${offering}`);
  for (const sshHost of new Set([host.sshHost, guest.sshHost].filter((name) => name !== undefined))) {
    await startTunnel(sshHost, bridge.origin);
    say(true, "tunnel", `${sshHost} reaches the bridge`);
  }
  console.log("READY");
  const provider = new BridgeClient({ origin: bridge.origin, credential: bridge.providerToken });
  return { bridge, provider, host: host.exchange, guest: guest.exchange };
}

/** The mock provider: each game links with a code, and this tool makes the match. */
async function mockMain(): Promise<void> {
  const secret = newResultSecret();
  const receiver = await startReceiver(secret);
  const { provider, host, guest } = await serveBridge({ connection: {}, resultsUrl: receiver.url, resultSecret: secret });
  const [p1, p2] = await Promise.all([link(provider, host, "player1"), link(provider, guest, "player2")]);
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
  await Promise.all([host.write("match", match.match_id), guest.write("match", match.match_id)]);
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

/** Reads each game's `file` until both have written it. */
function bothWrote(host: Exchange, guest: Exchange, file: string, what: string): Promise<[string, string]> {
  return Promise.all([awaitFile(host, file, what), awaitFile(guest, file, what)]);
}

/** Reads until `done` holds of what was read, or `seconds` have passed, and returns the last read. */
async function until<T>(read: () => Promise<T>, done: (value: T) => boolean, seconds: number): Promise<T> {
  const limit = Date.now() + seconds * 1000;
  for (;;) {
    const value = await read();
    if (done(value) || Date.now() > limit) return value;
    await sleep(3_000);
  }
}

/** The Challonge mode: real accounts signed in in the browser, a real set, the connector between them. */
async function siteMain(options: ChallongeConfig, connectorExe: string, connection: string): Promise<void> {
  // The bridge's callback has to be the address registered for the app.
  const origin = `http://127.0.0.1:${port}`;
  if (options.redirectUri !== `${origin}/v1/sites/${SITE}/callback`) {
    throw new Error(`the app's registered redirect URI is ${options.redirectUri}, which this run's bridge at ${origin} does not answer; use the registered port`);
  }
  const organizer = apiFor(options);
  const clientSecret = readSecret(options.clientSecretFile);
  const template = JSON.parse(readFileSync(new URL("../../../server/ember/bridge/deploy/sites/challonge.json", import.meta.url), "utf8")) as Json;

  // The set has to be open before anybody is asked to sign in.
  const notOpen = openProblems(await organizer.match(options.match), options.match);
  if (notOpen.length > 0) throw new Error(notOpen.join("; "));
  say(true, "challonge set", `${options.tournament} match ${options.match} is open`);

  const secret = newResultSecret();
  const resultsPort = port + 1;
  // Once the bridge says READY each game opens the browser for its sign-in,
  // where the person at that PC approves.
  const { bridge, provider, host, guest } = await serveBridge(
    {
      connection: { id: connection, kind: "direct", display_name: "Bracket connector", lookup_sites: [SITE], disputes: "restart" },
      resultsUrl: `http://127.0.0.1:${resultsPort}/results`,
      resultSecret: secret,
      site: { definition: signInSite(template, options.clientId), clientSecret },
    },
    `, offering ${SITE} sign-in`,
  );
  const [hostGame, guestGame] = (await bothWrote(host, guest, "ember-id", "its Ember ID")).map((line, i) => {
    const game = parseIdentityLine(line);
    if (!game) throw new Error(`${i === 0 ? "host" : "guest"}: its ember-id file is not "<Ember ID> <build>"`);
    return game;
  }) as [GameIdentity, GameIdentity];
  if (hostGame.build !== guestGame.build) throw new Error(`the two games run different builds (${hostGame.build}, ${guestGame.build}); the bridge would refuse the second claim`);

  const [hostAccount, guestAccount] = (await bothWrote(host, guest, "site-account", `its ${SITE} account`)).map((line, i) => {
    const account = parseAccountLine(line);
    if (!account) throw new Error(`${i === 0 ? "host" : "guest"}: its site-account file is not "<user ID> <name>"`);
    return account;
  }) as [SiteAccount, SiteAccount];
  const sameAccount = accountProblems(hostAccount, guestAccount);
  if (sameAccount.length > 0) throw new Error(sameAccount.join("; "));
  say(true, "accounts connected", "host and guest, two different accounts");

  // Both players are known to the bridge now, so the connector's first read
  // finds the set playable: the request budget goes on the match, not on
  // reads made while people sign in. Its database is new, as the bridge's is.
  const folder = join(args.work, "connector");
  mkdirSync(folder, { recursive: true });
  writeFileSync(join(folder, "provider.token"), bridge.providerToken, { mode: 0o600 });
  writeFileSync(join(folder, "result.secret"), secret, { mode: 0o600 });
  const configFile = join(folder, "brackets.json");
  const configuration = connectorConfig({
    bridgeOrigin: bridge.origin,
    connectionId: bridge.connection,
    providerTokenFile: join(folder, "provider.token"),
    buildId: hostGame.build,
    resultsPort,
    resultSecretFile: join(folder, "result.secret"),
    database: join(folder, "brackets.sqlite3"),
    tournament: options.tournament,
    apiKeyFile: options.apiKeyFile,
  });
  writeFileSync(configFile, JSON.stringify(configuration));
  const connector = own(spawn(connectorExe, ["serve", configFile], { stdio: ["ignore", "pipe", "pipe"] }), "the connector");
  let connectorLog = "";
  for (const stream of [connector.stdout!, connector.stderr!]) stream.on("data", (chunk: Buffer) => (connectorLog = (connectorLog + chunk.toString()).slice(-6000)));
  // What the connector printed is shown when the run did not pass. Its library prints only through `log!`, which is never given a secret (a test in ember-brackets fails on any other print), and its entry point prints no secret.
  stops.push(() => {
    if (failed && connectorLog) console.log(`connector log:\n${connectorLog.trimEnd()}`);
  });
  say(true, "connector", `started for ${options.tournament}`);

  // The connector made the match, so each game says which it plays.
  const [hostMatch, guestMatch] = await bothWrote(host, guest, "match-id", "the match to play");
  if (hostMatch !== guestMatch) throw new Error(`the games play different matches (${hostMatch}, ${guestMatch})`);
  const created = await provider.getMatch(hostMatch);
  const wrong = connectorMatchProblems(created, `${SITE}:${options.tournament}:${options.match}:`, [hostGame.emberId, guestGame.emberId]);
  if (wrong.length > 0) throw new Error(wrong.join("; "));
  say(true, "match created", `${hostMatch} for ${String(created.external_match_id)}`);

  // The host's fighter wins. The bridge signs the result when the match
  // completes and the connector answers it after verifying the signature;
  // it then reports the set to Challonge.
  while (!over()) await sleep(2_000);
  const winner = { emberId: hostGame.emberId };
  const finished = await until(() => provider.getMatch(hostMatch), (current) => current.state !== "completed" || current.provider_delivery_state === "delivered", 90);
  const problems = [...completedProblems(finished, winner), ...deliveryProblems(finished)];
  for (const problem of problems) say(false, "match result", problem);
  if (problems.length === 0) say(true, "match result", `completed, ${winner.emberId} won, the signed result was delivered`);

  // And the set on Challonge, read with the organizer's key, once the
  // connector has had time to report it. The bridge knows its players by its
  // own participant IDs, so the winner is the participant Challonge shows
  // under the account the host's game connected, and the loser the guest's.
  const [names, firstRead] = await Promise.all([organizer.participants(), organizer.match(options.match)]);
  const winnerId = participantFor(names, hostAccount.name);
  const loserId = participantFor(names, guestAccount.name);
  if (winnerId === null || loserId === null) throw new Error(`Challonge lists ${names.size} participants with a username, and they do not name both connected accounts once each`);
  let reported = firstRead;
  for (let tries = 0; tries < 3 && reported.state !== "complete"; tries++) {
    await sleep(20_000);
    reported = await organizer.match(options.match);
  }
  const unreported = reportedProblems(reported, winnerId, loserId);
  for (const problem of unreported) say(false, "challonge result", problem);
  if (unreported.length === 0) say(true, "challonge result", `match ${options.match} is complete, the host's account won ${reported.scores ?? ""}`);
  say(true, "challonge requests", `${organizer.requests} made by this tool, none of them writes`);
}

try {
  if (args.challonge) await siteMain(parseChallongeConfig(readFileSync(args.challonge.config, "utf8")), args.challonge.connector, args.challonge.connection);
  else await mockMain();
} catch (error) {
  say(false, "bridge side", (error as Error).message);
} finally {
  if (failed && bridgeLog) console.log(`bridge log:\n${bridgeLog.trimEnd()}`);
  await cleanUp();
}
console.log(failed ? "RESULT fail" : "RESULT pass");
process.exit(failed ? 1 : 0);
