#!/usr/bin/env node
// The tournament desk end to end in a real Chrome, driven over the DevTools
// protocol (no Puppeteer): sign in from Ember with the code and the number,
// create a Challonge tournament in the four steps, tiers, Discord, sign-ups,
// Ready, the live sets, deciding a game, the bracket and the public page, at
// phone and desktop widths.
//
//   node scripts/e2e-desk.mjs --local                     # against desk_e2e_stack (fakes)
//   node scripts/e2e-desk.mjs --staging https://bridge     # against a real bridge and Challonge
//   node scripts/e2e-desk.mjs --staging https://bridge --setup   # once: the organizer consent link
//
// --local reads stack.json from the stack's out folder (EMBER_E2E_OUT, default
// /mnt/d/ember-build/e2e-out); start the stack first:
//   EMBER_E2E_OUT=/mnt/d/ember-build/e2e-out cargo test -p ember-brackets --test desk_e2e_stack -- --ignored --nocapture
// --staging takes its Ember ID seed and an optional Discord webhook from
// ~/tournament-lab/e2e/ (ember-id.seed, discord-webhook).
// Chrome: a running one on 127.0.0.1:9222, else a headless Windows Chrome is
// started (profile C:\Users\Public\ember-e2e-chrome). Pictures go to the out folder.
import { execFileSync, spawn } from "node:child_process";
import { chrome, layout, must, results, step } from "./e2e/cdp.mjs";
import { existsSync, mkdirSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const args = process.argv.slice(2);
const local = args.includes("--local");
const staging = args.includes("--staging") ? args[args.indexOf("--staging") + 1] : null;
if (!local && !staging) {
  console.error("usage: e2e-desk.mjs --local | --staging <bridge origin> [--setup]");
  process.exit(2);
}
const out = process.env.EMBER_E2E_OUT || "/mnt/d/ember-build/e2e-out";
mkdirSync(out, { recursive: true });
const repo = new URL("..", import.meta.url).pathname;
const game = process.env.EMBER_DESK_GAME || "/mnt/d/ember-build/cargo-target/debug/examples/desk_game";

let stack;
let bridge;
let seed;
if (local) {
  stack = JSON.parse(readFileSync(join(out, "stack.json"), "utf8"));
  bridge = stack.bridge;
  seed = stack.seed;
} else {
  bridge = staging.replace(/\/$/, "");
  const dir = join(homedir(), "tournament-lab", "e2e");
  mkdirSync(dir, { recursive: true, mode: 0o700 });
  seed = join(dir, "ember-id.seed");
}

/** The game's side, signed with the test Ember ID (examples/desk_game.rs). */
function play(...command) {
  try {
    return JSON.parse(execFileSync(game, [bridge, seed, ...command], { encoding: "utf8" }));
  } catch (error) {
    return JSON.parse(error.stdout || '{"status":0,"body":"desk_game failed"}');
  }
}

if (args.includes("--setup")) {
  const id = play("id").body.ember_id;
  const organize = play("organize", "challonge");
  console.log("Test Ember ID:", id);
  // One organizer sign-in links the Challonge account and allows organizer
  // access together; the refresh token keeps it from then on.
  const url = organize.body.authorize_url;
  if (!url) {
    console.error("no sign-in link:", JSON.stringify(organize));
    process.exit(1);
  }
  // Opened in the Windows browser; it works for ten minutes.
  console.log("Sign in to Challonge as the organizer and approve, in the browser that opened:");
  console.log(url);
  spawn("powershell.exe", ["-NoProfile", "-Command", "Start-Process '" + url.replace(/'/g, "''") + "'"], { stdio: "ignore", detached: true }).unref();
  process.exit(0);
}

const control = (path) => fetch(stack.control + path, { method: "POST" }).then((r) => r.json());
const page = await chrome(out);
let event = null;
let challongeCalls = 0;

try {
  await step("signed-out page", async () => {
    await page.go(bridge + "/desk");
    await page.until("!document.getElementById('signed-out').hidden");
    await layout(page, "signed-out", true);
  });

  // Sign in from Ember: a wrong number turns the code down, the right one signs in.
  const code = async () => {
    await page.eval("document.getElementById('device-new').hidden ? document.getElementById('device-show').click() : document.getElementById('device-new').click()");
    await page.until("/^\\d{3} \\d{3}$/.test(document.getElementById('device-code').textContent)");
    return (await page.eval("document.getElementById('device-code').textContent")).replace(" ", "");
  };
  await step("a wrong number turns the code down", async () => {
    const shown = await code();
    const info = play("device", shown);
    must(info.status === 200 && Array.isArray(info.body.choices), "device info: " + JSON.stringify(info));
    await page.until("!document.getElementById('device-pick').hidden");
    const number = Number(await page.eval("document.getElementById('device-number').textContent"));
    must(info.body.choices.includes(number), "the page's number is one of the game's choices");
    const wrong = info.body.choices.find((choice) => choice !== number);
    must(play("approve", shown, String(wrong)).status >= 400, "a wrong number was taken");
    await page.until("/turned down|expired/.test(document.getElementById('device-left').textContent)");
  });
  await step("the right number signs this browser in", async () => {
    const shown = await code();
    play("device", shown);
    await page.until("!document.getElementById('device-pick').hidden");
    const number = await page.eval("document.getElementById('device-number').textContent");
    const approved = play("approve", shown, number);
    must(approved.status < 300, "approve: " + JSON.stringify(approved));
    await page.until("!document.getElementById('home').hidden");
    await layout(page, "home", true);
  });

  await step("details: a title over 60 characters is refused, a draft is saved", async () => {
    await page.eval("document.getElementById('new-tournament').click()");
    await page.until("!document.getElementById('wizard').hidden");
    await page.eval(`document.getElementById('title').removeAttribute('maxlength'); document.getElementById('title').value = 'x'.repeat(61); document.getElementById('next').click()`);
    await page.until("/60 characters/.test(document.getElementById('step-error').textContent)");
    await page.eval(`document.getElementById('title').value = 'ember-e2e ' + new Date().toISOString().slice(0, 16);
      document.querySelector('[data-link=url]').value = 'https://twitch.tv/ember-e2e';
      (() => { const now = new Date(Date.now() - 60000), pad = (n) => String(n).padStart(2, '0');
        document.getElementById('date').value = now.getFullYear() + '-' + pad(now.getMonth() + 1) + '-' + pad(now.getDate());
        document.getElementById('time').value = pad(now.getHours()) + ':' + pad(now.getMinutes()); })();
      document.getElementById('step-error').hidden = true`);
    await layout(page, "details", true);
    await page.eval("document.getElementById('next').click()");
    await page.until("/Service/.test(document.getElementById('step-now').textContent)");
    event = await page.eval("desk.event && desk.event.id");
    must(event, "no event id");
  });

  await step("service and rules: Challonge, tiers, created on Challonge", async () => {
    await page.eval(`document.getElementById('add-tier').click(); document.getElementById('add-tier').click()`);
    await page.eval(`(() => { const rows = document.querySelectorAll('#tiers .tier');
      rows[0].querySelector('[data-tier=from]').value = 'finals';
      rows[1].querySelector('[data-tier=from]').value = 'grand_final'; rows[1].querySelector('[data-tier=games]').value = '5'; })()`);
    // Four players: the first round is the winners semi-finals, so it stays FT2.
    await layout(page, "service", true);
    await page.eval("document.getElementById('next').click()");
    challongeCalls++;
    await page.until("/Discord/.test(document.getElementById('step-now').textContent)", 15000);
    const tiers = await page.eval("JSON.stringify(desk.event.settings.rounds.tiers)");
    must(/finals/.test(tiers) && /grand_final/.test(tiers), "tiers kept: " + tiers);
    must(await page.eval("Boolean(desk.event.tournament)"), "no Challonge tournament");
  });

  const webhook = local ? stack.webhook : (existsSync(join(homedir(), "tournament-lab/e2e/discord-webhook")) ? readFileSync(join(homedir(), "tournament-lab/e2e/discord-webhook"), "utf8").trim() : "");
  await step("Discord and sign-ups", async () => {
    await page.eval("document.querySelectorAll('#live-settings input[type=checkbox]').forEach((box) => box.checked = true)");
    // Pictured before the webhook is typed: on staging it is a real one.
    await layout(page, "discord", true);
    if (webhook) await page.eval(`document.querySelector('#live-settings input[type=url]').value = ${JSON.stringify(webhook)}`);
    await page.eval("document.getElementById('next').click()");
    await page.until("/Ready/.test(document.getElementById('step-now').textContent)");
    must(await page.eval("desk.event.sign_ups && desk.event.public"), "sign-ups and public not on");
  });

  await step("the Discord post has the title, tiers and public page", async () => {
    const post = await page.eval("document.getElementById('post').value");
    must(/ember-e2e/.test(post) && /Grand final FT5/.test(post) && /\/t\//.test(post), post);
  });

  await step("Ready", async () => {
    await layout(page, "ready", true);
    await page.eval("document.getElementById('next').click()");
    await page.until("desk.event.status !== 'draft'");
  });

  if (local) {
    await step("the bracket starts from the desk: a set is called on Discord and shows live", async () => {
      const tournament = await page.eval("desk.event.tournament");
      await control("/join/" + tournament);
      await page.eval("go(5)");
      await page.until("!document.getElementById('start-bracket').hidden", 15000);
      await layout(page, "start");
      await page.eval("document.getElementById('start-bracket').click()");
      await page.until("document.getElementById('start-bracket').hidden || /started/.test(document.body.textContent)", 15000);
      // The connector reads the started bracket, then calls its first sets.
      await control("/pass");
      await control("/pass");
      const posts = await (await fetch(stack.control + "/posts")).json();
      must(posts.some((post) => JSON.stringify(post).includes("ember-e2e") || JSON.stringify(post).includes("Round")), "no call posted: " + JSON.stringify(posts).slice(0, 200));
      await page.eval("go(5)");
      await page.until("document.querySelectorAll('#live-body .set').length > 0", 15000);
      await layout(page, "live");
    });

    await step("a disputed game is decided as a draw, then for player 1, then voided", async () => {
      await control("/dispute");
      await page.eval("readSets()");
      await page.until("/Needs/.test(document.getElementById('live-body').textContent)", 15000);
      const open = "[...document.querySelectorAll('#live-body .set')].find((c) => /Needs/.test(c.textContent)).querySelector('.primary').click()";
      const decideButton = (text) => `[...document.querySelectorAll('#decide button')].find((b) => b.textContent.trim() === ${JSON.stringify(text)}).click()`;
      await page.eval(open);
      await page.until("document.getElementById('decide').open");
      await layout(page, "decide");
      await page.eval(decideButton("Draw"));
      await page.until("!document.getElementById('decide').open");
      await page.until("/1 draw/.test(document.getElementById('live-body').textContent)", 15000);
      // Player 1 is given a game, then it is voided again.
      const games = "[...document.querySelectorAll('#live-body .set')].find((c) => /1 draw/.test(c.textContent))";
      await page.eval(games + ".querySelector('.wd-row button:last-child').click()");
      await page.until("document.getElementById('decide').open");
      await page.eval("[...document.querySelectorAll('#decide button')].find((b) => / won$/.test(b.textContent)).click()");
      await page.until("!document.getElementById('decide').open");
      await page.until(games + " && /\\b1\\b/.test(" + games + ".querySelector('.set-row b').textContent)", 15000);
      await page.eval(games + ".querySelector('.wd-row button:last-child').click()");
      await page.until("document.getElementById('decide').open");
      // The last game is player 1's: void that one, the draw stays.
      await page.eval("[...document.querySelectorAll('#decide button')].filter((b) => b.textContent.trim() === 'Void').pop().click()");
      await page.until("!document.getElementById('decide').open");
      await page.until(games + " && " + games + ".querySelector('.set-row b').textContent === '0'", 15000);
    });

    await step("the bracket view", async () => {
      await page.eval("[...document.querySelectorAll('#live-tabs button')].find((b) => b.textContent === 'Bracket').click()");
      await page.until("document.querySelector('#live-body .bracket')");
      await layout(page, "bracket");
    });
  }

  await step("the public page and its preview tags", async () => {
    const html = await (await fetch(bridge + "/t/" + event)).text();
    must(/og:title" content="ember-e2e/.test(html), "no og:title");
    await page.go(bridge + "/t/" + event);
    await page.until("document.body.textContent.includes('ember-e2e')", 15000);
    await layout(page, "public");
  });

  await step("stopped and deleted on Challonge too", async () => {
    await page.go(bridge + "/desk");
    await page.until("!document.getElementById('home').hidden", 15000);
    await page.eval(`home().then(() => open(desk.events.find((e) => e.id === ${JSON.stringify(event)})))`);
    await page.until("desk.event && desk.event.created_on_site");
    const tournament = await page.eval("desk.event.tournament");
    await page.eval("go(5); document.getElementById('stop').click()");
    await page.until("document.getElementById('decide').open");
    must(!/null/.test(await page.eval("document.getElementById('decide').textContent")), "the dialog says null");
    await layout(page, "stop");
    await page.eval("[...document.querySelectorAll('#decide button')].find((b) => /delete it on Challonge/.test(b.textContent)).click()");
    await page.until("!document.getElementById('decide').open && !desk.event", 15000);
    const listed = await page.eval("api('GET', '/v1/organizer/tournaments?site=challonge').then((r) => JSON.stringify(r.tournaments))");
    must(!listed.includes(JSON.stringify(tournament)), "still on Challonge: " + listed.slice(0, 200));
    event = null;
  });
} finally {
  // Never leave an event behind; only this run's event is touched, with its
  // Challonge tournament.
  if (event) {
    const left = await page.eval(`fetch('/web/session').then((r) => r.json()).then((session) =>
      fetch('/v1/organizer/events/${event}', { method: 'DELETE', credentials: 'same-origin',
        headers: { 'Content-Type': 'application/json', 'X-Ember-CSRF': session.csrf }, body: '{"on_site":true}' })).then((r) => r.status)`).catch((error) => error.message);
    console.log("event", event, "removed with its Challonge tournament:", left);
  }
  page.close();
  const failed = results.filter(([state]) => state === "FAIL");
  console.log(`\n${results.length - failed.length} passed, ${failed.length} failed; Challonge creations: ${challongeCalls}; pictures in ${out}`);
  process.exitCode = failed.length ? 1 : 0;
}
