use super::*;

#[tokio::test]
async fn local_discovery_and_invitation_exchange_need_no_relay() {
    let (host, _hosting) = bind().await.unwrap();
    let (guest, browsing) = bind().await.unwrap();
    assert!(host.addr().relay_urls().next().is_none());
    assert!(guest.addr().relay_urls().next().is_none());
    advertise(&host, Some(&info())).unwrap();
    timeout(Duration::from_secs(12), async {
        loop {
            if browsing
                .rooms
                .lock()
                .unwrap()
                .snapshot(Instant::now())
                .iter()
                .any(|r| r.endpoint == host.id())
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    })
    .await
    .expect("LAN discovery did not find the host");
    let ticket = crate::invite::Invite::create_lan(host.id(), "test".into(), 100)
        .unwrap()
        .encode()
        .unwrap();
    let accepting = host.clone();
    let server = tokio::spawn(async move {
        let connection = accepting.accept().await.unwrap().await.unwrap();
        serve(connection, ticket).await;
    });
    let text = invitation(&guest, host.id(), "test", 101).await.unwrap();
    assert!(crate::invite::Invite::is_lan_text(&text));
    server.await.unwrap();
    let mut changed = info();
    changed.name = "Renamed while resolving".into();
    changed.members = 2;
    changed.pulse = 11;
    advertise(&host, Some(&changed)).unwrap();
    timeout(Duration::from_secs(25), async {
        loop {
            if browsing
                .rooms
                .lock()
                .unwrap()
                .snapshot(Instant::now())
                .iter()
                .any(|room| room.info == changed)
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    })
    .await
    .expect("address resolution hid a room metadata update");
    advertise(&host, None).unwrap();
    timeout(Duration::from_secs(25), async {
        loop {
            if browsing
                .rooms
                .lock()
                .unwrap()
                .snapshot(Instant::now())
                .iter()
                .all(|room| room.endpoint != host.id())
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    })
    .await
    .expect("withdrawn LAN room did not disappear");
    host.close().await;
    guest.close().await;
}

fn info() -> Advertisement {
    Advertisement {
        name: "Local room".into(),
        build: "test".into(),
        members: 1,
        capacity: 16,
        pulse: 0,
    }
}

#[test]
fn discovery_expires_bounds_and_removes_withdrawn_rooms() {
    let mut rooms = Rooms::default();
    let time = Instant::now();
    let text = serde_json::to_string(&info()).unwrap();
    let id = iroh::SecretKey::generate().public();
    rooms.discovered(id, Some(&text), time);
    assert_eq!(rooms.snapshot(time).len(), 1);
    rooms.discovered(id, None, time);
    assert!(rooms.snapshot(time).is_empty());
    for _ in 0..MAX_ROOMS + 10 {
        rooms.discovered(iroh::SecretKey::generate().public(), Some(&text), time);
    }
    assert_eq!(rooms.snapshot(time).len(), MAX_ROOMS);
    assert!(rooms.snapshot(time + MAX_AGE).is_empty());
}

#[test]
fn invalid_advertisements_are_refused() {
    for text in ["{}", "[]", "null", "not json"] {
        let mut rooms = Rooms::default();
        rooms.discovered(
            iroh::SecretKey::generate().public(),
            Some(text),
            Instant::now(),
        );
        assert!(rooms.snapshot(Instant::now()).is_empty());
    }
    let mut bad = info();
    bad.name = "hidden\nroom".into();
    assert!(!bad.valid());
    bad = info();
    bad.capacity = 17;
    assert!(!bad.valid());
    bad = info();
    bad.members = 17;
    assert!(!bad.valid());
}

#[test]
fn lan_invites_preserve_admission_without_online_exports() {
    let host = iroh::SecretKey::generate().public();
    let coord = iroh::SecretKey::generate().public();
    let invite = crate::invite::Invite::create_lan(host, "test".into(), 100)
        .unwrap()
        .with_authority(coord, 1, 1)
        .unwrap();
    let encoded = invite.encode().unwrap();
    let decoded = crate::invite::Invite::parse_for_build(&encoded, 101, "test").unwrap();
    assert!(decoded.is_lan());
    assert!(decoded.address().relay_urls().next().is_none());
    assert!(
        decoded
            .coordination_address()
            .unwrap()
            .relay_urls()
            .next()
            .is_none()
    );
    assert!(decoded.encode_discord().is_err());
    assert!(crate::invite::Invite::parse_for_build(&encoded, 101, "other").is_err());
    assert!(crate::invite::Invite::parse(&encoded, decoded.expires()).is_err());
    assert!(crate::invite::Invite::parse(&encoded.replacen("sf4l1:", "sf4e1:", 1), 101).is_err());
}
