//! The bridge's side of a waiting sign-in: it asks Challonge's token endpoint
//! whether the player approved, no more often than the sign-in's interval,
//! and on approval reads who signed in and drops the token.
//!
//! A pass claims each due sign-in by moving its `next_poll_at` on, in a write
//! of its own, before it calls out, so a restarted bridge does not ask again
//! for one already asked about; passes run one at a time, so a slow answer is
//! never asked for twice. What an answer changes applies only while the
//! sign-in is still the one claimed: a cancel, a newer start or a disconnect
//! meanwhile wins.
//!
//! Each request is taken from the month's budget first (`budget`). Once it is
//! spent, a due sign-in the pass cannot ask about ends `failed`: it could
//! never finish.
use std::time::Duration;

use ember_protocol::challonge::SignInState;
use futures_util::future::join_all;
use rusqlite::{Transaction, params};
use zeroize::Zeroizing;

use super::{
    api::{self, Polled, User},
    budget, hold, holder, in_use, sign_in, store,
};
use crate::{AppState, config::Challonge, ctx::Ctx, error::Result, linking::Site};

/// How often the poller looks for due sign-ins.
const TICK: Duration = Duration::from_secs(1);
/// What `slow_down` adds to a sign-in's interval (RFC 8628).
const SLOW_DOWN_SECS: u64 = 5;

pub async fn poll_forever(state: AppState) {
    loop {
        poll(&state).await;
        tokio::time::sleep(TICK).await;
    }
}

/// One pass: asks Challonge about every waiting sign-in that is due, all at
/// once. Nothing when sign-in is off.
pub async fn poll(state: &AppState) {
    let Some(challonge) = sign_in(state).cloned() else {
        return;
    };
    let one_at_a_time = state.challonge_poll.lock().await;
    let (limits, now) = (challonge.clone(), state.now());
    let Ok(due) = state.db.write(move |tx| claim(tx, &limits, now)).await else {
        return;
    };
    join_all(due.into_iter().map(|due| ask(state, &challonge, due))).await;
    drop(one_at_a_time);
}

/// A sign-in this pass asks about, as its claim left it.
struct Due {
    ember_id: String,
    device_code: Zeroizing<String>,
    next_poll_at: u64,
}

/// Claims the due sign-ins the month's budget still has a poll for, the
/// longest due first, and fails the rest.
fn claim(tx: &Transaction<'_>, challonge: &Challonge, now: u64) -> Result<Vec<Due>> {
    let due: u64 = tx.query_row(
        "SELECT COUNT(*) FROM challonge_sign_ins WHERE state = 'waiting' AND next_poll_at <= ?1 AND expires_at > ?1",
        [now],
        |row| row.get(0),
    )?;
    let granted = budget::take(tx, challonge, now, due)?;
    let claimed = tx
        .prepare(
            "UPDATE challonge_sign_ins SET next_poll_at = ?1 + interval_secs
             WHERE ember_id IN (SELECT ember_id FROM challonge_sign_ins
               WHERE state = 'waiting' AND next_poll_at <= ?1 AND expires_at > ?1
               ORDER BY next_poll_at, ember_id LIMIT ?2)
             RETURNING ember_id, device_code, next_poll_at",
        )?
        .query_map(params![now, granted], |row| {
            Ok(Due {
                ember_id: row.get(0)?,
                device_code: Zeroizing::new(row.get(1)?),
                next_poll_at: row.get(2)?,
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    // Claimed ones are due later now; any still due had no request left.
    tx.execute(
        "UPDATE challonge_sign_ins SET state = 'failed', device_code = NULL
         WHERE state = 'waiting' AND next_poll_at <= ?1 AND expires_at > ?1",
        [now],
    )?;
    Ok(claimed)
}

/// What an answer does to the sign-in.
enum Outcome {
    SlowDown,
    Ended(SignInState),
    SignedIn(User),
}

async fn ask(state: &AppState, challonge: &Challonge, due: Due) {
    let secret = state.integrations.challonge_client_secret.as_deref();
    let outcome =
        match api::poll_token(challonge, secret.map(String::as_str), &due.device_code).await {
            Polled::Wait => return,
            Polled::SlowDown => Outcome::SlowDown,
            Polled::Ended(ended) => Outcome::Ended(ended),
            Polled::Token(token) if spend_one(state, challonge).await => {
                match api::me(challonge, &token).await {
                    Some(user) => Outcome::SignedIn(user),
                    None => Outcome::Ended(SignInState::Failed),
                }
            }
            Polled::Token(_) => Outcome::Ended(SignInState::Failed),
        };
    let ctx = Ctx::of(state);
    if state
        .db
        .write(move |tx| settle(tx, &ctx, &due, outcome))
        .await
        .is_ok()
    {
        state.committed();
    }
}

/// Takes one request from the month's budget, in a write of its own.
async fn spend_one(state: &AppState, challonge: &Challonge) -> bool {
    let (challonge, now) = (challonge.clone(), state.now());
    matches!(
        state
            .db
            .write(move |tx| budget::take(tx, &challonge, now, 1))
            .await,
        Ok(1)
    )
}

/// Applies an answer, only while the sign-in is still the one claimed: a
/// cancel, a newer start or a disconnect meanwhile wins.
fn settle(tx: &Transaction<'_>, ctx: &Ctx, due: &Due, outcome: Outcome) -> Result<()> {
    let current: bool = tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM challonge_sign_ins
           WHERE ember_id = ?1 AND device_code = ?2 AND state = 'waiting' AND next_poll_at = ?3)",
        params![due.ember_id, due.device_code.as_str(), due.next_poll_at],
        |row| row.get(0),
    )?;
    if !current {
        return Ok(());
    }
    let ember_id = due.ember_id.as_str();
    match outcome {
        Outcome::SlowDown => {
            tx.execute(
                "UPDATE challonge_sign_ins SET interval_secs = interval_secs + ?2, next_poll_at = next_poll_at + ?2
                 WHERE ember_id = ?1",
                params![ember_id, SLOW_DOWN_SECS],
            )?;
        }
        Outcome::Ended(ended) => {
            tx.execute(
                "UPDATE challonge_sign_ins SET state = ?2, device_code = NULL WHERE ember_id = ?1",
                params![ember_id, ended.as_str()],
            )?;
        }
        Outcome::SignedIn(user) => match holder(tx, &user.id)? {
            // Another Ember ID in use has the account: it stays there.
            Some(owner) if owner != ember_id && in_use(tx, &owner, ctx.now)? => {
                hold(tx, ember_id, &user.username)?;
            }
            // One unused for long: the player chooses (`choose`).
            Some(owner) if owner != ember_id => {
                tx.execute(
                    "UPDATE challonge_sign_ins SET state = 'moving', device_code = NULL,
                       pending_user_id = ?2, pending_username = ?3, pending_owner = ?4
                     WHERE ember_id = ?1",
                    params![ember_id, user.id, user.username, owner],
                )?;
            }
            _ => {
                Site::Challonge.end_sign_ins(tx, ember_id)?;
                store(tx, ctx, ember_id, &user.id, &user.username)?;
            }
        },
    }
    Ok(())
}
