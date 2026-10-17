//! What the bridge holds under each of BluMint's creation `Idempotency-Key`s,
//! which is how a close names a match. A key that made a match is the
//! match's `external_match_id`, as `blumint:<key>`, for as long as the match
//! exists, since the answer stored under it (`routes::idempotency`) is
//! forgotten after a day.
use rusqlite::{OptionalExtension, Transaction, params};

/// Who a creation's key belongs to among the stored answers. Each
/// registration issues a new credential, so it is the connection, not the
/// credential.
pub fn owner(connection_id: &str) -> String {
    format!("connection:{connection_id}")
}

/// The `external_match_id` of the match a creation under `key` makes. The
/// prefix keeps it apart from the lobby and tournament sets' own.
pub fn external_id(key: &str) -> String {
    format!("blumint:{key}")
}

/// The match the connection's key made, if it made one.
pub fn made_under(
    tx: &Transaction<'_>,
    tenant_id: &str,
    connection_id: &str,
    key: &str,
) -> rusqlite::Result<Option<String>> {
    tx.query_row(
        "SELECT id FROM matches WHERE tenant_id = ?1 AND connection_id = ?2 AND external_match_id = ?3",
        params![tenant_id, connection_id, external_id(key)],
        |row| row.get(0),
    )
    .optional()
}
