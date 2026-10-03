//! The outer handshake deadline of a join, through the production connect
//! paths: a host that takes the connection and never answers. A private join
//! ends as it always did, with no reason; a public one says it timed out.
use std::future::Future;

use ember_protocol::rooms::SignedRoomTicket;

use super::public_support::{BUILD, identity, ticket_for};
use super::*;
use crate::public_room::{self, PublicRoom};

/// Runs `dial` against `host`, which accepts the connection and says nothing
/// until `dial` has given up.
async fn against_silent_host<T>(host: &Endpoint, dial: impl Future<Output = T>) -> T {
    let (done, finished) = tokio::sync::oneshot::channel::<()>();
    let (outcome, _connection) = timeout(Duration::from_secs(60), async {
        tokio::join!(
            async {
                let outcome = dial.await;
                let _ = done.send(());
                outcome
            },
            async {
                let connection = host.accept().await.unwrap().await.unwrap();
                let _ = finished.await;
                connection
            }
        )
    })
    .await
    .unwrap();
    outcome
}

/// The `join_failed` event an actor serializes for a join that ended in
/// `error`; `member` makes it a public room's join.
async fn join_failed_event(
    local: &Endpoint,
    member: Option<SignedRoomTicket>,
    error: io::Error,
) -> String {
    let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut actor = test_actor(local.clone(), events);
    actor.epoch = 1;
    actor.opening = true;
    actor.public = member.map(|ticket| PublicRoom::Member { ticket });
    actor.completed_control(1, Err(error), None).await.unwrap();
    serde_json::to_string(&next_error(&mut receiver).await).unwrap()
}

#[tokio::test]
async fn a_private_host_that_never_answers_ends_the_join_with_no_reason() {
    let (host, guest) = (endpoint().await, endpoint().await);
    let invite =
        Invite::create(host.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
    let error = against_silent_host(
        &host,
        transport::connect_control_at(&guest, address(&host), &invite, &invite.proof(), false),
    )
    .await
    .err()
    .expect("the host never answered");
    assert!(!transport::is_handshake_timeout(&error), "saw '{error}'");
    assert!(!transport::is_host_unreachable(&error));
    // The event a private join has always ended with: no reason.
    assert_eq!(
        join_failed_event(&guest, None, error).await,
        r#"{"type":"error","request_id":0,"epoch":1,"code":"join_failed"}"#
    );
    host.close().await;
    guest.close().await;
}

#[tokio::test]
async fn a_public_host_that_never_answers_ends_the_join_as_a_timeout() {
    let (host, guest) = (endpoint().await, endpoint().await);
    let invite =
        Invite::create(host.id(), test_relay(), BUILD.into(), now().unwrap(), 3600).unwrap();
    let ticket = ticket_for(&invite, &identity(1), &guest);
    let error = against_silent_host(
        &host,
        public_room::connect_public_at(&guest, address(&host), &invite, &ticket),
    )
    .await
    .err()
    .expect("the host never answered");
    assert!(transport::is_handshake_timeout(&error), "saw '{error}'");
    assert_eq!(
        join_failed_event(&guest, Some(ticket), error).await,
        r#"{"type":"error","request_id":0,"epoch":1,"code":"join_failed","reason":"timeout"}"#
    );
    host.close().await;
    guest.close().await;
}
