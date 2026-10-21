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
        /// The joiner watches a tournament match (docs/design/MATCH_WATCH.md
        /// 5.3), with the full invitation the bridge gave it. Sent only when
        /// true.
        #[serde(default, skip_serializing_if = "std::ops::Not::not")]
        observer: bool,
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
    /// The room board is open (`on`) or closed. `view` numbers the
    /// subscription; an open one is renewed every few seconds and lapses
    /// without it. Answered by `Event::MemberLatency` while it lasts.
    LatencyWatch {
        epoch: u64,
        room: [u8; 16],
        view: u64,
        on: bool,
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
        /// This helper's latency endpoint for the room, and the region code
        /// of its relay when it has a known one. Absent without one.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        latency: Option<EndpointId>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        latency_relay: Option<String>,
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
        /// The skill badge in that ticket, when the bridge gave one.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        badge: Option<ember_protocol::skill::SkillBadge>,
        /// As in `Hosted`.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        latency: Option<EndpointId>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        latency_relay: Option<String>,
    },
    /// The latency to each other member of the room, for the board
    /// subscription `view`. Sent whole when it changes, and droppable.
    MemberLatency {
        epoch: u64,
        room: [u8; 16],
        view: u64,
        members: Vec<MemberLatencyRow>,
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
        /// After a bridge ends, its connection's close reason.
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
        // Most frames any room control's send queue held (capacity 64).
        control_queue_peak: u64,
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

/// Why a member's latency is unavailable.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LatencyUnavailable {
    Unreachable,
    Refused,
    InMatch,
    NoLatencyRoute,
    RoomUnavailable,
}

/// What is known of one member's latency. A value always carries its
/// estimate and an unavailable state always carries its reason.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LatencyState {
    Measuring,
    Value { rtt_ms: u16 },
    Unavailable { reason: LatencyUnavailable },
}

/// One member's latency. On the wire `state` is `measuring`, `value` with
/// `rtt_ms`, or `unavailable` with a `reason`, and a row with any other
/// combination is refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(into = "MemberLatencyWire", try_from = "MemberLatencyWire")]
pub struct MemberLatencyRow {
    pub member: u64,
    pub incarnation: u64,
    pub state: LatencyState,
}

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum LatencyStateName {
    Measuring,
    Value,
    Unavailable,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct MemberLatencyWire {
    member: u64,
    incarnation: u64,
    state: LatencyStateName,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    rtt_ms: Option<u16>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    reason: Option<LatencyUnavailable>,
}

impl From<MemberLatencyRow> for MemberLatencyWire {
    fn from(row: MemberLatencyRow) -> Self {
        let (state, rtt_ms, reason) = match row.state {
            LatencyState::Measuring => (LatencyStateName::Measuring, None, None),
            LatencyState::Value { rtt_ms } => (LatencyStateName::Value, Some(rtt_ms), None),
            LatencyState::Unavailable { reason } => {
                (LatencyStateName::Unavailable, None, Some(reason))
            }
        };
        Self {
            member: row.member,
            incarnation: row.incarnation,
            state,
            rtt_ms,
            reason,
        }
    }
}

impl TryFrom<MemberLatencyWire> for MemberLatencyRow {
    type Error = &'static str;

    fn try_from(wire: MemberLatencyWire) -> Result<Self, Self::Error> {
        let state = match (wire.state, wire.rtt_ms, wire.reason) {
            (LatencyStateName::Measuring, None, None) => LatencyState::Measuring,
            (LatencyStateName::Value, Some(rtt_ms), None) => LatencyState::Value { rtt_ms },
            (LatencyStateName::Unavailable, None, Some(reason)) => {
                LatencyState::Unavailable { reason }
            }
            _ => return Err("a latency row's fields do not match its state"),
        };
        Ok(Self {
            member: wire.member,
            incarnation: wire.incarnation,
            state,
        })
    }
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

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn row(state: LatencyState) -> MemberLatencyRow {
        MemberLatencyRow {
            member: 2,
            incarnation: 7,
            state,
        }
    }

    #[test]
    fn every_latency_state_serializes_as_the_native_reader_expects() {
        let cases = [
            (
                row(LatencyState::Measuring),
                json!({"member": 2, "incarnation": 7, "state": "measuring"}),
            ),
            (
                row(LatencyState::Value { rtt_ms: 62 }),
                json!({"member": 2, "incarnation": 7, "state": "value", "rtt_ms": 62}),
            ),
        ];
        for (row, wire) in cases {
            assert_eq!(serde_json::to_value(row).unwrap(), wire);
            assert_eq!(serde_json::from_value::<MemberLatencyRow>(wire).unwrap(), row);
        }
        for (reason, name) in [
            (LatencyUnavailable::Unreachable, "unreachable"),
            (LatencyUnavailable::Refused, "refused"),
            (LatencyUnavailable::InMatch, "in_match"),
            (LatencyUnavailable::NoLatencyRoute, "no_latency_route"),
            (LatencyUnavailable::RoomUnavailable, "room_unavailable"),
        ] {
            let row = row(LatencyState::Unavailable { reason });
            let wire = json!({"member": 2, "incarnation": 7, "state": "unavailable", "reason": name});
            assert_eq!(serde_json::to_value(row).unwrap(), wire);
            assert_eq!(serde_json::from_value::<MemberLatencyRow>(wire).unwrap(), row);
        }
    }

    #[test]
    fn the_field_order_of_a_row_is_unchanged() {
        let text = serde_json::to_string(&row(LatencyState::Value { rtt_ms: 5 })).unwrap();
        assert_eq!(text, r#"{"member":2,"incarnation":7,"state":"value","rtt_ms":5}"#);
    }

    #[test]
    fn a_row_whose_fields_do_not_match_its_state_is_refused() {
        for wire in [
            json!({"member": 2, "incarnation": 7, "state": "value"}),
            json!({"member": 2, "incarnation": 7, "state": "value", "rtt_ms": 5, "reason": "refused"}),
            json!({"member": 2, "incarnation": 7, "state": "unavailable"}),
            json!({"member": 2, "incarnation": 7, "state": "unavailable", "rtt_ms": 5}),
            json!({"member": 2, "incarnation": 7, "state": "unavailable", "reason": "gone"}),
            json!({"member": 2, "incarnation": 7, "state": "measuring", "rtt_ms": 5}),
            json!({"member": 2, "incarnation": 7, "state": "measuring", "reason": "refused"}),
            json!({"member": 2, "incarnation": 7, "state": "fast"}),
            json!({"member": 2, "incarnation": 7, "state": "measuring", "extra": 1}),
        ] {
            assert!(
                serde_json::from_value::<MemberLatencyRow>(wire.clone()).is_err(),
                "{wire}"
            );
        }
    }
}
