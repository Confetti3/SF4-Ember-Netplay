//! One SQLite connection in WAL mode with full fsync. Every write runs in
//! `BEGIN IMMEDIATE`, so writers are serialized and the uniqueness
//! constraints, not check-then-insert, decide races (spec 23.2).
use std::{
    path::Path,
    sync::{Arc, Mutex},
};

use rusqlite::{Connection, Transaction, TransactionBehavior};

use crate::error::ApiFailure;

/// Applied in order, each once, recorded by version. A version that shipped
/// keeps its number and its contents; a change is a new version at the end.
const MIGRATIONS: &[(i64, &str)] = &[
    (1, include_str!("../migrations/001_identity.sql")),
    (2, include_str!("../migrations/002_matches.sql")),
    (3, include_str!("../migrations/003_lobbies.sql")),
    (4, include_str!("../migrations/004_records.sql")),
    (5, include_str!("../migrations/005_tournaments.sql")),
    (6, include_str!("../migrations/006_play.sql")),
    (7, include_str!("../migrations/007_handoffs.sql")),
    (8, include_str!("../migrations/008_discord.sql")),
    (9, include_str!("../migrations/009_blumint.sql")),
    (10, include_str!("../migrations/010_discord_consent.sql")),
    (11, include_str!("../migrations/011_discord_moves.sql")),
    (12, include_str!("../migrations/012_rooms.sql")),
    (
        13,
        include_str!("../migrations/013_connection_settings.sql"),
    ),
    (14, include_str!("../migrations/014_room_details.sql")),
    (15, include_str!("../migrations/015_match_expiry.sql")),
    (
        16,
        include_str!("../migrations/016_discord_confirm_owner.sql"),
    ),
    (17, include_str!("../migrations/017_creator_region.sql")),
    (18, include_str!("../migrations/018_room_own_delay.sql")),
    (19, include_str!("../migrations/019_blumint_progress.sql")),
    (
        20,
        include_str!("../migrations/020_match_starting_wins.sql"),
    ),
    (21, include_str!("../migrations/021_blumint_status.sql")),
    (
        22,
        include_str!("../migrations/022_match_claim_refusals.sql"),
    ),
    (23, include_str!("../migrations/023_claim_liveness.sql")),
    (24, include_str!("../migrations/024_blumint_keys.sql")),
    (25, include_str!("../migrations/025_claim_ready.sql")),
    (
        26,
        include_str!("../migrations/026_blumint_refused_keys_dropped.sql"),
    ),
];

#[derive(Clone)]
pub struct Db(Arc<Mutex<Connection>>);

impl Db {
    pub fn open(path: &Path) -> rusqlite::Result<Self> {
        let connection = Connection::open(path)?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "synchronous", "FULL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.busy_timeout(std::time::Duration::from_secs(5))?;
        let mut db = connection;
        migrate(&mut db, MIGRATIONS)?;
        Ok(Self(Arc::new(Mutex::new(db))))
    }

    /// Runs `work` in an immediate (write) transaction on a blocking thread.
    pub async fn write<T, F>(&self, work: F) -> Result<T, ApiFailure>
    where
        T: Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, ApiFailure> + Send + 'static,
    {
        self.run(TransactionBehavior::Immediate, work).await
    }

    /// Runs `work` in a read transaction, a consistent snapshot.
    pub async fn read<T, F>(&self, work: F) -> Result<T, ApiFailure>
    where
        T: Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, ApiFailure> + Send + 'static,
    {
        self.run(TransactionBehavior::Deferred, work).await
    }

    async fn run<T, F>(&self, behavior: TransactionBehavior, work: F) -> Result<T, ApiFailure>
    where
        T: Send + 'static,
        F: FnOnce(&Transaction<'_>) -> Result<T, ApiFailure> + Send + 'static,
    {
        let db = self.0.clone();
        tokio::task::spawn_blocking(move || {
            let mut connection = db.lock().map_err(|_| ApiFailure::unavailable())?;
            let transaction = connection.transaction_with_behavior(behavior)?;
            let value = work(&transaction)?;
            transaction.commit()?;
            Ok(value)
        })
        .await
        .map_err(|_| ApiFailure::unavailable())?
    }
}

fn migrate(connection: &mut Connection, migrations: &[(i64, &str)]) -> rusqlite::Result<()> {
    connection.execute_batch(
        "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
    )?;
    for (version, sql) in migrations {
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let applied: bool = transaction.query_row(
            "SELECT EXISTS (SELECT 1 FROM schema_migrations WHERE version = ?1)",
            [version],
            |row| row.get(0),
        )?;
        if !applied {
            transaction.execute_batch(sql)?;
            transaction.execute(
                "INSERT INTO schema_migrations (version, applied_at) VALUES (?1, strftime('%s','now'))",
                [version],
            )?;
        }
        transaction.commit()?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table_exists(connection: &Connection, name: &str) -> bool {
        connection
            .query_row(
                "SELECT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1)",
                [name],
                |row| row.get(0),
            )
            .unwrap()
    }

    #[test]
    fn a_database_from_before_discord_upgrades_to_the_current_schema() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .unwrap();
        // Versions 1 to 7 as they shipped, the last with browser handoffs.
        migrate(&mut connection, &MIGRATIONS[..7]).unwrap();
        assert!(table_exists(&connection, "handoffs"));
        assert!(!table_exists(&connection, "discord_accounts"));

        migrate(&mut connection, MIGRATIONS).unwrap();
        assert!(!table_exists(&connection, "handoffs"));
        assert!(table_exists(&connection, "discord_accounts"));
        assert!(table_exists(&connection, "discord_sign_ins"));
        let links_sql: String = connection
            .query_row(
                "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'links'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert!(links_sql.contains("'discord'"), "{links_sql}");
        connection
            .prepare("SELECT delivery_attempts, delivery_next_at, delivery_first_at FROM matches")
            .unwrap();
        connection
            .prepare("SELECT claimed, pending_user_id, pending_username, pending_owner FROM discord_sign_ins")
            .unwrap();
        let versions: Vec<i64> = connection
            .prepare("SELECT version FROM schema_migrations ORDER BY version")
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap();
        assert_eq!(versions, (1..=MIGRATIONS.len() as i64).collect::<Vec<_>>());
    }

    #[test]
    fn blumint_connections_from_before_settings_keep_restarting_disputes() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .unwrap();
        migrate(&mut connection, &MIGRATIONS[..12]).unwrap();
        connection
            .execute_batch(
                "INSERT INTO tenants (id, name) VALUES ('bm', 'BluMint'), ('local', 'Local');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('bm-partner', 'bm', 'blumint', 'staging', 'BluMint', 1),
                          ('site', 'local', 'direct', 'staging', 'Site', 1);",
            )
            .unwrap();
        migrate(&mut connection, MIGRATIONS).unwrap();
        let disputes = |id: &str| -> String {
            connection
                .query_row(
                    "SELECT disputes FROM provider_connections WHERE id = ?1",
                    [id],
                    |row| row.get(0),
                )
                .unwrap()
        };
        assert_eq!(disputes("bm-partner"), "restart");
        assert_eq!(disputes("site"), "review");
    }

    #[test]
    fn matches_still_open_before_expiry_existed_get_a_day_from_their_last_change() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .unwrap();
        migrate(&mut connection, &MIGRATIONS[..14]).unwrap();
        connection
            .execute_batch(
                "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('c', 't', 'direct', 'staging', 'Site', 1);
                 INSERT INTO lobbies (id, tenant_id, connection_id, external_lobby_id, create_digest, revision, state,
                                      games_to_win, rotation, required_build_id, metadata, sets_started, sets_completed,
                                      next_position, streak, created_at, updated_at)
                   VALUES ('lob_1', 't', 'c', 'l1', '', 1, 'open', 2, 'winner_stays', '', '{}', 1, 0, 1, 0, 0, 0);
                 INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
                   assignment_generation, state, games_to_win, rules, required_build_id, metadata,
                   delivery_state, created_at, updated_at, lobby_id)
                   VALUES ('emt_open', 't', 'c', 'open', '', 1, 1, 'awaiting_players', 2, '{}', '', '{}', 'not_required', 1000, 1000, NULL),
                          ('emt_played', 't', 'c', 'played', '', 4, 1, 'between_games', 2, '{}', '', '{}', 'not_required', 1000, 5000, NULL),
                          ('emt_done', 't', 'c', 'done', '', 3, 1, 'completed', 2, '{}', '', '{}', 'not_required', 1000, 1500, NULL),
                          ('emt_set', 't', 'c', 'lobby:set', '', 1, 1, 'awaiting_players', 2, '{}', '', '{}', 'not_required', 1000, 1000, 'lob_1');",
            )
            .unwrap();
        migrate(&mut connection, MIGRATIONS).unwrap();
        let expires = |id: &str| -> (Option<i64>, Option<i64>) {
            connection
                .query_row(
                    "SELECT expires_at, expiry_secs FROM matches WHERE id = ?1",
                    [id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .unwrap()
        };
        assert_eq!(expires("emt_open"), (Some(1000 + 86400), Some(86400)));
        assert_eq!(expires("emt_played"), (Some(5000 + 86400), Some(86400)));
        assert_eq!(expires("emt_done"), (None, None));
        assert_eq!(expires("emt_set"), (None, None));
    }

    #[test]
    fn rooms_from_before_their_own_delay_existed_keep_the_shared_delay() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .unwrap();
        migrate(&mut connection, &MIGRATIONS[..17]).unwrap();
        connection
            .execute_batch(
                "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at)
                   VALUES ('old', 'Old room', 8, 'build', 'emb_old', 1000);",
            )
            .unwrap();
        migrate(&mut connection, MIGRATIONS).unwrap();
        let own_delay = |id: &str| -> i64 {
            connection
                .query_row(
                    "SELECT own_delay FROM rooms WHERE room_id = ?1",
                    [id],
                    |row| row.get(0),
                )
                .unwrap()
        };
        assert_eq!(own_delay("old"), 0);
    }

    #[test]
    fn blumint_matches_open_before_the_lifetime_cap_end_a_day_after_creation() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .unwrap();
        migrate(&mut connection, &MIGRATIONS[..17]).unwrap();
        // A game at hour 20 moved each deadline to hour 44.
        let late = 1000 + 44 * 3600;
        connection
            .execute_batch(&format!(
                "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('bm', 't', 'blumint', 'staging', 'BluMint', 1), ('c', 't', 'direct', 'staging', 'Site', 1);
                 INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
                   assignment_generation, state, games_to_win, rules, required_build_id, metadata,
                   delivery_state, created_at, updated_at, expires_at, expiry_secs)
                   VALUES ('emt_open', 't', 'bm', 'open', '', 3, 1, 'between_games', 2, '{{}}', '', '{{}}', 'queued', 1000, 73000, {late}, 86400),
                          ('emt_short', 't', 'bm', 'short', '', 1, 1, 'awaiting_players', 2, '{{}}', '', '{{}}', 'queued', 1000, 1000, 1000 + 3600, 3600),
                          ('emt_done', 't', 'bm', 'done', '', 4, 1, 'completed', 2, '{{}}', '', '{{}}', 'queued', 1000, 73000, {late}, 86400),
                          ('emt_direct', 't', 'c', 'direct', '', 3, 1, 'between_games', 2, '{{}}', '', '{{}}', 'not_required', 1000, 73000, {late}, 86400);"
            ))
            .unwrap();
        migrate(&mut connection, MIGRATIONS).unwrap();
        let expires = |id: &str| -> (i64, i64, [i64; 2], Option<i64>) {
            connection
                .query_row(
                    "SELECT expires_at, expiry_secs, starting_wins_p1, starting_wins_p2, ends_by FROM matches WHERE id = ?1",
                    [id],
                    |row| {
                        Ok((
                            row.get(0)?,
                            row.get(1)?,
                            [row.get(2)?, row.get(3)?],
                            row.get(4)?,
                        ))
                    },
                )
                .unwrap()
        };
        let capped = Some(1000 + 86400);
        assert_eq!(expires("emt_open"), (1000 + 86400, 86400, [0, 0], capped));
        assert_eq!(expires("emt_short"), (1000 + 3600, 3600, [0, 0], capped));
        assert_eq!(expires("emt_done"), (late, 86400, [0, 0], None));
        assert_eq!(expires("emt_direct"), (late, 86400, [0, 0], None));
    }

    #[test]
    fn blumint_matches_from_before_keys_were_kept_take_the_key_their_answer_names() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .unwrap();
        migrate(&mut connection, &MIGRATIONS[..23]).unwrap();
        // emt_kept's answer is still stored; emt_gone's was removed after a
        // day; emt_shut's was replaced when BluMint closed its key; another
        // connection's answer names emt_other, which is not BluMint's.
        connection
            .execute_batch(
                "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('bm', 't', 'blumint', 'staging', 'BluMint', 1), ('c', 't', 'direct', 'staging', 'Site', 1);
                 INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
                   assignment_generation, state, games_to_win, rules, required_build_id, metadata,
                   delivery_state, created_at, updated_at, expires_at, expiry_secs)
                   VALUES ('emt_kept', 't', 'bm', 'kept', '', 1, 1, 'awaiting_players', 2, '{}', '', '{}', 'queued', 1000, 1000, 87400, 86400),
                          ('emt_gone', 't', 'bm', 'gone', '', 1, 1, 'awaiting_players', 2, '{}', '', '{}', 'queued', 1000, 1000, 87400, 86400),
                          ('emt_shut', 't', 'bm', 'shut', '', 1, 1, 'cancelled', 2, '{}', '', '{}', 'not_required', 1000, 1000, 87400, 86400),
                          ('emt_other', 't', 'c', 'other', '', 1, 1, 'awaiting_players', 2, '{}', '', '{}', 'not_required', 1000, 1000, 87400, 86400);
                 INSERT INTO idempotency_records (actor, method, path, idempotency_key, command_digest, status, response, created_at)
                   VALUES ('connection:bm', 'POST', '/v1/blumint/matches', 'set-1', 'd', 200, CAST('{\"matchId\":\"emt_kept\",\"matchUrl\":\"u\"}' AS BLOB), 1000),
                          ('connection:bm', 'POST', '/v1/blumint/matches', 'set-3', 'refused', 409, CAST('{\"code\":\"idempotency_conflict\"}' AS BLOB), 1000),
                          ('cred_c', 'POST', '/v1/matches', 'direct-1', 'd', 200, CAST('{\"matchId\":\"emt_other\"}' AS BLOB), 1000);",
            )
            .unwrap();
        migrate(&mut connection, MIGRATIONS).unwrap();
        let key = |id: &str| -> Option<String> {
            connection
                .query_row(
                    "SELECT external_match_id FROM matches WHERE id = ?1",
                    [id],
                    |row| row.get(0),
                )
                .unwrap()
        };
        assert_eq!(key("emt_kept").as_deref(), Some("blumint:set-1"));
        assert_eq!(key("emt_gone").as_deref(), Some("gone"));
        assert_eq!(key("emt_shut").as_deref(), Some("shut"));
        assert_eq!(key("emt_other").as_deref(), Some("other"));
    }

    #[test]
    fn removals_from_before_withdrawals_were_recorded_still_hold() {
        let mut connection = Connection::open_in_memory().unwrap();
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .unwrap();
        migrate(&mut connection, &MIGRATIONS[..9]).unwrap();
        // Kate removed BluMint's link after her sign-in; Sam removed his
        // before signing in again. Kate also has a sign-in in flight.
        connection
            .execute_batch(
                "INSERT INTO tenants (id, name) VALUES ('bm', 'BluMint');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('bm-partner', 'bm', 'blumint', 'staging', 'BluMint', 1);
                 INSERT INTO identities (ember_id, public_key, created_at) VALUES ('emb_kate', 'kate-key', 0), ('emb_sam', 'sam-key', 0);
                 INSERT INTO external_accounts (id, connection_id, subject, participant_id, display_label, created_at)
                   VALUES ('ext_kate', 'bm-partner', 'discord:1', 'epl_kate', 'kate', 0),
                          ('ext_sam', 'bm-partner', 'discord:2', 'epl_sam', 'sam', 0);
                 INSERT INTO links (id, account_id, connection_id, ember_id, approved_via, claim_id, consented_at, approved_at, revoked_at, revoked_by)
                   VALUES ('lnk_kate', 'ext_kate', 'bm-partner', 'emb_kate', 'discord', 'discord:1', 100, 100, 200, 'unlinked_by_player'),
                          ('lnk_sam', 'ext_sam', 'bm-partner', 'emb_sam', 'discord', 'discord:2', 100, 100, 200, 'unlinked_by_player');
                 INSERT INTO discord_accounts (user_id, ember_id, username, connected_at)
                   VALUES ('1', 'emb_kate', 'kate', 100), ('2', 'emb_sam', 'sam', 300);
                 INSERT INTO discord_sign_ins (state_hash, ember_id, expires_at) VALUES (x'01', 'emb_kate', 900);",
            )
            .unwrap();
        migrate(&mut connection, MIGRATIONS).unwrap();
        let count = |connection: &Connection, sql: &str| -> i64 {
            connection.query_row(sql, [], |row| row.get(0)).unwrap()
        };
        let withdrawn = "SELECT COUNT(*) FROM discord_withdrawals WHERE connection_id = 'bm-partner' AND user_id = ";
        assert_eq!(count(&connection, &format!("{withdrawn}'1'")), 1);
        assert_eq!(count(&connection, &format!("{withdrawn}'2'")), 0);
        assert_eq!(
            count(&connection, "SELECT COUNT(*) FROM discord_sign_ins"),
            0
        );
        // A new sign-in replaces Kate's account row and gives the consent back.
        connection
            .execute_batch(
                "DELETE FROM discord_accounts WHERE user_id = '1';
                 INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES ('1', 'emb_kate', 'kate', 400);",
            )
            .unwrap();
        assert_eq!(count(&connection, &format!("{withdrawn}'1'")), 0);
    }
}
