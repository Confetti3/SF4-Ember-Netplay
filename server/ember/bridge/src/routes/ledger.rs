//! The match ledger shared by every route that changes a match: loading it,
//! moving its revision, its events, and settling its score after an attempt
//! is decided, whoever decided it (an organizer or two agreeing fighters).
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    encoding::Counter,
    event::Kind,
    matches::{DeliveryState, MatchCompleted, MatchState, Resolution, Score},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Serialize;
use serde_json::json;

use crate::{
    ctx::Ctx,
    db,
    delivery::{progress, status},
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    routes::{expiry, lobbies},
    tournament,
};

#[derive(Clone, Debug, Serialize)]
pub struct Participant {
    pub slot: u8,
    pub participant_id: String,
    pub ember_id: EmberId,
}

pub struct Match {
    pub id: String,
    pub tenant_id: String,
    pub connection_id: String,
    pub external_match_id: String,
    pub revision: u64,
    pub generation: u64,
    pub state: MatchState,
    pub games_to_win: u8,
    /// The lobby that plays this match as one of its sets.
    pub lobby_id: Option<String>,
    /// The tournament that plays this match as one of its bracket sets.
    pub tournament_id: Option<String>,
    /// Whether, and how far, its result has been sent to its platform.
    pub delivery: DeliveryState,
    /// Games each slot had won when the match started, P1 first.
    pub starting_wins: [u8; 2],
}

/// The match `id`. A state or delivery state this bridge does not know (a
/// newer bridge wrote it before a rollback) fails the read: the match answers
/// as unavailable rather than being read as some other state.
pub fn load(tx: &Transaction<'_>, id: &str) -> Result<Option<Match>> {
    Ok(read(tx, id)?)
}

/// `load` for a pass over many matches: one whose stored state this bridge
/// does not know is logged and passed over (`None`), so it never stops the
/// others.
pub fn load_listed(tx: &Transaction<'_>, id: &str) -> Result<Option<Match>> {
    match read(tx, id) {
        Err(error) if db::unknown(&error) => {
            eprintln!(
                "ember-bridge: match {id} has a stored state this bridge does not know; skipped"
            );
            Ok(None)
        }
        read => Ok(read?),
    }
}

fn read(tx: &Transaction<'_>, id: &str) -> rusqlite::Result<Option<Match>> {
    tx.query_row(
        "SELECT id, tenant_id, connection_id, external_match_id, revision, assignment_generation, state, games_to_win,
                lobby_id, tournament_id, delivery_state, starting_wins_p1, starting_wins_p2
         FROM matches WHERE id = ?1",
        [id],
        |row| {
            Ok(Match {
                id: row.get(0)?,
                tenant_id: row.get(1)?,
                connection_id: row.get(2)?,
                external_match_id: row.get(3)?,
                revision: row.get(4)?,
                generation: row.get(5)?,
                state: db::known(row, 6, MatchState::parse)?,
                games_to_win: row.get(7)?,
                lobby_id: row.get(8)?,
                tournament_id: row.get(9)?,
                delivery: db::known(row, 10, DeliveryState::parse)?,
                starting_wins: [row.get(11)?, row.get(12)?],
            })
        },
    )
    .optional()
}

/// Whether the identity has an active match on the connection other than `except`.
pub fn busy(
    tx: &Transaction<'_>,
    connection_id: &str,
    ember_id: &EmberId,
    except: &str,
) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM matches m JOIN match_participants p
           ON p.match_id = m.id AND p.assignment_generation = m.assignment_generation
          WHERE m.connection_id = ?1 AND p.ember_id = ?2 AND m.id != ?3
            AND m.state NOT IN ('completed', 'cancelled', 'failed', 'expired'))",
        params![connection_id, ember_id.as_str(), except],
        |row| row.get(0),
    )?)
}

pub fn participants(tx: &Transaction<'_>, id: &str, generation: u64) -> Result<Vec<Participant>> {
    tx.prepare(
        "SELECT slot, participant_id, ember_id FROM match_participants
         WHERE match_id = ?1 AND assignment_generation = ?2 ORDER BY slot",
    )?
    .query_map(params![id, generation], |row| {
        Ok((
            row.get::<_, u8>(0)?,
            row.get::<_, String>(1)?,
            row.get::<_, String>(2)?,
        ))
    })?
    .map(|row| {
        let (slot, participant_id, ember_id) = row?;
        Ok(Participant {
            slot,
            participant_id,
            ember_id: EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?,
        })
    })
    .collect()
}

/// Wins per slot, from the ledger rather than any room score: the match's
/// starting wins plus its accepted games, and the accepted attempts.
pub fn scores(tx: &Transaction<'_>, id: &str) -> Result<([u8; 2], Vec<String>)> {
    let mut wins: [u8; 2] = tx.query_row(
        "SELECT starting_wins_p1, starting_wins_p2 FROM matches WHERE id = ?1",
        [id],
        |row| Ok([row.get(0)?, row.get(1)?]),
    )?;
    let mut accepted = Vec::new();
    let rows = tx
        .prepare("SELECT id, outcome FROM attempts WHERE match_id = ?1 AND state = 'accepted' ORDER BY seq")?
        .query_map([id], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (attempt, outcome) in rows {
        match outcome.as_str() {
            "p1_win" => wins[0] += 1,
            "p2_win" => wins[1] += 1,
            _ => continue,
        }
        accepted.push(attempt);
    }
    Ok((wins, accepted))
}

pub fn stale(current: u64) -> ApiFailure {
    ApiFailure::new(
        ErrorCode::StaleRevision,
        "The match changed. Refresh it and try again.",
    )
    .detail("current_revision", current.to_string())
}

/// Moves the match to `state` under its next revision. A game starting or
/// ending moves it, so its platform's status of it is observed again
/// (`delivery::status`).
pub fn bump(tx: &Transaction<'_>, found: &Match, state: MatchState, now: u64) -> Result<u64> {
    let revision = found.revision + 1;
    let changed = tx.execute(
        "UPDATE matches SET revision = ?1, state = ?2, updated_at = ?3 WHERE id = ?4 AND revision = ?5",
        params![revision, state.as_str(), now, found.id, found.revision],
    )?;
    if changed != 1 {
        return Err(stale(found.revision));
    }
    // A match that ends here has its result to send (`delivery::results`).
    db::queued();
    // A claim's `ready` is otherwise left as the fighter last said
    // (`unready_before`). Each Ember claims a change of its own seat's Ready
    // within seconds.
    status::observe(tx, &found.id, now)?;
    Ok(revision)
}

/// Takes back the Ready of every claim on the match from no later than the
/// first report of `attempt_id`, a game just decided. The room clears both
/// seats when it ends a decided game, before either fighter reports it, so
/// such a Ready is from before the game ended, and the Ember's own claim
/// saying so may still be on its way. A later claim stays as it is: a fighter
/// may press Ready for the next game before this one is decided. A called-off
/// game does not come here, since the room then leaves Ready to the fighter
/// who did not take it back.
pub fn unready_before(tx: &Transaction<'_>, match_id: &str, attempt_id: &str) -> Result<()> {
    tx.execute(
        "UPDATE match_claims SET ready = 0
         WHERE match_id = ?1 AND ready = 1
           AND claimed_at <= (SELECT MIN(received_at) FROM game_reports WHERE attempt_id = ?2)",
        params![match_id, attempt_id],
    )?;
    Ok(())
}

pub fn match_event(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    kind: Kind,
    data: serde_json::Value,
) -> Result<i64> {
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind,
            tenant_id: &found.tenant_id,
            connection_id: Some(&found.connection_id),
            subject: format!("matches/{}", found.id),
            match_id: Some(&found.id),
            ember_id: None,
            // A match's events carry its roster, so players read only their
            // own matches' events, not every set of a lobby or tournament.
            lobby_id: None,
            tournament_id: None,
            data,
        },
    )
}

pub fn release(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    roster: &[Participant],
) -> Result<()> {
    let players: Vec<EmberId> = roster.iter().map(|p| p.ember_id.clone()).collect();
    release_players(tx, ctx, &found.connection_id, &players)
}

/// A match has ended, so its players are free: a tournament set waiting on
/// them starts first, then a lobby they wait in may start its next set.
pub fn release_players(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    players: &[EmberId],
) -> Result<()> {
    tournament::on_players_free(tx, ctx, connection_id, players)?;
    lobbies::on_players_free(tx, ctx, connection_id, players)
}

/// Who decided the attempts that just changed.
pub enum Cause<'a> {
    /// An organizer's adjudication. On a completed match it is a correction.
    Organizer {
        adjudication_id: &'a str,
        reason: &'a str,
    },
    /// Two fighters' agreeing reports.
    Players { report_ids: Vec<String> },
}

/// The match after `settle`.
pub struct Settled {
    pub revision: u64,
    pub state: MatchState,
    pub scores: Vec<Score>,
}

/// Works out the score from the accepted attempts after they changed, applies
/// the lobby, bracket and double-booking rules for a reopened set, moves the
/// revision, emits `events` and the score, correction and completion events,
/// and frees the players of a completed set.
pub fn settle(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    found: &Match,
    roster: &[Participant],
    wins_before: [u8; 2],
    cause: Cause<'_>,
    events: Vec<(Kind, serde_json::Value)>,
) -> Result<Settled> {
    let correcting = found.state == MatchState::Completed;
    let (wins, accepted) = scores(tx, &found.id)?;
    let n = found.games_to_win;
    let winner_slot = (0..2).find(|&slot| wins[slot] >= n);
    let next = if winner_slot.is_some() {
        MatchState::Completed
    } else {
        MatchState::BetweenGames
    };
    // A platform the bridge sends results to takes one per match (BluMint
    // answers a second with 409), so from the first send on the result is
    // final here too, and the bridge never sends a stale one.
    if correcting
        && !matches!(
            found.delivery,
            DeliveryState::NotRequired | DeliveryState::Queued
        )
    {
        return Err(ApiFailure::new(
            ErrorCode::LeaseConflict,
            "The result has been sent to the tournament platform, so it cannot be corrected.",
        ));
    }
    // A lobby seats its next set as soon as one ends, so a finished lobby set
    // cannot be reopened.
    if correcting && next != MatchState::Completed && found.lobby_id.is_some() {
        return Err(ApiFailure::new(
            ErrorCode::LeaseConflict,
            "The lobby has moved on to its next set, so this set cannot be reopened.",
        ));
    }
    // Reopening a bracket set takes back what its result fed, as long as no
    // later set has a game recorded; that frees the players for the check below.
    let mut freed = Vec::new();
    if correcting && let Some(tournament_id) = &found.tournament_id {
        if next == MatchState::Completed {
            tournament::check_correction(tx, tournament_id)?;
        } else {
            freed = tournament::on_reopen(tx, ctx, tournament_id, &found.id)?;
        }
    }
    // A correction that reopens the match makes its players busy again, so it
    // must not double-book one who has started another match meanwhile.
    if correcting && next != MatchState::Completed {
        for participant in roster {
            if busy(tx, &found.connection_id, &participant.ember_id, &found.id)? {
                return Err(ApiFailure::new(
                    ErrorCode::LeaseConflict,
                    "A player has another active match. Finish or cancel it before reopening this one.",
                )
                .detail("slot", participant.slot));
            }
        }
    }
    progress::keep_reported_wins(tx, found, wins_before, wins)?;
    let revision = bump(tx, found, next, ctx.now)?;
    // A decided game is activity, which moves the match's deadline.
    expiry::extend(tx, ctx.now, &found.id)?;
    progress::game_settled(tx, found, wins_before, wins, next, accepted.len() as u64)?;
    // The reopened match now holds its players; the ones a bracket
    // correction released from later sets can be picked up elsewhere.
    if !freed.is_empty() {
        release_players(tx, ctx, &found.connection_id, &freed)?;
    }
    let revision_text = revision.to_string();
    for (kind, mut data) in events {
        data["match_revision"] = revision_text.clone().into();
        match_event(tx, ctx, found, kind, data)?;
    }
    let score_rows: Vec<Score> = roster
        .iter()
        .map(|p| Score {
            ember_id: p.ember_id.clone(),
            wins: wins[usize::from(p.slot)],
        })
        .collect();
    if wins != wins_before {
        match_event(
            tx,
            ctx,
            found,
            Kind::ScoreChanged,
            json!({ "match_id": found.id, "match_revision": revision_text, "games_to_win": n, "scores": score_rows }),
        )?;
    }
    if let (
        true,
        Cause::Organizer {
            adjudication_id,
            reason,
        },
    ) = (correcting, &cause)
    {
        match_event(
            tx,
            ctx,
            found,
            Kind::MatchCorrected,
            json!({
                "match_id": found.id,
                "match_revision": revision_text,
                "state": next,
                "adjudication_id": adjudication_id,
                "reason": reason,
            }),
        )?;
    }
    if let Some(slot) = winner_slot {
        let completed = MatchCompleted {
            match_id: found.id.clone(),
            match_revision: Counter(revision),
            assignment_generation: Counter(found.generation),
            state: MatchState::Completed,
            games_to_win: n,
            scores: score_rows.clone(),
            winner_id: roster[slot].ember_id.clone(),
            resolution: match &cause {
                Cause::Organizer { .. } => Resolution::OrganizerAdjudication,
                Cause::Players { .. } => Resolution::PlayerAgreement,
            },
            accepted_attempt_ids: accepted,
            evidence_report_ids: match &cause {
                Cause::Organizer { .. } => Vec::new(),
                Cause::Players { report_ids } => report_ids.clone(),
            },
            provider_delivery_state: found.delivery,
            starting_wins: found.starting_wins,
        };
        completed.check().map_err(|_| ApiFailure::unavailable())?;
        match_event(
            tx,
            ctx,
            found,
            Kind::MatchCompleted,
            serde_json::to_value(&completed).unwrap_or_default(),
        )?;
        if !correcting {
            if let Some(lobby_id) = &found.lobby_id {
                lobbies::on_set_completed(tx, ctx, lobby_id, &found.id, slot as u8, &score_rows)?;
            }
            if let Some(tournament_id) = &found.tournament_id {
                tournament::on_match_completed(
                    tx,
                    ctx,
                    tournament_id,
                    &found.id,
                    slot as u8,
                    &score_rows,
                )?;
            }
            release(tx, ctx, found, roster)?;
        }
    }
    Ok(Settled {
        revision,
        state: next,
        scores: score_rows,
    })
}
