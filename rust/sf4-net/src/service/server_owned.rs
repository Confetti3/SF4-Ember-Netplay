//! What a server-owned (public) room changes in the actor beyond admission
//! (`public.rs`). The host is the room's only voter for its whole life and
//! is not a room member, a member takes coordination from that host alone,
//! and a room that loses its host ends instead of electing another. See
//! `docs/design/PUBLIC_ROOMS.md`. Private rooms take none of these paths.
use ember_protocol::EmberId;

use super::*;
use crate::public_room::PublicRoom;

/// How long the host of a server-owned room may stay unwritable, as a member
/// sees it, before the member gives up on the room. A private room's election
/// window (`RECOVERY_ELECTION_GRACE`) is shorter because it ends in a
/// replacement leader; here nothing replaces the host, so a brief outage
/// should not end the room.
const HOST_LOSS_GRACE: Duration = Duration::from_secs(20);

impl Actor {
    /// How many control connections this actor accepts. A private host is one
    /// of its room's sixteen members, so it has fifteen remote ones; a
    /// server-owned host has no seat of its own and serves all sixteen.
    pub(super) fn max_control_peers(&self) -> usize {
        if self.is_public_host() {
            MAX_CONTROL_PEERS + 1
        } else {
            MAX_CONTROL_PEERS
        }
    }

    /// The voters a room of `members` should have: a server-owned room has
    /// its host and nobody else.
    pub(super) fn desired_voters(&self, members: usize) -> usize {
        if self.server_owned() {
            1
        } else {
            recovery::stable_voter_count(members)
        }
    }

    /// The members a committed checkpoint names. A server-owned host is the
    /// room's leader but not one of its members, so it is added to the roster
    /// here: reconciliation never treats it as departed for being absent from
    /// the native member list, and a room whose members have all left still
    /// has a roster to retire them against.
    pub(super) fn committed_roster(&self, checkpoint: &[u8]) -> Option<BTreeSet<EndpointId>> {
        if !self.is_public_host() {
            return committed_primary_endpoints(checkpoint);
        }
        let mut roster = primary_endpoints(checkpoint, true)?;
        roster.insert(self.endpoint.id());
        Some(roster)
    }

    /// The records a member of a server-owned room is told about. Members
    /// never dial one another's coordination endpoints: a learner receives
    /// from the leader and asks the leader, and nothing else. A record of
    /// another member therefore carries its endpoint id and no route, so one
    /// member never learns another's addresses; the host's own record keeps
    /// its route, since that is the one members dial.
    pub(super) fn membership_view(&self, admissions: Vec<Admission>) -> Vec<Admission> {
        if !self.server_owned() {
            return admissions;
        }
        let host = self.recovery.as_ref().map(|recovery| recovery.incarnation);
        admissions
            .into_iter()
            .map(|mut admission| {
                if Some(admission.incarnation) != host {
                    admission.coordination_address =
                        EndpointAddr::new(admission.coordination_endpoint);
                }
                admission
            })
            .collect()
    }

    /// The host of this server-owned room's coordination stayed unwritable
    /// for the grace. A private room asks a surviving voter to campaign; a
    /// server-owned room has none, so a member ends the room, as a Leave
    /// would.
    pub(super) async fn handle_failed_leader(&mut self, leader: u64) -> io::Result<()> {
        if !self.server_owned() {
            self.trigger_recovery_election_for_leader(leader).await;
            return Ok(());
        }
        // Gameplay is independent of the room's health: a match in progress is
        // left to finish, and the room ends at the next check once no link is
        // open.
        if self.games.values().any(|slot| slot.stats.is_some()) {
            return Ok(());
        }
        let epoch = self.epoch;
        self.clear_room();
        self.emit(Event::RoomClosed { epoch })
    }

    /// The host of the server-owned room this actor is a member of.
    pub(super) fn host_endpoint(&self) -> Option<EndpointId> {
        self.host_address.as_ref().map(|address| address.id)
    }

    /// The control a probe reservation for `peer` leaves on: the peer's own,
    /// or, for a member of a server-owned room, the host's.
    pub(super) fn probe_reservation_route(&self, peer: EndpointId) -> Option<EndpointId> {
        let route = if self.server_owned() && !self.is_public_host() {
            self.host_endpoint()?
        } else {
            peer
        };
        self.controls.contains_key(&route).then_some(route)
    }

    /// A member's probe reservation reached the host of a server-owned room.
    /// Members hold no control to one another, so the host passes the frame
    /// to the member it names. The host checks that the sender is the source
    /// it claims; the target checks the committed reservation itself. False
    /// closes the sender's control, as any refused coordination frame does.
    pub(super) fn relay_probe_reservation(
        &mut self,
        sender: EndpointId,
        frame: CoordinationControl,
    ) -> bool {
        let CoordinationControl::ProbeReservation {
            room,
            source,
            source_incarnation,
            target_incarnation,
            ..
        } = &frame
        else {
            return false;
        };
        let admitted = |endpoint: Option<EndpointId>, incarnation: u64| {
            self.admissions
                .values()
                .find(|admission| {
                    admission.room == *room
                        && admission.incarnation == incarnation
                        && endpoint.is_none_or(|endpoint| admission.primary_endpoint == endpoint)
                })
                .map(|admission| admission.primary_endpoint)
        };
        if *source != sender || admitted(Some(sender), *source_incarnation).is_none() {
            return false;
        }
        // A target that has left, or whose control is down, misses the frame
        // and the source's check times out.
        let Some(target) = admitted(None, *target_incarnation).filter(|target| *target != sender)
        else {
            return true;
        };
        let Ok(payload) = serde_json::to_string(&frame) else {
            return true;
        };
        let id = self.next_transport_message;
        self.next_transport_message = self.next_transport_message.saturating_add(1);
        if let Some(control) = self.controls.get(&target) {
            let _ = control.try_send(ControlFrame {
                message_id: id,
                payload: payload.into_bytes(),
            });
        }
        true
    }

    /// How long a leader may stay unwritable before the room acts on it.
    pub(super) fn leader_loss_grace(&self) -> Duration {
        if self.server_owned() {
            HOST_LOSS_GRACE
        } else {
            RECOVERY_ELECTION_GRACE
        }
    }

    /// Records the account a control from `peer` is installed under. A
    /// waiting replacement is not a member until it is installed, so this runs
    /// at the install and never for a candidate. The seat is eligible to
    /// return past its ticket once `prune_public_members` binds it to the
    /// member's coordination admission.
    pub(super) fn remember_public_member(&mut self, peer: EndpointId, account: &EmberId) {
        if let Some(PublicRoom::Host(host)) = self.public.as_mut() {
            host.admit_member(peer, account.clone());
        }
    }

    /// Brings the host's seats in line with the coordination roster: a seat
    /// is bound to its member's live admission and ends when that admission
    /// retires, whatever controls remain; one with no admission yet lasts only
    /// as long as its installed control. Parked candidates are not members.
    pub(super) fn prune_public_members(&mut self) {
        let live = self
            .admissions
            .values()
            .filter(|admission| {
                !self.retired_incarnations.contains(&admission.incarnation)
                    && !self
                        .pending_retired_incarnations
                        .contains(&admission.incarnation)
            })
            .map(|admission| (admission.primary_endpoint, admission.incarnation))
            .collect::<BTreeSet<_>>();
        let connected = self.controls.keys().copied().collect::<BTreeSet<_>>();
        if let Some(PublicRoom::Host(host)) = self.public.as_mut() {
            host.sync_members(&live, &connected);
        }
    }
}
