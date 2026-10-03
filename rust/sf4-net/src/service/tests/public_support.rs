//! Fixtures shared by the public-room test modules: the bridge and build
//! names, a signing identity and tickets, a running public host actor with
//! its command channel, and a member's coordination admission.
use ember_protocol::{
    SigningIdentity,
    play::VERSION as TICKET_VERSION,
    rooms::{RoomTicket, SignedRoomTicket, TICKET_SECS},
};
use zeroize::Zeroizing;

use super::*;
use crate::public_room::{self, PublicHost, PublicRoom};
pub(super) const BRIDGE: &str = "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";
pub(super) const BUILD: &str = "test-build";

pub(super) fn identity(seed: u8) -> SigningIdentity {
    SigningIdentity::from_seed(&Zeroizing::new([seed; 32])).unwrap()
}

pub(super) fn host_state() -> PublicHost {
    PublicHost::parse(
        &identity(9).public_key().to_b64u(),
        "k1",
        BRIDGE,
        identity(1).ember_id().as_str(),
    )
    .unwrap()
}

pub(super) fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

pub(super) fn ticket_for(
    invite: &Invite,
    account: &SigningIdentity,
    endpoint: &Endpoint,
) -> SignedRoomTicket {
    let issued_at = now().unwrap();
    RoomTicket {
        version: TICKET_VERSION,
        bridge_id: BRIDGE.into(),
        room_id: hex(&invite.room()),
        ember_id: account.ember_id().clone(),
        endpoint_id: endpoint.id().to_string(),
        issued_at,
        expires_at: issued_at + TICKET_SECS,
    }
    .sign(&identity(9), "k1")
    .unwrap()
}

/// A public host actor running its loop, with its command channel.
pub(super) struct PublicHostFixture {
    pub(super) host: Endpoint,
    pub(super) invite: Invite,
    pub(super) events: mpsc::Receiver<Event>,
    pub(super) commands: mpsc::Sender<Request>,
    _fault: watch::Sender<bool>,
    _scope: TaskScope,
}

pub(super) async fn public_host() -> PublicHostFixture {
    let host = endpoint().await;
    let invite =
        Invite::create(host.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
    let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (fault, failure) = watch::channel(false);
    let mut actor = test_actor(host.clone(), events_tx);
    actor.epoch = 1;
    actor.room = Some(invite.room());
    actor.hosted = Some(invite.clone());
    actor.room_invite = Some(invite.clone());
    actor.public = Some(PublicRoom::Host(host_state()));
    assert!(actor.server_owned());
    let service = tokio::spawn(async move {
        let result = actor.run(command_rx, failure).await;
        actor.clear_room();
        actor.tasks.shutdown().await;
        result
    });
    PublicHostFixture {
        host,
        invite,
        events,
        commands,
        _fault: fault,
        _scope: TaskScope(vec![service.abort_handle()]),
    }
}

impl PublicHostFixture {
    pub(super) async fn dial(
        &self,
        from: &Endpoint,
        ticket: &SignedRoomTicket,
    ) -> io::Result<transport::ControlChannel> {
        let connection = from
            .connect(address(&self.host), CONTROL_ALPN)
            .await
            .unwrap();
        public_room::connect_public_on(connection, &self.invite, ticket).await
    }

    pub(super) async fn dial_plain(
        &self,
        from: &Endpoint,
    ) -> io::Result<transport::ControlChannel> {
        let connection = from
            .connect(address(&self.host), CONTROL_ALPN)
            .await
            .unwrap();
        transport::connect_control_on(connection, &self.invite).await
    }

    pub(super) async fn no_event(&mut self, kind: &str) {
        let waited = timeout(Duration::from_millis(600), next(&mut self.events, kind)).await;
        assert!(waited.is_err(), "unexpected {kind} event");
    }
}

pub(super) fn member_admission(
    room: [u8; 16],
    incarnation: u64,
    primary: EndpointId,
    route: &Endpoint,
) -> Admission {
    Admission {
        room,
        incarnation,
        authority_term: 1,
        coordination_endpoint: route.id(),
        coordination_address: address(route),
        primary_endpoint: primary,
    }
}
