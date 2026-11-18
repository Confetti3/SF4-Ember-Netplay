//! Checkpoint import: the begin, chunk and end commands that bring a native
//! checkpoint into the helper.

use super::*;
use crate::wire::IpcBytes;

const ROOM: [u8; 16] = [47; 16];

/// An actor in epoch 1 of `ROOM`, with no coordination session, and a
/// one-chunk checkpoint for it to import.
async fn importer() -> (Endpoint, Actor, mpsc::Receiver<Event>, CheckpointTransfer) {
    let endpoint = endpoint().await;
    let (events, receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(endpoint.clone(), events);
    actor.epoch = 1;
    actor.room = Some(ROOM);
    let transfer = CheckpointTransfer::new(ROOM, 5, 2, 3, 4, vec![7; 100]).unwrap();
    (endpoint, actor, receiver, transfer)
}

async fn begin(actor: &mut Actor, transfer: &CheckpointTransfer, digest: String) {
    actor
        .checkpoint_begin(
            1,
            ROOM,
            transfer.transfer,
            transfer.term,
            transfer.base_revision,
            transfer.revision,
            transfer.bytes.len() as u32,
            digest,
        )
        .await
        .unwrap();
}

async fn chunk(actor: &mut Actor, transfer: &CheckpointTransfer, epoch: u64) {
    actor
        .checkpoint_chunk(
            epoch,
            ROOM,
            transfer.transfer,
            transfer.term,
            transfer.base_revision,
            transfer.revision,
            0,
            IpcBytes(transfer.bytes.to_vec()),
        )
        .await
        .unwrap();
}

#[tokio::test]
async fn an_uppercase_digest_reads_the_same_at_begin_and_end() {
    let (endpoint, mut actor, mut receiver, transfer) = importer().await;
    let digest = transfer.digest_hex().to_ascii_uppercase();
    begin(&mut actor, &transfer, digest.clone()).await;
    chunk(&mut actor, &transfer, 1).await;
    assert!(matches!(
        receiver.recv().await,
        Some(Event::CheckpointAck { offset: 100, .. })
    ));

    actor
        .checkpoint_end(
            1,
            ROOM,
            transfer.transfer,
            transfer.term,
            transfer.base_revision,
            transfer.revision,
            transfer.bytes.len() as u32,
            digest,
        )
        .await
        .unwrap();
    // The end matched what began; with no coordination session the body
    // then cannot be proposed.
    assert!(matches!(
        receiver.recv().await,
        Some(Event::Error { code, .. }) if code == "coordination_unavailable"
    ));
    endpoint.close().await;
}
