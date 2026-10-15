//! Tournaments: single elimination, double elimination and round robin run
//! by the bridge on a provider connection (an extension beyond EMBER-TB-001).
//!
//! The bracket's layout comes from `ember_protocol::tournament::plan`, worked
//! out again from the format and entrant count whenever it is needed; only
//! each node's state is stored. Each set is an ordinary match, so scoring,
//! adjudication and the busy check are the match code's. When a set completes
//! the bracket moves on in the same transaction: byes and walkovers are
//! decided, and every set whose players are known and free gets its match. A
//! set whose player is busy elsewhere waits, and starts as soon as that match
//! ends; lobbies leave such a player alone so the bracket comes first.
//!
//! This module is the domain: the persisted tournament and bracket, how the
//! bracket progresses, corrections and standings. It knows nothing of HTTP;
//! `routes::tournaments` adapts requests and builds the response bodies. Every
//! function takes the caller's transaction, so a set completing, a link ending
//! or a correction moves the bracket in the same commit.
mod correction;
mod lifecycle;
mod progress;
mod standings;
mod state;

pub use correction::{check_correction, on_match_completed, on_reopen};
pub use lifecycle::{cancel, create, on_unlink, register, start, withdraw_participant};
pub use progress::{on_players_free, waiting};
pub use standings::round_robin_table;
pub use state::{entrants, load_bracket, set_json};

use std::collections::BTreeMap;

use ember_protocol::{api::ErrorCode, event::Kind, tournament::Format};
use rusqlite::{OptionalExtension, Transaction, params};

use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
};

/// `external_match_id` prefix of tournament sets; providers cannot create one.
pub const MATCH_PREFIX: &str = "tournament:";

pub struct Tournament {
    pub id: String,
    pub tenant_id: String,
    pub connection_id: String,
    pub external_tournament_id: String,
    pub revision: u64,
    pub state: String,
    pub format: Format,
    pub games_to_win: u8,
    pub finals_games_to_win: u8,
    pub grand_final_reset: bool,
    pub required_build_id: String,
    pub metadata: BTreeMap<String, String>,
}

pub fn load(tx: &Transaction<'_>, id: &str) -> Result<Option<Tournament>> {
    tx.query_row(
        "SELECT id, tenant_id, connection_id, external_tournament_id, revision, state, format, games_to_win,
                finals_games_to_win, grand_final_reset, required_build_id, metadata
         FROM tournaments WHERE id = ?1",
        [id],
        |row| {
            Ok(Tournament {
                id: row.get(0)?,
                tenant_id: row.get(1)?,
                connection_id: row.get(2)?,
                external_tournament_id: row.get(3)?,
                revision: row.get(4)?,
                state: row.get(5)?,
                format: Format::parse(&row.get::<_, String>(6)?).unwrap_or(Format::SingleElimination),
                games_to_win: row.get(7)?,
                finals_games_to_win: row.get(8)?,
                grand_final_reset: row.get(9)?,
                required_build_id: row.get(10)?,
                metadata: serde_json::from_str(&row.get::<_, String>(11)?).unwrap_or_default(),
            })
        },
    )
    .optional()
    .map_err(Into::into)
}

/// Whether a request that may be retried changed anything. Both answer with
/// the tournament as it now stands.
pub enum Applied {
    Unchanged(Tournament),
    Changed(Tournament),
}

fn touch(tx: &Transaction<'_>, tournament_id: &str, now: u64) -> Result<u64> {
    Ok(tx.query_row(
        "UPDATE tournaments SET revision = revision + 1, updated_at = ?2 WHERE id = ?1 RETURNING revision",
        params![tournament_id, now],
        |row| row.get(0),
    )?)
}

fn tournament_event(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tournament: &Tournament,
    kind: Kind,
    mut data: serde_json::Value,
    revision: u64,
) -> Result<i64> {
    data["tournament_id"] = tournament.id.clone().into();
    data["tournament_revision"] = revision.to_string().into();
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind,
            tenant_id: &tournament.tenant_id,
            connection_id: Some(&tournament.connection_id),
            subject: format!("tournaments/{}", tournament.id),
            match_id: None,
            ember_id: None,
            lobby_id: None,
            tournament_id: Some(&tournament.id),
            data,
        },
    )
}

fn closed() -> ApiFailure {
    ApiFailure::new(
        ErrorCode::StaleRevision,
        "The tournament is over or cancelled.",
    )
}
