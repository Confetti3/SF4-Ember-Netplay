//! Code-based account linking (spec 11): the state changes of an intent, a
//! claim and a link, with no HTTP in them.
//!
//! A verified external account (a provider backend, or the mock provider's
//! browser login) creates an intent and sees a short code once. The player
//! enters it in Ember, whose helper submits a signed claim. The verified
//! account then approves that exact claim and Ember ID. A code alone never
//! finishes a link.
//!
//! Every function takes the caller's transaction, so a link change and the
//! events, audit rows and propagation it causes commit together. The route
//! and mock-browser modules adapt requests to these functions.
mod approval;
mod claim;
mod code;
mod intent;
mod revoke;

pub use approval::{ApproveCommand, approve, reject};
pub use claim::{ClaimCommand, cancel_claim, claim};
pub use code::{CODE_LIFETIME_SECS, check_subject};
pub use intent::{IntentCreated, Owner, browser_intent, cancel_intent, create_intent, view_intent};
pub use revoke::{Unlinker, revoke_link, unlink};

use rusqlite::{Transaction, params};

use crate::{error::Result, util::new_id};

/// The bridge record for an external subject, created on first use.
/// Subjects are opaque strings compared byte for byte (LINK-11).
pub fn account(
    tx: &Transaction<'_>,
    connection_id: &str,
    subject: &str,
    label: &str,
    now: u64,
) -> Result<(String, String)> {
    check_subject(subject)?;
    let label: String = label.chars().filter(|c| !c.is_control()).take(64).collect();
    tx.execute(
        "INSERT OR IGNORE INTO external_accounts (id, connection_id, subject, participant_id, display_label, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![new_id("ext"), connection_id, subject, new_id("epl"), label, now],
    )?;
    Ok(tx.query_row(
        "SELECT id, participant_id FROM external_accounts WHERE connection_id = ?1 AND subject = ?2",
        params![connection_id, subject],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?)
}

/// Expires intents past their time and every pending claim on an expired
/// intent. The one place an intent and its claim end together.
pub fn expire(tx: &Transaction<'_>, now: u64) -> Result<()> {
    tx.execute(
        "UPDATE link_intents SET state = 'expired' WHERE state IN ('created', 'claim_pending') AND expires_at <= ?1",
        [now],
    )?;
    tx.execute(
        "UPDATE link_claims SET state = 'expired', decided_at = ?1
         WHERE state = 'pending' AND intent_id IN (SELECT id FROM link_intents WHERE state = 'expired')",
        [now],
    )?;
    Ok(())
}
