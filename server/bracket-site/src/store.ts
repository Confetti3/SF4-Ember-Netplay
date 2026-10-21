// Tournaments on local disk, one SQLite row each.
import { DatabaseSync } from "node:sqlite";

import type { Tournament } from "./tournament.ts";

// ponytail: each write saves the whole tournament, fine for brackets of a few
// hundred players; give sets their own rows if events get bigger.
export class Store {
  /** Shared with the sign-in's tables. */
  readonly db: DatabaseSync;

  constructor(path: string) {
    this.db = new DatabaseSync(path);
    this.db.exec("PRAGMA journal_mode = WAL; CREATE TABLE IF NOT EXISTS tournaments (id TEXT PRIMARY KEY, doc TEXT NOT NULL)");
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

  close(): void {
    this.db.close();
  }
}
