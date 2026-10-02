//! What a connection's platform expects of its matches (`config::Policy`),
//! read from the kind the connection's record keeps. A kind never changes,
//! and the record stays when the connection leaves the configuration, so its
//! matches keep their policy.
use ember_protocol::api::ErrorCode;
use rusqlite::{OptionalExtension, Transaction};

use crate::{
    AppState,
    config::Policy,
    error::{ApiFailure, Result},
};

/// The policy of the connection's platform.
pub fn of(tx: &Transaction<'_>, connection_id: &str) -> Result<Policy> {
    let kind: String = tx
        .query_row(
            "SELECT kind FROM provider_connections WHERE id = ?1",
            [connection_id],
            |row| row.get(0),
        )
        .optional()?
        .ok_or_else(ApiFailure::unavailable)?;
    Ok(Policy::of(&kind))
}

/// Refuses the generic match, lobby and tournament routes to a connection
/// whose platform makes its matches through its own API
/// (`Policy::sends_results`).
pub async fn generic_connection(state: &AppState, connection_id: &str) -> Result<()> {
    let id = connection_id.to_owned();
    let own_api = state
        .db
        .read(move |tx| Ok(of(tx, &id)?.sends_results))
        .await?;
    if own_api {
        return Err(ApiFailure::new(
            ErrorCode::Forbidden,
            "This connection's platform creates its matches through its own API.",
        ));
    }
    Ok(())
}
