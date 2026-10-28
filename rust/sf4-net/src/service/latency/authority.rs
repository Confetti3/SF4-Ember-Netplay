//! The one authority snapshot every latency step is checked against, and the
//! fences that revoke it (MEMBER_LATENCY.md 3.3).
use std::sync::Arc;

use iroh::EndpointId;

use crate::coordination::{Committed, LatencyRoster, RosterInvalid, latency_roster_of};

/// This helper's binding in its room: primary endpoint, latency endpoint and
/// process incarnation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Tuple {
    pub primary: EndpointId,
    pub latency: EndpointId,
    pub incarnation: u64,
}

/// A remote member as the committed roster binds it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PeerBinding {
    pub member: u64,
    pub primary: EndpointId,
    pub latency: EndpointId,
    pub incarnation: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum UnavailableReason {
    NoRoom,
    NoRecovery,
    InvalidCommit,
    NoLocalRow,
    /// This helper has a game, prepared or running.
    InMatch,
}

#[derive(Clone, Debug)]
pub(crate) enum AuthorityState {
    // Which reason is read by tests and debug output only.
    #[allow(dead_code)]
    Unavailable(UnavailableReason),
    Valid {
        room: [u8; 16],
        epoch: u64,
        local: Tuple,
        /// Our member id in `roster`.
        member: u64,
        roster: Arc<LatencyRoster>,
    },
}

#[derive(Clone, Debug)]
pub(crate) struct LatencyAuthority {
    /// Counts every revocation in this helper's life; never decreases.
    pub revocation: u64,
    pub state: AuthorityState,
}

impl LatencyAuthority {
    pub(crate) fn unavailable(revocation: u64, reason: UnavailableReason) -> Self {
        Self {
            revocation,
            state: AuthorityState::Unavailable(reason),
        }
    }

    /// The only authorization rule for latency, used at every step: `local`
    /// is our current tuple, and the roster holds exactly one row for
    /// `remote.member`, with that primary endpoint, latency endpoint and
    /// incarnation, which is not ours.
    pub(crate) fn authorizes(&self, local: &Tuple, remote: &PeerBinding) -> bool {
        match &self.state {
            AuthorityState::Valid {
                local: current,
                roster,
                ..
            } => {
                current == local
                    && remote.latency != current.latency
                    && roster.row(remote.member).is_some_and(|row| {
                        row.primary == remote.primary
                            && row.latency == Some(remote.latency)
                            && row.incarnation == remote.incarnation
                    })
            }
            AuthorityState::Unavailable(_) => false,
        }
    }

    pub(crate) fn local(&self) -> Option<Tuple> {
        match &self.state {
            AuthorityState::Valid { local, .. } => Some(*local),
            AuthorityState::Unavailable(_) => None,
        }
    }

    pub(crate) fn local_member(&self) -> Option<u64> {
        match &self.state {
            AuthorityState::Valid { member, .. } => Some(*member),
            AuthorityState::Unavailable(_) => None,
        }
    }

    pub(crate) fn roster(&self) -> Option<&Arc<LatencyRoster>> {
        match &self.state {
            AuthorityState::Valid { roster, .. } => Some(roster),
            AuthorityState::Unavailable(_) => None,
        }
    }

    /// The binding of the one other roster row whose latency endpoint is
    /// `remote`, when the authority authorizes it: the acceptor's identity
    /// check.
    pub(crate) fn binding_for(&self, remote: EndpointId) -> Option<(Tuple, PeerBinding)> {
        let local = self.local()?;
        let row = self.roster()?.by_latency(remote)?;
        let binding = PeerBinding {
            member: row.member,
            primary: row.primary,
            latency: remote,
            incarnation: row.incarnation,
        };
        self.authorizes(&local, &binding)
            .then_some((local, binding))
    }

    /// The binding of `member`'s row, when it has a latency endpoint and the
    /// authority authorizes it: the dialer's start.
    pub(crate) fn binding_of(&self, member: u64) -> Option<(Tuple, PeerBinding)> {
        let local = self.local()?;
        let row = self.roster()?.row(member)?;
        let binding = PeerBinding {
            member,
            primary: row.primary,
            latency: row.latency?,
            incarnation: row.incarnation,
        };
        self.authorizes(&local, &binding)
            .then_some((local, binding))
    }
}

/// What the authority is computed from, read by the actor in one step.
pub(crate) struct Inputs {
    pub room: Option<[u8; 16]>,
    pub epoch: u64,
    /// Our primary and latency endpoints; no latency endpoint means no room
    /// to measure in.
    pub endpoints: Option<(EndpointId, EndpointId)>,
    /// The room's store: its owner's incarnation, the committed checkpoint
    /// and its observation count, read together.
    pub store: Option<(u64, Arc<Committed>, u64)>,
    pub has_game: bool,
}

/// What an observation changed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Change {
    /// The revocation counter moved: everything authorized before is gone.
    pub revoked: bool,
    /// The authority stayed valid for the same binding with another roster.
    pub roster: bool,
}

/// The last commit read: compared by revision and term, then as the same
/// commit (`Committed::same_commit`), which is almost always the pointer test.
type CommitKey = Arc<Committed>;

/// Turns store observations into the authority, keeping the revocation
/// counter and the parse cache. It lives as long as the helper.
#[derive(Default)]
pub(crate) struct Observer {
    revocation: u64,
    /// The store last read (its owner's incarnation) and its count then.
    last_observation: Option<(u64, u64)>,
    /// The last commit parsed, success or failure.
    cache: Option<(CommitKey, Result<Arc<LatencyRoster>, RosterInvalid>)>,
    current: Option<Arc<LatencyAuthority>>,
    /// How many checkpoints were parsed, for tests.
    pub(crate) parses: u64,
}

impl Observer {
    pub(crate) fn current(&self) -> Arc<LatencyAuthority> {
        self.current.clone().unwrap_or_else(|| {
            Arc::new(LatencyAuthority::unavailable(
                self.revocation,
                UnavailableReason::NoRoom,
            ))
        })
    }

    /// Forgets the room's commits when it ends, so the next room starts with
    /// no observation and no roster. The revocation counter carries on.
    pub(crate) fn forget(&mut self) {
        self.cache = None;
        self.last_observation = None;
    }

    /// The roster of the last commit read, when it was one.
    pub(crate) fn last_roster(&self) -> Option<Arc<LatencyRoster>> {
        self.cache
            .as_ref()
            .and_then(|(_, roster)| roster.as_ref().ok().cloned())
    }

    fn roster(&mut self, committed: &Arc<Committed>) -> Result<Arc<LatencyRoster>, RosterInvalid> {
        if let Some((cached, result)) = &self.cache
            && cached.revision() == committed.revision()
            && cached.term() == committed.term()
            && cached.same_commit(committed)
        {
            return result.clone();
        }
        self.parses += 1;
        // The commit's own view: the actor's roster read shares its parse.
        let result = latency_roster_of(committed).map(Arc::new);
        self.cache = Some((committed.clone(), result.clone()));
        result
    }

    /// Applies one reading of the actor's state and the store.
    pub(crate) fn observe(&mut self, inputs: Inputs) -> Change {
        let mut skipped = false;
        let mut roster = None;
        match &inputs.store {
            Some((owner, committed, count)) => {
                if let Some((last_owner, last_count)) = self.last_observation
                    && last_owner == *owner
                    && *count != last_count
                    && *count != last_count.saturating_add(1)
                {
                    // At least one commit or snapshot was never seen.
                    skipped = true;
                }
                self.last_observation = Some((*owner, *count));
                roster = Some(self.roster(committed));
            }
            None => self.last_observation = None,
        }
        let state = match (inputs.room, inputs.endpoints, &inputs.store, roster) {
            (None, _, _, _) | (_, None, _, _) => {
                AuthorityState::Unavailable(UnavailableReason::NoRoom)
            }
            (_, _, None, _) | (_, _, _, None) => {
                AuthorityState::Unavailable(UnavailableReason::NoRecovery)
            }
            _ if inputs.has_game => AuthorityState::Unavailable(UnavailableReason::InMatch),
            (_, _, _, Some(Err(_))) => {
                AuthorityState::Unavailable(UnavailableReason::InvalidCommit)
            }
            (Some(room), Some((primary, latency)), Some((owner, ..)), Some(Ok(roster))) => {
                let local = Tuple {
                    primary,
                    latency,
                    incarnation: *owner,
                };
                let mut rows = roster.rows().iter().filter(|row| {
                    row.primary == local.primary
                        && row.latency == Some(local.latency)
                        && row.incarnation == local.incarnation
                });
                match (rows.next(), rows.next()) {
                    (Some(row), None) => AuthorityState::Valid {
                        room,
                        epoch: inputs.epoch,
                        local,
                        member: row.member,
                        roster,
                    },
                    _ => AuthorityState::Unavailable(UnavailableReason::NoLocalRow),
                }
            }
        };
        let previous = self.current();
        let (revoked, roster_changed) = match (&previous.state, &state) {
            (
                AuthorityState::Valid {
                    room,
                    epoch,
                    local,
                    roster,
                    ..
                },
                AuthorityState::Valid {
                    room: next_room,
                    epoch: next_epoch,
                    local: next_local,
                    roster: next_roster,
                    ..
                },
            ) => {
                let same = room == next_room && epoch == next_epoch && local == next_local;
                (!same, same && !Arc::ptr_eq(roster, next_roster))
            }
            (AuthorityState::Valid { .. }, AuthorityState::Unavailable(_)) => (true, false),
            _ => (false, false),
        };
        let revoked = revoked || skipped;
        if revoked {
            self.revocation = self.revocation.saturating_add(1);
        }
        self.current = Some(Arc::new(LatencyAuthority {
            revocation: self.revocation,
            state,
        }));
        Change {
            revoked,
            roster: roster_changed && !revoked,
        }
    }
}
