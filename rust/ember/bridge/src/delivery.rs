//! At-least-once delivery (spec 20.4, 20.5). Each queue has one worker: it
//! leases what is due, the longest due first, as many as it sends at once,
//! and sends them. Leasing numbers the attempt, and an outcome is recorded
//! only under the latest lease, so an attempt that outlived its lease never
//! overwrites the one that replaced it. Retries follow one schedule with
//! jitter until a day has passed since the first attempt.
//!
//! Two queues use it: signed webhooks from the outbox (`webhooks`), and
//! match results sent to the platform that created the match
//! (`routes::blumint::Results`).
pub mod webhooks;

use std::{future::Future, sync::Arc, time::Duration};

use futures_util::StreamExt;
use rusqlite::Transaction;

use crate::{AppState, error::Result, util::random};

const CONCURRENCY: usize = 4;
/// Items leased per pass: as many as are sent at once, so each one starts
/// as soon as it is leased.
pub const BATCH: i64 = CONCURRENCY as i64;
/// A leased item is not leased again for this long, longer than one attempt
/// can take (a DNS lookup and an HTTP request, each with its own timeout).
const LEASE_SECS: u64 = 60;
const BUDGET_SECS: u64 = 24 * 60 * 60;
const SCHEDULE: &[u64] = &[1, 5, 15, 60, 300, 900];
const HOURLY: u64 = 3600;

/// Something a queue sends, as it was leased.
pub struct Leased<T> {
    pub item: T,
    /// This attempt's number, which leasing stored as the item's attempts.
    /// A later lease stores the next number, so this one's outcome no longer
    /// applies.
    pub attempt: u32,
    pub first_attempt_at: Option<u64>,
}

/// What one attempt came to.
pub enum Outcome {
    Delivered,
    /// Worth trying again later.
    Retry {
        error: String,
        retry_after: Option<u64>,
    },
    /// Refused in a way no retry changes.
    Refused(String),
    /// No longer to be sent (deleted, disabled or revoked since the lease);
    /// nothing was sent, and whatever ended it already updated the queue.
    Cancelled,
}

/// Where an item stands after an attempt.
pub enum Settled {
    Delivered,
    Retry {
        at: u64,
        error: String,
    },
    /// Refused for good.
    Refused(String),
    /// Out of retries: a day passed since the first attempt.
    Expired(String),
}

/// What a queue records after an attempt.
pub struct Record {
    /// When the attempt ended.
    pub at: u64,
    pub first_attempt_at: u64,
    pub settled: Settled,
}

pub trait Queue: Send + Sync + 'static {
    type Item: Send + Sync + 'static;

    /// Up to `BATCH` items due at `now`, the longest due first, each leased
    /// in this write: not taken again before `until`, and its attempts
    /// counted up to this attempt's number.
    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Self::Item>>>;

    fn attempt(&self, state: &AppState, item: &Self::Item) -> impl Future<Output = Outcome> + Send;

    /// Records `record` if the item is still waiting under this lease (its
    /// attempts are still `leased.attempt`); otherwise a later lease owns it.
    fn record(
        &self,
        tx: &Transaction<'_>,
        leased: &Leased<Self::Item>,
        record: &Record,
    ) -> Result<()>;
}

/// Runs passes of `queue` forever, waiting for a commit or a second when
/// nothing was due.
pub async fn run<Q: Queue>(state: AppState, queue: Q) {
    let queue = Arc::new(queue);
    loop {
        if !pass(&state, &queue).await {
            let _ = tokio::time::timeout(Duration::from_secs(1), state.delivery.notified()).await;
        }
    }
}

/// One pass: leases what is due and sends it. False when nothing was due.
pub async fn pass<Q: Queue>(state: &AppState, queue: &Arc<Q>) -> bool {
    let now = state.now();
    let leasing = queue.clone();
    let due = state
        .db
        .write(move |tx| leasing.lease(tx, now, now + LEASE_SECS))
        .await
        .unwrap_or_default();
    let busy = !due.is_empty();
    futures_util::stream::iter(due)
        .for_each_concurrent(CONCURRENCY, |leased| async move {
            let outcome = queue.attempt(state, &leased.item).await;
            if let Some(record) = settle(state.now(), &leased, outcome) {
                let recording = queue.clone();
                let _ = state
                    .db
                    .write(move |tx| recording.record(tx, &leased, &record))
                    .await;
            }
        })
        .await;
    busy
}

/// Where an attempt leaves the item, or None when it was cancelled.
fn settle<T>(now: u64, leased: &Leased<T>, outcome: Outcome) -> Option<Record> {
    let first_attempt_at = leased.first_attempt_at.unwrap_or(now);
    let settled = match outcome {
        Outcome::Cancelled => return None,
        Outcome::Delivered => Settled::Delivered,
        Outcome::Refused(error) => Settled::Refused(error),
        Outcome::Retry { error, retry_after } => {
            let base = SCHEDULE
                .get(leased.attempt.saturating_sub(1) as usize)
                .copied()
                .unwrap_or(HOURLY);
            // Up to 20 percent jitter so retries from many items spread out.
            let jitter = u64::from(random::<1>()[0]) * base / 1275;
            let delay = (base + jitter).max(retry_after.unwrap_or(0).min(HOURLY));
            if now + delay > first_attempt_at + BUDGET_SECS {
                Settled::Expired(error)
            } else {
                Settled::Retry {
                    at: now + delay,
                    error,
                }
            }
        }
    };
    Some(Record {
        at: now,
        first_attempt_at,
        settled,
    })
}
