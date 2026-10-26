//! Process entry: pipe reader and writer tasks around one `Actor`.
use super::*;

pub async fn run<S: AsyncRead + AsyncWrite + Unpin + Send + 'static>(
    stream: S,
    endpoint: Endpoint,
    relay_only: bool,
) -> io::Result<()> {
    run_on_port(stream, endpoint, relay_only, None).await
}

/// `run` with the exact UDP port the coordination endpoint binds when this
/// process hosts a room. Hosting fails if the port cannot be bound.
pub async fn run_on_port<S: AsyncRead + AsyncWrite + Unpin + Send + 'static>(
    stream: S,
    endpoint: Endpoint,
    relay_only: bool,
    coordination_port: Option<u16>,
) -> io::Result<()> {
    let (mut reader, mut writer) = tokio::io::split(stream);
    let (commands, receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (events, mut outbound) = mpsc::channel::<Event>(IPC_QUEUE_CAPACITY);
    let (failed_ipc, failure) = watch::channel(false);
    let writer_failed = failed_ipc.clone();
    let relays = endpoint.clone();
    let (tournament, tournament_task) = crate::tournament::spawn(
        events.clone(),
        endpoint.id().to_string(),
        Box::new(move || network::known_home_region(&relays)),
    );
    let reader_task = tokio::spawn(async move {
        let mut last_id = 1;
        while let Ok(mut frame) = wire::read_ipc(&mut reader).await {
            if frame.message_id <= last_id {
                break;
            }
            last_id = frame.message_id;
            let parsed = decode_command(&frame.payload);
            // A tournament request's text holds its passphrase; the parsed
            // request wipes its own copy, and this one goes now.
            zeroize::Zeroize::zeroize(&mut frame.payload);
            let Ok(command) = parsed else {
                break;
            };
            if let Command::Tournament { request } = command {
                tournament.submit(frame.message_id, request);
                continue;
            }
            if commands
                .send(Request {
                    id: frame.message_id,
                    command,
                })
                .await
                .is_err()
            {
                break;
            }
        }
        let _ = failed_ipc.send(true);
    });
    let writer_task = tokio::spawn(async move {
        let mut message_id = 2;
        while let Some(event) = outbound.recv().await {
            let stopped = matches!(event, Event::Stopped);
            let payload = encode_event(event)?;
            wire::write_ipc(
                &mut writer,
                &ControlFrame {
                    message_id,
                    payload,
                },
            )
            .await?;
            if stopped {
                return Ok::<(), io::Error>(());
            }
            message_id += 1;
        }
        Err(failed("IPC writer ended"))
    });
    let writer_abort = writer_task.abort_handle();
    // Watch the writer independently: a blocked reader must not hide its death.
    let writer_watch = tokio::spawn(async move {
        let result = writer_task.await;
        if !matches!(result, Ok(Ok(()))) {
            let _ = writer_failed.send(true);
        }
    });
    let _task_scope = TaskScope(vec![
        tournament_task.abort_handle(),
        reader_task.abort_handle(),
        writer_abort,
        writer_watch.abort_handle(),
    ]);
    let mut actor = Actor {
        endpoint: endpoint.clone(),
        relay_only,
        epoch: 0,
        opening: false,
        room: None,
        hosted: None,
        room_invite: None,
        host_address: None,
        controls: BTreeMap::new(),
        parked_controls: BTreeMap::new(),
        games: BTreeMap::new(),
        closed_generation: 0,
        tasks: JoinSet::new(),
        events: EventOutbox::new(events),
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
        pending_admissions: BTreeMap::new(),
        applied_admission_members: BTreeSet::new(),
        incoming_transfer: None,
        pending_checkpoint_proposal: None,
        pending_checkpoint_retry: None,
        outgoing_transfer: None,
        committed_native_members: None,
        pending_retired_incarnations: BTreeSet::new(),
        retired_incarnations: BTreeSet::new(),
        last_exported_revision: 0,
        pending_checkpoint_ack: None,
        pending_checkpoint_committed: None,
        next_transport_message: TRANSPORT_MESSAGE_ID_BASE,
        next_coordination_operation: 1,
        pending_coordination_refresh: None,
        pending_membership_operation: None,
        pending_admission_operation: None,
        pending_admission_bindings: Vec::new(),
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        last_control_rebound: None,
        unwritable_leader_since: None,
        reconnect_target: None,
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        own_probes: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        pending_probe_reservations: BTreeMap::new(),
        join_settled: false,
        join_control_losses: 0,
        join_first_loss: None,
        retirement_started: None,
        departure_failed: false,
        short: ShortLinks::default(),
        public: None,
        coordination_port,
        refresh_roster: None,
        seats: Default::default(),
        wake: Arc::new(Notify::new()),
        latency: Default::default(),
    };
    let result = actor.run(receiver, failure).await;
    actor.clear_room();
    actor.tasks.shutdown().await;
    endpoint.close().await;
    if result.is_ok() {
        let _ = actor.events.flush();
        let _ = actor.emit(Event::Stopped);
    }
    drop(actor);
    reader_task.abort();
    // A client that never reads cannot hold shutdown open indefinitely.
    let abort = writer_watch.abort_handle();
    if timeout(Duration::from_secs(2), writer_watch).await.is_err() {
        abort.abort();
    }
    result
}

/// The IPC payload of `event`. A checkpoint chunk and a room message go as a
/// JSON header and a raw body, so their bytes are neither base64 encoded nor
/// escaped into a JSON string; every other event goes as JSON.
pub fn encode_event(event: Event) -> io::Result<Vec<u8>> {
    match event {
        Event::CheckpointChunk {
            epoch,
            room,
            transfer,
            term,
            base_revision,
            revision,
            offset,
            data,
        } => raw_payload(
            &Event::CheckpointChunk {
                epoch,
                room,
                transfer,
                term,
                base_revision,
                revision,
                offset,
                data: wire::IpcBytes::default(),
            },
            "data",
            &data.0,
        ),
        Event::Message {
            epoch,
            peer,
            message_id,
            payload,
        } if !payload.is_empty() => raw_payload(
            &Event::Message {
                epoch,
                peer,
                message_id,
                payload: String::new(),
            },
            "payload",
            payload.as_bytes(),
        ),
        event => serde_json::to_vec(&event).map_err(|_| failed("IPC serialize")),
    }
}

/// A command from its IPC payload, JSON or raw-body.
pub fn decode_command(payload: &[u8]) -> io::Result<Command> {
    match wire::split_ipc_payload(payload)? {
        wire::IpcPayload::Json(json) => {
            serde_json::from_slice(json).map_err(|_| failed("invalid IPC command"))
        }
        wire::IpcPayload::Raw { header, body } => decode_raw(header, body, |command, bytes| {
            let Command::CheckpointChunk { data, .. } = command else {
                return false;
            };
            data.0 = bytes;
            true
        }),
    }
}

/// `encode_event` for commands, for whatever stands in for the game.
pub fn encode_command(command: Command) -> io::Result<Vec<u8>> {
    match command {
        Command::CheckpointChunk {
            epoch,
            room,
            transfer,
            term,
            base_revision,
            revision,
            offset,
            data,
        } => raw_payload(
            &Command::CheckpointChunk {
                epoch,
                room,
                transfer,
                term,
                base_revision,
                revision,
                offset,
                data: wire::IpcBytes::default(),
            },
            "data",
            &data.0,
        ),
        Command::Send {
            epoch,
            peer,
            message_id,
            payload,
            control,
        } if !payload.is_empty() => raw_payload(
            &Command::Send {
                epoch,
                peer,
                message_id,
                payload: String::new(),
                control,
            },
            "payload",
            payload.as_bytes(),
        ),
        command => serde_json::to_vec(&command).map_err(|_| failed("IPC serialize")),
    }
}

/// `decode_command` for events, for whatever stands in for the game.
pub fn decode_event(payload: &[u8]) -> io::Result<Event> {
    match wire::split_ipc_payload(payload)? {
        wire::IpcPayload::Json(json) => {
            serde_json::from_slice(json).map_err(|_| failed("invalid IPC event"))
        }
        wire::IpcPayload::Raw { header, body } => decode_raw(header, body, |event, bytes| {
            let Event::CheckpointChunk { data, .. } = event else {
                return false;
            };
            data.0 = bytes;
            true
        }),
    }
}

/// `message` serialized without its `field`, which goes as the raw body.
fn raw_payload<T: Serialize>(message: &T, field: &str, body: &[u8]) -> io::Result<Vec<u8>> {
    let mut header = serde_json::to_value(message).map_err(|_| failed("IPC serialize"))?;
    if header
        .as_object_mut()
        .and_then(|fields| fields.remove(field))
        .is_none()
    {
        return Err(failed("IPC serialize"));
    }
    let header = serde_json::to_vec(&header).map_err(|_| failed("IPC serialize"))?;
    wire::raw_ipc_payload(&header, body)
}

/// A raw-body message with its body put back. A text body (`payload`) is
/// restored as the string field it replaces; a byte body (`data`) is parsed
/// as an empty placeholder that `restore` then fills. A header that already
/// has the body's field, or names a message without a raw form, is refused.
fn decode_raw<T: serde::de::DeserializeOwned>(
    header: &[u8],
    body: &[u8],
    restore: impl FnOnce(&mut T, Vec<u8>) -> bool,
) -> io::Result<T> {
    let mut fields: serde_json::Map<String, serde_json::Value> =
        serde_json::from_slice(header).map_err(|_| failed("invalid raw IPC header"))?;
    let field = match fields.get("type").and_then(serde_json::Value::as_str) {
        Some("checkpoint_chunk") => "data",
        Some("message" | "send") => "payload",
        _ => return Err(failed("raw body on an IPC message without one")),
    };
    if fields.contains_key(field) {
        return Err(failed("raw IPC header repeats its body"));
    }
    let text = if field == "payload" {
        String::from_utf8(body.to_vec()).map_err(|_| failed("raw IPC payload is not text"))?
    } else {
        String::new()
    };
    fields.insert(field.into(), serde_json::Value::String(text));
    let mut message = serde_json::from_value(serde_json::Value::Object(fields))
        .map_err(|_| failed("invalid raw IPC message"))?;
    if field == "data" && !restore(&mut message, body.to_vec()) {
        return Err(failed("invalid raw IPC message"));
    }
    Ok(message)
}

// Dropping a cancelled service future must not detach pipe reader/writer tasks.
pub(super) struct TaskScope(pub(super) Vec<AbortHandle>);
impl Drop for TaskScope {
    fn drop(&mut self) {
        for task in &self.0 {
            task.abort();
        }
    }
}
