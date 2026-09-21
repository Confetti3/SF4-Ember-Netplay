//! Process entry: pipe reader and writer tasks around one `Actor`.
use super::*;

pub async fn run<S: AsyncRead + AsyncWrite + Unpin + Send + 'static>(
    stream: S,
    endpoint: Endpoint,
    relay_only: bool,
) -> io::Result<()> {
    let (mut reader, mut writer) = tokio::io::split(stream);
    let (commands, receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (events, mut outbound) = mpsc::channel::<Event>(IPC_QUEUE_CAPACITY);
    let (failed_ipc, failure) = watch::channel(false);
    let writer_failed = failed_ipc.clone();
    let reader_task = tokio::spawn(async move {
        let mut last_id = 1;
        while let Ok(frame) = wire::read_ipc(&mut reader).await {
            if frame.message_id <= last_id {
                break;
            }
            last_id = frame.message_id;
            let Ok(command) = serde_json::from_slice(&frame.payload) else {
                break;
            };
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
            let payload = serde_json::to_vec(&event).map_err(|_| failed("IPC serialize"))?;
            wire::write_ipc(
                &mut writer,
                &ControlFrame {
                    message_id,
                    payload,
                },
            )
            .await?;
            if matches!(event, Event::Stopped) {
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
        games: BTreeMap::new(),
        closed_generation: 0,
        tasks: JoinSet::new(),
        events,
        recovery: None,
        admissions: BTreeMap::new(),
        admission_order: Vec::new(),
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
        deferred_admissions: VecDeque::new(),
        pending_membership_publications: BTreeSet::new(),
        last_coordination_state: None,
        coordination_writable: false,
        last_control_rebound: None,
        unwritable_leader_since: None,
        reconnect_target: None,
        probe_reservations: BTreeMap::new(),
        pending_game_admissions: BTreeMap::new(),
        pending_probe_invalidations: BTreeMap::new(),
        probe_peers: BTreeSet::new(),
        probe_permissions: BTreeMap::new(),
        pending_probe_authorizations: BTreeMap::new(),
        retirement_started: None,
    };
    let result = actor.run(receiver, failure).await;
    actor.clear_room();
    actor.tasks.shutdown().await;
    endpoint.close().await;
    if result.is_ok() {
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

// Dropping a cancelled service future must not detach pipe reader/writer tasks.
pub(super) struct TaskScope(pub(super) Vec<AbortHandle>);
impl Drop for TaskScope {
    fn drop(&mut self) {
        for task in &self.0 {
            task.abort();
        }
    }
}
