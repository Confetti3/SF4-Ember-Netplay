//! What a room bound to a tournament match changes in the actor
//! (docs/design/MATCH_WATCH.md 5.4). The binding, which every room change
//! commits inside the room's checkpoint, names the match's two fighters and
//! the watchers the bridge admitted. In such a room only the fighters vote,
//! only endpoints the binding names may open a control, and every other
//! admission is retired through the pending-retirement path, its control
//! closed once that retirement is confirmed. A room without a binding takes
//! none of these paths.
use ember_protocol::play::MAX_WATCHERS;
use serde_json::Value;

use super::*;

/// The endpoints a bound room's binding names.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct BoundRoster {
    pub fighters: BTreeSet<EndpointId>,
    pub watchers: BTreeSet<EndpointId>,
}

impl BoundRoster {
    /// The binding in a committed envelope, at
    /// `checkpoint.room.snapshot.tournament`. None when the room is not bound:
    /// the object is absent or empty, as the native side writes an unbound
    /// room. A binding not in the shape the native side writes is read the
    /// same way, so the room keeps the rules it has without one; the native
    /// side checks that shape before it commits a binding.
    ///
    /// An endpoint is caller-chosen hex the bridge does not check is a key.
    /// One that is not can never be a peer's, so it names no one and the
    /// binding still holds for the rest: a watcher cannot unbind a room by
    /// registering such an endpoint.
    pub(super) fn read(envelope: &Value) -> Option<Self> {
        let tournament = envelope
            .get("checkpoint")?
            .get("room")?
            .get("snapshot")?
            .get("tournament")?
            .as_object()?;
        if tournament.is_empty() {
            return None;
        }
        let endpoints = |list: &Value, count: std::ops::RangeInclusive<usize>| {
            let list = list.as_array().filter(|list| count.contains(&list.len()))?;
            let texts = list
                .iter()
                .map(|named| named.get("endpoint")?.as_str())
                .collect::<Option<Vec<_>>>()?;
            Some(
                texts
                    .into_iter()
                    .filter_map(|text| text.parse().ok())
                    .collect::<BTreeSet<EndpointId>>(),
            )
        };
        let fighters = endpoints(tournament.get("fighters")?, 2..=2)?;
        let watchers = match tournament.get("watchers") {
            Some(watchers) => endpoints(watchers, 0..=MAX_WATCHERS)?,
            None => BTreeSet::new(),
        };
        Some(Self { fighters, watchers })
    }

    /// Whether the binding names `endpoint`, as a fighter or a watcher.
    pub(super) fn names(&self, endpoint: &EndpointId) -> bool {
        self.fighters.contains(endpoint) || self.watchers.contains(endpoint)
    }
}

/// What a committed checkpoint says about who is in the room: the native
/// roster, and the binding when the room is bound.
#[derive(Clone)]
pub(super) struct CommittedRoom {
    pub members: BTreeSet<EndpointId>,
    pub bound: Option<BoundRoster>,
}

/// Who may hold a vote: anyone in a room without a binding, and only the
/// binding's fighters in a bound one. Membership work runs on its own task,
/// so it takes this by value.
#[derive(Clone)]
pub(super) struct VoterPolicy(pub(super) Option<BTreeSet<EndpointId>>);

impl VoterPolicy {
    pub(super) fn allows(&self, endpoint: &EndpointId) -> bool {
        self.0
            .as_ref()
            .is_none_or(|fighters| fighters.contains(endpoint))
    }

    /// Whether the member with incarnation `id` may vote. In a bound room an
    /// incarnation with no admission on record never does.
    pub(super) fn allows_member(&self, admissions: &[Admission], id: u64) -> bool {
        admissions
            .iter()
            .find(|admission| admission.incarnation == id)
            .map_or(self.0.is_none(), |admission| {
                self.allows(&admission.primary_endpoint)
            })
    }

    /// The members of `retained` that may vote, which sets the room's voter
    /// count: two fighters keep one voter however many watch.
    pub(super) fn voting(&self, retained: &BTreeSet<EndpointId>) -> usize {
        retained
            .iter()
            .filter(|endpoint| self.allows(endpoint))
            .count()
    }
}

impl Actor {
    /// The binding of the room's last committed checkpoint this actor read,
    /// up to one refresh (a second) behind the leader's commit.
    pub(super) fn bound_roster(&self) -> Option<&BoundRoster> {
        let (room, _, committed) = self.refresh_roster.as_ref()?;
        if self.room != Some(*room) {
            return None;
        }
        committed.as_ref()?.bound.as_ref()
    }

    pub(super) fn voter_policy(&self) -> VoterPolicy {
        VoterPolicy(self.bound_roster().map(|bound| bound.fighters.clone()))
    }

    /// Whether a control from `peer` may open: in a bound room only from an
    /// endpoint the binding names.
    pub(super) fn admits_control(&self, peer: &EndpointId) -> bool {
        self.bound_roster().is_none_or(|bound| bound.names(peer))
    }

    /// Fence and close what a bound room's binding does not name, on each
    /// committed-roster read. An admission is only fenced here: it leaves
    /// through the membership operation, by the old quorum while its control
    /// still answers, and its control closes once that retirement is
    /// confirmed (`close_retired_strangers`). A control that never presented
    /// an Admission holds no seat and no vote, so it closes now. One whose
    /// Admission is still being applied is left to that operation, which
    /// either refuses it, closing it, or binds it, and the next read fences
    /// it like any other.
    pub(super) fn retire_unnamed(&mut self, own: u64) -> io::Result<()> {
        let Some(bound) = self.bound_roster().cloned() else {
            return Ok(());
        };
        let fenced = self
            .admissions
            .values()
            .filter(|admission| {
                admission.incarnation != own
                    && !bound.names(&admission.primary_endpoint)
                    && !self.retired_incarnations.contains(&admission.incarnation)
            })
            .map(|admission| admission.incarnation)
            .collect::<Vec<_>>();
        self.pending_retired_incarnations.extend(fenced);
        let unbound = self
            .controls
            .iter()
            .filter(|(peer, control)| !bound.names(peer) && control.session() == Session::Unbound)
            .map(|(peer, _)| *peer)
            .collect::<Vec<_>>();
        unbound
            .into_iter()
            .try_for_each(|peer| self.close_stranger(peer))
    }

    /// Close the controls of `confirmed` retirements whose endpoint the
    /// current binding does not name. A control for a named endpoint is a
    /// member coming back, left to the rejoin handling.
    pub(super) fn close_retired_strangers(&mut self, confirmed: &BTreeSet<u64>) -> io::Result<()> {
        let Some(bound) = self.bound_roster() else {
            return Ok(());
        };
        let retired = self
            .controls
            .iter()
            .filter(|(peer, control)| {
                !bound.names(peer)
                    && matches!(control.session(), Session::Bound(id) if confirmed.contains(&id))
            })
            .map(|(peer, _)| *peer)
            .collect::<Vec<_>>();
        retired
            .into_iter()
            .try_for_each(|peer| self.close_stranger(peer))
    }

    /// As `Command::CloseControl` closes one.
    fn close_stranger(&mut self, peer: EndpointId) -> io::Result<()> {
        let control = self.remove_control(peer);
        self.emit(Event::ControlClosed {
            epoch: self.epoch,
            peer,
            control,
        })
    }
}
