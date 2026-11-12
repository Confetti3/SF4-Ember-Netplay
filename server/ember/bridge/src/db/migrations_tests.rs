//! What a database of every age becomes when this bridge opens it: the
//! records of a bridge from before file names, the two files 055 has meant,
//! and the data each migration carries forward.
use rusqlite::Connection;

use super::*;

fn has_table(connection: &Connection, name: &str) -> bool {
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
    assert!(has_table(&connection, "handoffs"));
    assert!(!has_table(&connection, "discord_accounts"));

    migrate(&mut connection, MIGRATIONS).unwrap();
    assert!(!has_table(&connection, "handoffs"));
    assert!(has_table(&connection, "discord_accounts"));
    assert!(has_table(&connection, "discord_sign_ins"));
    let links_sql: String = connection
        .query_row(
            "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'links'",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert!(links_sql.contains("'discord'"), "{links_sql}");
    assert!(links_sql.contains("'site'"), "{links_sql}");
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
    let listed: Vec<i64> = MIGRATIONS
        .iter()
        .map(|migration| migration.version)
        .collect();
    assert_eq!(versions, listed);
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
fn links_from_before_sign_in_sites_keep_their_rows_and_indexes() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    migrate(&mut connection, &MIGRATIONS[..26]).unwrap();
    connection
        .execute_batch(
            "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
             INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
               VALUES ('c', 't', 'direct', 'staging', 'Site', 1);
             INSERT INTO identities (ember_id, public_key, created_at) VALUES ('emb_player1', 'player1-key', 0), ('emb_player2', 'player2-key', 0);
             INSERT INTO external_accounts (id, connection_id, subject, participant_id, display_label, created_at)
               VALUES ('ext_player1', 'c', 'player1', 'epl_player1', 'player1', 0), ('ext_player2', 'c', 'discord:2', 'epl_player2', 'player2', 0);
             INSERT INTO links (id, account_id, connection_id, ember_id, approved_via, claim_id, consented_at, approved_at, revoked_at, revoked_by)
               VALUES ('lnk_old', 'ext_player1', 'c', 'emb_player1', 'browser', 'lkc_1', 10, 10, 20, 'unlinked_by_player'),
                      ('lnk_player1', 'ext_player1', 'c', 'emb_player1', 'provider_proxy', 'lkc_2', 30, 30, NULL, NULL),
                      ('lnk_player2', 'ext_player2', 'c', 'emb_player2', 'discord', 'discord:2', 40, 40, NULL, NULL),
                      ('lnk_gone', 'ext_player2', 'c', 'emb_player2', 'discord', 'discord:2', 5, 5, 8, 'unlinked_by_player');
             INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES ('2', 'emb_player2', 'player2', 1);
             INSERT INTO discord_withdrawals (user_id, connection_id) VALUES ('2', 'c');",
        )
        .unwrap();
    let indexes = |connection: &Connection| -> Vec<(String, Option<String>)> {
        connection
            .prepare("SELECT name, sql FROM sqlite_master WHERE type = 'index' AND tbl_name = 'links' ORDER BY name")
            .unwrap()
            .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap()
    };
    let foreign_keys = |connection: &Connection| -> Vec<(String, String, String)> {
        connection
            .prepare("SELECT \"from\", \"table\", \"to\" FROM pragma_foreign_key_list('links') ORDER BY \"from\"")
            .unwrap()
            .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap()
    };
    let before = indexes(&connection);
    let keys_before = foreign_keys(&connection);
    assert_eq!(keys_before.len(), 3);
    assert!(
        before
            .iter()
            .any(|(name, _)| name == "links_active_identity")
    );
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_eq!(indexes(&connection), before);
    assert_eq!(foreign_keys(&connection), keys_before);
    let broken: i64 = connection
        .query_row("SELECT COUNT(*) FROM pragma_foreign_key_check", [], |row| {
            row.get(0)
        })
        .unwrap();
    assert_eq!(broken, 0);
    let rows: Vec<(String, String, Option<i64>)> = connection
        .prepare("SELECT id, approved_via, revoked_at FROM links ORDER BY id")
        .unwrap()
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(
        rows,
        [
            ("lnk_gone".into(), "discord".into(), Some(8)),
            ("lnk_old".into(), "browser".into(), Some(20)),
            ("lnk_player1".into(), "provider_proxy".into(), None),
            ("lnk_player2".into(), "discord".into(), None),
        ]
    );
    // The Discord consent BluMint's lookups read is untouched.
    let withdrawn: i64 = connection
        .query_row(
            "SELECT COUNT(*) FROM discord_withdrawals w JOIN discord_accounts a ON a.user_id = w.user_id
             WHERE w.connection_id = 'c' AND a.ember_id = 'emb_player2'",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(withdrawn, 1);
    // A second active link for the same Ember ID is still refused, and a
    // sign-in site's link is accepted once Player 1's ends.
    let insert = |id: &str, via: &str| {
        connection.execute(
            "INSERT INTO links (id, account_id, connection_id, ember_id, approved_via, claim_id, consented_at, approved_at)
             VALUES (?1, 'ext_player1', 'c', 'emb_player1', ?2, 'startgg:5', 50, 50)",
            [id, via],
        )
    };
    assert!(insert("lnk_twice", "site").is_err());
    connection
        .execute(
            "UPDATE links SET revoked_at = 60 WHERE id = 'lnk_player1'",
            [],
        )
        .unwrap();
    assert!(insert("lnk_other", "elsewhere").is_err());
    insert("lnk_site", "site").unwrap();
    for table in [
        "site_accounts",
        "site_sign_ins",
        "site_withdrawals",
        "site_requests",
    ] {
        assert!(has_table(&connection, table), "{table}");
    }
    // A site's sign-in row has a Discord sign-in's columns, after its
    // site.
    let columns = |table: &str| -> Vec<String> {
        connection
            .prepare(&format!(
                "SELECT name FROM pragma_table_info('{table}') ORDER BY cid"
            ))
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap()
    };
    let mut site_sign_ins = columns("site_sign_ins");
    assert_eq!(site_sign_ins.remove(0), "site");
    assert_eq!(site_sign_ins, columns("discord_sign_ins"));
    // A site's withdrawals go with its account row, and only that site's.
    connection
        .execute_batch(
            "INSERT INTO site_accounts (site, user_id, ember_id, username, username_key, connected_at)
               VALUES ('a', '1', 'emb_player1', 'player1', 'player1', 1), ('b', '1', 'emb_player1', 'player1', 'player1', 1);
             INSERT INTO site_withdrawals (site, user_id, connection_id) VALUES ('a', '1', 'c'), ('b', '1', 'c');
             DELETE FROM site_accounts WHERE site = 'a';",
        )
        .unwrap();
    let sites: Vec<String> = connection
        .prepare("SELECT site FROM site_withdrawals")
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(sites, ["b"]);
}

#[test]
fn removals_from_before_withdrawals_were_recorded_still_hold() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    migrate(&mut connection, &MIGRATIONS[..9]).unwrap();
    // Player 1 removed BluMint's link after signing in; Player 2 removed theirs
    // before signing in again. Player 1 also has a sign-in in flight.
    connection
        .execute_batch(
            "INSERT INTO tenants (id, name) VALUES ('bm', 'BluMint');
             INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
               VALUES ('bm-partner', 'bm', 'blumint', 'staging', 'BluMint', 1);
             INSERT INTO identities (ember_id, public_key, created_at) VALUES ('emb_player1', 'player1-key', 0), ('emb_player2', 'player2-key', 0);
             INSERT INTO external_accounts (id, connection_id, subject, participant_id, display_label, created_at)
               VALUES ('ext_player1', 'bm-partner', 'discord:1', 'epl_player1', 'player1', 0),
                      ('ext_player2', 'bm-partner', 'discord:2', 'epl_player2', 'player2', 0);
             INSERT INTO links (id, account_id, connection_id, ember_id, approved_via, claim_id, consented_at, approved_at, revoked_at, revoked_by)
               VALUES ('lnk_player1', 'ext_player1', 'bm-partner', 'emb_player1', 'discord', 'discord:1', 100, 100, 200, 'unlinked_by_player'),
                      ('lnk_player2', 'ext_player2', 'bm-partner', 'emb_player2', 'discord', 'discord:2', 100, 100, 200, 'unlinked_by_player');
             INSERT INTO discord_accounts (user_id, ember_id, username, connected_at)
               VALUES ('1', 'emb_player1', 'player1', 100), ('2', 'emb_player2', 'player2', 300);
             INSERT INTO discord_sign_ins (state_hash, ember_id, expires_at) VALUES (x'01', 'emb_player1', 900);",
        )
        .unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    let count = |connection: &Connection, sql: &str| -> i64 {
        connection.query_row(sql, [], |row| row.get(0)).unwrap()
    };
    // Both links came through the rebuild of `links` as they were.
    assert_eq!(
        count(
            &connection,
            "SELECT COUNT(*) FROM links WHERE connection_id = 'bm-partner' AND approved_via = 'discord'
               AND claim_id IN ('discord:1', 'discord:2') AND revoked_at = 200
               AND revoked_by = 'unlinked_by_player'"
        ),
        2
    );
    let withdrawn = "SELECT COUNT(*) FROM discord_withdrawals WHERE connection_id = 'bm-partner' AND user_id = ";
    assert_eq!(count(&connection, &format!("{withdrawn}'1'")), 1);
    assert_eq!(count(&connection, &format!("{withdrawn}'2'")), 0);
    assert_eq!(
        count(&connection, "SELECT COUNT(*) FROM discord_sign_ins"),
        0
    );
    // A new sign-in replaces Player 1's account row and gives the consent back.
    connection
        .execute_batch(
            "DELETE FROM discord_accounts WHERE user_id = '1';
             INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES ('1', 'emb_player1', 'player1', 400);",
        )
        .unwrap();
    assert_eq!(count(&connection, &format!("{withdrawn}'1'")), 0);
}

#[test]
fn rooms_from_before_watching_existed_keep_their_revision_as_the_fighters_revision() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    // The schema as it stood before watching (028).
    let before_watching: Vec<_> = MIGRATIONS
        .iter()
        .take_while(|migration| migration.version < 28)
        .copied()
        .collect();
    migrate(&mut connection, &before_watching).unwrap();
    connection
        .execute_batch(
            "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
             INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
               VALUES ('c', 't', 'direct', 'staging', 'Site', 1);
             INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
               assignment_generation, state, games_to_win, rules, required_build_id, metadata,
               delivery_state, created_at, updated_at)
               VALUES ('emt_live', 't', 'c', 'live', '', 1, 1, 'ready', 2, '{}', '', '{}', 'not_required', 1000, 1000);
             INSERT INTO match_rooms (match_id, binding_revision, bound_endpoints)
               VALUES ('emt_live', 3, '[]');
             INSERT INTO match_claims (match_id, ember_id, endpoint_id, helper_instance_id, build_id, claimed_at)
               VALUES ('emt_live', 'emb_p1', 'e', 'ins_x', 'build', 1000);",
        )
        .unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    let (fighters, watchers, link_holders): (i64, Option<String>, i64) = connection
        .query_row(
            "SELECT r.fighters_revision, r.bound_watchers, c.watch_link_holders
             FROM match_rooms r, provider_connections c WHERE r.match_id = 'emt_live' AND c.id = 'c'",
            [],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .unwrap();
    assert_eq!((fighters, watchers, link_holders), (3, None, 0));
    // A claim made before watching existed does not say its helper reads
    // watchers.
    let watch: bool = connection
        .query_row("SELECT watch FROM match_claims", [], |row| row.get(0))
        .unwrap();
    assert!(!watch);
}

#[test]
fn archive_games_kept_by_crc_give_way_to_games_kept_by_digest() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    migrate(&mut connection, &MIGRATIONS[..49]).unwrap();
    // A game 049 still holds and one it stored, whose replay is gone.
    connection
        .execute_batch(
            "INSERT INTO archive_games (crc, saved_at, size, sent_by, priority, replay, next_attempt_at, received_at)
               VALUES ('0badf00d', 1, 200, 'emb_p1', 1, x'00', 1, 1);
             INSERT INTO archive_games (crc, saved_at, size, sent_by, priority, state, next_attempt_at, received_at, stored_at)
               VALUES ('0badf00e', 1, 200, 'emb_p1', 1, 'stored', 1, 1, 2);",
        )
        .unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    let count = |table: &str| -> i64 {
        connection
            .query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |row| {
                row.get(0)
            })
            .unwrap()
    };
    assert_eq!(count("archive_games"), 0);
    assert_eq!(count("archive_sets"), 0);
    // Two games with one CRC are two games.
    connection
        .execute_batch(
            "INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, sent_by, priority, replay, next_attempt_at, received_at)
               VALUES ('aa', '0badf00d', 1, 200, 1, 2, 'emb_p1', 1, x'00', 1, 1),
                      ('bb', '0badf00d', 1, 200, 1, 2, 'emb_p1', 1, x'01', 1, 1);",
        )
        .unwrap();
    assert_eq!(count("archive_games"), 2);
    connection
        .prepare("SELECT attempt_id, archived FROM match_replays")
        .unwrap();
    connection
        .prepare("SELECT title, last_saved_at FROM archive_events UNION ALL SELECT code, 0 FROM archive_countries")
        .unwrap();
}

/// Migration 055 gives the games kept before it their sides' players,
/// their keys (stored games only) with the keys' counts, and the kept
/// total, before any of its triggers fire; the triggers keep them after.
#[test]
fn archive_games_kept_before_the_listing_index_are_indexed() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    migrate(&mut connection, &MIGRATIONS[..54]).unwrap();
    connection
        .execute_batch(
            "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
             INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
               VALUES ('c', 't', 'direct', 'staging', 'Site', 1);
             INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
               assignment_generation, state, games_to_win, rules, required_build_id, metadata,
               delivery_state, created_at, updated_at, expires_at, expiry_secs)
               VALUES ('emt_set', 't', 'c', 'set', '', 1, 2, 'completed', 2, '{}', '', '{}', 'not_required', 1, 1, 9, 9);
             INSERT INTO match_participants (match_id, assignment_generation, slot, participant_id, ember_id)
               VALUES ('emt_set', 1, 0, 'p-old', 'emb_old'), ('emt_set', 2, 0, 'p-x', 'emb_x'),
                      ('emt_set', 2, 1, 'p-y', 'emb_y');
             INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, name_a, country_a,
               sent_by, sent_side, priority, state, next_attempt_at, received_at, game)
               VALUES ('aa', '1', 5, 100, 3, 3, 'Player1', 'JP', 'emb_p1', 0, 1, 'stored', 0, 0, 'g1'),
                      ('bb', '2', 6, 200, 1, 2, NULL, NULL, 'emb_p1', NULL, 1, 'pending', 0, 0, 'g2'),
                      ('cc', '3', 7, 300, 1, 2, 'Player1', NULL, 'emb_p2', 1, 1, 'duplicate', 0, 0, 'g1');
             INSERT INTO archive_games (digest, crc, saved_at, size, fighter_a, fighter_b, match_id, event_title,
               round_label, phase, sent_by, priority, state, next_attempt_at, received_at, game)
               VALUES ('dd', '4', 8, 400, 4, 5, 'emt_set', 'Cup', 'Winners Final', 'Top 8', 'emb_x', 0, 'stored', 0, 0, 'g3');",
        )
        .unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    let players = |digest: &str| -> (Option<String>, Option<String>) {
        connection
            .query_row(
                "SELECT player_a, player_b FROM archive_games WHERE digest = ?1",
                [digest],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap()
    };
    assert_eq!(players("aa"), (Some("emb_p1".into()), None));
    assert_eq!(players("bb"), (None, None));
    assert_eq!(players("cc"), (None, Some("emb_p2".into())));
    assert_eq!(players("dd"), (Some("emb_x".into()), Some("emb_y".into())));
    let keys = |connection: &Connection| -> Vec<(String, String)> {
        connection
            .prepare("SELECT lower(hex(digest)), key FROM archive_keys ORDER BY digest, key")
            .unwrap()
            .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap()
    };
    let expected: Vec<(String, String)> = [
        ("aa", "*"),
        ("aa", "c:JP"),
        ("aa", "f:3"),
        ("aa", "n:Player1"),
        ("aa", "o"),
        ("aa", "t:0"),
        ("dd", "*"),
        ("dd", "e:Cup"),
        ("dd", "f:4"),
        ("dd", "f:5"),
        ("dd", "r:top8"),
        ("dd", "r:winners"),
        ("dd", "t:0"),
    ]
    .into_iter()
    .map(|(digest, key)| (digest.to_owned(), key.to_owned()))
    .collect();
    assert_eq!(keys(&connection), expected);
    let count = |connection: &Connection, key: &str| -> Option<i64> {
        connection
            .query_row(
                "SELECT games FROM archive_key_counts WHERE key = ?1",
                [key],
                |row| row.get(0),
            )
            .map(Some)
            .or_else(|error| match error {
                rusqlite::Error::QueryReturnedNoRows => Ok(None),
                error => Err(error),
            })
            .unwrap()
    };
    assert_eq!(count(&connection, "*"), Some(2));
    assert_eq!(count(&connection, "t:0"), Some(2));
    assert_eq!(count(&connection, "n:Player1"), Some(1));
    let kept: (i64, i64) = connection
        .query_row("SELECT games, bytes FROM archive_kept", [], |row| {
            Ok((row.get(0)?, row.get(1)?))
        })
        .unwrap();
    assert_eq!(kept, (4, 1000));
    // From here the triggers keep them: a game stored, one deleted.
    connection
        .execute_batch(
            "UPDATE archive_games SET state = 'stored' WHERE digest = 'bb';
             DELETE FROM archive_games WHERE digest = 'aa';",
        )
        .unwrap();
    assert_eq!(count(&connection, "*"), Some(2));
    assert_eq!(count(&connection, "n:Player1"), None);
    assert_eq!(count(&connection, "f:1"), Some(1));
    let kept: (i64, i64) = connection
        .query_row("SELECT games, bytes FROM archive_kept", [], |row| {
            Ok((row.get(0)?, row.get(1)?))
        })
        .unwrap();
    assert_eq!(kept, (3, 900));
}

/// What a bridge from before file names did with `migrations`: each applied
/// in its own transaction and recorded by number only, at time 1000.
fn as_an_older_bridge(connection: &mut Connection, migrations: &[(i64, &str)]) {
    connection
        .execute_batch(
            "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)",
        )
        .unwrap();
    for (version, sql) in migrations {
        let transaction = connection.transaction().unwrap();
        transaction.execute_batch(sql).unwrap();
        transaction
            .execute(
                "INSERT INTO schema_migrations (version, applied_at) VALUES (?1, 1000)",
                [version],
            )
            .unwrap();
        transaction.commit().unwrap();
    }
}

/// This bridge's files up to `last`, as an older bridge listed them.
fn files_to(last: i64) -> Vec<(i64, &'static str)> {
    MIGRATIONS
        .iter()
        .take_while(|migration| migration.version <= last)
        .map(|migration| (migration.version, migration.sql))
        .collect()
}

fn file(version: i64) -> Migration {
    *MIGRATIONS
        .iter()
        .find(|migration| migration.version == version)
        .unwrap()
}

/// `schema_migrations` as (version, name, checksum).
fn records(connection: &Connection) -> Vec<(i64, Option<String>, Option<String>)> {
    connection
        .prepare("SELECT version, name, checksum FROM schema_migrations ORDER BY version")
        .unwrap()
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap()
}

fn applied_at(connection: &Connection, version: i64) -> i64 {
    connection
        .query_row(
            "SELECT applied_at FROM schema_migrations WHERE version = ?1",
            [version],
            |row| row.get(0),
        )
        .unwrap()
}

/// Every file of this bridge is recorded by its number, name and checksum,
/// and nothing else is.
fn assert_every_file_recorded(connection: &Connection) {
    let expected: Vec<_> = MIGRATIONS
        .iter()
        .map(|migration| {
            (
                migration.version,
                Some(migration.name.to_owned()),
                Some(migration.checksum()),
            )
        })
        .collect();
    assert_eq!(records(connection), expected);
}

fn refusal(result: Result<(), OpenError>) -> String {
    match result {
        Err(OpenError::Migration(text)) => text,
        other => panic!("expected a refusal, got {other:?}"),
    }
}

fn wired_room(connection: &Connection) {
    connection
        .execute(
            "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at, wired_only)
             VALUES ('wired', 'Wired room', 8, 'build', 'emb_player1', 1000, 1)",
            [],
        )
        .unwrap();
}

fn wired_only(connection: &Connection) -> i64 {
    connection
        .query_row(
            "SELECT wired_only FROM rooms WHERE room_id = 'wired'",
            [],
            |row| row.get(0),
        )
        .unwrap()
}

#[test]
fn every_file_under_migrations_is_listed_once_in_order() {
    let mut files: Vec<String> =
        std::fs::read_dir(concat!(env!("CARGO_MANIFEST_DIR"), "/migrations"))
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
    files.sort();
    let listed: Vec<&str> = MIGRATIONS.iter().map(|migration| migration.name).collect();
    assert_eq!(files, listed);
    assert!(
        MIGRATIONS
            .windows(2)
            .all(|pair| pair[0].version < pair[1].version)
    );
}

#[test]
fn a_checksum_is_the_same_for_a_windows_checkout() {
    let unix = Migration {
        version: 1,
        name: "001_example.sql",
        sql: "SELECT 1;\nSELECT 2;\n",
    };
    let windows = Migration {
        sql: "SELECT 1;\r\nSELECT 2;\r\n",
        ..unix
    };
    assert_eq!(unix.checksum(), windows.checksum());
    let changed = Migration {
        sql: "SELECT 1;\n",
        ..unix
    };
    assert_ne!(unix.checksum(), changed.checksum());
}

#[test]
fn a_new_database_records_each_file_by_name_and_checksum() {
    let mut connection = Connection::open_in_memory().unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    let first = applied_at(&connection, 56);
    // Opening it again changes nothing.
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_eq!(applied_at(&connection, 56), first);
}

/// Production: an older bridge ran 001 to 054, the archive branch's 055,
/// then release's 056, recording numbers only.
#[test]
fn records_by_number_only_take_this_bridges_names_and_keep_when_they_ran() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(56));
    // An older bridge's record has no name or checksum columns at all.
    let named: bool = connection
        .query_row(
            "SELECT EXISTS (SELECT 1 FROM pragma_table_info('schema_migrations') WHERE name = 'name')",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert!(!named);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_eq!(applied_at(&connection, 55), 1000);
    assert_eq!(applied_at(&connection, 56), 1000);
}

#[test]
fn a_database_an_older_bridge_left_at_054_runs_055_and_056() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(54));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_eq!(applied_at(&connection, 54), 1000);
    assert_ne!(applied_at(&connection, 55), 1000);
    assert!(has_table(&connection, "archive_keys"));
    connection.prepare("SELECT wired_only FROM rooms").unwrap();
}

/// Staging and the Nightly bridge: release and nightly listed 056 and left
/// 055 out, so the archive listing index never ran there.
#[test]
fn a_database_at_056_without_055_runs_the_archive_listing_index() {
    let mut connection = Connection::open_in_memory().unwrap();
    let without_055: Vec<_> = files_to(56)
        .into_iter()
        .filter(|(version, _)| *version != 55)
        .collect();
    as_an_older_bridge(&mut connection, &without_055);
    wired_room(&connection);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert_ne!(applied_at(&connection, 55), 1000);
    assert_eq!(applied_at(&connection, 56), 1000);
    assert!(has_table(&connection, "archive_keys"));
    assert_eq!(wired_only(&connection), 1);
}

/// The wired-only rule was first committed as 055 (35494c167), the same
/// file 056 is now. A bridge built from it recorded the column as 55; the
/// bridge after it would then skip the index and fail 056 on the column.
#[test]
fn a_database_whose_055_was_the_wired_only_column_moves_it_to_056_and_runs_the_index() {
    let mut connection = Connection::open_in_memory().unwrap();
    let mut old = files_to(54);
    old.push((55, file(56).sql));
    as_an_older_bridge(&mut connection, &old);
    wired_room(&connection);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    // 56 is when the column came; 55 ran just now.
    assert_eq!(applied_at(&connection, 56), 1000);
    assert_ne!(applied_at(&connection, 55), 1000);
    assert!(has_table(&connection, "archive_keys"));
    assert_eq!(wired_only(&connection), 1);
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
}

#[test]
fn an_unnamed_055_that_is_neither_file_is_refused() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(54));
    connection
        .execute(
            "INSERT INTO schema_migrations (version, applied_at) VALUES (55, 1000)",
            [],
        )
        .unwrap();
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("055"), "{text}");
    // Nothing changed: the record has its two columns still, and the index
    // did not run.
    let columns: i64 = connection
        .query_row(
            "SELECT COUNT(*) FROM pragma_table_info('schema_migrations')",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(columns, 2);
    assert!(!has_table(&connection, "archive_keys"));
}

#[test]
fn a_changed_file_or_another_file_under_a_number_is_refused_by_name() {
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(56));
    migrate(&mut connection, MIGRATIONS).unwrap();
    let tamper = |connection: &Connection, set: &str| {
        connection
            .execute(
                &format!("UPDATE schema_migrations SET {set} WHERE version = 30"),
                [],
            )
            .unwrap();
    };
    tamper(&connection, "checksum = '00'");
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("030_ranking_choices.sql"), "{text}");
    tamper(
        &connection,
        &format!(
            "checksum = '{}', name = '030_elsewhere.sql'",
            file(30).checksum()
        ),
    );
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("030_elsewhere.sql"), "{text}");
    assert!(text.contains("030_ranking_choices.sql"), "{text}");
    tamper(&connection, "name = '030_ranking_choices.sql'");
    migrate(&mut connection, MIGRATIONS).unwrap();
}

#[test]
fn a_number_this_bridge_lacks_is_refused_below_its_newest_and_left_alone_above() {
    let mut connection = Connection::open_in_memory().unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    // A bridge from a line without 030 opens it.
    let without_030: Vec<_> = MIGRATIONS
        .iter()
        .filter(|migration| migration.version != 30)
        .copied()
        .collect();
    let text = refusal(migrate(&mut connection, &without_030));
    assert!(text.contains("030"), "{text}");
    // A bridge rolled back to before 055 leaves a newer bridge's 055 and 056.
    migrate(&mut connection, &MIGRATIONS[..54]).unwrap();
    assert_every_file_recorded(&connection);
}

#[test]
fn an_unnamed_record_is_adopted_up_to_059_and_refused_above() {
    // A bridge from before the ledger, at 056.
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(56));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);

    // Production and staging: the shared library's bridge ran its 057 to 059,
    // which this bridge carries byte for byte, and recorded numbers only.
    let mut connection = Connection::open_in_memory().unwrap();
    as_an_older_bridge(&mut connection, &files_to(59));
    migrate(&mut connection, MIGRATIONS).unwrap();
    assert_every_file_recorded(&connection);
    assert!(has_table(&connection, "fighter_levels"));

    // An unnamed 060 is no file this bridge knows: its own are named.
    let mut connection = Connection::open_in_memory().unwrap();
    let mut older = files_to(59);
    older.push((60, "SELECT 1;"));
    as_an_older_bridge(&mut connection, &older);
    let text = refusal(migrate(&mut connection, MIGRATIONS));
    assert!(text.contains("060"), "{text}");
    assert!(text.contains("another line"), "{text}");
    // Nothing of this bridge's was applied on top of it.
    assert!(!has_table(&connection, "fighter_levels"));
}

#[test]
fn a_new_database_runs_the_librarys_files_then_the_1_6_files() {
    let mut connection = Connection::open_in_memory().unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    let versions: Vec<i64> = records(&connection).iter().map(|r| r.0).collect();
    assert_eq!(&versions[55..], &[56, 57, 58, 59, 60, 61, 62]);
    assert!(has_table(&connection, "fighter_levels"));
    assert!(has_table(&connection, "social_accounts"));
    assert!(has_table(&connection, "account_points"));
}
