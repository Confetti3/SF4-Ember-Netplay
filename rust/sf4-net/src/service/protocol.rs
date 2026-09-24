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
    Hosted {
        epoch: u64,
        invitation: String,
        room: [u8; 16],
    },
    Connected {
        epoch: u64,
        peer: EndpointId,
        room: [u8; 16],
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
        /// authorized. This is observational metadata; the committed probe
        /// connection is upgraded in place before GGPO owns its datagrams.
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
    // Helper load over the last second, once per second while a room is open.
    // The actor's 2 ms tick shares the runtime workers with every bridge task,
    // so its lag is the scheduling delay a gameplay packet can also see.
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
