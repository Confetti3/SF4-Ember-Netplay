//! The read-back: every few minutes the bridge is asked about each live
//! match (bridge requests cost nothing against a site's budget). A finished
//! match whose result the bridge is failing to deliver is recorded as if it
//! had been delivered, under the bridge's own `webhook-id`, so a delivery
//! that gets through later is a repeat. A result the bridge has not started
//! sending is left alone: until then an organizer can still correct it.
use std::collections::HashSet;

use ember_protocol::{matches::DeliveryState, partner::result_id};

use super::Bracket;
use crate::{
    log,
    sets::{ExternalId, Received},
};

/// How often the bridge is asked about each live match: more than the
/// bridge's delivery lease, so a delivery seen in progress twice is stuck.
const RECONCILE_SECS: u64 = 5 * 60;

/// What the read-back keeps between passes.
#[derive(Default)]
pub(super) struct Memory {
    next: u64,
    /// Matches whose result was being delivered at the last read-back.
    delivering: HashSet<String>,
    /// Matches the bridge could not be asked about at the last read-back,
    /// so each run of failures is logged once.
    failing: HashSet<String>,
}

/// Whether a finished match's result is taken from the bridge: only once
/// its delivery has started and is not getting through, since the bridge
/// lets an organizer correct a result until it starts sending it.
fn read_back(delivery: DeliveryState, delivering_before: bool) -> bool {
    match delivery {
        DeliveryState::Retrying | DeliveryState::Failed | DeliveryState::Ambiguous => true,
        DeliveryState::Delivering => delivering_before,
        DeliveryState::NotRequired | DeliveryState::Queued | DeliveryState::Delivered => false,
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
        let mut delivering = HashSet::new();
        let mut failing = HashSet::new();
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
            if over.delivery == DeliveryState::Delivering {
                delivering.insert(match_id.clone());
            }
            if !read_back(over.delivery, memory.delivering.contains(&match_id)) {
                continue;
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
                    "{} set {set_id}: the bridge cannot deliver the result of Ember match {match_id}; it is read from the bridge",
                    self.key
                ),
                Ok(false) => {}
                Err(error) => log!("{}: database: {error}", self.key),
            }
        }
        memory.delivering = delivering;
        memory.failing = failing;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_result_whose_delivery_is_failing_is_read_back() {
        let cases = [
            (DeliveryState::Retrying, false, true),
            (DeliveryState::Failed, false, true),
            (DeliveryState::Ambiguous, false, true),
            // Still correctable, or already here.
            (DeliveryState::Queued, true, false),
            (DeliveryState::NotRequired, true, false),
            (DeliveryState::Delivered, true, false),
            // Delivering: only when it was at the last read-back too.
            (DeliveryState::Delivering, false, false),
            (DeliveryState::Delivering, true, true),
        ];
        for (delivery, before, expected) in cases {
            assert_eq!(
                read_back(delivery, before),
                expected,
                "{delivery:?} {before}"
            );
        }
    }
}
