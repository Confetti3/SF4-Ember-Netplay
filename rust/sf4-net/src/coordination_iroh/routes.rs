//! The route table of authenticated room members.
//!
//! The bindings and the tombstones of retired incarnations sit under one
//! lock. Each admission checks the tombstones and binds in the same write
//! section, and each retirement records the tombstone and marks the binding
//! in one, so a retirement can never land between an admission's check and
//! its insert and leave the retired incarnation a live route.
use super::{failure, retired::RetiredFilter};
use crate::coordination::Ownership;
use iroh::{EndpointAddr, EndpointId};
use std::{
    collections::BTreeMap,
    io,
    time::{Duration, Instant},
};

/// How long a retired member keeps its read-only route.
const DEPARTURE_ROUTE: Duration = Duration::from_secs(30);

#[derive(Clone, Debug)]
pub(super) struct MemberBinding {
    pub coordination: EndpointAddr,
    pub primary: Option<EndpointId>,
    /// A committed departure keeps a short-lived, read-only route so the
    /// departing helper can query the successor's voter set after retain=false
    /// stops replicating membership to it. All Raft-mutating methods remain
    /// rejected while this marker is present.
    pub retired_until: Option<Instant>,
}

impl MemberBinding {
    fn live(&self) -> bool {
        self.retired_until.is_none()
    }

    fn expired(&self, now: Instant) -> bool {
        self.retired_until.is_some_and(|until| until <= now)
    }
}

/// What a served RPC from a bound member may ask for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Access {
    /// Every method.
    Member,
    /// Only the authority read: a retired member finishing its departure
    /// proof.
    Departing,
}

pub(super) struct Routes {
    // Populated by authenticated admission, never by an incoming RPC. Routes
    // bind the Raft process incarnation to the exact endpoint public key.
    bindings: BTreeMap<u64, MemberBinding>,
    // A fixed-size tombstone filter lets expired route records be evicted
    // without ever making the exact retired incarnation admissible again.
    // False positives fail closed; the filter has no false negatives.
    retired: RetiredFilter,
}

impl Routes {
    pub fn new(room: [u8; 16], ownership: Ownership) -> Self {
        Self {
            bindings: BTreeMap::new(),
            retired: RetiredFilter::new(room, ownership),
        }
    }

    pub fn binding(&self, incarnation: u64) -> Option<&MemberBinding> {
        self.bindings.get(&incarnation)
    }

    pub fn live_primaries(&self) -> Vec<EndpointId> {
        self.bindings
            .values()
            .filter(|binding| binding.live())
            .filter_map(|binding| binding.primary)
            .collect()
    }

    /// Drop the routes whose departure window has passed and return their
    /// incarnations, whose connections the caller closes.
    pub fn sweep(&mut self, now: Instant) -> Vec<u64> {
        let expired: Vec<u64> = self
            .bindings
            .iter()
            .filter_map(|(id, known)| known.expired(now).then_some(*id))
            .collect();
        for id in &expired {
            self.bindings.remove(id);
        }
        expired
    }

    /// Bind `incarnation` to its authenticated coordination address and, when
    /// the control stream supplied one, its primary game endpoint. At most
    /// `limit` live routes exist at once.
    pub fn admit(
        &mut self,
        incarnation: u64,
        address: EndpointAddr,
        primary: Option<EndpointId>,
        limit: usize,
    ) -> io::Result<()> {
        if incarnation == 0
            || self.retired.contains(incarnation)
            || (self.bindings.values().filter(|known| known.live()).count() >= limit
                && !self.bindings.contains_key(&incarnation))
            || self.bindings.iter().any(|(id, known)| {
                *id != incarnation && known.live() && known.coordination.id == address.id
            })
            // A primary game endpoint is an authenticated, process-local
            // binding.  Reusing it for a second live incarnation would let
            // a stale process receive the new member's gameplay/control
            // routing, so the binding must remain one-to-one until the old
            // incarnation is explicitly revoked.
            || primary.as_ref().is_some_and(|candidate| {
                self.bindings.iter().any(|(id, known)| {
                    *id != incarnation && known.live() && known.primary.as_ref() == Some(candidate)
                })
            })
        {
            return Err(failure());
        }
        if let Some(known) = self.bindings.get_mut(&incarnation) {
            // A retired incarnation is a permanent tombstone. A restarted
            // process receives a fresh random incarnation; replaying an old
            // Admission must never reopen its Raft mutation route.
            if !known.live() {
                return Err(failure());
            }
            if known.coordination.id != address.id
                || primary.is_some_and(|candidate| {
                    known.primary.is_some_and(|existing| existing != candidate)
                })
            {
                return Err(failure());
            }
            known.coordination = address;
            if primary.is_some() {
                known.primary = primary;
            }
            return Ok(());
        }
        self.bindings.insert(
            incarnation,
            MemberBinding {
                coordination: address,
                primary,
                retired_until: None,
            },
        );
        Ok(())
    }

    /// Tombstone `incarnation` and keep its route, if it has one, only for
    /// the bounded departure proof.
    pub fn retire(&mut self, incarnation: u64, now: Instant) {
        self.retired.insert(incarnation);
        if let Some(binding) = self.bindings.get_mut(&incarnation) {
            binding.retired_until.get_or_insert(now + DEPARTURE_ROUTE);
        }
    }

    pub fn revoke(&mut self, incarnation: u64) {
        self.bindings.remove(&incarnation);
    }

    /// Whether `remote` holds a route a served connection may use.
    pub fn serves(&self, remote: EndpointId, now: Instant) -> bool {
        self.bindings
            .values()
            .any(|known| known.coordination.id == remote && !known.expired(now))
    }

    /// What an RPC from `source`, arriving over a connection authenticated as
    /// `remote`, may ask for; `None` when `source` has no route there.
    pub fn access(&self, source: u64, remote: EndpointId, now: Instant) -> Option<Access> {
        let known = self
            .bindings
            .get(&source)
            .filter(|known| known.coordination.id == remote && !known.expired(now))?;
        Some(if known.live() {
            Access::Member
        } else {
            Access::Departing
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LIMIT: usize = 4;

    fn key(seed: u8) -> EndpointId {
        iroh::SecretKey::from_bytes(&[seed; 32]).public()
    }

    fn routes() -> Routes {
        Routes::new([5; 16], Ownership::Private)
    }

    #[test]
    fn a_retirement_before_admission_leaves_no_route() {
        let mut routes = routes();
        let now = Instant::now();
        routes.retire(2, now);

        assert!(routes.admit(2, EndpointAddr::new(key(2)), Some(key(12)), LIMIT).is_err());
        assert!(routes.binding(2).is_none());
        assert!(!routes.serves(key(2), now));
        assert_eq!(routes.access(2, key(2), now), None);
    }

    #[test]
    fn a_retired_member_keeps_only_the_authority_read_until_its_route_expires() {
        let mut routes = routes();
        let now = Instant::now();
        routes.admit(2, EndpointAddr::new(key(2)), Some(key(12)), LIMIT).unwrap();
        assert_eq!(routes.access(2, key(2), now), Some(Access::Member));
        // The route answers only for the endpoint it was bound to.
        assert_eq!(routes.access(2, key(3), now), None);

        routes.retire(2, now);
        assert_eq!(routes.access(2, key(2), now), Some(Access::Departing));
        assert!(routes.serves(key(2), now));
        assert!(routes.live_primaries().is_empty());
        assert!(routes.admit(2, EndpointAddr::new(key(2)), Some(key(12)), LIMIT).is_err());

        let later = now + DEPARTURE_ROUTE;
        assert_eq!(routes.access(2, key(2), later), None);
        assert!(!routes.serves(key(2), later));
        assert_eq!(routes.sweep(later), vec![2]);
        // The tombstone outlives the swept route.
        assert!(routes.admit(2, EndpointAddr::new(key(2)), None, LIMIT).is_err());
    }

    #[test]
    fn a_retired_route_frees_its_place_and_primary_for_a_fresh_incarnation() {
        let mut routes = routes();
        let now = Instant::now();
        for id in 1..=LIMIT as u64 {
            routes
                .admit(id, EndpointAddr::new(key(id as u8)), None, LIMIT)
                .unwrap();
        }
        assert!(routes.admit(9, EndpointAddr::new(key(9)), None, LIMIT).is_err());

        routes.retire(1, now);
        routes.admit(9, EndpointAddr::new(key(1)), None, LIMIT).unwrap();
        assert_eq!(routes.access(9, key(1), now), Some(Access::Member));
        assert_eq!(routes.access(1, key(1), now), Some(Access::Departing));
    }
}
