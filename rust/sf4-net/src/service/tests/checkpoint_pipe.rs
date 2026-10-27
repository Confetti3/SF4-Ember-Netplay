//! What a committed checkpoint costs on the helper-to-game pipe: the leader's
//! own commit as a `checkpoint_committed_local` marker with its mandatory
//! fallback to the full transfer, and checkpoint chunks and room messages as
//! raw-body IPC frames rather than base64 or escaped text inside JSON.
use super::*;
use crate::wire::{IpcBytes, IpcPayload};

const FRAME_HEADER: usize = 14;

fn room_actor(endpoint: Endpoint, capacity: usize) -> (Actor, mpsc::Receiver<Event>, [u8; 16]) {
    let (events, receiver) = mpsc::channel(capacity);
    let mut actor = test_actor(endpoint, events);
    let room = [46; 16];
    actor.epoch = 1;
    actor.room = Some(room);
    (actor, receiver, room)
}

/// Plays the native receiver for one full export: every chunk is acknowledged
/// as it arrives and the end at its length. Returns the events, in order,
/// through the committed marker.
async fn drain_full_export(
    actor: &mut Actor,
    receiver: &mut mpsc::Receiver<Event>,
    room: [u8; 16],
) -> Vec<Event> {
    let mut seen = Vec::new();
    loop {
        let event = timeout(Duration::from_secs(5), receiver.recv())
            .await
            .expect("export stalled")
            .unwrap();
        let ack = match &event {
            Event::CheckpointChunk {
                transfer,
                offset,
                data,
                ..
            } => Some((*transfer, offset + data.0.len() as u32)),
            Event::CheckpointEnd {
                transfer, length, ..
            } => Some((*transfer, *length)),
            _ => None,
        };
        let done = matches!(event, Event::CheckpointCommitted { .. });
        seen.push(event);
        if let Some((transfer, offset)) = ack {
            actor.checkpoint_ack(1, room, transfer, offset).unwrap();
        }
        if done {
            return seen;
        }
    }
}

/// Bytes each event takes on the pipe today (raw bodies) and as JSON with
/// base64 chunks, which is how every event went before.
fn pipe_bytes(events: &[Event]) -> (usize, usize) {
    events.iter().fold((0, 0), |(raw, json), event| {
        let before = serde_json::to_vec(event).unwrap().len();
        let after = encode_event(clone_event(event)).unwrap().len();
        (raw + FRAME_HEADER + after, json + FRAME_HEADER + before)
    })
}

fn clone_event(event: &Event) -> Event {
    serde_json::from_slice(&serde_json::to_vec(event).unwrap()).unwrap()
}

/// A checkpoint-like JSON text of about `size` bytes.
fn checkpoint_text(size: usize) -> Vec<u8> {
    let mut text = String::from(r#"{"members":["#);
    let mut member = 0u64;
    while text.len() + 200 < size {
        if member > 0 {
            text.push(',');
        }
        text.push_str(&format!(
            r#"{{"member":{member},"data":{{"name":"Player {member}","authenticatedEndpoint":"{:064x}","note":"quoted \"text\" and \\ slashes"}}}}"#,
            member * 7919
        ));
        member += 1;
    }
    text.push_str("]}");
    text.into_bytes()
}

#[tokio::test]
async fn own_commit_goes_as_one_marker_answered_at_full_length() {
    let endpoint = endpoint().await;
    let (mut actor, mut receiver, room) = room_actor(endpoint.clone(), IPC_QUEUE_CAPACITY);
    let bytes = checkpoint_text(100 * 1024);
    let transfer = CheckpointTransfer::new(room, 31, 4, 9, 10, bytes.clone()).unwrap();

    actor.export_checkpoint(transfer.clone(), true);

    let Some(Event::CheckpointCommittedLocal {
        epoch,
        room: marked_room,
        transfer: id,
        term,
        base_revision,
        revision,
        length,
        digest,
    }) = receiver.recv().await
    else {
        panic!("the leader's own commit must go as a local marker");
    };
    assert_eq!(
        (epoch, marked_room, id, term, base_revision, revision),
        (1, room, 31, 4, 9, 10)
    );
    assert_eq!(length as usize, bytes.len());
    assert_eq!(digest, transfer.digest_hex());
    // Nothing else crosses the pipe for this commit, and the revision is not
    // exported until the native side answers.
    assert!(receiver.try_recv().is_err());
    assert_eq!(actor.last_exported_revision, 0);
    actor.pump_outgoing_checkpoint();
    assert!(receiver.try_recv().is_err(), "the marker goes once");
    // Only the full length or 0 is an answer.
    actor.checkpoint_ack(1, room, 31, 16 * 1024).unwrap();
    assert!(matches!(
        receiver.recv().await,
        Some(Event::Error { code, .. }) if code == "invalid_checkpoint_ack"
    ));
    actor.checkpoint_ack(1, room, 31, length).unwrap();
    assert!(actor.outgoing_transfer.is_none());
    assert_eq!(actor.last_exported_revision, 10);
    assert!(receiver.try_recv().is_err());

    let marker_bytes = FRAME_HEADER
        + encode_event(Event::CheckpointCommittedLocal {
            epoch: 1,
            room,
            transfer: 31,
            term: 4,
            base_revision: 9,
            revision: 10,
            length,
            digest: transfer.digest_hex(),
        })
        .unwrap()
        .len();
    let (mut full, mut full_receiver, _) = room_actor(endpoint.clone(), IPC_QUEUE_CAPACITY);
    full.export_checkpoint(transfer, false);
    let (raw, base64) = pipe_bytes(&drain_full_export(&mut full, &mut full_receiver, room).await);
    println!(
        "leader's own 100 KiB commit on the pipe: {marker_bytes} bytes as a marker, \
         {raw} as a raw transfer, {base64} as the base64 transfer"
    );
    assert!(marker_bytes < 400);
    endpoint.close().await;
}

#[tokio::test]
async fn a_zero_answer_sends_the_whole_transfer_after_the_marker() {
    let endpoint = endpoint().await;
    let (mut actor, mut receiver, room) = room_actor(endpoint.clone(), IPC_QUEUE_CAPACITY);
    let bytes = checkpoint_text(40 * 1024);
    let transfer = CheckpointTransfer::new(room, 32, 4, 10, 11, bytes.clone()).unwrap();
    actor.export_checkpoint(transfer, true);
    let Some(Event::CheckpointCommittedLocal { length, .. }) = receiver.recv().await else {
        panic!("marker first");
    };
    // The native side no longer holds the proposal (a term change, a
    // checkpoint-scope error or its own proposal timeout cleared it).
    actor.checkpoint_ack(1, room, 32, 0).unwrap();
    let events = drain_full_export(&mut actor, &mut receiver, room).await;
    assert!(matches!(
        events.first(),
        Some(Event::CheckpointBegin { transfer: 32, length: begun, .. }) if *begun == length
    ));
    let mut body = Vec::new();
    for event in &events {
        if let Event::CheckpointChunk { offset, data, .. } = event {
            assert_eq!(*offset as usize, body.len());
            body.extend_from_slice(&data.0);
        }
    }
    assert_eq!(body, bytes);
    assert!(matches!(
        events.last(),
        Some(Event::CheckpointCommitted {
            transfer: 32,
            revision: 11,
            ..
        })
    ));
    assert_eq!(actor.last_exported_revision, 11);
    endpoint.close().await;
}

#[tokio::test]
async fn an_unanswered_marker_times_out_like_a_transfer() {
    let endpoint = endpoint().await;
    let (mut actor, mut receiver, room) = room_actor(endpoint.clone(), IPC_QUEUE_CAPACITY);
    actor.last_exported_revision = 11;
    let transfer = CheckpointTransfer::new(room, 33, 4, 11, 12, b"{}".to_vec()).unwrap();
    actor.export_checkpoint(transfer, true);
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointCommittedLocal { transfer: 33, .. })
    ));
    // An old game never gets here: it fails the id-1 authentication against
    // this helper's versioned answer. This game leaves a marker unanswered
    // when it is Closing, Idle or Failed, or the marker names an epoch or
    // room it is not in. The slot is then freed by the transfer timeout,
    // which reports it and leaves the revision to the pump, which exports it
    // in full.
    actor.outgoing_transfer.as_mut().unwrap().started =
        tokio::time::Instant::now() - CHECKPOINT_TRANSFER_TIMEOUT - Duration::from_millis(1);
    actor.expire_checkpoint_transfers();
    assert!(actor.outgoing_transfer.is_none());
    assert!(matches!(
        receiver.recv().await,
        Some(Event::Error { code, .. }) if code == "checkpoint_send_timeout"
    ));
    assert_eq!(actor.last_exported_revision, 11);
    endpoint.close().await;
}

#[tokio::test]
async fn the_marker_waits_behind_the_previous_committed_marker() {
    let endpoint = endpoint().await;
    let (mut actor, mut receiver, room) = room_actor(endpoint.clone(), LIFECYCLE_EVENT_RESERVE + 1);
    assert!(actor.emit_bulk(Event::CheckpointAck {
        epoch: 1,
        room,
        transfer: 16,
        offset: 4,
    }));
    actor.pending_checkpoint_committed = Some(PendingCheckpointCommitted {
        epoch: 1,
        room,
        transfer: 17,
        term: 3,
        base_revision: 7,
        revision: 8,
        length: 4,
        digest: recovery::hex_digest(&recovery::sha256(b"old")),
    });
    let own = CheckpointTransfer::new(room, 18, 3, 8, 9, b"next".to_vec()).unwrap();
    actor.export_checkpoint(own, true);
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointAck { transfer: 16, .. })
    ));
    actor.pump_outgoing_checkpoint();
    assert!(receiver.try_recv().is_err(), "the marker takes the transfer's slot");
    actor.pump_pending_checkpoint_committed();
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointCommitted { transfer: 17, .. })
    ));
    // An answer before the marker went out is not one.
    actor.checkpoint_ack(1, room, 18, 4).unwrap();
    assert!(matches!(
        receiver.recv().await,
        Some(Event::Error { code, .. }) if code == "invalid_checkpoint_ack"
    ));
    actor.pump_outgoing_checkpoint();
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointCommittedLocal {
            transfer: 18,
            revision: 9,
            ..
        })
    ));
    endpoint.close().await;
}

#[tokio::test]
async fn chunks_cross_the_pipe_as_raw_bodies() {
    let endpoint = endpoint().await;
    for size in [60 * 1024, 460 * 1024] {
        let (mut actor, mut receiver, room) = room_actor(endpoint.clone(), IPC_QUEUE_CAPACITY);
        let bytes = checkpoint_text(size);
        let transfer = CheckpointTransfer::new(room, 34, 4, 1, 2, bytes.clone()).unwrap();
        actor.export_checkpoint(transfer, false);
        let events = drain_full_export(&mut actor, &mut receiver, room).await;
        let mut body = Vec::new();
        let mut chunks = 0;
        let (mut chunk_raw, mut chunk_base64) = (0, 0);
        for event in &events {
            let payload = encode_event(clone_event(event)).unwrap();
            if let Event::CheckpointChunk { data, .. } = event {
                chunks += 1;
                chunk_raw += FRAME_HEADER + payload.len();
                chunk_base64 += FRAME_HEADER + serde_json::to_vec(event).unwrap().len();
                let IpcPayload::Raw { header, body: raw } =
                    crate::wire::split_ipc_payload(&payload).unwrap()
                else {
                    panic!("a chunk goes as a raw body");
                };
                assert!(!header.windows(6).any(|w| w == b"\"data\""));
                assert_eq!(raw, &data.0[..]);
                body.extend_from_slice(raw);
                // The game reads it back to the same event.
                assert_eq!(
                    serde_json::to_vec(&decode_event(&payload).unwrap()).unwrap(),
                    serde_json::to_vec(event).unwrap()
                );
            } else {
                assert_eq!(payload[0], b'{', "other events stay JSON");
            }
        }
        assert_eq!(body, bytes);
        let (all_raw, all_base64) = pipe_bytes(&events);
        println!(
            "{} KiB commit, {chunks} chunks: chunk frames {chunk_raw} bytes raw vs {chunk_base64} base64 \
             (saves {}); whole export {all_raw} vs {all_base64} (saves {})",
            bytes.len() / 1024,
            chunk_base64 - chunk_raw,
            all_base64 - all_raw
        );
        assert!(chunk_raw * 4 < chunk_base64 * 3 + chunks * 4 * 300);
    }
    endpoint.close().await;
}

#[test]
fn commands_and_messages_round_trip_through_both_payload_kinds() {
    let room = [47; 16];
    let peer = iroh::SecretKey::generate().public();
    let chunk = || Command::CheckpointChunk {
        epoch: 1,
        room,
        transfer: 5,
        term: 2,
        base_revision: 3,
        revision: 4,
        offset: 16384,
        data: IpcBytes(vec![0, 1, 255, b'"', b'\\']),
    };
    let raw = encode_command(chunk()).unwrap();
    assert_eq!(raw[0], crate::wire::IPC_RAW_BODY);
    let json = serde_json::to_vec(&chunk()).unwrap();
    for payload in [&raw, &json] {
        let Command::CheckpointChunk { offset, data, .. } = decode_command(payload).unwrap()
        else {
            panic!("a chunk decodes as a chunk");
        };
        assert_eq!(offset, 16384);
        assert_eq!(data.0, vec![0, 1, 255, b'"', b'\\']);
    }
    let text = r#"{"type":"hello","name":"Player 1 \"quoted\"","n":[1,2]}"#;
    let send = || Command::Send {
        epoch: 1,
        peer,
        message_id: 9,
        payload: text.into(),
        control: 3,
    };
    let raw = encode_command(send()).unwrap();
    let IpcPayload::Raw { body, .. } = crate::wire::split_ipc_payload(&raw).unwrap() else {
        panic!("a send goes as a raw body");
    };
    assert_eq!(body, text.as_bytes());
    for payload in [raw, serde_json::to_vec(&send()).unwrap()] {
        let Command::Send {
            payload, control, ..
        } = decode_command(&payload).unwrap()
        else {
            panic!("a send decodes as a send");
        };
        assert_eq!((payload.as_str(), control), (text, 3));
    }
    let message = encode_event(Event::Message {
        epoch: 1,
        peer,
        message_id: 9,
        payload: text.into(),
    })
    .unwrap();
    let Event::Message { payload, .. } = decode_event(&message).unwrap() else {
        panic!("a message decodes as a message");
    };
    assert_eq!(payload, text);
    // An empty payload has no raw form and stays JSON.
    assert_eq!(
        encode_event(Event::Message {
            epoch: 1,
            peer,
            message_id: 10,
            payload: String::new(),
        })
        .unwrap()[0],
        b'{'
    );
}

#[test]
fn malformed_raw_commands_are_refused() {
    let raw = |header: &str, body: &[u8]| {
        crate::wire::raw_ipc_payload(header.as_bytes(), body).unwrap()
    };
    let chunk = r#"{"type":"checkpoint_chunk","epoch":1,"room":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],"transfer":1,"term":1,"base_revision":0,"revision":1,"offset":0}"#;
    assert!(decode_command(&raw(chunk, b"x")).is_ok());
    for payload in [
        // The body's field may not also be in the header.
        raw(&chunk.replace("\"offset\":0", "\"offset\":0,\"data\":\"\""), b"x"),
        // Only chunks and sends have a raw form.
        raw(r#"{"type":"status"}"#, b"x"),
        raw(r#"{"type":"shutdown"}"#, b"x"),
        raw(r#"{"epoch":1}"#, b"x"),
        // A room payload is text.
        raw(
            r#"{"type":"send","epoch":1,"peer":"x","message_id":2}"#,
            &[0xff, 0xfe],
        ),
        // Unknown header fields are refused as in JSON.
        raw(&chunk.replace("\"offset\":0", "\"offset\":0,\"extra\":1"), b"x"),
    ] {
        assert!(decode_command(&payload).is_err());
    }
}
