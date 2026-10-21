// A read-only bracket page: one column a round, each set with its players
// and score.
import { roundLabel, status } from "./bracket.ts";
import { stats } from "./live.ts";
import type { Report } from "./report.ts";
import { beforeLive, type Tournament } from "./tournament.ts";

const escape = (text: string) => text.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

const STATES: Record<Tournament["state"], string> = {
  draft: "Being set up.",
  planned: "Sign-ups are not open yet.",
  sign_ups: "Sign-ups are open.",
  check_in: "Check-in is open.",
  live: "Live.",
  finished: "Finished.",
  archived: "Finished.",
  cancelled: "Cancelled.",
};

/**
 * `viewer`: the signed-in Discord user, who gets the sign-up and check-in
 * buttons; `signIn`: whether the site signs people in at all.
 */
/**
 * The home page: who is signed in, the tournaments open or live, and the
 * reports of those finished lately. `viewer`: the signed-in Discord name;
 * `signIn`: whether the site signs people in at all.
 */
export function home(tournaments: Tournament[], viewer: string | null, signIn: boolean): string {
  const OPEN = ["sign_ups", "check_in", "live"];
  const open = tournaments.filter((each) => OPEN.includes(each.state));
  const finished = tournaments
    .filter((each) => each.report)
    .sort((a, b) => b.report!.finished_at.localeCompare(a.report!.finished_at))
    .slice(0, 10);
  const link = (each: Tournament, path: string, note: string) => `<li><a href="/t/${encodeURIComponent(each.id)}${path}">${escape(each.title)}</a> <span class="note">${note}</span></li>`;
  const who = !signIn
    ? ""
    : viewer
      ? `<form method="post" action="/signout"><p>Signed in as ${escape(viewer)}. <button>Sign out</button></p></form>`
      : `<p><a class="button" href="/signin?back=/">Sign in with Discord</a></p>`;
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ember brackets</title>
<style>
body { font: 16px system-ui, sans-serif; margin: 16px; background: #fff; color: #111; }
li { margin: 6px 0; } .note { color: #666; }
.button, button { display: inline-block; padding: 8px 12px; border: 1px solid #999; border-radius: 4px; background: none; color: inherit; font: inherit; text-decoration: none; }
@media (prefers-color-scheme: dark) { body { background: #111; color: #eee; } .note { color: #aaa; } }
</style></head><body>
<h1>Ember brackets</h1>
${who}
<h2>Now</h2>
${open.length ? `<ul>${open.map((each) => link(each, "", STATES[each.state])).join("")}</ul>` : "<p>No tournament is open or live right now.</p>"}
<h2>Results</h2>
${finished.length ? `<ul>${finished.map((each) => link(each, "/report", each.report!.podium[0] ? `won by ${escape(each.report!.podium[0].name)}` : "")).join("")}</ul>` : "<p>No results yet.</p>"}
</body></html>
`;
}

export function page(tournament: Tournament, viewer: string | null, signIn: boolean): string {
  const name = (id: string | null) => (id ? escape(tournament.entrants.find((each) => each.ember_id === id)?.name ?? id) : "<i>to be decided</i>");
  const rounds = Array.from({ length: tournament.rounds }, (_, i) => i + 1).map((round) => {
    const sets = tournament.sets.filter((set) => set.round === round).map((set) => {
      const rows = set.entries.map((entry, i) => {
        const score = set.scores ? String(set.scores[i]) : "";
        const won = set.winner && set.winner === entry ? ' class="won"' : "";
        return `<tr${won}><td>${name(entry)}</td><td>${score}</td></tr>`;
      });
      return `<table aria-label="Set ${set.id}, ${status(set)}">${rows.join("")}</table>`;
    });
    return `<section><h2>${roundLabel(round, tournament.rounds)}</h2>${sets.join("")}</section>`;
  });
  const entrants = tournament.entrants.map((entrant) => `<li>${escape(entrant.name)}${entrant.ember_id ? "" : " (waiting for Discord to be connected in Ember)"}</li>`).join("");
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escape(tournament.title)}</title>${tournament.state === "live" ? '\n<meta http-equiv="refresh" content="30">' : ""}
<style>
body { font: 16px system-ui, sans-serif; margin: 16px; background: #fff; color: #111; }
.rounds { display: flex; gap: 24px; overflow-x: auto; }
table { border-collapse: collapse; margin: 8px 0; min-width: 12em; }
td { border: 1px solid #999; padding: 4px 8px; }
td + td { width: 2em; text-align: right; }
.won { font-weight: bold; }
@media (prefers-color-scheme: dark) { body { background: #111; color: #eee; } td { border-color: #666; } }
</style></head><body>
<h1>${escape(tournament.title)}</h1>
<p>First to ${tournament.games_to_win}. ${STATES[tournament.state]}</p>
${signIn ? buttons(tournament, viewer) : ""}
${tournament.state === "live" || tournament.state === "finished" ? summary(tournament) : ""}
${beforeLive(tournament) ? `<h2>Entrants</h2><ol>${entrants}</ol>` : `<div class="rounds">${rounds.join("")}</div>`}
<script>
for (const button of document.querySelectorAll("button[data-path]")) {
  button.addEventListener("click", async () => {
    const answer = await fetch(button.dataset.path, { method: button.dataset.method, headers: { "content-type": "application/json" }, body: "{}" });
    if (answer.ok) location.reload();
    else document.getElementById("said").textContent = (await answer.json()).error;
  });
}
</script>
</body></html>
`;
}

/** The after-report as a page to share: the podium first. */
export function reportPage(report: Report, id: string): string {
  const medal = ["", "1st", "2nd", "3rd"];
  const podium = report.podium.map((each) => `<li>${medal[each.place] ?? each.place} ${escape(each.name)}</li>`).join("");
  const players = report.players
    .map((player) => {
      const path = player.path.map((step) => `${escape(step.round)}: ${step.won ? "beat" : "lost to"} ${escape(step.opponent)} ${step.score[0]}-${step.score[1]}`).join("; ");
      const characters = player.characters ? Object.keys(player.characters).join(", ") : "not known";
      return `<tr><td>${player.place ?? ""}</td><td>${escape(player.name)}</td><td>${player.sets_won}-${player.sets_lost}</td><td>${player.games_won}-${player.games_lost}</td><td>${escape(characters)}</td><td>${path}</td></tr>`;
    })
    .join("");
  const upsets = report.upsets.map((upset) => `<li>${escape(upset.winner)} over ${escape(upset.loser)} (${upset.gap} seeds)</li>`).join("");
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escape(report.title)}: results</title>
<meta property="og:title" content="${escape(report.title)}: results">
<meta property="og:description" content="${escape(report.podium.map((each) => `${medal[each.place] ?? each.place} ${each.name}`).join(" · "))}">
<style>
body { font: 16px system-ui, sans-serif; margin: 16px; background: #fff; color: #111; }
table { border-collapse: collapse; } td, th { border: 1px solid #999; padding: 4px 8px; text-align: left; }
.wide { overflow-x: auto; }
@media (prefers-color-scheme: dark) { body { background: #111; color: #eee; } td, th { border-color: #666; } }
</style></head><body>
<h1>${escape(report.title)}</h1>
<ol aria-label="Podium">${podium}</ol>
<p>${report.games} games${report.longest_set ? `, the longest set ${report.longest_set.games} games` : ""}${report.perfects === null ? "" : `, ${report.perfects} perfects, ${report.time_outs} time-outs`}.</p>
${upsets ? `<h2>Upsets</h2><ul>${upsets}</ul>` : ""}
<h2>Players</h2>
<div class="wide"><table><tr><th>Place</th><th>Player</th><th>Sets</th><th>Games</th><th>Characters</th><th>Path</th></tr>${players}</table></div>
<p><a href="/t/${encodeURIComponent(id)}">The bracket</a></p>
</body></html>
`;
}

/** The live numbers and the latest news, while it runs and once it is over. */
function summary(tournament: Tournament): string {
  const numbers = stats(tournament);
  const upsets = numbers.upsets.slice(0, 3).map((upset) => `${escape(upset.winner)} over ${escape(upset.loser)}`);
  const news = (tournament.feed ?? []).slice(-5).reverse().map((item) => `<li>${escape(item.text)}</li>`).join("");
  return `<p>${numbers.sets.live} live, ${numbers.sets.done} of ${numbers.sets.total} sets done.${upsets.length ? ` Upsets: ${upsets.join(", ")}.` : ""}</p>${news ? `<ul aria-label="Latest">${news}</ul>` : ""}`;
}

/** What the viewer can do now: sign in, sign up, leave or check in. */
function buttons(tournament: Tournament, viewer: string | null): string {
  if (!["sign_ups", "check_in"].includes(tournament.state)) return "";
  const path = `/tournaments/${encodeURIComponent(tournament.id)}`;
  if (!viewer) return `<p><a href="/signin?back=${encodeURIComponent(`${path}/bracket`)}">Sign in with Discord</a> to sign up.</p>`;
  const entrant = tournament.entrants.find((each) => each.discord === viewer);
  const button = (label: string, method: string, to: string) => `<button data-method="${method}" data-path="${to}">${label}</button>`;
  const shown = !entrant
    ? tournament.state === "sign_ups" ? button("Sign up", "POST", `${path}/entrants`) : ""
    : [
        tournament.state === "check_in" && !entrant.checked_in ? button("Check in", "POST", `${path}/check-in`) : "",
        entrant.checked_in ? "You are checked in." : "",
        button("Leave", "DELETE", `${path}/entrants/${viewer}`),
      ].join(" ");
  return `<p>${shown}</p><p id="said" role="status"></p>`;
}
