//! Creating a match, by a provider (`routes::matches`), a lobby for its next
//! set, a tournament for a bracket set or BluMint: every path meets in
//! `insert_match`.
use std::collections::BTreeMap;

use ember_protocol::{
    EmberId,
    api::ErrorCode,
    encoding::Counter,
    event::{EventParticipant, Kind, MatchCreatedData},
    matches::{MatchState, Participant as Assigned, Rules},
};
use rusqlite::{Transaction, params};

use super::{busy, load, match_event, participants};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    routes::{expiry, policy},
};

/// Results entered by an organizer; Ember plays no part in the match.
pub const ORGANIZER_PROFILE: &str = "organizer-reported-v1";

/// A match about to be created, by a provider or by a lobby for its next set.
pub struct NewMatch<'a> {
    pub tenant_id: &'a str,
    pub connection_id: &'a str,
    pub external_match_id: &'a str,
    pub digest: &'a str,
    pub rules: &'a Rules,
    pub required_build_id: &'a str,
    pub metadata: &'a BTreeMap<String, String>,
    /// Ordered by slot.
    pub participants: [&'a Assigned; 2],
    /// Games each slot has already won when the match starts (`019`).
    pub starting_wins: [u8; 2],
    pub lobby_id: Option<&'a str>,
    pub tournament_id: Option<&'a str>,
}

/// Whether `participant_id` on the connection is currently linked to `ember_id`.
pub fn linked(
    tx: &Transaction<'_>,
    connection_id: &str,
    participant_id: &str,
    ember_id: &EmberId,
) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM external_accounts a JOIN links l ON l.account_id = a.id
          WHERE a.connection_id = ?1 AND a.participant_id = ?2 AND l.ember_id = ?3 AND l.revoked_at IS NULL)",
        params![connection_id, participant_id, ember_id.as_str()],
        |row| row.get(0),
    )?)
}

/// Checks the roster against current links and other active matches, then
/// records the match and its `match.created` event. Returns the new ID.
pub fn insert_match(tx: &Transaction<'_>, ctx: &Ctx, new: &NewMatch<'_>) -> Result<String> {
    // Every creation path meets here, so the two fighters are told apart here.
    let [first, second] = new.participants;
    if first.ember_id == second.ember_id || first.participant_id == second.participant_id {
        return Err(ApiFailure::invalid(
            "A match is between two different players.",
        ));
    }
    for participant in new.participants {
        if !linked(
            tx,
            new.connection_id,
            &participant.participant_id,
            &participant.ember_id,
        )? {
            return Err(ApiFailure::invalid(
                "A participant is not currently linked to that Ember ID on this connection.",
            )
            .detail("slot", participant.slot));
        }
    }
    for participant in new.participants {
        if busy(tx, new.connection_id, &participant.ember_id, "")? {
            return Err(ApiFailure::new(
                ErrorCode::LeaseConflict,
                "A participant already has an active match.",
            )
            .detail("slot", participant.slot));
        }
    }
    let id = crate::util::new_id("emt");
    // Whether the platform is sent the result is decided once, here; from
    // then on the match's own delivery state says.
    let sends_results = ctx
        .config
        .connection(new.connection_id)
        .is_some_and(|(_, connection)| connection.sends_results());
    let delivery = if sends_results {
        "queued"
    } else {
        "not_required"
    };
    let policy = policy::of(tx, new.connection_id)?;
    let lifetime = expiry::lifetime(
        ctx,
        new.lobby_id.is_some() || new.tournament_id.is_some(),
        policy,
    );
    let expires_at = lifetime.map(|secs| ctx.now + secs);
    let ends_by = lifetime
        .and(policy.max_lifetime_secs)
        .map(|cap| ctx.now + cap);
    tx.execute(
        "INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
            assignment_generation, state, games_to_win, rules, required_build_id, metadata, delivery_state,
            created_at, updated_at, lobby_id, tournament_id, expires_at, expiry_secs, starting_wins_p1, starting_wins_p2, ends_by)
         VALUES (?1, ?2, ?3, ?4, ?5, 1, 1, 'awaiting_players', ?6, ?7, ?8, ?9, ?13, ?10, ?10, ?11, ?12, ?14, ?15,
            ?16, ?17, ?18)",
        params![
            id,
            new.tenant_id,
            new.connection_id,
            new.external_match_id,
            new.digest,
            new.rules.games_to_win,
            serde_json::to_string(new.rules).unwrap_or_default(),
            new.required_build_id,
            serde_json::to_string(new.metadata).unwrap_or_default(),
            ctx.now,
            new.lobby_id,
            new.tournament_id,
            delivery,
            expires_at,
            lifetime,
            new.starting_wins[0],
            new.starting_wins[1],
            ends_by
        ],
    )?;
    for participant in new.participants {
        tx.execute(
            "INSERT INTO match_participants (match_id, assignment_generation, slot, participant_id, ember_id)
             VALUES (?1, 1, ?2, ?3, ?4)",
            params![id, participant.slot, participant.participant_id, participant.ember_id.as_str()],
        )?;
    }
    let found = load(tx, &id)?.ok_or_else(ApiFailure::unavailable)?;
    let roster = participants(tx, &id, 1)?;
    match_event(
        tx,
        ctx,
        &found,
        Kind::MatchCreated,
        serde_json::to_value(MatchCreatedData {
            match_id: id.clone(),
            external_match_id: new.external_match_id.to_owned(),
            match_revision: Counter(1),
            assignment_generation: Counter(1),
            state: Some(MatchState::AwaitingPlayers),
            games_to_win: new.rules.games_to_win,
            participants: roster
                .into_iter()
                .map(|p| EventParticipant {
                    slot: p.slot,
                    participant_id: p.participant_id,
                    ember_id: p.ember_id,
                })
                .collect(),
            metadata: new.metadata.clone(),
        })
        .unwrap_or_default(),
    )?;
    Ok(id)
}
