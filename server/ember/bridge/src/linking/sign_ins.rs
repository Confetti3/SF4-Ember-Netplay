//! The sign-ins in flight on each site (`routes::sign_in`), one table per
//! site, shaped alike: a keyed hash of the state the browser carries, the
//! Ember ID that started it and when it lapses; whether an answer claimed it;
//! and, once the bridge's page asks the player, the account it asks about
//! with the Ember ID that held it then. Every query on those tables is here.
use rusqlite::{OptionalExtension, Transaction, params};

use super::Site;
use crate::error::Result;

fn table(site: Site) -> &'static str {
    match site {
        Site::Discord => "discord_sign_ins",
        Site::Challonge => "challonge_sign_ins",
    }
}

pub fn insert(
    tx: &Transaction<'_>,
    site: Site,
    hash: &[u8; 32],
    ember_id: &str,
    expires_at: u64,
) -> Result<()> {
    tx.execute(
        &format!(
            "INSERT INTO {} (state_hash, ember_id, expires_at) VALUES (?1, ?2, ?3)",
            table(site)
        ),
        params![hash.as_slice(), ember_id, expires_at],
    )?;
    Ok(())
}

/// Claims the sign-in `hash` names for the first answer that arrives for it,
/// and says whether this was that answer.
pub fn claim(tx: &Transaction<'_>, site: Site, hash: &[u8; 32], now: u64) -> Result<bool> {
    Ok(tx.execute(
        &format!(
            "UPDATE {} SET claimed = 1 WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 0",
            table(site)
        ),
        params![hash.as_slice(), now],
    )? == 1)
}

/// The Ember ID that started the claimed sign-in `hash` names, while it is
/// still there and has asked nothing yet.
pub fn claimed(
    tx: &Transaction<'_>,
    site: Site,
    hash: &[u8; 32],
    now: u64,
) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            &format!(
                "SELECT ember_id FROM {}
                 WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 1 AND pending_user_id IS NULL",
                table(site)
            ),
            params![hash.as_slice(), now],
            |row| row.get(0),
        )
        .optional()?)
}

/// The account a page asks about, and the Ember ID that held it then.
pub struct Question {
    pub ember_id: String,
    pub user_id: String,
    pub username: String,
    pub owner: Option<String>,
}

/// Turns the claimed sign-in `hash` names into the page's question, keyed by
/// the question's own secret from here on: the state stops naming it.
pub fn ask(
    tx: &Transaction<'_>,
    site: Site,
    hash: &[u8; 32],
    secret: &[u8; 32],
    question: &Question,
) -> Result<()> {
    tx.execute(
        &format!(
            "UPDATE {} SET state_hash = ?2, pending_user_id = ?3, pending_username = ?4, pending_owner = ?5
             WHERE state_hash = ?1",
            table(site)
        ),
        params![
            hash.as_slice(),
            secret.as_slice(),
            question.user_id,
            question.username,
            question.owner
        ],
    )?;
    Ok(())
}

/// Takes the claimed sign-in `hash` names if it is still there: the Ember ID
/// that started it.
pub fn take(tx: &Transaction<'_>, site: Site, hash: &[u8; 32], now: u64) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            &format!(
                "DELETE FROM {} WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 1
                 RETURNING ember_id",
                table(site)
            ),
            params![hash.as_slice(), now],
            |row| row.get(0),
        )
        .optional()?)
}

/// Takes the question `secret` keys, if it is still waiting for its answer.
pub fn take_question(
    tx: &Transaction<'_>,
    site: Site,
    secret: &[u8; 32],
    now: u64,
) -> Result<Option<Question>> {
    Ok(tx
        .query_row(
            &format!(
                "DELETE FROM {}
                 WHERE state_hash = ?1 AND expires_at > ?2 AND claimed = 1 AND pending_user_id IS NOT NULL
                 RETURNING ember_id, pending_user_id, pending_username, pending_owner",
                table(site)
            ),
            params![secret.as_slice(), now],
            |row| {
                Ok(Question {
                    ember_id: row.get(0)?,
                    user_id: row.get(1)?,
                    username: row.get(2)?,
                    owner: row.get(3)?,
                })
            },
        )
        .optional()?)
}

/// Ends the Ember ID's sign-ins in flight on `site`.
pub fn end(tx: &Transaction<'_>, site: Site, ember_id: &str) -> Result<()> {
    tx.execute(
        &format!("DELETE FROM {} WHERE ember_id = ?1", table(site)),
        [ember_id],
    )?;
    Ok(())
}

/// Drops the sign-ins no site answered in time.
pub fn expire(tx: &Transaction<'_>, now: u64) -> Result<()> {
    for site in Site::ALL {
        tx.execute(
            &format!("DELETE FROM {} WHERE expires_at <= ?1", table(site)),
            [now],
        )?;
    }
    Ok(())
}
