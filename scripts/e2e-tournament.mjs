#!/usr/bin/env node
// A whole tournament on two PCs with nobody at either game
// (docs/guides/TOURNAMENT_TEST.md): the organizer's game on this PC signs the
// web desk in with the code and its number, the desk creates a Challonge
// tournament with sign-ups, both games sign up, the desk readies it and
// starts the bracket, the games play the set (a draw, then a win), the score
// reaches Challonge, and the desk stops it and deletes it there. Pictures of
// the desk and of both games go to the out folder.
//
//   node scripts/e2e-tournament.mjs [--config <file>] [--keep]
//
// Runs from WSL on the organizer's PC. Both games are self-test builds
// (lab: stage-tournament-selftest) run by scripts/run-selftest.ps1, here and,
// through ssh and a scheduled task in the signed-in session, on the other PC.
// Each keeps its Ember ID in a folder of its own; the first run waits for a
// person to approve Challonge in each PC's browser (the player's account on
// both, organizer access here), later runs need nobody. --keep leaves the
// tournament on Challonge. The config (default ~/tournament-lab/e2e/games.json):
//
//   { "bridge": "https://ember-bridge.fraccie.be", "connection": "desk",
//     "out": "/mnt/d/ember-build/e2e-games/out",
//     "local": { "package": "C:\\...\\stage-tournament-selftest", "work": "D:\\ember-build\\e2e-games" },
//     "remote": { "ssh": "user@host", "package": "C:\\ember-ci\\tournament-selftest\\stage",
//                 "work": "C:\\ember-ci\\tournament-selftest", "task": "EmberSelfTest" } }
import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";
import { chrome, layout, must, results, sleep, step } from "./e2e/cdp.mjs";

const args = process.argv.slice(2);
const configFile = args.includes("--config") ? args[args.indexOf("--config") + 1] : join(homedir(), "tournament-lab/e2e/games.json");
const config = JSON.parse(readFileSync(configFile, "utf8"));
const bridge = config.bridge.replace(/\/$/, "");
const out = config.out;
mkdirSync(out, { recursive: true });
const repo = new URL("..", import.meta.url).pathname;
const linux = (windows) => execFileSync("wslpath", ["-u", windows], { encoding: "utf8" }).trim();
const ssh = (command) => execFileSync("ssh", ["-o", "BatchMode=yes", config.remote.ssh, command], { encoding: "utf8" }).replace(/\r/g, "");
const scp = (from, to) => execFileSync("scp", ["-q", "-o", "BatchMode=yes", from, config.remote.ssh + ":" + to.replace(/\\/g, "/")]);
const log = (...text) => console.log(new Date().toTimeString().slice(0, 8), ...text);

// --- The exchange folders: this PC's on disk, the other's through ssh.

const local = { work: config.local.work, dir: join(linux(config.local.work), "exchange") };
const remote = { work: config.remote.work, dir: config.remote.work + "\\exchange" };
const read = (side, name) => {
  if (side === local) return existsSync(join(local.dir, name)) ? readFileSync(join(local.dir, name), "utf8").split(/\r?\n/)[0].trim() : "";
  try { return ssh(`type "${remote.dir}\\${name}" 2>nul`).split("\n")[0].trim(); } catch { return ""; }
};
const write = (side, name, line) => {
  if (side === local) return writeFileSync(join(local.dir, name), line + "\n");
  const file = join(out, ".exchange-" + name);
  writeFileSync(file, line + "\r\n");
  scp(file, remote.dir + "\\" + name);
  rmSync(file);
};
// A game that closed early ends every wait with its own words.
const closed = [];
// ...and the run: the steps after it would only wait out their timeouts, so
// it goes straight to deleting its tournament.
async function play(name, run) {
  await step(name, run);
  if (closed.length) throw new Error(closed.join("; "));
}
async function wait(side, name, minutes, what) {
  const end = Date.now() + minutes * 60000;
  for (;;) {
    if (closed.length) throw new Error(closed.join("; "));
    const line = read(side, name);
    if (line) return line;
    if (Date.now() > end) throw new Error("timed out waiting for " + (what || name));
    await sleep(3000);
  }
}

// --- The games.

const env = { SF4E_SELFTEST_SIGN_UP: "1", SF4E_SELFTEST_DRAW_FIRST: "1" };
// Each game's Ember folder: the config's own (`ember`, such as a PC's real
// Ember ID that holds the organizer's accounts), else one kept in its work folder.
const emberDir = (side) => {
  const pc = side === local ? config.local : config.remote;
  return pc.ember || pc.work + "\\ember";
};
const selftest = (side, test) => ["-PackageDir", side === local ? config.local.package : config.remote.package,
  "-Test", test, "-Exchange", (side === local ? config.local.work : config.remote.work) + "\\exchange", "-Bridge", bridge,
  "-Connection", config.connection, "-Site", "challonge", "-KeptEmberDir", emberDir(side),
  "-TimeoutMinutes", "45"];

function startLocal() {
  const work = linux(config.local.work);
  rmSync(local.dir, { recursive: true, force: true });
  mkdirSync(local.dir, { recursive: true });
  copyFileSync(join(repo, "scripts/run-selftest.ps1"), join(work, "run-selftest.ps1"));
  const vars = { ...env, SF4E_SELFTEST_ORGANIZER: "1" };
  const child = spawn("powershell.exe", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", config.local.work + "\\run-selftest.ps1", ...selftest(local, "tournament-host")],
    { env: { ...process.env, ...vars, WSLENV: [process.env.WSLENV, ...Object.keys(vars)].filter(Boolean).join(":") }, stdio: ["ignore", "pipe", "pipe"] });
  let text = "";
  child.stdout.on("data", (data) => { text += data; });
  child.stderr.on("data", (data) => { text += data; });
  return new Promise((resolve) => child.on("exit", (code) => resolve({ code, text: text.replace(/\r/g, "") })));
}

function startRemote() {
  const w = config.remote.work;
  // The other PC's package is copied when this PC's differs from the copy there.
  const stamp = execFileSync("stat", ["-c", "%s-%Y", join(linux(config.local.package), "sf4-net.exe")], { encoding: "utf8" }).trim();
  if (ssh(`type "${w}\\stage.stamp" 2>nul || echo none`).trim() !== stamp) {
    log("copying the self-test package to the other PC");
    const tar = join(out, ".stage.tar");
    execFileSync("tar", ["-cf", tar, "-C", linux(config.local.package), "."]);
    ssh(`powershell -NoProfile -Command "Remove-Item -Recurse -Force '${config.remote.package}' -EA 0; New-Item -ItemType Directory -Force '${config.remote.package}' | Out-Null"`);
    scp(tar, w + "\\stage.tar");
    rmSync(tar);
    ssh(`powershell -NoProfile -Command "tar -xf '${w}\\stage.tar' -C '${config.remote.package}'; Remove-Item '${w}\\stage.tar'; Set-Content '${w}\\stage.stamp' '${stamp}'"`);
  }
  ssh(`powershell -NoProfile -Command "Remove-Item -Recurse -Force '${remote.dir}','${w}\\out.txt' -EA 0; New-Item -ItemType Directory -Force '${remote.dir}' | Out-Null"`);
  scp(join(repo, "scripts/run-selftest.ps1"), w + "\\run-selftest.ps1");
  const job = join(out, ".run.cmd");
  const quoted = selftest(remote, "tournament-guest").map((arg) => (/[\s]/.test(arg) ? `"${arg}"` : arg)).join(" ");
  writeFileSync(job, "@echo off\r\n" + Object.entries(env).map(([k, v]) => `set ${k}=${v}\r\n`).join("") +
    `powershell -NoProfile -ExecutionPolicy Bypass -File "${w}\\run-selftest.ps1" ${quoted} > "${w}\\out.txt" 2>&1\r\necho EXIT %ERRORLEVEL% >> "${w}\\out.txt"\r\n`);
  scp(job, w + "\\run.cmd");
  rmSync(job);
  // A process started over ssh has no desktop: the game starts through a task
  // that runs in the signed-in user's session.
  ssh(`powershell -NoProfile -Command "$a=New-ScheduledTaskAction -Execute '${w}\\run.cmd'; $p=New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive; Register-ScheduledTask ${config.remote.task} -Action $a -Principal $p -Force | Out-Null; Start-ScheduledTask ${config.remote.task}"`);
  return (async () => {
    for (;;) {
      await sleep(10000);
      const text = (() => { try { return ssh(`type "${w}\\out.txt" 2>nul`); } catch { return ""; } })();
      const exit = text.match(/EXIT (\d+)/);
      if (exit) return { code: Number(exit[1]), text };
    }
  })();
}

// A picture of a game's window: on this PC through PowerShell, on the other
// through a task in its signed-in session.
const shotScript = `param([string]$Path)
Add-Type -AssemblyName System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices; public static class W { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r); public struct R { public int L, T, Rt, B; } }'
$g = Get-Process SSFIV* -EA 0 | ? MainWindowHandle -ne 0 | Select -First 1
if (-not $g) { exit 1 }
[void][W]::SetForegroundWindow($g.MainWindowHandle); Start-Sleep -Milliseconds 300
$r = New-Object W+R; [void][W]::GetWindowRect($g.MainWindowHandle, [ref]$r)
$b = New-Object System.Drawing.Bitmap ($r.Rt - $r.L), ($r.B - $r.T); [System.Drawing.Graphics]::FromImage($b).CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
$b.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
`;
function shotLocal(name) {
  const work = linux(config.local.work);
  writeFileSync(join(work, "shot.ps1"), shotScript);
  try {
    execFileSync("powershell.exe", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", config.local.work + "\\shot.ps1", "-Path", config.local.work + "\\shot.png"]);
    copyFileSync(join(work, "shot.png"), join(out, name + ".png"));
  } catch { log("no picture of this PC's game for", name); }
}
async function shotRemote(name) {
  const w = config.remote.work;
  const script = join(out, ".shot.ps1");
  writeFileSync(script, shotScript);
  scp(script, w + "\\shot.ps1");
  rmSync(script);
  ssh(`powershell -NoProfile -Command "Remove-Item '${w}\\shot.png' -EA 0; $a=New-ScheduledTaskAction -Execute powershell.exe -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ${w}\\shot.ps1 -Path ${w}\\shot.png'; $p=New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive; Register-ScheduledTask EmberShot -Action $a -Principal $p -Force | Out-Null; Start-ScheduledTask EmberShot"`);
  for (let i = 0; i < 20; i++) {
    await sleep(1000);
    try { execFileSync("scp", ["-q", "-o", "BatchMode=yes", config.remote.ssh + ":" + (w + "\\shot.png").replace(/\\/g, "/"), join(out, name + ".png")]); return; } catch {}
  }
  log("no picture of the other PC's game for", name);
}
const shots = (name) => Promise.all([shotLocal(name + "-organizer"), shotRemote(name + "-player")]);

// --- The run.

const webhookFile = join(homedir(), "tournament-lab/e2e/discord-webhook");
const webhook = existsSync(webhookFile) ? readFileSync(webhookFile, "utf8").trim() : "";

log("starting both games against", bridge, webhook ? "with Discord calls" : "without Discord");
const ended = [false, false];
const games = [startLocal(), startRemote()].map((game, index) => game.then((result) => {
  ended[index] = true;
  const fails = (result.text.match(/FAIL.*/g) || []).join("; ");
  // A game that passed has done its part; one that failed ends the run.
  if (result.code !== 0 || fails) closed.push((index ? "the other PC's game" : "this PC's game") + " closed: " + (fails || "exit " + result.code));
  return result;
}));
// A run that ends early stops the games it started, so the next run finds
// both PCs free. Each game's script then puts the saves back as after any game.
// Only the package's launcher and the game it started, so one stuck before
// the game opened goes too: a player's own game on either PC is left alone.
const stopScript = (pkg) => "Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -like '" + pkg + "\\*' -or " +
  `($_.Name -eq 'SSFIV.exe' -and (Get-Process -Id $_.ParentProcessId -EA 0).Path -like '${pkg}\\*') } | Sort-Object Name -Descending | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }`;
function stopGames() {
  if (!ended[0]) { try { execFileSync("powershell.exe", ["-NoProfile", "-Command", stopScript(config.local.package)], { stdio: "ignore" }); } catch {} }
  if (!ended[1]) { try { ssh(`powershell -NoProfile -Command "${stopScript(config.remote.package)}"`); } catch {} }
}
for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, () => { stopGames(); process.exit(130); });
const page = await chrome(out);
let event = null;

try {
  await play("both games connected their Challonge accounts", async () => {
    log("first run: approve Challonge in the browser on each PC (5 minutes each)");
    await Promise.all([wait(local, "site-account", 12, "this PC's account"), wait(remote, "site-account", 12, "the other PC's account")]);
    await shots("01-connected");
  });

  await play("the organizer's game signs the web desk in with the code and its number", async () => {
    await page.go(bridge + "/desk");
    await page.until("!document.getElementById('signed-out').hidden", 15000);
    await layout(page, "02-desk-signed-out", true);
    await page.eval("document.getElementById('device-show').click()");
    await page.until("/^\\d{3} \\d{3}$/.test(document.getElementById('device-code').textContent)");
    const code = (await page.eval("document.getElementById('device-code').textContent")).replace(" ", "");
    await layout(page, "03-desk-code");
    log("organizer access: first run, approve it in this PC's browser");
    write(local, "desk-code", code);
    const offered = (await wait(local, "desk-device", 12, "the game reading the code")).split(" ")[0].split(",");
    await page.until("!document.getElementById('device-pick').hidden", 15000);
    const number = await page.eval("document.getElementById('device-number').textContent");
    await layout(page, "04-desk-number");
    must(offered.includes(number) && offered.length === 3, "the game offers " + offered + ", the page shows " + number);
    write(local, "desk-number", number);
    await wait(local, "desk-approved", 2, "the game approving");
    await page.until("!document.getElementById('home').hidden", 15000);
    // A run that failed may have left its tournament: deleted here, on Challonge too.
    const left = await page.eval(`api('GET', '/v1/organizer/events').then((r) => Promise.all(r.events
      .filter((e) => e.title.startsWith('ember-2p ') && e.created_on_site)
      .map((e) => api('DELETE', '/v1/organizer/events/' + e.id, { on_site: true }).then(() => 'deleted', (error) => error.message))))
      .then((done) => done.join('; '))`).catch((error) => error.message);
    if (left) log("tournaments an earlier run left:", left);
    await page.eval("home()");
    await layout(page, "05-desk-home", true);
  });

  await play("the desk creates a single elimination Challonge tournament, first to 1, with sign-ups", async () => {
    await page.eval("document.getElementById('new-tournament').click()");
    await page.until("!document.getElementById('wizard').hidden");
    await page.eval(`document.getElementById('title').value = 'ember-2p ' + new Date().toISOString().slice(0, 16);
      (() => { const now = new Date(Date.now() - 60000), pad = (n) => String(n).padStart(2, '0');
        document.getElementById('date').value = now.getFullYear() + '-' + pad(now.getMonth() + 1) + '-' + pad(now.getDate());
        document.getElementById('time').value = pad(now.getHours()) + ':' + pad(now.getMinutes()); })()`);
    await layout(page, "06-details", true);
    await page.eval("document.getElementById('next').click()");
    await page.until("/Service/.test(document.getElementById('step-now').textContent)", 15000);
    event = await page.eval("desk.event.id");
    await page.eval(`document.querySelector('[data-field=format]').value = JSON.stringify('single elimination');
      document.getElementById('games').value = '1'`);
    await layout(page, "07-service", true);
    await page.eval("document.getElementById('next').click()");
    await page.until("/Discord/.test(document.getElementById('step-now').textContent)", 20000);
    must(await page.eval("Boolean(desk.event.tournament)"), "no Challonge tournament");
    // With a webhook in ~/tournament-lab/e2e/discord-webhook, calls and scores
    // go to that Discord channel; it is never printed.
    await page.eval(`document.querySelectorAll('#live-settings input[type=checkbox]').forEach((box) => { box.checked = ${JSON.stringify(Boolean(webhook))} || !/score/.test(box.parentElement.textContent); })`);
    // The picture is taken before the webhook is typed: it would show it.
    await layout(page, "08-discord", true);
    if (webhook) await page.eval(`document.querySelector('#live-settings input[type=url]').value = ${JSON.stringify(webhook)}`);
    await page.eval("document.getElementById('next').click()");
    await page.until("/Ready/.test(document.getElementById('step-now').textContent)");
    must(await page.eval("desk.event.sign_ups"), "sign-ups are not on");
  });

  await play("both games sign up for it in Ember", async () => {
    write(local, "event", event);
    write(remote, "event", event);
    await Promise.all([wait(local, "signed-up", 5, "this PC's sign-up"), wait(remote, "signed-up", 5, "the other PC's sign-up")]);
    await page.eval("refreshEvent()");
    await page.until("desk.event.signed_up === 2", 15000);
    await layout(page, "09-ready", true);
  });

  await play("the desk readies it and starts the bracket on Challonge", async () => {
    await page.eval("document.getElementById('next').click()");
    await page.until("desk.event.status !== 'draft'", 15000);
    await page.eval("go(5)");
    await page.until("!document.getElementById('start-bracket').hidden", 30000);
    await layout(page, "10-start");
    await page.eval("document.getElementById('start-bracket').click()");
    await page.until("document.getElementById('start-bracket').hidden || /Bracket started/.test(document.body.textContent)", 20000);
    // The bracket connector reads Challonge every two minutes at most.
    log("waiting for the connector to read the bracket");
    for (const end = Date.now() + 12 * 60000; !(await page.eval("readSets().then(() => view.sets.length > 0)")); await sleep(10000))
      if (Date.now() > end) throw new Error("the connector never read the bracket");
    await layout(page, "11-live");
  });

  await play("the games play the set: a draw on time, then the organizer's win", async () => {
    let shot = 0;
    for (const end = Date.now() + 15 * 60000; ;) {
      await page.eval("readSets()");
      if (await page.eval("view.sets.some((set) => set.desk_state === 'done')")) break;
      if (Date.now() > end) throw new Error("the set never finished");
      if (shot < 4 && (await page.eval("view.sets.some((set) => set.desk_state === 'playing')"))) await shots("12-playing-" + ++shot);
      await sleep(15000);
    }
    await layout(page, "13-done");
    if (webhook) must(/posted on Discord/.test(await page.eval("document.getElementById('live-body').textContent")), "the call did not reach Discord");
    // Players with Discord linked get the match link privately from the Ember bot.
    const dm = await page.eval("(document.getElementById('live-body').textContent.match(/Private message: [^\\n]*?(?=Games|$)/) || [''])[0]");
    log(dm || "no private messages (the bridge has no Discord bot)");
    if (dm) must(!/refused/.test(dm), dm);
    const set = JSON.parse(await page.eval("JSON.stringify(view.sets.find((set) => set.desk_state === 'done'))"));
    must(set.score && Math.max(...set.score) === 1 && Math.min(...set.score) === 0, "score " + JSON.stringify(set.score));
    must(/1 draw/.test(await page.eval("document.getElementById('live-body').textContent")), "the draw is not shown");
  });

  await play("both games finished their part", async () => {
    const [mine, theirs] = await Promise.all(games);
    writeFileSync(join(out, "organizer-game.txt"), mine.text);
    writeFileSync(join(out, "player-game.txt"), theirs.text);
    must(mine.code === 0, "this PC's game: " + (mine.text.match(/FAIL.*/g) || ["exit " + mine.code]).join("; "));
    must(theirs.code === 0, "the other PC's game: " + (theirs.text.match(/FAIL.*/g) || ["exit " + theirs.code]).join("; "));
  });

  await play("the public page shows the result", async () => {
    await page.go(bridge + "/t/" + event);
    await page.until("document.body.textContent.includes('ember-2p')", 15000);
    await layout(page, "14-public");
  });

  if (!args.includes("--keep")) {
    await play("stopped and deleted on Challonge", async () => {
      await page.go(bridge + "/desk");
      await page.until("!document.getElementById('home').hidden", 15000);
      await page.eval(`home().then(() => open(desk.events.find((e) => e.id === ${JSON.stringify(event)})))`);
      await page.until("desk.event && desk.event.created_on_site");
      const tournament = await page.eval("desk.event.tournament");
      await page.eval("go(5); document.getElementById('stop').click()");
      await page.until("document.getElementById('decide').open");
      await layout(page, "15-stop");
      await page.eval("[...document.querySelectorAll('#decide button')].find((b) => /delete it on Challonge/.test(b.textContent)).click()");
      await page.until("!document.getElementById('decide').open && !desk.event", 20000);
      const listed = await page.eval("api('GET', '/v1/organizer/tournaments?site=challonge').then((r) => JSON.stringify(r.tournaments))");
      must(!listed.includes(JSON.stringify(tournament)), "still on Challonge");
      event = null;
    });
  }
} finally {
  if (event && !args.includes("--keep")) {
    const status = await page.eval(`api('DELETE', '/v1/organizer/events/${event}', { on_site: true }).then(() => 'deleted', (e) => e.message)`).catch((e) => e.message);
    log("this run's tournament, with Challonge's:", status);
  }
  page.close();
  if (!ended.every(Boolean)) {
    stopGames();
    await Promise.race([Promise.allSettled(games), sleep(120000)]);
  }
  const failed = results.filter(([state]) => state === "FAIL");
  console.log(`\n${results.length - failed.length} passed, ${failed.length} failed; pictures in ${out}`);
  process.exit(failed.length ? 1 : 0);
}
