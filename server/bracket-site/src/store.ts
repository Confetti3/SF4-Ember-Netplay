// Tournaments on local disk, one SQLite row each.
import { DatabaseSync } from "node:sqlite";

import type { Award } from "./awards.ts";
import type { League } from "./league.ts";
import type { Tournament } from "./tournament.ts";

// ponytail: each write saves the whole tournament, fine for brackets of a few
// hundred players; give sets their own rows if events get bigger.
export class Store {
  /** Shared with the sign-in's tables. */
  readonly db: DatabaseSync;

  constructor(path: string) {
    this.db = new DatabaseSync(path);
    this.db.exec(`PRAGMA journal_mode = WAL;
      CREATE TABLE IF NOT EXISTS tournaments (id TEXT PRIMARY KEY, doc TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS leagues (id TEXT PRIMARY KEY, doc TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS awards (discord TEXT NOT NULL, kind TEXT NOT NULL, given_for TEXT NOT NULL, doc TEXT NOT NULL,
        PRIMARY KEY (discord, kind, given_for))`);
  }

  get(id: string): Tournament | undefined {
    const row = this.db.prepare("SELECT doc FROM tournaments WHERE id = ?").get(id) as { doc: string } | undefined;
    return row && (JSON.parse(row.doc) as Tournament);
  }

  all(): Tournament[] {
    return (this.db.prepare("SELECT doc FROM tournaments ORDER BY id").all() as { doc: string }[]).map((row) => JSON.parse(row.doc) as Tournament);
  }

  /** Adds a new tournament; false when the ID is taken. */
  add(tournament: Tournament): boolean {
    return this.db.prepare("INSERT OR IGNORE INTO tournaments (id, doc) VALUES (?, ?)").run(tournament.id, JSON.stringify(tournament)).changes === 1;
  }

  save(tournament: Tournament): void {
    this.db.prepare("UPDATE tournaments SET doc = ? WHERE id = ?").run(JSON.stringify(tournament), tournament.id);
  }

  delete(id: string): boolean {
    return this.db.prepare("DELETE FROM tournaments WHERE id = ?").run(id).changes === 1;
  }

  league(id: string): League | undefined {
    const row = this.db.prepare("SELECT doc FROM leagues WHERE id = ?").get(id) as { doc: string } | undefined;
    return row && (JSON.parse(row.doc) as League);
  }

  leagues(): League[] {
    return (this.db.prepare("SELECT doc FROM leagues ORDER BY id").all() as { doc: string }[]).map((row) => JSON.parse(row.doc) as League);
  }

  /** The league holding season `id`. */
  leagueOf(season: string): League | undefined {
    return this.leagues().find((league) => league.seasons.some((each) => each.id === season));
  }

  /** Adds a league, or saves it again; false when a new one's ID is taken. */
  putLeague(league: League, adding: boolean): boolean {
    const sql = adding ? "INSERT OR IGNORE INTO leagues (doc, id) VALUES (?, ?)" : "UPDATE leagues SET doc = ? WHERE id = ?";
    return this.db.prepare(sql).run(JSON.stringify(league), league.id).changes === 1;
  }

  /** Keeps each award once: the same award for the same event again is ignored. Those new. */
  addAwards(awards: Award[]): Award[] {
    const add = this.db.prepare("INSERT OR IGNORE INTO awards (discord, kind, given_for, doc) VALUES (?, ?, ?, ?)");
    return awards.filter((award) => add.run(award.discord, award.kind, award.tournament ?? `season:${award.season}`, JSON.stringify(award)).changes === 1);
  }

  awardsOf(discord: string): Award[] {
    return (this.db.prepare("SELECT doc FROM awards WHERE discord = ? ORDER BY rowid").all(discord) as { doc: string }[]).map((row) => JSON.parse(row.doc) as Award);
  }

  close(): void {
    this.db.close();
  }
}
