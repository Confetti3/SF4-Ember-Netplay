//! Who a room seats, and what a room bound to a tournament match changes in
//! the actor (docs/design/MATCH_WATCH.md 5.4). Every room change commits the
//! room's checkpoint, and a match room's names its two fighters and the
//! watchers the bridge admitted. In such a room only the fighters vote, only
//! an endpoint the seats name may open a control or present an Admission,
//! and the coordination leader retires every other admission through the
//! pending-retirement path, closing its control once that retirement is
//! confirmed.
//!
//! An open room takes none of these paths. Two changes made with them reach
//! every room: an admission counts the distinct endpoints it would leave in
//! the room rather than their incarnations, and a membership operation never
//! adds back a member it has fenced.
use ember_protocol::play::MAX_WATCHERS;
use serde_json::Value;

use super::*;

/// A commit's native roster, with the room and the commit it was read from
/// (`Actor::observe_commit`).
pub(super) type SeenCommit = (
    [u8; 16],
    Arc<crate::coordination::Committed>,
    Option<BTreeSet<EndpointId>>,
);

/// The endpoints a match room's checkpoint names.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct MatchRoster {
    pub fighters: BTreeSet<EndpointId>,
    pub watchers: BTreeSet<EndpointId>,
}

impl MatchRoster {
    /// Whether the roster names `endpoint`, as a fighter or a watcher.
    pub(super) fn names(&self, endpoint: &EndpointId) -> bool {
        self.fighters.contains(endpoint) || self.watchers.contains(endpoint)
    }
}

/// Who a room seats, as its last readable commit says.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(super) enum Seats {
    /// Not bound to a match: whoever the room admits.
    #[default]
    Open,
    /// Bound to a match: its fighters and watchers only.
    Match(MatchRoster),
}

impl Seats {
    /// The seats a committed envelope names, at
    /// `checkpoint.room.snapshot.tournament`: open when the object is absent,
    /// null or empty, as the native side writes an unbound room. `None` for
    /// a binding that is there but not in the shape the native side writes:
    /// the native side checks that shape before it commits one, so it means
    /// the writer and this reader have drifted, and the room keeps the seats
    /// it had (`Actor::observe_commit`) rather than lose its rules.
    ///
    /// An endpoint is caller-chosen hex the bridge does not check is a key.
    /// One that is not can never be a peer's, so it names no one and the
    /// binding still holds for the rest: a watcher cannot open a room by
    /// registering such an endpoint.
    pub(super) fn read(envelope: &Value) -> Option<Self> {
        let Some(tournament) = envelope.pointer("/checkpoint/room/snapshot/tournament") else {
            return Some(Self::Open);
        };
        let Some(tournament) = tournament.as_object() else {
            return tournament.is_null().then_some(Self::Open);
        };
        if tournament.is_empty() {
            return Some(Self::Open);
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
        Some(Self::Match(MatchRoster { fighters, watchers }))
    }

    /// Whether the seats take `endpoint`.
    fn admits(&self, endpoint: &EndpointId) -> bool {
        match self {
            Self::Open => true,
            Self::Match(roster) => roster.names(endpoint),
        }
    }
}

/// Who may hold a vote. Membership work runs on its own task, so it takes
/// this by value.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) enum Voters {
    /// A server-owned room: its host, for the room's whole life.
    Host,
    /// An open room: any member.
    Anyone,
    /// A match room: its two fighters.
    Fighters(BTreeSet<EndpointId>),
}

impl Voters {
    pub(super) fn may_vote(&self, endpoint: &EndpointId) -> bool {
        match self {
            Self::Host => false,
            Self::Anyone => true,
            Self::Fighters(fighters) => fighters.contains(endpoint),
        }
    }

    /// Whether the member with incarnation `id` may vote. One with no
    /// admission on record may only in an open room.
    pub(super) fn may_vote_member(&self, admissions: &BTreeMap<u64, Admission>, id: u64) -> bool {
        admissions
            .get(&id)
            .map_or(*self == Self::Anyone, |admission| {
                self.may_vote(&admission.primary_endpoint)
            })
    }

    /// The voters a room of `roster` should have: the stable count for the
    /// members that may vote, so two fighters keep one voter however many
    /// watch, and a server-owned room keeps its host alone. Callers pass the
    /// roster they mean, the committed one or the admitted one.
    pub(super) fn desired(&self, roster: &BTreeSet<EndpointId>) -> usize {
        match self {
            Self::Host => 1,
            _ => recovery::stable_voter_count(
                roster
                    .iter()
                    .filter(|endpoint| self.may_vote(endpoint))
                    .count(),
            ),
        }
    }

    /// Whether membership work may change the voter set: never in a
    /// server-owned room, whose host is its only voter.
    pub(super) fn changes_voters(&self) -> bool {
        *self != Self::Host
    }
}

impl Actor {
    /// The native roster of `committed`, the room's latest commit, read once
    /// per commit: the refresh runs every second, and the checkpoint paths
    /// read the same commit. The same read seats the room, so every rule
    /// below follows the commit whose roster it is applied to. A binding that
    /// cannot be read leaves the seats as they were: fencing on a guess would
    /// retire members, and opening the room would let watchers into the vote.
    /// One line to stderr says so, once per such commit; it is a diagnostic
    /// for the helper's log, not something the game acts on, so it is no
    /// event. A server-owned room is never bound, and its binding is never
    /// read.
    pub(super) fn observe_commit(
        &mut self,
        room: [u8; 16],
        committed: &Arc<crate::coordination::Committed>,
    ) -> Option<BTreeSet<EndpointId>> {
        if let Some((seen_room, seen, roster)) = &self.refresh_roster
            && *seen_room == room
            && seen.same_commit(committed)
        {
            return roster.clone();
        }
        let envelope = serde_json::from_slice::<Value>(committed.checkpoint().as_bytes()).ok();
        let roster = envelope
            .as_ref()
            .and_then(|envelope| self.committed_roster(envelope));
        if let Some(envelope) = &envelope
            && !self.server_owned()
        {
            match Seats::read(envelope) {
                Some(seats) => self.seats = seats,
                None => eprintln!(
                    "sf4-net: a room commit carries a tournament binding this helper cannot read; keeping the last one"
                ),
            }
        }
        self.refresh_roster = Some((room, committed.clone(), roster.clone()));
        roster
    }

    pub(super) fn voters(&self) -> Voters {
        if self.server_owned() {
            return Voters::Host;
        }
        match &self.seats {
            Seats::Open => Voters::Anyone,
            Seats::Match(roster) => Voters::Fighters(roster.fighters.clone()),
        }
    }

    /// Whether a control from `peer` may open or present an Admission: in a
    /// match room only from an endpoint its seats name, or from the
    /// coordination leader. A follower must reach its leader whoever that
    /// is; a leader the seats do not name hands the room off once the native
    /// side removes it.
    pub(super) fn admits_control(&self, peer: &EndpointId) -> bool {
        self.seats.admits(peer) || self.current_leader_primary() == Some(*peer)
    }

    /// Whether a joiner that only watches is taken: by a match room alone.
    pub(super) fn takes_observers(&self) -> bool {
        matches!(self.seats, Seats::Match(_))
    }

    /// Fence and close what a match room's seats do not name, on each
    /// committed-roster read of the coordination leader `own`. The leader
    /// alone does: it runs the membership operation and publishes the
    /// tombstones its followers act on, and a follower that fenced a leader
    /// the seats do not name would close its own control to it. An admission
    /// is only fenced here: it leaves through the membership operation, by
    /// the old quorum while its control still answers, and its control
    /// closes once that retirement is confirmed (`close_retired_strangers`).
    /// A control that never presented an Admission holds no seat and no
    /// vote, so it closes now. One whose Admission is still being applied is
    /// left to that operation, which either refuses it, closing it, or binds
    /// it, and the next read fences it like any other.
    ///
    /// The leader itself is never fenced, named or not. One the seats do not
    /// name (leadership moved to an unnamed member while the room still held
    /// three voters) therefore keeps the room and its seat. Fencing it would
    /// take a handoff like a leaving leader's, which the fence does not make.
    /// The same bind kicks an honest unnamed member natively, and it leaves,
    /// so the room hands off through the filtered departure path; a hostile
    /// one runs no code of this helper.
    pub(super) fn retire_unnamed(&mut self, own: u64) -> io::Result<()> {
        let Seats::Match(roster) = self.seats.clone() else {
            return Ok(());
        };
        let fenced = self
            .admissions
            .values()
            .filter(|admission| {
                admission.incarnation != own
                    && !roster.names(&admission.primary_endpoint)
                    && !self.retired_incarnations.contains(&admission.incarnation)
            })
            .map(|admission| admission.incarnation)
            .collect::<Vec<_>>();
        self.pending_retired_incarnations.extend(fenced);
        let unbound = self
            .controls
            .iter()
            .filter(|(peer, control)| !roster.names(peer) && control.session() == Session::Unbound)
            .map(|(peer, _)| *peer)
            .collect::<Vec<_>>();
        unbound
            .into_iter()
            .try_for_each(|peer| self.close_control(peer))
    }

    /// Close the controls of `confirmed` retirements whose endpoint the
    /// current seats do not name. A control for a named endpoint is a member
    /// coming back, left to the rejoin handling.
    pub(super) fn close_retired_strangers(&mut self, confirmed: &BTreeSet<u64>) -> io::Result<()> {
        let Seats::Match(roster) = &self.seats else {
            return Ok(());
        };
        let retired = self
            .controls
            .iter()
            .filter(|(peer, control)| {
                !roster.names(peer)
                    && matches!(control.session(), Session::Bound(id) if confirmed.contains(&id))
            })
            .map(|(peer, _)| *peer)
            .collect::<Vec<_>>();
        retired
            .into_iter()
            .try_for_each(|peer| self.close_control(peer))
    }
}
