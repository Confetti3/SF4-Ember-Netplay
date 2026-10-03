//! What a connection's platform expects of its matches (`config::Policy`),
//! read from the kind and disputes setting the connection's record keeps.
//! Neither ever changes, and the record stays when the connection leaves the
//! configuration, so its matches keep their policy.
use ember_protocol::api::ErrorCode;
use rusqlite::{OptionalExtension, Transaction};

use crate::{
    AppState,
    config::{Disputes, Policy},
    error::{ApiFailure, Result},
};

/// The policy of the connection's platform.
pub fn of(tx: &Transaction<'_>, connection_id: &str) -> Result<Policy> {
    let (kind, disputes): (String, String) = tx
        .query_row(
            "SELECT kind, disputes FROM provider_connections WHERE id = ?1",
            [connection_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?
        .ok_or_else(ApiFailure::unavailable)?;
    let disputes = Disputes::parse(&disputes).ok_or_else(ApiFailure::unavailable)?;
    Ok(Policy::of(&kind, disputes))
}

/// Refuses the generic match, lobby and tournament routes to a connection
/// whose platform makes its matches through its own API
/// (`Policy::own_api_only`).
pub async fn generic_connection(state: &AppState, connection_id: &str) -> Result<()> {
    let id = connection_id.to_owned();
    let own_api = state
        .db
        .read(move |tx| Ok(of(tx, &id)?.own_api_only))
        .await?;
    if own_api {
        return Err(ApiFailure::new(
            ErrorCode::Forbidden,
            "This connection's platform creates its matches through its own API.",
        ));
    }
    Ok(())
}

/// Whether the connection's record is enabled.
pub fn enabled(tx: &Transaction<'_>, connection_id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT enabled FROM provider_connections WHERE id = ?1",
        [connection_id],
        |row| row.get(0),
    )?)
}

/// `enabled`, read on its own; false when it cannot be read.
pub async fn stored_enabled(state: &AppState, connection_id: &str) -> bool {
    let id = connection_id.to_owned();
    state
        .db
        .read(move |tx| enabled(tx, &id))
        .await
        .unwrap_or(false)
}
