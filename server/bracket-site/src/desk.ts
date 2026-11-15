// The organizers' desk: their tournaments run from a browser signed in with
// Discord, on the site's own routes (the session cookie; writes carry the
// site's Origin). One page; its script draws the list and one tournament.
import { escape } from "./page.ts";

const STYLE = `body { font: 16px system-ui, sans-serif; margin: 16px; max-width: 960px; background: #fff; color: #111; }
li { margin: 6px 0; } .note { color: #666; } .error { color: #b00; }
.button, button { display: inline-block; padding: 6px 10px; border: 1px solid #999; border-radius: 4px; background: none; color: inherit; font: inherit; text-decoration: none; }
input, select { font: inherit; padding: 4px; } label { display: block; margin: 6px 0; }
table { border-collapse: collapse; } td, th { padding: 4px 8px; border-bottom: 1px solid #ccc; text-align: left; vertical-align: top; }
@media (prefers-color-scheme: dark) { body { background: #111; color: #eee; } .note { color: #aaa; } .error { color: #f66; } }`;

/** `who`: the signed-in Discord name and whether they organize, or null when no one is. */
export function deskPage(who: { name: string; organizer: boolean } | null): string {
  const body = !who
    ? `<p><a class="button" href="/signin?back=/desk">Sign in with Discord</a></p>`
    : !who.organizer
      ? `<p>Signed in as ${escape(who.name)}. The desk is for organizers: enter the invite the site's operator gave you.</p>
<form id="redeem"><label>Invite <input name="code" required pattern="[A-Za-z0-9_-]{1,64}"></label><button>Become an organizer</button></form>
<p class="error" id="error" role="alert"></p><script>${REDEEM}</script>`
      : `<p>Signed in as ${escape(who.name)}. <a href="/desk">Your tournaments</a></p>
<p class="error" id="error" role="alert"></p><main id="desk"></main><script>${DESK}</script>`;
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ember desk</title>
<style>${STYLE}</style></head><body>
<h1>Tournament desk</h1>
${body}
</body></html>
`;
}

/** The fetch both scripts share: the site's own JSON, or its error shown. */
const API = `const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => "&#" + c.charCodeAt(0) + ";");
async function api(method, path, body) {
  $("error").textContent = "";
  const r = await fetch(path, { method, headers: body ? { "content-type": "application/json" } : {}, body: body && JSON.stringify(body) });
  const t = await r.text();
  const v = t ? JSON.parse(t) : {};
  if (!r.ok) { $("error").textContent = v.error || "Failed (" + r.status + ")"; throw new Error(v.error); }
  return v;
}`;

const REDEEM = `${API}
$("redeem").onsubmit = async (e) => { e.preventDefault(); await api("POST", "/organizer-invites/redeem", { code: e.target.code.value }); location.reload(); };`;

const DESK = `${API}
const EDITABLE = ["draft", "planned", "sign_ups", "check_in"];
const LABELS = { plan: "Plan", open_sign_ups: "Open sign-ups", close_sign_ups: "Close sign-ups", open_check_in: "Open check-in", reopen_sign_ups: "Reopen sign-ups", start: "Start", reset: "Reset bracket", archive: "Archive", cancel: "Cancel" };
const REASON = "Decided at the tournament desk";
const T = (id) => "/tournaments/" + encodeURIComponent(id);
async function list() {
  const { tournaments: all } = await api("GET", "/tournaments");
  $("desk").innerHTML = '<h2>Your tournaments</h2><ul>' + all.map((t) => '<li><a href="#' + esc(t.id) + '">' + esc(t.title) + '</a> <span class="note">' + esc(t.state) + '</span></li>').join("") + '</ul>'
    + '<form id="new"><label>New tournament <input name="title" required maxlength="100"></label><button>Make it</button></form>';
  $("new").onsubmit = async (e) => { e.preventDefault(); const t = await api("POST", "/tournaments", { title: e.target.title.value }); location.hash = t.id; };
}
async function show(id) {
  const t = await api("GET", T(id));
  const names = new Map(t.entrants.map((e) => [e.ember_id, e.name]));
  const name = (entry) => entry ? esc(names.get(entry) ?? entry) : '<span class="note">TBD</span>';
  let html = '<h2>' + esc(t.title) + '</h2><p>' + esc(t.state) + ' · <a href="/t/' + esc(t.id) + '">Public page</a></p>';
  if (EDITABLE.includes(t.state)) html += '<form id="settings"><h3>Settings</h3>'
    + '<label>Title <input name="title" required maxlength="100" value="' + esc(t.title) + '"></label>'
    + '<label>Starts <input name="starts_at" type="datetime-local" value="' + esc(t.starts_at ? t.starts_at.slice(0, 16) : "") + '"> <span class="note">UTC</span></label>'
    + '<label>Games to win <input name="games_to_win" type="number" min="1" max="10" value="' + t.games_to_win + '"></label>'
    + '<label>At most <input name="cap" type="number" min="2" max="1024" value="' + (t.cap ?? "") + '"> entrants</label>'
    + '<label><input name="check_in" type="checkbox"' + (t.check_in ? " checked" : "") + '> Check-in before the bracket</label><button>Save</button></form>';
  html += '<form id="webhook"><h3>Discord channel</h3><p class="note">' + (t.discord ? "Posting to a channel." : "Not posting to Discord.") + '</p>'
    + '<label>Webhook URL <input name="url" type="url" size="50"></label><button>Set</button>' + (t.discord ? ' <button type="button" id="unhook">Stop posting</button>' : "") + '</form>';
  html += '<h3>State</h3><p>' + t.actions.map((a) => '<button data-action="' + a + '">' + esc(LABELS[a] ?? a) + '</button> ').join("")
    + (t.actions.includes("start") ? '<label>Seeding <select id="seeding"><option value="join">Order of joining</option><option value="random">Random</option><option value="points">Season points</option></select></label>' : "")
    + (t.state === "draft" ? ' <button id="delete">Delete</button>' : "") + '</p>';
  html += '<h3>Entrants (' + t.entrants.length + ')</h3><ul>' + t.entrants.map((e) => '<li>' + esc(e.name)
    + (e.checked_in ? ' <span class="note">checked in</span>' : "") + (e.ember_id ? "" : ' <span class="note">no Ember ID yet</span>') + '</li>').join("") + '</ul>';
  if (t.sets.length) html += '<h3>Sets</h3><table><tr><th>Round</th><th>Players</th><th>Score</th><th>Status</th><th></th></tr>' + t.sets.map((s) => '<tr><td>' + esc(s.round_label) + '</td><td>'
    + name(s.entries[0]) + '<br>' + name(s.entries[1]) + '</td><td>' + (s.scores ? s.scores.join(" - ") : "") + '</td><td>' + esc(s.status) + (s.held ? " (held)" : "") + (s.underway ? " (playing)" : "") + '</td><td>'
    + (s.status === "open" ? '<button data-hold="' + esc(s.id) + '" data-held="' + !s.held + '">' + (s.held ? "Release" : "Hold") + '</button> <button data-call="' + esc(s.id) + '">Call again</button> ' : "")
    + (s.ember_match ? '<button data-games="' + esc(s.id) + '">Games</button>' : "") + '<div id="games-' + esc(s.id) + '"></div></td></tr>').join("") + '</table>';
  $("desk").innerHTML = html;
  const again = () => show(id);
  const form = $("settings");
  if (form) form.onsubmit = async (e) => { e.preventDefault(); await api("PATCH", T(id), { title: form.title.value, starts_at: form.starts_at.value ? form.starts_at.value + ":00Z" : null, games_to_win: Number(form.games_to_win.value), cap: form.cap.value ? Number(form.cap.value) : null, check_in: form.check_in.checked }); again(); };
  $("webhook").onsubmit = async (e) => { e.preventDefault(); await api("PATCH", T(id), { discord_webhook: e.target.url.value }); again(); };
  if ($("unhook")) $("unhook").onclick = async () => { await api("PATCH", T(id), { discord_webhook: null }); again(); };
  if ($("delete")) $("delete").onclick = async () => { if (confirm("Delete " + t.title + "?")) { await api("DELETE", T(id)); location.hash = ""; } };
  for (const b of document.querySelectorAll("[data-action]")) b.onclick = async () => {
    const action = b.dataset.action;
    if (action === "cancel" && !confirm("Cancel " + t.title + "?")) return;
    await api("PUT", T(id) + "/state", action === "start" ? { action, seeding: $("seeding").value } : { action }); again();
  };
  for (const b of document.querySelectorAll("[data-hold]")) b.onclick = async () => { await api("POST", T(id) + "/sets/" + b.dataset.hold + "/hold", { held: b.dataset.held === "true" }); again(); };
  for (const b of document.querySelectorAll("[data-call]")) b.onclick = async () => { await api("POST", T(id) + "/sets/" + b.dataset.call + "/call", {}); b.textContent = "Called"; };
  for (const b of document.querySelectorAll("[data-games]")) b.onclick = () => games(t, t.sets.find((s) => s.id === b.dataset.games), name);
}
/** A set's games from its Ember match, each decided, drawn or voided here. */
async function games(t, set, name) {
  const path = T(t.id) + "/sets/" + set.id;
  const { revision, games } = await api("GET", path + "/games");
  const box = $("games-" + set.id);
  const decide = (label, body) => '<button data-body="' + esc(JSON.stringify(body)) + '">' + label + '</button> ';
  box.innerHTML = '<ol>' + games.map((g) => '<li>' + esc(g.outcome ?? g.state) + ' '
    + (g.state === "review" ? decide("Win: " + name(set.entries[0]), { kind: "game_result", winner_slot: 0, attempt_id: g.attempt_id }) + decide("Win: " + name(set.entries[1]), { kind: "game_result", winner_slot: 1, attempt_id: g.attempt_id }) + decide("Draw", { kind: "game_result", draw: true, attempt_id: g.attempt_id }) : "")
    + (g.state === "accepted" ? decide("Void", { kind: "void_game", attempt_id: g.attempt_id }) : "") + '</li>').join("") + '</ol>'
    + (set.status === "open" ? '<p>Add a game: ' + decide(name(set.entries[0]), { kind: "game_result", winner_slot: 0 }) + decide(name(set.entries[1]), { kind: "game_result", winner_slot: 1 }) + '</p>' : "");
  for (const b of box.querySelectorAll("[data-body]")) b.onclick = async () => {
    await api("POST", path + "/adjudications", { reason: REASON, expected_revision: String(revision), ...JSON.parse(b.dataset.body) });
    show(t.id);
  };
}
const route = () => (location.hash.length > 1 ? show(decodeURIComponent(location.hash.slice(1))) : list()).catch(() => {});
onhashchange = route;
route();`;
