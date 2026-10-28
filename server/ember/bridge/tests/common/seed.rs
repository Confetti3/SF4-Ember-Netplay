//! Rows tests write straight into the bridge's database in place of a
//! sign-in or a game, so the SQL that restates the schema is in one place.
use ember_bridge::AppState;

/// Writes the Discord account a Discord sign-in would, connected at second
/// 1 (tests/discord.rs covers the sign-in itself).
pub async fn link_discord(state: &AppState, user_id: &str, ember_id: &str, name: &str) {
    link_discord_at(state, user_id, ember_id, name, 1).await;
}

/// `link_discord`, connected at second `connected_at`.
pub async fn link_discord_at(
    state: &AppState,
    user_id: &str,
    ember_id: &str,
    name: &str,
    connected_at: u64,
) {
    let (user_id, ember_id, name) = (user_id.to_owned(), ember_id.to_owned(), name.to_owned());
    state
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, ?3, ?4)",
                rusqlite::params![user_id, ember_id, name, connected_at],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

/// Writes an account on the sign-in site `site`, connected at second 1. A
/// row the database refuses, as a second account for the same user, is the
/// error.
pub async fn link_site(
    state: &AppState,
    site: &str,
    user_id: &str,
    ember_id: &str,
    username: &str,
    username_key: Option<&str>,
) -> rusqlite::Result<()> {
    let (site, user_id, ember_id, username, username_key) = (
        site.to_owned(),
        user_id.to_owned(),
        ember_id.to_owned(),
        username.to_owned(),
        username_key.map(str::to_owned),
    );
    state
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO site_accounts (site, user_id, ember_id, username, username_key, connected_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, 1)",
                rusqlite::params![site, user_id, ember_id, username, username_key],
            )?;
            Ok(())
        })
        .await
        .map_err(|_| rusqlite::Error::QueryReturnedNoRows)
}

/// Gives the match a permitted game nobody reports, which times out.
pub async fn silent_permitted_attempt(state: &AppState, match_id: &str) {
    let match_id = match_id.to_owned();
    state
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO attempts (id, match_id, assignment_generation, seq, source, state, created_at, start_by, permit_id)
                 VALUES ('att_silent', ?1, 1, 1, 'player_agreement', 'permitted', 0, 0, 'pmt_silent')",
                [&match_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}
