//! Joining a room: the IPC join, and the dial of the room an invitation
//! names.
use ember_protocol::rooms::SignedRoomTicket;

use super::*;

/// How a join came by its invitation, which decides what it presents to the
/// room.
pub(super) enum JoinVia<'a> {
    /// Pasted, or given by a bridge. `observer` when the joiner only watches
    /// a tournament match.
    Pasted { observer: bool },
    /// Opened from a short link with this code. The invitation must derive
    /// the same code, so a record cannot stand in for another room.
    Short(&'a str),
    /// A public room, joined with the member's ticket.
    Public(SignedRoomTicket),
}

impl Actor {
    /// `Command::Join`. A short link is looked up first; the full invitation
    /// it stands for then takes the same path as a pasted one. A watcher
    /// always has the full invitation from its bridge.
    pub(super) fn join_command(
        &mut self,
        id: u64,
        epoch: u64,
        invitation: &str,
        build: String,
        observer: bool,
    ) -> io::Result<()> {
        // Consume an accepted room epoch before parsing the invitation.
        // Rejection then belongs to the caller's attempt and Leave can
        // acknowledge cancellation even though no connection was made.
        if !self.begin(epoch, &build) {
            return self.error_at(id, epoch, "invalid_room_state");
        }
        match crate::short_invite::parse(invitation) {
            Some(_) if observer => {
                // The invitation is well formed; the request is not, so the
                // refusal names that rather than a malformed link.
                self.clear_room();
                self.error_because(
                    id,
                    "invalid_or_incompatible_invitation",
                    Some("observer_short_link"),
                )
            }
            Some(code) => self.join_short(id, epoch, code, build),
            None => self.join_invitation(id, invitation, build, JoinVia::Pasted { observer }),
        }
    }

    /// Joins the room `invitation` names, as `via` came by it.
    pub(super) fn join_invitation(
        &mut self,
        id: u64,
        invitation: &str,
        build: String,
        via: JoinVia<'_>,
    ) -> io::Result<()> {
        let epoch = self.epoch;
        let time = now().ok();
        let invite = time.and_then(|time| Invite::parse_for_build(invitation, time, &build).ok());
        let invite = match invite {
            Some(invite)
                if invite.build() == build
                    && match via {
                        JoinVia::Short(code) => invite.short_code() == code,
                        _ => true,
                    } =>
            {
                invite
            }
            Some(_) => {
                self.clear_room();
                return self.error_because(
                    id,
                    "invalid_or_incompatible_invitation",
                    Some("malformed"),
                );
            }
            None => {
                let reason = time.map_or("malformed", |time| {
                    Invite::rejection_reason(invitation, time, &build)
                });
                self.clear_room();
                return self.error_because(id, "invalid_or_incompatible_invitation", Some(reason));
            }
        };
        // A copy taken while this helper still led the room names this
        // endpoint. Iroh refuses to dial itself, which would otherwise
        // read as an unreachable host.
        if invite.endpoint() == self.endpoint.id() {
            self.clear_room();
            return self.error_because(id, "invalid_or_incompatible_invitation", Some("own_room"));
        }
        self.room = Some(invite.room());
        self.host_address = Some(invite.address());
        let endpoint = self.endpoint.clone();
        let proof = invite.proof_for(matches!(via, JoinVia::Pasted { observer: true }));
        let ticket = match via {
            JoinVia::Public(ticket) => Some(ticket),
            JoinVia::Pasted { .. } | JoinVia::Short(_) => None,
        };
        let latency = self.latency_binding();
        self.tasks.spawn(async move {
            let (result, latency) = latency::beside(latency, async {
                match &ticket {
                    Some(ticket) => {
                        crate::public_room::connect_public(&endpoint, &invite, ticket).await
                    }
                    None => {
                        transport::connect_control_with(&endpoint, &invite, &proof, false).await
                    }
                }
            })
            .await;
            Completion::GuestControl(epoch, invite, result, latency)
        });
        Ok(())
    }
}
