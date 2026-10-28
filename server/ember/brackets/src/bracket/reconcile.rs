//! The read-back: every few minutes the bridge is asked about each live
//! match (bridge requests cost nothing against a site's budget). A finished
//! match whose result the bridge has started sending (from then on nobody can
//! correct it) is recorded as if it had been delivered, under the bridge's
//! own `webhook-id`, so the delivery, if it gets through, is a repeat. A
//! result the bridge has not started sending is left alone, since an
//! organizer can still correct it; one still unsent at two read-backs in a
//! row is logged once, since the bridge may never send it.
use std::collections::HashSet;

use ember_protocol::{matches::DeliveryState, partner::result_id};

use super::Bracket;
use crate::{
    log,
    sets::{ExternalId, Received},
};

/// How often the bridge is asked about each live match.
const RECONCILE_SECS: u64 = 5 * 60;

/// What the read-back keeps between passes, so each run of trouble with a
/// match is logged once.
#[derive(Default)]
pub(super) struct Memory {
    /// When the read-back is next due.
    pub(super) next: u64,
    /// Matches the bridge could not be asked about at the last read-back.
    failing: HashSet<String>,
    /// Finished matches whose result the bridge had not started sending.
    unsent: HashSet<String>,
    /// Of those, the ones already logged.
    unsent_logged: HashSet<String>,
}

/// What the read-back does with a finished match.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Next {
    /// Its result is final on the bridge: settle it from there.
    ReadBack,
    /// Not sent yet, and still correctable.
    Unsent,
    /// Already delivered here.
    Done,
}

fn next(delivery: DeliveryState) -> Next {
    match delivery {
        DeliveryState::Delivering
        | DeliveryState::Retrying
        | DeliveryState::Failed
        | DeliveryState::Ambiguous => Next::ReadBack,
        DeliveryState::Queued | DeliveryState::NotRequired => Next::Unsent,
        DeliveryState::Delivered => Next::Done,
    }
}

impl Bracket {
    pub(super) async fn reconcile(&self, memory: &mut Memory, now: u64) {
        if now < memory.next {
            return;
        }
        memory.next = now + RECONCILE_SECS;
        let live = match self.store().live(&self.key) {
            Ok(live) => live,
            Err(error) => return log!("{}: database: {error}", self.key),
        };
        let mut failing = HashSet::new();
        let mut unsent = HashSet::new();
        let mut unsent_logged = HashSet::new();
        for (set_id, tracked) in live {
            let Some(match_id) = tracked.match_id else {
                continue;
            };
            let over = match self.context.bridge.over(&match_id).await {
                Ok(over) => over,
                Err(error) => {
                    if !memory.failing.contains(&match_id) {
                        log!(
                            "{} set {set_id}: cannot ask the bridge about Ember match {match_id}: {error}",
                            self.key
                        );
                    }
                    failing.insert(match_id);
                    continue;
                }
            };
            let Some(over) = over else {
                continue;
            };
            match next(over.delivery) {
                Next::Done => continue,
                Next::Unsent => {
                    if memory.unsent_logged.contains(&match_id) {
                        unsent_logged.insert(match_id.clone());
                    } else if memory.unsent.contains(&match_id) {
                        log!(
                            "{} set {set_id}: Ember match {match_id} has ended, and the bridge has not started sending its result; check the connection's results_url and result secret on the bridge",
                            self.key
                        );
                        unsent_logged.insert(match_id.clone());
                    }
                    unsent.insert(match_id);
                    continue;
                }
                Next::ReadBack => {}
            }
            let received = Received {
                id: ExternalId {
                    bracket: self.key.clone(),
                    set: set_id.clone(),
                    attempt: tracked.attempt,
                },
                match_id: match_id.clone(),
                players: over.players,
                ended: over.ended,
            };
            match self
                .store()
                .receive(&result_id(&match_id), &received, now, false)
            {
                Ok(true) => log!(
                    "{} set {set_id}: the result of Ember match {match_id} is read from the bridge, which has not delivered it yet",
                    self.key
                ),
                Ok(false) => {}
                Err(error) => log!("{}: database: {error}", self.key),
            }
        }
        memory.failing = failing;
        memory.unsent = unsent;
        memory.unsent_logged = unsent_logged;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_result_is_read_back_once_the_bridge_has_started_sending_it() {
        let cases = [
            (DeliveryState::Delivering, Next::ReadBack),
            (DeliveryState::Retrying, Next::ReadBack),
            (DeliveryState::Failed, Next::ReadBack),
            (DeliveryState::Ambiguous, Next::ReadBack),
            (DeliveryState::Queued, Next::Unsent),
            (DeliveryState::NotRequired, Next::Unsent),
            (DeliveryState::Delivered, Next::Done),
        ];
        for (delivery, expected) in cases {
            assert_eq!(next(delivery), expected, "{delivery:?}");
        }
    }
}
