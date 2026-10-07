//! IPC schema between the native client and the helper: commands in, events out.
use super::*;

// No Debug derive: invitations and match capabilities must not reach logs.
#[derive(Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum Command {
    Status,
    Host {
        epoch: u64,
        build: String,
    },
    Join {
        epoch: u64,
        invitation: String,
        build: String,
    },
    /// Host a public (server-owned) room. `ticket_key` is the bridge's public
    /// key as `/v1/signing-keys` publishes it; members are admitted by tickets
    /// it signed under `ticket_kid` for `bridge_id`. The room id is the
    /// caller's, since the tickets already name it; it is never all zero.
    /// `creator` is the Ember ID the room was created for: until a member has
    /// been admitted, only a ticket for that account is.
    HostPublic {
        epoch: u64,
        build: String,
        room: [u8; 16],
        ticket_key: String,
        ticket_kid: String,
        bridge_id: String,
        creator: String,
    },
    /// Join a public room with the ticket the bridge issued for this endpoint.
    JoinPublic {
        epoch: u64,
        invitation: String,
        ticket: ember_protocol::rooms::SignedRoomTicket,
        build: String,
    },
    /// Refuse an Ember ID for the rest of this public room, closing any
    /// control admitted under it.
    BanAccount {
        epoch: u64,
        account: String,
    },
    Leave {
        epoch: u64,
        /// Replacement-room teardown may retire local state without trying
        /// to mutate the old Raft membership. Normal departure remains the
        /// quorum-confirmed path when this is false.
        #[serde(default)]
        abandon: bool,
    },
    Send {
        epoch: u64,
        peer: EndpointId,
        message_id: u64,
        payload: String,
        /// The control connection the native side numbered this message
        /// for, from its connected event. A newer connection from the same
        /// endpoint belongs to a session the native side has not seen yet,
        /// so a message for the old one is refused rather than delivered.
        /// Zero, from a native side that does not track it, delivers to the
        /// current connection.
        #[serde(default)]
        control: u64,
    },
    CloseControl {
        epoch: u64,
        peer: EndpointId,
    },
    PrepareGame {
        epoch: u64,
        peer: EndpointId,
        room: [u8; 16],
        generation: u64,
        capability: [u8; 32],
        local_port: u16,
        max_packet: usize,
        dial: bool,
    },
    EndMatch {
        epoch: u64,
        generation: u64,
    },
    EndPeer {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
    },
    CheckpointBegin {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointChunk {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        offset: u32,
        data: String,
    },
    CheckpointEnd {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointAck {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        offset: u32,
    },
    ProbeRequest {
        epoch: u64,
        room: [u8; 16],
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        #[serde(default)]
        benchmark: bool,
    },
    /// Publish this room's short link, answered by `Event::ShortInvite`.
    ShortInvite {
        epoch: u64,
    },
    /// Identity and tournament-bridge requests. The IPC reader hands these
    /// to `crate::tournament`; they never reach the room actor.
    Tournament {
        request: crate::tournament::Request,
    },
    Shutdown,
}

#[derive(Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum Event {
    DiscordInvite {
        epoch: u64,
        invitation: String,
        secret: String,
    },
    /// The answer to `Command::ShortInvite`: `status` is `ready` with the
    /// link, or `unavailable` with an empty link when the link service could
    /// not take the room. The link stays the same for the room's life.
    ShortInvite {
        epoch: u64,
        link: String,
        status: String,
    },
    CoordinationState {
        epoch: u64,
        room: [u8; 16],
        term: u64,
        revision: u64,
        incarnation: u64,
        leader: String,
        writable: bool,
        leader_local: bool,
        voter_count: usize,
        learner_count: usize,
    },
    CheckpointBegin {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointChunk {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        offset: u32,
        data: String,
    },
    CheckpointEnd {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    CheckpointAck {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        offset: u32,
    },
    CheckpointCommitted {
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    },
    ControlRebound {
        epoch: u64,
        room: [u8; 16],
        leader: String,
        members: Vec<String>,
        member_incarnations: BTreeMap<String, u64>,
    },
    ProbeResult {
        epoch: u64,
        room: [u8; 16],
        peer: EndpointId,
        request: u64,
        pair_revision: u64,
        route: String,
        status: String,
        sample_count: u32,
        loss_count: u32,
        p95_rtt_us: u64,
        recommended_delay: i16,
        #[serde(default)]
        metrics: crate::probe::Metrics,
    },
    Status {
        request_id: u64,
        endpoint: EndpointId,
        ip_transports: usize,
        /// The fixed UDP port the endpoint holds, or 0 when the OS chose it.
        fixed_port: u16,
        epoch: u64,
        peers: usize,
        games: usize,
    },
    /// A privacy-safe summary of this endpoint's network, sent when it first
    /// becomes known and again whenever it changes. It belongs to the endpoint,
    /// not to a room, so it has no epoch. It carries no address, port or relay
    /// URL.
    NetworkReport {
        /// Home relay region: `use1`, `usw1`, `euc1`, `aps1`, `other`, or
        /// empty while no home relay is chosen.
        relay: String,
        relay_connected: bool,
        /// A UDP round trip to a relay completed.
        udp: bool,
        /// `open`, `strict` (the public address differs by destination, so
        /// hole punching usually fails), `no_udp`, or `checking` before the
        /// first net report.
        nat: String,
        captive_portal: bool,
    },
    Hosted {
        epoch: u64,
        invitation: String,
        room: [u8; 16],
    },
    Connected {
        epoch: u64,
        peer: EndpointId,
        room: [u8; 16],
        /// The control connection this event is about, echoed by its close.
        control: u64,
        /// The Ember ID a public host's ticket admitted this connection
        /// under. Absent in a private room.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        account: Option<String>,
    },
    Message {
        epoch: u64,
        peer: EndpointId,
        message_id: u64,
        payload: String,
    },
    ControlClosed {
        epoch: u64,
        peer: EndpointId,
        control: u64,
    },
    /// The peer announced its departure over its still-open control. The
    /// native room removes that member now rather than after the departure
    /// grace that follows a silent control close.
    PeerDeparted {
        epoch: u64,
        peer: EndpointId,
    },
    /// The peer's Admission for a process incarnation this actor had not
    /// admitted before was accepted: a new room session on that endpoint,
    /// not a control reconnect. Its native message numbering starts over.
    PeerSession {
        epoch: u64,
        peer: EndpointId,
        incarnation: u64,
    },
    Sent {
        request_id: u64,
        epoch: u64,
        message_id: u64,
    },
    GameWaiting {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
    },
    GameReady {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        virtual_port: u16,
        max_packet: usize,
        /// The selected Iroh path at the moment the gameplay bridge is
        /// authorized. This is observational metadata.
        route: String,
        /// Whether that path reaches the peer on a fixed port.
        fixed_port: bool,
    },
    GameFailed {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        reason: crate::bridge::Failure,
        route: String,
        max_packet: usize,
        max_datagram: usize,
    },
    GameClosed {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        /// Set when the link never formed: what this helper saw instead.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
    Statistics {
        epoch: u64,
        peer: EndpointId,
        generation: u64,
        sent_packets: u64,
        received_packets: u64,
        sent_bytes: u64,
        received_bytes: u64,
        rejected_packets: u64,
        congestion_events: u64,
        local_drops: u64,
        route: String,
    },
    // Helper load since the last report: once a second during a game, and
    // every IDLE_LOAD_REPORT_SECS while a room is open with no game. A report
    // the event queue had no room for is folded into the next one. During a
    // game the actor ticks every 2 ms on the runtime workers it shares with
    // every bridge task, so its lag is the scheduling delay a gameplay packet
    // can also see.
    HelperLoad {
        epoch: u64,
        actor_tick_lag_max_us: u64,
        actor_tick_body_max_us: u64,
        // Fewest free slots seen in the IPC event queue (capacity 128).
        event_queue_free_min: u64,
    },
    // An actor step ran past stall::STALL_REPORT. Sent while it is still
    // stuck and again when it ends; not tied to a room epoch.
    HelperStall {
        stage: String,
        stalled_ms: u64,
        ended: bool,
    },
    RoomClosed {
        epoch: u64,
    },
    Error {
        request_id: u64,
        epoch: u64,
        #[serde(skip_serializing_if = "Option::is_none")]
        probe_failure: Option<u8>,
        #[serde(skip_serializing_if = "Option::is_none")]
        peer: Option<EndpointId>,
        code: String,
        /// Why a `control_send_failed` happened: `replaced`, `missing`,
        /// `queue_full`, `queue_closed` or `invalid`. Diagnostic only.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
    /// The answer to a `tournament` command (`request_id` 0 for the status
    /// sent at startup). `identity` is the public identity status after the
    /// operation; `reason` is a stable failure code. Neither ever carries a
    /// key, passphrase or bridge token.
    Tournament {
        request_id: u64,
        op: String,
        ok: bool,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        identity: Option<serde_json::Value>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        data: Option<serde_json::Value>,
    },
    Stopped,
}

/// Native C++ payloads are JSON text. Serializing that text as a JSON
/// `String` doubles every quote and can push an otherwise valid near-limit
/// message over the 64 KiB control frame bound. Keep the public Rust field a
/// String for the existing IPC API, but emit valid JSON payloads as a
/// RawValue so the wrapper adds only its small envelope overhead.
pub(super) struct NativeControlMessage {
    pub(super) kind: String,
    pub(super) message_id: u64,
    pub(super) payload: String,
}

impl Serialize for NativeControlMessage {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        use serde::ser::SerializeStruct;
        let mut state = serializer.serialize_struct("NativeControlMessage", 3)?;
        state.serialize_field("type", &self.kind)?;
        state.serialize_field("message_id", &self.message_id)?;
        if let Ok(raw) = serde_json::value::RawValue::from_string(self.payload.clone()) {
            state.serialize_field("payload", &raw)?;
        } else {
            state.serialize_field("payload", &self.payload)?;
        }
        state.end()
    }
}

impl<'de> Deserialize<'de> for NativeControlMessage {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(deny_unknown_fields)]
        struct Wire {
            #[serde(rename = "type")]
            kind: String,
            message_id: u64,
            payload: Box<serde_json::value::RawValue>,
        }
        let wire = Wire::deserialize(deserializer)?;
        let encoded = wire.payload.get();
        let payload = if encoded.starts_with('"') {
            serde_json::from_str(encoded).map_err(serde::de::Error::custom)?
        } else {
            encoded.to_owned()
        };
        Ok(Self {
            kind: wire.kind,
            message_id: wire.message_id,
            payload,
        })
    }
}
