// A read-only bracket page: one column a round, each set with its players
// and score.
import { roundLabel, status, type Tournament } from "./bracket.ts";

const escape = (text: string) => text.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

export function page(tournament: Tournament): string {
  const name = (id: string | null) => (id ? escape(tournament.entrants.find((each) => each.id === id)?.name ?? id) : "<i>to be decided</i>");
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
  const entrants = tournament.entrants.map((entrant) => `<li>${escape(entrant.name)}</li>`).join("");
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
<p>First to ${tournament.games_to_win}. ${tournament.state === "open" ? "Not started yet." : ""}</p>
${tournament.state === "open" ? `<h2>Entrants</h2><ol>${entrants}</ol>` : `<div class="rounds">${rounds.join("")}</div>`}
</body></html>
`;
}
