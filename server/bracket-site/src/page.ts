// A read-only bracket page: one column a round, each set with its players
// and score.
import { roundLabel, status } from "./bracket.ts";
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
<title>${escape(tournament.title)}</title>
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
