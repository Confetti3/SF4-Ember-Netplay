//! Public (server-owned) rooms in the actor: the room's public marker, ticket
//! admission of incoming controls, and bans. `HostPublic` and `JoinPublic`
//! run the same paths as `Host` and `Join`; what differs is the proof a
//! control presents and who may connect.
use ember_protocol::{EmberId, rooms::SignedRoomTicket};

use super::*;
use crate::public_room::{self, PublicHost, PublicRoom};

impl Actor {
    /// Whether this room has no leader change: a public host is its room's
    /// only voter, and a member takes coordination only from that host.
    pub(super) fn server_owned(&self) -> bool {
        self.public.is_some()
    }

    /// The ticket a server-owned member presents to its host, when it is one.
    pub(super) fn member_ticket(&self) -> Option<SignedRoomTicket> {
        match &self.public {
            Some(PublicRoom::Member { ticket }) => Some(ticket.clone()),
            _ => None,
        }
    }

    pub(super) fn is_public_host(&self) -> bool {
        matches!(self.public, Some(PublicRoom::Host(_)))
    }

    /// Opens the room: an invitation naming this endpoint once it is online.
    /// `room` is the id a public room was asked for; a private room's is
    /// generated.
    pub(super) fn spawn_host(&mut self, epoch: u64, build: String, room: Option<[u8; 16]>) {
        let endpoint = self.endpoint.clone();
        self.tasks.spawn(async move {
            let result = timeout(transport::HANDSHAKE_TIMEOUT, async {
                endpoint.online().await;
                let relay = endpoint
                    .addr()
                    .relay_urls()
                    .next()
                    .cloned()
                    .ok_or_else(|| failed("relay_unavailable"))?;
                host_invite(endpoint.id(), relay, build, room)
            })
            .await
            .unwrap_or_else(|_| Err(failed("relay_unavailable")));
            Completion::Hosted(epoch, result)
        });
    }

    pub(super) fn host_public(
        &mut self,
        id: u64,
        epoch: u64,
        build: String,
        room: [u8; 16],
        keys: (&str, &str, &str, &str),
    ) -> io::Result<()> {
        if !self.begin(epoch, &build) {
            return self.error_at(id, epoch, "invalid_room_state");
        }
        let (ticket_key, ticket_kid, bridge_id, creator) = keys;
        let host = PublicHost::parse(ticket_key, ticket_kid, bridge_id, creator);
        let Some(host) = host.filter(|_| room != [0; 16]) else {
            self.clear_room();
            return self.error_at(id, epoch, "invalid_request");
        };
        self.public = Some(PublicRoom::Host(host));
        self.spawn_host(epoch, build, Some(room));
        Ok(())
    }

    pub(super) fn join_public(
        &mut self,
        id: u64,
        epoch: u64,
        invitation: &str,
        build: String,
        ticket: SignedRoomTicket,
    ) -> io::Result<()> {
        if !self.begin(epoch, &build) {
            return self.error_at(id, epoch, "invalid_room_state");
        }
        // Set before the join so a refused one clears it with the room.
        self.public = Some(PublicRoom::Member { ticket });
        self.join_invitation(id, invitation, build, None)
    }

    /// The accounts of the live and waiting controls and the endpoint each
    /// came from.
    fn held_accounts(&self) -> BTreeMap<EmberId, EndpointId> {
        let live = self
            .controls
            .iter()
            .filter_map(|(peer, control)| control.account().map(|account| (account.clone(), *peer)));
        let waiting = self.parked_controls.iter().filter_map(|(peer, parked)| {
            parked
                .channel
                .account
                .as_ref()
                .map(|account| (account.clone(), *peer))
        });
        live.chain(waiting).collect()
    }

    /// A control connection reached a server-owned room. A public host reads
    /// only a ticket proof and never takes the member-rebind path, since its
    /// room has no leader change; a member of such a room is never dialed.
    /// Nothing is counted against `max_control_peers` or allocated until the
    /// proof passes: the connection that fails closes with no slot taken.
    pub(super) fn accept_public_incoming(&mut self, epoch: u64, connection: Connection) {
        let peer = connection.remote_id();
        self.prune_public_members();
        let policy = match &self.public {
            Some(PublicRoom::Host(host)) => host.policy(),
            _ => return connection.close(1u32.into(), b"room unavailable"),
        };
        let Some(invite) = self.hosted.clone().or_else(|| self.room_invite.clone()) else {
            return connection.close(1u32.into(), b"room unavailable");
        };
        // A full room still reads the proof, so a well-formed one is told it
        // was refused rather than left to see the connection close.
        let full =
            self.controls.len() >= self.max_control_peers() && !self.controls.contains_key(&peer);
        let held = self.held_accounts();
        self.tasks.spawn(async move {
            Completion::Control(
                epoch,
                public_room::accept_public_control(connection, &invite, policy, held, full).await,
            )
        });
    }

    /// The accept task decided on a snapshot. A ban, a second endpoint under
    /// the same account, a retired seat or a ticket's expiry can have landed
    /// since, so the actor decides again, on its own state and a fresh clock,
    /// before the control is installed. It is the same decision the accept
    /// task made (`AdmissionPolicy::decide`).
    pub(super) fn refuse_public_control(&mut self, channel: &ControlChannel) -> bool {
        if !self.is_public_host() {
            return false;
        }
        let (Some(ticket), Some(room)) = (channel.ticket.as_ref(), self.room) else {
            return true;
        };
        let Ok(now) = now() else {
            return true;
        };
        self.prune_public_members();
        let held = self.held_accounts();
        let Some(PublicRoom::Host(host)) = &self.public else {
            return true;
        };
        host.decide(
            ticket,
            &public_room::hex(&room),
            channel.connection.remote_id(),
            now,
            &held,
        )
        .is_err()
    }

    /// Bans an account for the room's life and closes what it holds.
    pub(super) fn ban_account(&mut self, id: u64, epoch: u64, account: &str) -> io::Result<()> {
        if !self.matches(epoch) {
            return self.error(id, "stale_epoch");
        }
        let Some(PublicRoom::Host(host)) = self.public.as_mut() else {
            return self.error(id, "invalid_room_state");
        };
        let Ok(account) = EmberId::parse(account) else {
            return self.error(id, "invalid_request");
        };
        if host.ban(account.clone()).is_err() {
            return self.error(id, "ban_limit");
        }
        let parked: Vec<_> = self
            .parked_controls
            .iter()
            .filter(|(_, parked)| parked.channel.account.as_ref() == Some(&account))
            .map(|(peer, _)| *peer)
            .collect();
        for peer in parked {
            if let Some(parked) = self.parked_controls.remove(&peer) {
                parked.channel.connection.close(1u32.into(), b"banned");
            }
        }
        let live: Vec<_> = self
            .controls
            .iter()
            .filter(|(_, control)| control.account() == Some(&account))
            .map(|(peer, _)| *peer)
            .collect();
        for peer in live {
            let control = self.remove_control(peer);
            self.emit(Event::ControlClosed {
                epoch,
                peer,
                control,
            })?;
        }
        Ok(())
    }
}

/// The invitation of a room being hosted: in the room id a public room was
/// asked for, or in one of its own.
pub(super) fn host_invite(
    endpoint: EndpointId,
    relay: iroh::RelayUrl,
    build: String,
    room: Option<[u8; 16]>,
) -> io::Result<Invite> {
    match room {
        Some(room) => {
            Invite::create_in_room(endpoint, relay, build, now()?, INVITE_LIFETIME, room)
        }
        None => Invite::create(endpoint, relay, build, now()?, INVITE_LIFETIME),
    }
}
