//! The sign-ins in flight (`routes::sign_in`): Discord's in its own table,
//! every configured site's in `site_sign_ins`, shaped alike: a keyed hash of
//! the state the browser carries, the Ember ID that started it and when it
//! lapses; whether an answer claimed it; and, once the bridge's page asks the
//! player, the account it asks about with the Ember ID that held it then.
//! Every query on those tables is here.
use rusqlite::{OptionalExtension, Transaction, named_params, params};

use super::site::{Site, Tables};
use crate::error::Result;

const SIGN_INS: Tables = Tables {
    discord: "discord_sign_ins",
    sites: "site_sign_ins",
};

pub fn insert(
    tx: &Transaction<'_>,
    site: &Site,
    hash: &[u8; 32],
    ember_id: &str,
    expires_at: u64,
) -> Result<()> {
    match site {
        Site::Discord => tx.execute(
            "INSERT INTO discord_sign_ins (state_hash, ember_id, expires_at) VALUES (?1, ?2, ?3)",
            params![hash.as_slice(), ember_id, expires_at],
        )?,
        Site::Configured(id) => tx.execute(
            "INSERT INTO site_sign_ins (site, state_hash, ember_id, expires_at) VALUES (?1, ?2, ?3, ?4)",
            params![id, hash.as_slice(), ember_id, expires_at],
        )?,
    };
    Ok(())
}

/// Claims the sign-in `hash` names for the first answer that arrives for it,
/// and says whether this was that answer.
pub fn claim(tx: &Transaction<'_>, site: &Site, hash: &[u8; 32], now: u64) -> Result<bool> {
    let (table, scope) = site.scope(SIGN_INS);
    Ok(tx.execute(
        &format!(
            "UPDATE {table} SET claimed = 1
             WHERE {scope}state_hash = :hash AND expires_at > :now AND claimed = 0"
        ),
        &*site.bind(named_params! { ":hash": hash.as_slice(), ":now": now }),
    )? == 1)
}

/// The Ember ID that started the claimed sign-in `hash` names, while it is
/// still there and has asked nothing yet.
pub fn claimed(
    tx: &Transaction<'_>,
    site: &Site,
    hash: &[u8; 32],
    now: u64,
) -> Result<Option<String>> {
    let (table, scope) = site.scope(SIGN_INS);
    Ok(tx
        .query_row(
            &format!(
                "SELECT ember_id FROM {table}
                 WHERE {scope}state_hash = :hash AND expires_at > :now AND claimed = 1
                   AND pending_user_id IS NULL"
            ),
            &*site.bind(named_params! { ":hash": hash.as_slice(), ":now": now }),
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
    site: &Site,
    hash: &[u8; 32],
    secret: &[u8; 32],
    question: &Question,
) -> Result<()> {
    let (table, scope) = site.scope(SIGN_INS);
    tx.execute(
        &format!(
            "UPDATE {table} SET state_hash = :secret, pending_user_id = :user_id,
               pending_username = :username, pending_owner = :owner
             WHERE {scope}state_hash = :hash"
        ),
        &*site.bind(named_params! { ":hash": hash.as_slice(),
            ":secret": secret.as_slice(),
            ":user_id": question.user_id,
            ":username": question.username,
            ":owner": question.owner,
        }),
    )?;
    Ok(())
}

/// Takes the claimed sign-in `hash` names if it is still there: the Ember ID
/// that started it.
pub fn take(
    tx: &Transaction<'_>,
    site: &Site,
    hash: &[u8; 32],
    now: u64,
) -> Result<Option<String>> {
    let (table, scope) = site.scope(SIGN_INS);
    Ok(tx
        .query_row(
            &format!(
                "DELETE FROM {table}
                 WHERE {scope}state_hash = :hash AND expires_at > :now AND claimed = 1
                 RETURNING ember_id"
            ),
            &*site.bind(named_params! { ":hash": hash.as_slice(), ":now": now }),
            |row| row.get(0),
        )
        .optional()?)
}

/// Takes the question `secret` keys, if it is still waiting for its answer.
pub fn take_question(
    tx: &Transaction<'_>,
    site: &Site,
    secret: &[u8; 32],
    now: u64,
) -> Result<Option<Question>> {
    let (table, scope) = site.scope(SIGN_INS);
    Ok(tx
        .query_row(
            &format!(
                "DELETE FROM {table}
                 WHERE {scope}state_hash = :secret AND expires_at > :now AND claimed = 1
                   AND pending_user_id IS NOT NULL
                 RETURNING ember_id, pending_user_id, pending_username, pending_owner"
            ),
            &*site.bind(named_params! { ":secret": secret.as_slice(), ":now": now }),
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
pub fn end(tx: &Transaction<'_>, site: &Site, ember_id: &str) -> Result<()> {
    let (table, scope) = site.scope(SIGN_INS);
    tx.execute(
        &format!("DELETE FROM {table} WHERE {scope}ember_id = :ember_id"),
        &*site.bind(named_params! { ":ember_id": ember_id }),
    )?;
    Ok(())
}

/// Ends the Ember ID's sign-ins in flight on every site, Discord's included.
pub fn end_everywhere(tx: &Transaction<'_>, ember_id: &str) -> Result<()> {
    for table in [SIGN_INS.discord, SIGN_INS.sites] {
        tx.execute(
            &format!("DELETE FROM {table} WHERE ember_id = ?1"),
            [ember_id],
        )?;
    }
    Ok(())
}

/// Drops the sign-ins no site answered in time, on every site.
pub fn expire(tx: &Transaction<'_>, now: u64) -> Result<()> {
    for table in [SIGN_INS.discord, SIGN_INS.sites] {
        tx.execute(
            &format!("DELETE FROM {table} WHERE expires_at <= ?1"),
            [now],
        )?;
    }
    Ok(())
}
