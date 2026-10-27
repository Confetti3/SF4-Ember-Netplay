//! Probe reservations, clock offsets and seat commits.

use super::*;

/// Seconds the checked PC's wall clock is set ahead of the checking peer's
/// (negative: behind). Each is a different PC pair.
const PEER_CLOCK_OFFSETS: [i64; 11] = [0, 5, 15, 19, 21, 30, 60, 300, 3600, -60, -3600];

/// The field report: two PCs whose clocks differ could not check the
/// connection between them. The checking peer stamps its reservation on its
/// own clock; the checked PC, its clock set ahead or behind, accepts the
/// frame whatever the difference, and the permission lasts the same span on
/// its monotonic clock.
#[tokio::test]
async fn a_reservation_is_accepted_whatever_the_checked_pcs_clock_says() {
    phase(
        "a_reservation_is_accepted_whatever_the_checked_pcs_clock_says",
        Duration::from_secs(90),
        async {
            let mut fixture = ReservationFixture::start().await;
            let remote = fixture.remote.id();
            let mut refused = Vec::new();
            let mut lifetimes = Vec::new();
            for (index, offset) in PEER_CLOCK_OFFSETS.into_iter().enumerate() {
                let request = 20 + index as u64;
                fixture.expires = now().unwrap() + crate::probe::window(false).as_secs();
                fixture.reserve(request).await;
                let frame = fixture.frame(request);
                let accepted = WALL_CLOCK_OFFSET
                    .scope(
                        offset,
                        fixture.actor.accept_coordination_control(remote, &frame),
                    )
                    .await;
                if !matches!(accepted, Ok(true)) {
                    refused.push(offset);
                    continue;
                }
                let permission = fixture
                    .actor
                    .probe_permissions
                    .get(&remote)
                    .expect("an accepted reservation installs its permission");
                assert_eq!(permission.request, request);
                let lifetime = permission
                    .expires
                    .saturating_duration_since(tokio::time::Instant::now());
                let full = crate::probe::window(true);
                if lifetime > full || lifetime + Duration::from_secs(2) < full {
                    lifetimes.push((offset, lifetime.as_secs()));
                }
            }
            assert!(
                refused.is_empty() && lifetimes.is_empty(),
                "refused with the clock ahead by {refused:?} s; \
             permission lifetimes off the window (offset, s): {lifetimes:?}"
            );
            fixture.stop().await;
        },
    )
    .await;
}

/// The check itself, end to end on the checked side: its clock is half a
/// minute ahead of the checking peer's, and the peer's measurement completes
/// with samples instead of timing out unanswered.
#[tokio::test]
async fn a_check_of_a_pc_whose_clock_is_ahead_completes() {
    phase(
        "a_check_of_a_pc_whose_clock_is_ahead_completes",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let remote = fixture.remote.id();
            fixture.expires = now().unwrap() + crate::probe::window(false).as_secs();
            fixture.reserve(7).await;
            let frame = fixture.frame(7);
            let accepted = WALL_CLOCK_OFFSET
                .scope(
                    30,
                    fixture.actor.accept_coordination_control(remote, &frame),
                )
                .await;
            let ReservationFixture {
                host,
                remote: checker,
                actor,
                events,
                recovery,
                source,
                room,
                ..
            } = fixture;
            let (_commands, command_rx) = mpsc::channel(IPC_QUEUE_CAPACITY);
            let (_fault, failure) = watch::channel(false);
            let mut actor = actor;
            let service = tokio::spawn(WALL_CLOCK_OFFSET.scope(30, async move {
                let result = actor.run(command_rx, failure).await;
                actor.tasks.shutdown().await;
                result
            }));
            let checked = run_probe(
                checker.clone(),
                address(&host),
                room,
                host.id(),
                7,
                5,
                false,
            )
            .await;
            service.abort();
            drop(events);
            let completion = checked.unwrap_or_else(|error| {
                panic!("the check failed ({error}); the reservation was answered {accepted:?}")
            });
            let summary = crate::recovery::summarize_datagram_probe(
                &completion.samples_us,
                completion.metrics.expected,
                completion.metrics.sent,
            );
            assert_eq!(summary.status, "ready");
            assert!(
                summary.sample_count >= 80,
                "{} samples",
                summary.sample_count
            );
            if let Some(connection) = completion.connection {
                connection.close(0u32.into(), b"done");
            }
            source.stop().await;
            recovery.stop().await;
            host.close().await;
            checker.close().await;
        },
    )
    .await;
}

/// This PC's wall clock is corrected (or set by hand) while its own check
/// waits for the room to commit the reservation. Whichever way it jumps, the
/// check goes ahead: how long the authorization stays usable is counted on
/// the monotonic clock from when it was requested.
#[tokio::test]
async fn a_wall_clock_jump_during_a_check_changes_nothing() {
    phase(
        "a_wall_clock_jump_during_a_check_changes_nothing",
        Duration::from_secs(90),
        async {
            for (request, jump) in [(3, 3600i64), (4, 30), (5, -3600)] {
                let mut fixture = ReservationFixture::start().await;
                fixture.connect_control().await;
                let (room, remote) = (fixture.room, fixture.remote.id());
                fixture
                    .actor
                    .spawn_probe(1, room, remote, request, 5, false)
                    .await
                    .unwrap();
                WALL_CLOCK_OFFSET
                    .scope(jump, complete_next(&mut fixture.actor))
                    .await;
                while let Ok(event) = fixture.events.try_recv() {
                    assert!(
                        !matches!(event, Event::Error { .. }),
                        "a {jump} s jump refused the check: {}",
                        serde_json::to_string(&event).unwrap()
                    );
                }
                assert!(
                    fixture.actor.own_probes.contains_key(&remote),
                    "a {jump} s jump dropped the check"
                );
                fixture.stop().await;
            }
        },
    )
    .await;
}

/// The monotonic deadline still holds: an authorization that completes after
/// its window, with the wall clock saying it is fresh, is refused.
#[tokio::test]
async fn a_late_probe_authorization_is_refused_by_the_monotonic_clock() {
    phase(
        "a_late_probe_authorization_is_refused_by_the_monotonic_clock",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            fixture.connect_control().await;
            let (room, remote) = (fixture.room, fixture.remote.id());
            fixture
                .actor
                .spawn_probe(1, room, remote, 3, 5, false)
                .await
                .unwrap();
            let completion = timeout(Duration::from_secs(10), fixture.actor.tasks.join_next())
                .await
                .expect("worker completion")
                .expect("a worker")
                .expect("worker result");
            let Completion::ProbeAuthorization(key, Ok(mut authorization)) = completion else {
                panic!("the authorization was not committed");
            };
            authorization.deadline = tokio::time::Instant::now();
            let task_count = fixture.actor.tasks.len();
            WALL_CLOCK_OFFSET
                .scope(
                    -3600,
                    fixture
                        .actor
                        .completed(Completion::ProbeAuthorization(key, Ok(authorization))),
                )
                .await
                .unwrap();
            assert!(!fixture.actor.own_probes.contains_key(&remote));
            assert_eq!(fixture.actor.tasks.len(), task_count);
            fixture.stop().await;
        },
    )
    .await;
}

#[tokio::test]
async fn an_expired_reservation_wait_leaves_a_replacement_control_open() {
    phase(
        "an_expired_reservation_wait_leaves_a_replacement_control_open",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let original = fixture.connect_control().await;
            // The reservation's Raft entry never arrives, so its wait is still
            // pending when the control that presented it is replaced.
            let payload = fixture.frame(9);
            let remote = fixture.remote.id();
            assert!(
                fixture
                    .actor
                    .accept_coordination_control(remote, &payload)
                    .await
                    .unwrap()
            );
            assert_eq!(
                fixture
                    .actor
                    .pending_probe_reservations
                    .get(&remote)
                    .map(|key| key.control),
                Some(original)
            );
            let replacement = fixture.connect_control().await;
            assert_ne!(replacement, original);
            // The wait expires unapplied and settles against the old control's id.
            complete_next(&mut fixture.actor).await;
            assert!(fixture.actor.pending_probe_reservations.is_empty());
            assert_eq!(
                fixture.actor.controls.get(&remote).map(ControlWorker::id),
                Some(replacement)
            );
            assert!(fixture.actor.probe_permissions.is_empty());
            fixture.no_control_closed();
            fixture.stop().await;
        },
    )
    .await;
}

#[tokio::test]
async fn an_expired_reservation_wait_still_closes_the_control_that_presented_it() {
    phase(
        "an_expired_reservation_wait_still_closes_the_control_that_presented_it",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let control = fixture.connect_control().await;
            let payload = fixture.frame(9);
            let remote = fixture.remote.id();
            assert!(
                fixture
                    .actor
                    .accept_coordination_control(remote, &payload)
                    .await
                    .unwrap()
            );
            complete_next(&mut fixture.actor).await;
            assert!(!fixture.actor.controls.contains_key(&remote));
            assert!(matches!(
                next(&mut fixture.events, "control_closed").await,
                Event::ControlClosed { control: closed, .. } if closed == control
            ));
            fixture.stop().await;
        },
    )
    .await;
}

/// A check asked for the moment both seats fill: the frame reaches this
/// replica before the seat commit and the reservation's entry behind it. It
/// waits for them instead of closing the room control, and the permission
/// follows once they apply.
#[tokio::test]
async fn a_reservation_ahead_of_the_seat_commit_waits_for_it() {
    phase(
        "a_reservation_ahead_of_the_seat_commit_waits_for_it",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let control = fixture.connect_control().await;
            let remote = fixture.remote.id();
            // The fixture seats the pair at table revision 5; the source checks
            // the pair as seated at revision 6, which this replica lacks.
            let frame = fixture.frame_at(11, 6);
            fixture.present(remote, &frame).await;
            assert!(fixture.actor.probe_permissions.is_empty());
            assert!(
                fixture
                    .actor
                    .pending_probe_reservations
                    .contains_key(&remote)
            );
            fixture.seat_pair(6).await;
            fixture.commit_reservation(11, 6).await;
            complete_next(&mut fixture.actor).await;
            assert!(fixture.actor.pending_probe_reservations.is_empty());
            assert_eq!(
                fixture
                    .actor
                    .probe_permissions
                    .get(&remote)
                    .map(|permission| permission.request),
                Some(11)
            );
            assert_eq!(
                fixture.actor.controls.get(&remote).map(ControlWorker::id),
                Some(control)
            );
            fixture.no_control_closed();
            fixture.stop().await;
        },
    )
    .await;
}

/// The frame waits for its entry, and by the time the entry applies a newer
/// commit has moved the table on (a spectator's Watch, the opponent's Ready).
/// The check is stale, not invalid: it is dropped with no permission, and the
/// room control stays open.
#[tokio::test]
async fn a_reservation_superseded_during_its_wait_is_dropped() {
    phase(
        "a_reservation_superseded_during_its_wait_is_dropped",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let control = fixture.connect_control().await;
            let remote = fixture.remote.id();
            let frame = fixture.frame_at(14, 5);
            fixture.present(remote, &frame).await;
            assert!(
                fixture
                    .actor
                    .pending_probe_reservations
                    .contains_key(&remote)
            );
            // Revision 6 is committed no later than the entry becomes visible, so
            // the wait never sees the pair still at 5.
            fixture.seat_pair(6).await;
            fixture.commit_reservation(14, 5).await;
            complete_next(&mut fixture.actor).await;
            fixture.dropped_quietly(control);
            fixture.stop().await;
        },
    )
    .await;
}

/// The same stale check when its frame arrives after both the entry and the
/// newer table revision have applied: dropped at once, nothing closed.
#[tokio::test]
async fn a_superseded_reservation_arriving_late_is_dropped() {
    phase(
        "a_superseded_reservation_arriving_late_is_dropped",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let control = fixture.connect_control().await;
            let remote = fixture.remote.id();
            fixture.commit_reservation(15, 5).await;
            fixture.seat_pair(6).await;
            let frame = fixture.frame_at(15, 5);
            let tasks = fixture.actor.tasks.len();
            fixture.present(remote, &frame).await;
            assert_eq!(fixture.actor.tasks.len(), tasks);
            fixture.dropped_quietly(control);
            fixture.stop().await;
        },
    )
    .await;
}

/// In a server-owned room the host relays the reservation. One whose entry
/// never applies is dropped when the wait runs out: no control closes, the
/// host's least of all.
#[tokio::test]
async fn a_relayed_reservation_that_never_applies_is_dropped() {
    phase(
        "a_relayed_reservation_that_never_applies_is_dropped",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let source_control = fixture.connect_control().await;
            let remote = fixture.remote.id();
            let relay_host = endpoint().await;
            fixture.actor.public = Some(crate::public_room::PublicRoom::Member {
                ticket: public_support::ticket_for(
                    &fixture.invite,
                    &public_support::identity(1),
                    &fixture.host,
                ),
            });
            fixture.actor.host_address = Some(address(&relay_host));
            let frame = fixture.frame_at(13, 7);
            fixture.present(relay_host.id(), &frame).await;
            assert!(
                fixture
                    .actor
                    .pending_probe_reservations
                    .contains_key(&remote)
            );
            complete_next(&mut fixture.actor).await;
            fixture.dropped_quietly(source_control);
            relay_host.close().await;
            fixture.stop().await;
        },
    )
    .await;
}

#[tokio::test]
async fn a_newer_applied_reservation_supersedes_an_older_pending_wait() {
    phase(
        "a_newer_applied_reservation_supersedes_an_older_pending_wait",
        Duration::from_secs(60),
        async {
            let mut fixture = ReservationFixture::start().await;
            let control = fixture.connect_control().await;
            let remote = fixture.remote.id();
            // Request 9 is not applied and waits on a task.
            let older = fixture.frame(9);
            assert!(
                fixture
                    .actor
                    .accept_coordination_control(remote, &older)
                    .await
                    .unwrap()
            );
            assert!(
                fixture
                    .actor
                    .pending_probe_reservations
                    .contains_key(&remote)
            );
            // Request 10 is already applied and takes the fast path.
            fixture.reserve(10).await;
            let newer = fixture.frame(10);
            assert!(
                fixture
                    .actor
                    .accept_coordination_control(remote, &newer)
                    .await
                    .unwrap()
            );
            assert!(fixture.actor.pending_probe_reservations.is_empty());
            assert_eq!(
                fixture
                    .actor
                    .probe_permissions
                    .get(&remote)
                    .map(|permission| permission.request),
                Some(10)
            );
            // The older wait expires unapplied and changes nothing.
            complete_next(&mut fixture.actor).await;
            assert_eq!(
                fixture.actor.controls.get(&remote).map(ControlWorker::id),
                Some(control)
            );
            assert_eq!(
                fixture
                    .actor
                    .probe_permissions
                    .get(&remote)
                    .map(|permission| permission.request),
                Some(10)
            );
            fixture.no_control_closed();
            fixture.stop().await;
        },
    )
    .await;
}
