//! Short links shared by a room.

use super::*;

/// A link service on loopback, the same code as the one deployed.
async fn short_service() -> (String, ember_short::AppState) {
    let state =
        ember_short::AppState::new(ember_short::Limits::default(), ember_short::system_clock());
    let listener = tokio::net::TcpListener::bind((Ipv4Addr::LOCALHOST, 0))
        .await
        .unwrap();
    let port = listener.local_addr().unwrap().port();
    let served = state.clone();
    tokio::spawn(async move {
        let _ = ember_short::serve_until(listener, served, std::future::pending()).await;
    });
    (format!("http://127.0.0.1:{port}/s/v1/"), state)
}

/// Runs finished tasks through `completed` until a short-link task has
/// been handled. Tasks a cleared room aborted are skipped.
async fn complete_short(actor: &mut Actor) {
    loop {
        let finished = timeout(Duration::from_secs(20), actor.tasks.join_next())
            .await
            .expect("a task finishes")
            .expect("a task was running");
        let completion = match finished {
            Ok(completion) => completion,
            Err(error) if error.is_cancelled() => continue,
            Err(error) => panic!("task failed: {error}"),
        };
        let short = matches!(
            completion,
            Completion::ShortPublished(..)
                | Completion::ShortResolved(..)
                | Completion::ShortAdopted(..)
        );
        actor.completed(completion).await.unwrap();
        if short {
            return;
        }
    }
}

fn short_join(epoch: u64, invitation: &str, build: &str) -> Request {
    Request {
        id: 40 + epoch,
        command: Command::Join {
            epoch,
            invitation: invitation.into(),
            build: build.into(),
            observer: false,
        },
    }
}

async fn joined_reason(
    guest: &mut Actor,
    events: &mut mpsc::Receiver<Event>,
    epoch: u64,
    text: &str,
    build: &str,
) -> Option<String> {
    assert!(
        guest
            .command_now(short_join(epoch, text, build))
            .await
            .unwrap()
    );
    complete_short(guest).await;
    match timeout(Duration::from_secs(5), events.recv())
        .await
        .unwrap()
        .unwrap()
    {
        Event::Error {
            epoch: refused,
            ref code,
            reason,
            ..
        } => {
            assert_eq!(refused, epoch);
            assert_eq!(code, "invalid_or_incompatible_invitation");
            assert_eq!(guest.room, None);
            assert!(!guest.opening);
            reason
        }
        _ => panic!("expected a refused join"),
    }
}

#[test]
fn short_invite_ipc_round_trips() {
    let command: Command = serde_json::from_str(r#"{"type":"short_invite","epoch":3}"#).unwrap();
    assert!(matches!(command, Command::ShortInvite { epoch: 3 }));
    assert!(
        serde_json::from_str::<Command>(r#"{"type":"short_invite","epoch":3,"extra":1}"#).is_err()
    );
    let event = serde_json::to_value(Event::ShortInvite {
        epoch: 3,
        link: "https://embernetplay.link/j#0000-0000-0000".into(),
        status: "ready".into(),
    })
    .unwrap();
    assert_eq!(event["type"], "short_invite");
    assert_eq!(event["status"], "ready");
}

#[tokio::test]
async fn a_short_link_opens_the_room_it_was_made_for() {
    let (service, store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    host.epoch = 4;
    let start = now().unwrap();
    let invite = Invite::create(
        host_endpoint.id(),
        test_relay(),
        "test-build".into(),
        start,
        INVITE_LIFETIME,
    )
    .unwrap();
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());

    // A stale epoch, or no room, is answered without a link.
    host.short_invite_command(3).unwrap();
    assert!(
        matches!(host_events.recv().await.unwrap(), Event::ShortInvite { epoch: 3, ref link, ref status } if link.is_empty() && status == "unavailable")
    );
    assert!(host.tasks.is_empty());

    host.short_invite_command(4).unwrap();
    complete_short(&mut host).await;
    let Event::ShortInvite {
        epoch,
        link,
        status,
    } = host_events.recv().await.unwrap()
    else {
        panic!("expected the short link");
    };
    assert_eq!((epoch, status.as_str()), (4, "ready"));
    assert!(link.starts_with(crate::short_invite::LINK_PREFIX));
    assert_eq!(link.len(), 42);
    assert_eq!(crate::short_invite::parse(&link), Some(invite.short_code()));
    assert_eq!(store.len(), 1);
    // Asked again, the same link comes back at once.
    host.short_invite_command(4).unwrap();
    assert!(host.tasks.is_empty());
    assert!(
        matches!(host_events.recv().await.unwrap(), Event::ShortInvite { link: ref again, .. } if *again == link)
    );

    // The guest opens it, in any of the forms a player might paste, and
    // goes on to dial the host the record names.
    let code = crate::short_invite::display_code(&invite.short_code());
    for (epoch, pasted) in [
        (1, link.clone()),
        (2, code.to_lowercase()),
        (3, format!(" {code}\r\n")),
    ] {
        assert!(
            guest
                .command_now(short_join(epoch, &pasted, "test-build"))
                .await
                .unwrap()
        );
        assert_eq!(guest.room, None);
        complete_short(&mut guest).await;
        assert_eq!(guest.room, Some(invite.room()));
        assert_eq!(
            guest.host_address.as_ref().map(|address| address.id),
            Some(host_endpoint.id())
        );
        assert!(guest.opening);
        assert!(guest_events.try_recv().is_err());
        guest.clear_room();
    }

    // The joiner still checks its own build against the record.
    assert_eq!(
        joined_reason(&mut guest, &mut guest_events, 5, &link, "other-build")
            .await
            .as_deref(),
        Some("other_build")
    );

    // A renewed invitation is stored again under the same link.
    let renewed_at = start + INVITE_LIFETIME * 3 / 4;
    host.renew_invitation(renewed_at);
    let _ = host_events.recv().await.unwrap();
    host.pump_short_link(renewed_at);
    assert!(!host.tasks.is_empty());
    complete_short(&mut host).await;
    assert!(host_events.try_recv().is_err());
    let keys = crate::short_invite::derive(&invite.short_code()).unwrap();
    let stored = crate::short_invite::fetch(&service, &keys).await.unwrap();
    assert_eq!(stored, host.room_invite.as_ref().unwrap().encode().unwrap());
    // Unchanged and recently stored: nothing to do.
    host.pump_short_link(now().unwrap());
    assert!(host.tasks.is_empty());
    // Leaving the room forgets the link.
    host.clear_room();
    assert!(host.short.room.is_none());
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

#[tokio::test]
async fn a_new_leader_keeps_a_shared_short_link_current() {
    let (service, store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    host.epoch = 4;
    guest.epoch = 7;
    let start = now().unwrap();
    let invite = Invite::create(
        host_endpoint.id(),
        test_relay(),
        "test-build".into(),
        start,
        INVITE_LIFETIME,
    )
    .unwrap();
    // The guest leads the room now: its invitation names its own endpoint.
    let coordination = iroh::SecretKey::generate().public();
    let led = invite
        .clone()
        .with_authority_route(guest_endpoint.id(), coordination, 2, 9)
        .unwrap();
    guest.room_invite = Some(led.clone());

    // Nobody shared a link yet: the new leader finds no record and publishes
    // nothing.
    guest.adopt_short_link();
    complete_short(&mut guest).await;
    assert!(guest.short.room.is_none() && guest.short.adopt_at == 0);
    guest.pump_short_link(now().unwrap());
    assert!(guest.tasks.is_empty());
    assert_eq!(store.len(), 0);

    // The host shares one, then leaves.
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());
    host.short_invite_command(4).unwrap();
    complete_short(&mut host).await;
    assert!(
        matches!(host_events.recv().await.unwrap(), Event::ShortInvite { ref status, .. } if status == "ready")
    );
    host.clear_room();

    // The new leader takes the link over and stores its own invitation under
    // it, without telling the native side anything new.
    guest.adopt_short_link();
    complete_short(&mut guest).await;
    assert!(guest.short.room.is_some());
    guest.pump_short_link(now().unwrap());
    assert!(!guest.tasks.is_empty());
    complete_short(&mut guest).await;
    assert!(guest_events.try_recv().is_err());
    let keys = crate::short_invite::derive(&invite.short_code()).unwrap();
    let stored = crate::short_invite::fetch(&service, &keys).await.unwrap();
    assert_eq!(stored, led.encode().unwrap());
    // Asked for the link, the leader gives the same one at once.
    guest.short_invite_command(7).unwrap();
    assert!(guest.tasks.is_empty());
    assert!(
        matches!(guest_events.recv().await.unwrap(), Event::ShortInvite { ref link, .. } if crate::short_invite::parse(link) == Some(invite.short_code()))
    );
    // Leaving forgets it.
    guest.clear_room();
    assert!(guest.short.room.is_none() && !guest.short.adopting);

    // A lookup the service does not answer is tried again later, a few
    // times at most.
    let mut lone = test_actor(endpoint().await, mpsc::channel(IPC_QUEUE_CAPACITY).0);
    lone.short.service = "http://127.0.0.1:9/s/v1/".into();
    lone.room_invite = Some(led.clone());
    for _ in 0..3 {
        lone.adopt_short_link();
        complete_short(&mut lone).await;
        assert!(lone.short.room.is_none());
    }
    assert_eq!(lone.short.adopt_at, 0);
    lone.adopt_short_link();
    assert!(lone.tasks.is_empty());
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

/// This member's copy of the room's invitation once it leads: the same room
/// and capability, its own route.
fn led_by(invite: &Invite, leader: &Endpoint, term: u64) -> Invite {
    invite
        .clone()
        .with_authority_route(
            leader.id(),
            iroh::SecretKey::generate().public(),
            term,
            term,
        )
        .unwrap()
}

/// Runs the short-link task the pump starts (a lookup that was asked for, or
/// a store of the invitation), if any. Returns whether one ran.
async fn pump_adopt(actor: &mut Actor) -> bool {
    actor.pump_short_link(now().unwrap());
    if actor.tasks.is_empty() {
        return false;
    }
    complete_short(actor).await;
    true
}

/// The link the service holds for the room now.
async fn stored_link(service: &str, invite: &Invite) -> String {
    let keys = crate::short_invite::derive(&invite.short_code()).unwrap();
    crate::short_invite::fetch(service, &keys).await.unwrap()
}

#[test]
fn only_a_change_of_leader_looks_for_a_shared_link() {
    let mut short = ShortLinks::default();
    // The game that opened the room leads it from the start: nothing to take over.
    short.follow_leader(true, 1, false, true, 100);
    assert_eq!(short.adopt_at, 0);
    // Refreshes under the same lead never ask again.
    short.follow_leader(true, 1, false, true, 200);
    assert_eq!(short.adopt_at, 0);
    // An election with no leader yet is not another member leading.
    short.follow_leader(false, 2, false, true, 300);
    short.follow_leader(true, 2, false, true, 400);
    assert_eq!(short.adopt_at, 0);
    // Another member leads, then the lead comes back: look once.
    short.follow_leader(false, 3, true, true, 500);
    assert_eq!(short.adopt_at, 0);
    short.follow_leader(true, 4, false, true, 600);
    assert_eq!(short.adopt_at, 600);
    short.adopt_at = 0;
    short.follow_leader(true, 4, false, true, 700);
    assert_eq!(short.adopt_at, 0);
    // A member that joined looks as soon as it leads.
    let mut joined = ShortLinks::default();
    joined.follow_leader(true, 2, false, false, 0);
    assert_eq!(joined.adopt_at, 1);
    // Leaving forgets all of it, the service excepted.
    joined.service = "http://127.0.0.1:9/s/v1/".into();
    joined.clear();
    assert!(joined.room.is_none() && joined.led_term == 0 && !joined.followed);
    assert_eq!(joined.service, "http://127.0.0.1:9/s/v1/");
}

#[tokio::test]
async fn the_host_keeps_a_link_a_departed_member_shared() {
    let (service, _store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, _host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let (guest_events_tx, mut guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    let invite = Invite::create(
        host_endpoint.id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        INVITE_LIFETIME,
    )
    .unwrap();
    host.epoch = 4;
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());
    leads(&mut host, 1);
    host.short
        .follow_leader(true, 1, false, true, now().unwrap());
    guest.epoch = 7;
    guest.room_invite = Some(invite.clone());

    // A member leaves before anyone shared a link: one lookup finds nothing.
    let history = BTreeSet::new();
    host.apply_confirmed_retirements(BTreeSet::from([21]), &history);
    assert!(pump_adopt(&mut host).await);
    assert!(host.short.room.is_none() && host.short.adopt_at == 0);
    assert!(!pump_adopt(&mut host).await);
    // The same departure confirmed again asks nothing.
    host.apply_confirmed_retirements(BTreeSet::from([21]), &history);
    assert_eq!(host.short.adopt_at, 0);
    // With every worker busy, a lookup waits for one instead of being lost.
    while host.tasks.len() < MAX_TASKS {
        host.tasks.spawn(std::future::pending::<Completion>());
    }
    host.apply_confirmed_retirements(BTreeSet::from([20]), &history);
    host.pump_short_link(now().unwrap());
    assert!(host.tasks.len() == MAX_TASKS && host.short.adopt_at != 0);
    host.tasks.abort_all();
    while host.tasks.join_next().await.is_some() {}
    assert!(pump_adopt(&mut host).await);
    assert_eq!(host.short.adopt_at, 0);

    // The guest shares a link, then leaves while the host leads on.
    guest.short_invite_command(7).unwrap();
    complete_short(&mut guest).await;
    assert!(
        matches!(guest_events.recv().await.unwrap(), Event::ShortInvite { ref status, .. } if status == "ready")
    );
    guest.clear_room();
    host.apply_confirmed_retirements(BTreeSet::from([22]), &history);
    assert!(pump_adopt(&mut host).await);
    assert!(host.short.room.is_some());
    // The host stores its own invitation under the guest's link from now on.
    host.renew_invitation(now().unwrap() + INVITE_LIFETIME / 2);
    assert!(pump_adopt(&mut host).await);
    let renewed = host.room_invite.clone().unwrap();
    assert_ne!(renewed.encode().unwrap(), invite.encode().unwrap());
    assert_eq!(
        stored_link(&service, &invite).await,
        renewed.encode().unwrap()
    );
    // Holding the link, the host looks nothing up for later departures.
    host.apply_confirmed_retirements(BTreeSet::from([23]), &history);
    assert_eq!(host.short.adopt_at, 0);
    // A member that does not lead never looks.
    let mut follower = test_actor(endpoint().await, mpsc::channel(IPC_QUEUE_CAPACITY).0);
    follower.short.service = service.clone();
    follower.room_invite = Some(invite.clone());
    follower.apply_confirmed_retirements(BTreeSet::from([24]), &history);
    assert!(!pump_adopt(&mut follower).await);
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

#[tokio::test]
async fn the_host_takes_a_shared_link_back_with_the_lead() {
    let (service, _store) = short_service().await;
    let host_endpoint = endpoint().await;
    let guest_endpoint = endpoint().await;
    let (host_events_tx, _host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let (guest_events_tx, _guest_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(host_endpoint.clone(), host_events_tx);
    let mut guest = test_actor(guest_endpoint.clone(), guest_events_tx);
    host.short.service = service.clone();
    guest.short.service = service.clone();
    let invite = Invite::create(
        host_endpoint.id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        INVITE_LIFETIME,
    )
    .unwrap();
    host.hosted = Some(invite.clone());
    host.room_invite = Some(invite.clone());
    host.short
        .follow_leader(true, 1, false, true, now().unwrap());

    // The guest leads for a while and shares a link under its own route.
    host.short
        .follow_leader(false, 2, true, true, now().unwrap());
    let guest_route = led_by(&invite, &guest_endpoint, 2);
    guest.epoch = 7;
    guest.room_invite = Some(guest_route.clone());
    guest.short_invite_command(7).unwrap();
    complete_short(&mut guest).await;
    assert_eq!(
        stored_link(&service, &invite).await,
        guest_route.encode().unwrap()
    );
    guest.clear_room();

    // The lead comes back to the host, which takes the link over.
    let host_route = led_by(&invite, &host_endpoint, 3);
    host.room_invite = Some(host_route.clone());
    leads(&mut host, 3);
    host.short
        .follow_leader(true, 3, false, true, now().unwrap());
    assert!(pump_adopt(&mut host).await);
    assert!(host.short.room.is_some());
    assert!(pump_adopt(&mut host).await);
    assert_eq!(
        stored_link(&service, &invite).await,
        host_route.encode().unwrap()
    );
    host_endpoint.close().await;
    guest_endpoint.close().await;
}

#[tokio::test]
async fn a_shared_link_follows_the_lead_through_every_handoff() {
    let (service, _store) = short_service().await;
    let endpoints = [
        endpoint().await,
        endpoint().await,
        endpoint().await,
        endpoint().await,
    ];
    let (mut members, mut _events) = (Vec::new(), Vec::new());
    for member in &endpoints {
        let (events_tx, events) = mpsc::channel(IPC_QUEUE_CAPACITY);
        _events.push(events);
        let mut actor = test_actor(member.clone(), events_tx);
        actor.short.service = service.clone();
        actor.epoch = 5;
        members.push(actor);
    }
    let invite = Invite::create(
        endpoints[0].id(),
        test_relay(),
        "test-build".into(),
        now().unwrap(),
        INVITE_LIFETIME,
    )
    .unwrap();
    members[0].hosted = Some(invite.clone());
    for member in &mut members {
        member.room_invite = Some(invite.clone());
    }
    // The host leads and a member that joined shares the link, then the host
    // leaves.
    members[0]
        .short
        .follow_leader(true, 1, false, true, now().unwrap());
    members[1].short_invite_command(5).unwrap();
    complete_short(&mut members[1]).await;
    members[0].clear_room();
    // The lead passes 1, 2, 3, each leaving after its term: every new leader
    // holds the link and stores its own route under it.
    for (term, index) in [(2u64, 1usize), (3, 2), (4, 3)] {
        for (other, member) in members.iter_mut().enumerate().skip(1) {
            if other != index {
                member
                    .short
                    .follow_leader(false, term, true, false, now().unwrap());
            }
        }
        let route = led_by(&invite, &endpoints[index], term);
        let leader = &mut members[index];
        leader.room_invite = Some(route.clone());
        leads(leader, term);
        leader
            .short
            .follow_leader(true, term, false, false, now().unwrap());
        // The member that published already holds the link; the others take it over.
        while pump_adopt(leader).await {}
        assert!(leader.short.room.is_some());
        assert_eq!(
            stored_link(&service, &invite).await,
            route.encode().unwrap()
        );
        leader.clear_room();
    }
    for member in endpoints {
        member.close().await;
    }
}

#[tokio::test]
async fn a_short_link_that_cannot_be_opened_says_why() {
    let (service, _store) = short_service().await;
    let own = endpoint().await;
    let (events_tx, mut events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut guest = test_actor(own.clone(), events_tx);
    guest.short.service = service.clone();
    let host_id = iroh::SecretKey::generate().public();
    let start = now().unwrap();

    // Nothing stored under this code: mistyped, or its room has closed.
    let missing = Invite::create(host_id, test_relay(), "test-build".into(), start, 3600).unwrap();
    let missing_link = crate::short_invite::link(&missing.short_code());
    assert_eq!(
        joined_reason(&mut guest, &mut events, 1, &missing_link, "test-build")
            .await
            .as_deref(),
        Some("short_unknown")
    );

    // An invitation that has expired since it was stored.
    let expired = Invite::create(
        host_id,
        test_relay(),
        "test-build".into(),
        start - 4000,
        3600,
    )
    .unwrap();
    let keys = crate::short_invite::derive(&expired.short_code()).unwrap();
    crate::short_invite::publish(&service, &keys, &expired.encode().unwrap(), 600)
        .await
        .unwrap();
    let expired_link = crate::short_invite::link(&expired.short_code());
    assert_eq!(
        joined_reason(&mut guest, &mut events, 2, &expired_link, "test-build")
            .await
            .as_deref(),
        Some("expired")
    );

    // A record whose invitation belongs to another room is refused, even
    // though it opens with the code.
    let other = Invite::create(host_id, test_relay(), "test-build".into(), start, 3600).unwrap();
    let wanted = Invite::create(host_id, test_relay(), "test-build".into(), start, 3600).unwrap();
    let keys = crate::short_invite::derive(&wanted.short_code()).unwrap();
    crate::short_invite::publish(&service, &keys, &other.encode().unwrap(), 600)
        .await
        .unwrap();
    let wanted_link = crate::short_invite::link(&wanted.short_code());
    assert_eq!(
        joined_reason(&mut guest, &mut events, 3, &wanted_link, "test-build")
            .await
            .as_deref(),
        Some("malformed")
    );

    // The service is down: the player is told to use the full invitation.
    guest.short.service = "http://127.0.0.1:9/s/v1/".into();
    assert_eq!(
        joined_reason(&mut guest, &mut events, 4, &missing_link, "test-build")
            .await
            .as_deref(),
        Some("short_unavailable")
    );

    // A host whose service is down hears so once and keeps no link.
    let (host_events_tx, mut host_events) = mpsc::channel(IPC_QUEUE_CAPACITY);
    let mut host = test_actor(own.clone(), host_events_tx);
    host.short.service = "http://127.0.0.1:9/s/v1/".into();
    host.epoch = 6;
    host.hosted =
        Some(Invite::create(own.id(), test_relay(), "test-build".into(), start, 3600).unwrap());
    host.short_invite_command(6).unwrap();
    complete_short(&mut host).await;
    assert!(
        matches!(host_events.recv().await.unwrap(), Event::ShortInvite { epoch: 6, ref link, ref status } if link.is_empty() && status == "unavailable")
    );
    assert!(host.short.room.is_none());

    // A Leave while the lookup runs drops its answer.
    guest.short.service = service;
    assert!(
        guest
            .command_now(short_join(7, &missing_link, "test-build"))
            .await
            .unwrap()
    );
    guest.clear_room();
    while let Some(finished) = guest.tasks.join_next().await {
        if let Ok(completion) = finished {
            guest.completed(completion).await.unwrap();
        }
    }
    assert!(events.try_recv().is_err());
    assert_eq!(guest.room, None);
    own.close().await;
}
