//! At-least-once delivery (spec 20.4, 20.5). Each queue has one worker: it
//! leases what is due, the longest due first, as many as it sends at once,
//! and sends them. Leasing numbers the attempt, and an outcome is recorded
//! only under the latest lease, so an attempt that outlived its lease never
//! overwrites the one that replaced it. Retries follow one schedule with
//! jitter until a day has passed since the first attempt.
//!
//! Four queues use it: signed webhooks from the outbox (`webhooks`), match
//! results sent to the platform that created the match (`results`), and,
//! for a platform that hears how a set stands while it is played
//! (`Policy::reports_progress`, BluMint), the set's score (`progress`) and
//! the match's status (`status`).
pub mod progress;
pub mod results;
pub mod status;
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

/// The longest a worker with nothing due waits before it looks again. A
/// commit that queued work wakes every worker at once (`db::queued`), but a
/// write from another process (the command line, an operator's script) wakes
/// none, so the wait stays short.
const IDLE_SECS: u64 = 1;

/// Runs passes of `queue` forever, waiting for a commit that queued work, or
/// `IDLE_SECS`, when nothing was due.
pub async fn run<Q: Queue>(state: AppState, queue: Q) {
    let queue = Arc::new(queue);
    let mut commits = state.db.commits();
    loop {
        // Marked seen before the pass, so a commit during the pass ends the
        // wait that follows it.
        commits.borrow_and_update();
        if !pass(&state, &queue).await {
            let _ = tokio::time::timeout(Duration::from_secs(IDLE_SECS), commits.changed()).await;
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
        .unwrap_or_else(|error| {
            eprintln!(
                "delivery: {} could not lease: {error:?}",
                std::any::type_name::<Q>()
            );
            Vec::new()
        });
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

#[cfg(test)]
mod tests {
    use std::{
        sync::atomic::{AtomicUsize, Ordering},
        time::Instant,
    };

    use super::*;
    use crate::{Clock, Config, Db, Keys, integrations};

    /// A queue with nothing to send that counts its passes.
    struct Counting(Arc<AtomicUsize>);

    impl Queue for Counting {
        type Item = ();

        fn lease(&self, _: &Transaction<'_>, _: u64, _: u64) -> Result<Vec<Leased<()>>> {
            self.0.fetch_add(1, Ordering::SeqCst);
            Ok(Vec::new())
        }

        async fn attempt(&self, _: &AppState, _: &()) -> Outcome {
            Outcome::Cancelled
        }

        fn record(&self, _: &Transaction<'_>, _: &Leased<()>, _: &Record) -> Result<()> {
            Ok(())
        }
    }

    async fn until(passes: &[Arc<AtomicUsize>], count: usize) {
        while passes
            .iter()
            .any(|pass| pass.load(Ordering::SeqCst) < count)
        {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn one_commit_wakes_every_queue() {
        let db = Db::open(std::path::Path::new(":memory:")).unwrap();
        let state = AppState::new(
            Config::initial("brg_test".into()),
            Keys::generate(),
            integrations::Secrets::default(),
            db,
            Clock::default(),
        );
        let passes: Vec<Arc<AtomicUsize>> = (0..4).map(|_| Arc::default()).collect();
        let workers: Vec<_> = passes
            .iter()
            .map(|pass| tokio::spawn(run(state.clone(), Counting(pass.clone()))))
            .collect();
        // Each worker found nothing on its first pass and waits.
        until(&passes, 1).await;
        let waiting = Instant::now();
        // A write that queued something.
        state
            .db
            .write(|_| {
                crate::db::queued();
                Ok(())
            })
            .await
            .unwrap();
        until(&passes, 2).await;
        // All four passed again well before any wait could have run out.
        assert!(
            waiting.elapsed() < Duration::from_millis(IDLE_SECS * 1000 * 3 / 4),
            "{:?}",
            waiting.elapsed()
        );
        // A write that queued nothing wakes none of them.
        let quiet = Instant::now();
        state.db.write(|_| Ok(())).await.unwrap();
        tokio::time::sleep(Duration::from_millis(200)).await;
        if quiet.elapsed() < Duration::from_millis(IDLE_SECS * 1000 / 2) {
            assert!(passes.iter().all(|pass| pass.load(Ordering::SeqCst) == 2));
        }
        for worker in workers {
            worker.abort();
        }
    }
}
