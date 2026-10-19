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

/// What a committed envelope says about a tournament binding.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) enum Binding {
    /// The room is not bound: the object is absent or empty, as the native
    /// side writes an unbound room.
    Unbound,
    Bound(BoundRoster),
    /// A binding is there, but not in the shape the native side writes.
    Unreadable,
}

impl BoundRoster {
    /// The binding in a committed envelope, at
    /// `checkpoint.room.snapshot.tournament`. A binding not in the shape the
    /// native side writes is `Unreadable`, not `Unbound`: the native side
    /// checks that shape before it commits a binding, so it means the writer
    /// and this reader have drifted. The room then keeps the last binding it
    /// could read (`Actor::keep_last_binding`) rather than lose its rules.
    ///
    /// An endpoint is caller-chosen hex the bridge does not check is a key.
    /// One that is not can never be a peer's, so it names no one and the
    /// binding still holds for the rest: a watcher cannot unbind a room by
    /// registering such an endpoint.
    pub(super) fn read(envelope: &Value) -> Binding {
        let Some(tournament) = envelope.pointer("/checkpoint/room/snapshot/tournament") else {
            return Binding::Unbound;
        };
        let Some(tournament) = tournament.as_object() else {
            return if tournament.is_null() {
                Binding::Unbound
            } else {
                Binding::Unreadable
            };
        };
        if tournament.is_empty() {
            return Binding::Unbound;
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
        let read = || {
            let fighters = endpoints(tournament.get("fighters")?, 2..=2)?;
            let watchers = match tournament.get("watchers") {
                Some(watchers) => endpoints(watchers, 0..=MAX_WATCHERS)?,
                None => BTreeSet::new(),
            };
            Some(Self { fighters, watchers })
        };
        read().map_or(Binding::Unreadable, Binding::Bound)
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
    /// The checkpoint carries a binding that could not be read, so `bound`
    /// is empty for want of one. Cleared once `keep_last_binding` has seen it.
    pub unreadable: bool,
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
    pub(super) fn allows_member(&self, admissions: &BTreeMap<u64, Admission>, id: u64) -> bool {
        admissions.get(&id).map_or(self.0.is_none(), |admission| {
            self.allows(&admission.primary_endpoint)
        })
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

    /// `room`, a commit just read, with the binding this actor last read in
    /// its place when the commit's own could not be read. Fencing on it would
    /// retire members by a guess, and treating the room as unbound would let
    /// watchers into the vote, so the last good binding keeps ruling until a
    /// readable one, or an empty one, replaces it. One line to stderr says so,
    /// once per such commit, since the commit is read once.
    pub(super) fn keep_last_binding(&self, room: Option<CommittedRoom>) -> Option<CommittedRoom> {
        let mut room = room?;
        if room.unreadable {
            eprintln!(
                "sf4-net: a room commit carries a tournament binding this helper cannot read; keeping the last one"
            );
            room.bound = self.bound_roster().cloned();
            room.unreadable = false;
        }
        Some(room)
    }

    /// The voters a room of `endpoints` should have. Only the members that
    /// may vote count, so two fighters keep one voter however many watch, and
    /// a server-owned room keeps its host alone. The one place the count is
    /// made: callers pass the roster they mean, the committed one or the
    /// admitted one, and the membership tasks take the number.
    pub(super) fn desired_voters(&self, endpoints: &BTreeSet<EndpointId>) -> usize {
        if self.server_owned() {
            return 1;
        }
        let policy = self.voter_policy();
        recovery::stable_voter_count(
            endpoints
                .iter()
                .filter(|endpoint| policy.allows(endpoint))
                .count(),
        )
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
    ///
    /// The local incarnation `own` is never fenced, named or not. A leader the
    /// binding does not name (leadership moved to an unnamed member while the
    /// room still held three voters) therefore keeps the room and its seat.
    /// Fencing it would take a handoff like a leaving leader's, which the
    /// fence does not make. The same bind kicks an honest unnamed member
    /// natively, and it leaves, so the room hands off through the filtered
    /// departure path; a hostile one runs no code of this helper.
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
