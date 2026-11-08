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
fn a_stored_name_not_known_fails_the_read_and_says_so() {
    let connection = Connection::open_in_memory().unwrap();
    let read = |text: &str| {
        connection.query_row("SELECT ?1", [text], |row| {
            known(row, 0, |text| (text == "queued").then_some(()))
        })
    };
    read("queued").unwrap();
    let error = read("a_later_state").unwrap_err();
    assert!(unknown(&error), "{error:?}");
    // Other failures are not mistaken for it.
    let error = connection
        .query_row("SELECT 1", [], |row| row.get::<_, String>(0))
        .unwrap_err();
    assert!(!unknown(&error));
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
    let listed: Vec<i64> = MIGRATIONS.iter().map(|(version, _)| *version).collect();
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
fn rooms_from_before_games_existed_are_sf4_rooms() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    let before = MIGRATIONS
        .iter()
        .position(|(version, _)| *version == 57)
        .unwrap();
    migrate(&mut connection, &MIGRATIONS[..before]).unwrap();
    connection
        .execute_batch(
            "INSERT INTO rooms (room_id, name, capacity, build_id, creator_ember_id, created_at)
               VALUES ('old', 'Old room', 8, 'build', 'emb_old', 1000);",
        )
        .unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    let game: String = connection
        .query_row("SELECT game FROM rooms WHERE room_id = 'old'", [], |row| {
            row.get(0)
        })
        .unwrap();
    assert_eq!(game, ember_protocol::rooms::DEFAULT_GAME);
}

#[test]
fn ratings_sittings_and_choices_from_before_games_existed_are_sf4s() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    let before = MIGRATIONS
        .iter()
        .position(|(version, _)| *version == 58)
        .unwrap();
    migrate(&mut connection, &MIGRATIONS[..before]).unwrap();
    connection
        .execute_batch(
            "INSERT INTO skill_ratings (discord_user_id, rating, deviation, volatility, band, played_at)
               VALUES ('100', 1612.5, 81.25, 0.06, 6, 1000);
             INSERT INTO rated_pairs (first, second, games, played_at) VALUES ('100', '200', 9, 1000);
             INSERT INTO ranking_choices (discord_user_id, ranked, changed_at, name, country)
               VALUES ('100', 1, 900, 'Player 1', 'GB'), ('200', 0, NULL, NULL, NULL);",
        )
        .unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    type Rating = (String, String, f64, f64, f64, u8, i64);
    let rating: Rating = connection
        .query_row("SELECT * FROM skill_ratings", [], |row| {
            Ok((
                row.get(0)?,
                row.get(1)?,
                row.get(2)?,
                row.get(3)?,
                row.get(4)?,
                row.get(5)?,
                row.get(6)?,
            ))
        })
        .unwrap();
    assert_eq!(
        rating,
        ("usf4".into(), "100".into(), 1612.5, 81.25, 0.06, 6, 1000)
    );
    let pair: (String, String, String, i64, i64) = connection
        .query_row("SELECT * FROM rated_pairs", [], |row| {
            Ok((
                row.get(0)?,
                row.get(1)?,
                row.get(2)?,
                row.get(3)?,
                row.get(4)?,
            ))
        })
        .unwrap();
    assert_eq!(pair, ("usf4".into(), "100".into(), "200".into(), 9, 1000));
    type Choice = (
        String,
        String,
        bool,
        Option<i64>,
        Option<String>,
        Option<String>,
    );
    let choices: Vec<Choice> = connection
        .prepare("SELECT * FROM ranking_choices ORDER BY discord_user_id")
        .unwrap()
        .query_map([], |row| {
            Ok((
                row.get(0)?,
                row.get(1)?,
                row.get(2)?,
                row.get(3)?,
                row.get(4)?,
                row.get(5)?,
            ))
        })
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(
        choices,
        vec![
            (
                "usf4".into(),
                "100".into(),
                true,
                Some(900),
                Some("Player 1".into()),
                Some("GB".into())
            ),
            ("usf4".into(), "200".into(), false, None, None, None),
        ]
    );
    // A row written without a game, as before, is SF4's; each game keeps
    // its own row for the same account.
    connection
        .execute_batch(
            "INSERT INTO skill_ratings (discord_user_id, rating, deviation, volatility, band, played_at)
               VALUES ('300', 1500, 350, 0.06, 0, 2000);
             INSERT INTO skill_ratings (game, discord_user_id, rating, deviation, volatility, band, played_at)
               VALUES ('sfv', '100', 1500, 350, 0.06, 0, 2000);",
        )
        .unwrap();
    let games: Vec<(String, String)> = connection
        .prepare(
            "SELECT game, discord_user_id FROM skill_ratings ORDER BY game, discord_user_id",
        )
        .unwrap()
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?)))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(
        games,
        vec![
            ("sfv".into(), "100".into()),
            ("usf4".into(), "100".into()),
            ("usf4".into(), "300".into()),
        ]
    );
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
        assert!(table_exists(&connection, table), "{table}");
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
    // A new sign-in replaces Kate's account row and gives the consent back.
    connection
        .execute_batch(
            "DELETE FROM discord_accounts WHERE user_id = '1';
             INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES ('1', 'emb_kate', 'kate', 400);",
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
        .take_while(|(version, _)| *version < 28)
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

#[test]
fn matches_lobbies_and_tournaments_from_before_games_existed_are_usf4s() {
    let mut connection = Connection::open_in_memory().unwrap();
    connection
        .pragma_update(None, "foreign_keys", "ON")
        .unwrap();
    let before = MIGRATIONS
        .iter()
        .position(|(version, _)| *version == 59)
        .unwrap();
    migrate(&mut connection, &MIGRATIONS[..before]).unwrap();
    let old_rows = |suffix: &str| {
        format!(
            "INSERT INTO lobbies (id, tenant_id, connection_id, external_lobby_id, create_digest, revision, state,
                                  games_to_win, rotation, required_build_id, metadata, sets_started, sets_completed,
                                  next_position, streak, created_at, updated_at)
               VALUES ('lob_{suffix}', 't', 'c', 'l{suffix}', '', 1, 'open', 2, 'winner_stays', '', '{{}}', 1, 0, 1, 0, 0, 0);
             INSERT INTO tournaments (id, tenant_id, connection_id, external_tournament_id, create_digest, revision,
                                      state, format, games_to_win, finals_games_to_win, grand_final_reset,
                                      required_build_id, metadata, created_at, updated_at)
               VALUES ('etn_{suffix}', 't', 'c', 't{suffix}', '', 1, 'registration', 'round_robin', 2, 2, 0, '', '{{}}', 0, 0);
             INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
               assignment_generation, state, games_to_win, rules, required_build_id, metadata,
               delivery_state, created_at, updated_at, lobby_id, tournament_id)
               VALUES ('emt_{suffix}', 't', 'c', 'm{suffix}', '', 1, 1, 'completed', 2, '{{}}', '', '{{}}',
                       'not_required', 1000, 1000, 'lob_{suffix}', 'etn_{suffix}');"
        )
    };
    connection
        .execute_batch(&format!(
            "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
             INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
               VALUES ('c', 't', 'direct', 'staging', 'Site', 1);
             {}",
            old_rows("old")
        ))
        .unwrap();
    migrate(&mut connection, MIGRATIONS).unwrap();
    // The binary from before 059 writes these rows as it always did, with
    // no game, on the migrated database.
    connection.execute_batch(&old_rows("written")).unwrap();
    let game = |table: &str, id: &str| -> String {
        connection
            .query_row(
                &format!("SELECT game FROM {table} WHERE id = ?1"),
                [id],
                |row| row.get(0),
            )
            .unwrap()
    };
    for suffix in ["old", "written"] {
        assert_eq!(game("matches", &format!("emt_{suffix}")), "usf4");
        assert_eq!(game("lobbies", &format!("lob_{suffix}")), "usf4");
        assert_eq!(game("tournaments", &format!("etn_{suffix}")), "usf4");
    }
}
