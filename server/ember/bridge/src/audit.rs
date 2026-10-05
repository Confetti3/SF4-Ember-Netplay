//! The audit log: one row per privileged action, written in the same
//! transaction as the change it records.
use rusqlite::{Transaction, params};

use crate::error::Result;

pub fn audit(
    tx: &Transaction<'_>,
    now: u64,
    (class, id): (&str, String),
    action: &str,
    target: &str,
    result: &str,
    reason: Option<&str>,
) -> Result<()> {
    tx.execute(
        "INSERT INTO audit_entries (at, actor_class, actor_id, action, target, result, reason)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![now, class, id, action, target, result, reason],
    )?;
    Ok(())
}
