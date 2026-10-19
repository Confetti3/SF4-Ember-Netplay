//! Organizer grants (`routes::sign_in`, docs/design/TOURNAMENT_DESK.md): the
//! tokens an organizer sign-in kept for a connected site account, sealed
//! with the bridge's encryption key. A grant belongs to the account row in
//! `site_accounts` and goes with it. Every query on `organizer_grants` is
//! here.
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use zeroize::{Zeroize, Zeroizing};

use crate::{error::Result, secrets::Keys};

/// What a grant holds once opened.
#[derive(Serialize, Deserialize)]
pub struct Grant {
    pub access: String,
    pub refresh: Option<String>,
    /// The scopes the sign-in asked for.
    pub scope: String,
    /// When the access token lapses.
    pub expires_at: u64,
}

impl Drop for Grant {
    fn drop(&mut self) {
        self.access.zeroize();
        self.refresh.zeroize();
    }
}

impl Grant {
    pub fn seal(&self, keys: &Keys) -> Vec<u8> {
        let plain = Zeroizing::new(serde_json::to_vec(self).expect("a grant serializes"));
        keys.seal(&plain)
    }

    /// The grant `sealed` holds, or None if it does not open under `keys`.
    pub fn open(keys: &Keys, sealed: &[u8]) -> Option<Self> {
        serde_json::from_slice(&keys.open(sealed)?).ok()
    }
}

/// Stores `sealed` as the grant of the account `user_id` on `site`, if the
/// account is still connected.
pub fn save(
    tx: &Transaction<'_>,
    site: &str,
    user_id: &str,
    sealed: &[u8],
    now: u64,
) -> Result<()> {
    tx.execute(
        "INSERT INTO organizer_grants (site, user_id, sealed, refreshed_at)
         SELECT ?1, ?2, ?3, ?4 FROM site_accounts WHERE site = ?1 AND user_id = ?2
         ON CONFLICT (site, user_id) DO UPDATE SET sealed = excluded.sealed,
             refreshed_at = excluded.refreshed_at",
        params![site, user_id, sealed, now],
    )?;
    Ok(())
}

/// The account and sealed grant the Ember ID holds on `site`, if any.
pub fn held(tx: &Transaction<'_>, site: &str, ember_id: &str) -> Result<Option<(String, Vec<u8>)>> {
    Ok(tx
        .query_row(
            "SELECT g.user_id, g.sealed FROM organizer_grants g
             JOIN site_accounts a ON a.site = g.site AND a.user_id = g.user_id
             WHERE g.site = ?1 AND a.ember_id = ?2",
            params![site, ember_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?)
}

/// Removes the Ember ID's grant on `site`; the account stays.
pub fn remove(tx: &Transaction<'_>, site: &str, ember_id: &str) -> Result<()> {
    tx.execute(
        "DELETE FROM organizer_grants WHERE site = ?1 AND user_id IN
             (SELECT user_id FROM site_accounts WHERE site = ?1 AND ember_id = ?2)",
        params![site, ember_id],
    )?;
    Ok(())
}
