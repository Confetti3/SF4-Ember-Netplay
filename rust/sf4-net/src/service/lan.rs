//! LAN is an explicit room transport policy. A browser lease owns the idle
//! endpoint; an open LAN room keeps it until departure, independently of UI.
use super::*;

#[derive(Default)]
pub(super) struct State {
    online: Option<(Endpoint, transport::Transport)>,
    discovery: Option<crate::lan::Discovery>,
    pub host: Option<crate::lan::Advertisement>,
    pub advertising: bool,
    pub serving: JoinSet<()>,
    view: u64,
    until: Option<Instant>,
}

impl State {
    pub(super) fn unadvertise(&mut self, endpoint: &Endpoint) {
        if self.online.is_some() {
            let _ = crate::lan::advertise(endpoint, None);
        }
        self.serving.abort_all();
        self.host = None;
        self.advertising = false;
    }
}

impl Actor {
    pub(super) fn lan_room(&mut self, epoch: u64, name: String, capacity: u8, members: u8) {
        if !self.matches(epoch) || self.lan.online.is_none() {
            return;
        }
        let Some(invite) = self
            .room_invite
            .as_ref()
            .or(self.hosted.as_ref())
            .filter(|i| i.is_lan())
        else {
            return;
        };
        let info = crate::lan::Advertisement {
            name,
            build: invite.build().into(),
            members,
            capacity,
            pulse: self.lan.host.as_ref().map_or(0, |a| a.pulse),
        };
        if info.valid() {
            self.lan.host = Some(info);
        }
    }
    pub(super) async fn start_lan(&mut self, opening: bool) -> io::Result<()> {
        if self.lan.online.is_some() {
            return Ok(());
        }
        if self.room.is_some()
            || (self.opening && !opening)
            || self.transport == transport::Transport::RelayOnly
        {
            return Err(failed("lan_unavailable"));
        }
        let (endpoint, discovery) = timeout(Duration::from_secs(3), crate::lan::bind())
            .await
            .map_err(|_| failed("lan_unavailable"))??;
        self.lan.online = Some((
            std::mem::replace(&mut self.endpoint, endpoint),
            self.transport,
        ));
        self.transport = transport::Transport::Lan;
        self.lan.discovery = Some(discovery);
        self.emit(status(0, &self.endpoint, self.epoch, 0, 0))
    }

    pub(super) async fn stop_lan(&mut self) {
        if let Some((online, policy)) = self.lan.online.take() {
            self.transport = policy;
            let lan = std::mem::replace(&mut self.endpoint, online);
            // Browser generations outlive an endpoint. An old renewal must
            // not reopen discovery after the user has left that view.
            self.lan = State {
                view: self.lan.view,
                ..State::default()
            };
            lan.close().await;
            let _ = self.emit(status(
                0,
                &self.endpoint,
                self.epoch,
                self.controls.len(),
                self.games.len(),
            ));
        }
    }

    pub(super) async fn lan_watch(&mut self, view: u64, on: bool) -> io::Result<()> {
        if view == 0 || view < self.lan.view {
            return Ok(());
        }
        self.lan.view = view;
        if on {
            if self.start_lan(false).await.is_err() {
                return self.emit(Event::LanRooms {
                    view,
                    status: "unavailable".into(),
                    rooms: vec![],
                });
            }
            self.lan.until = Some(Instant::now() + Duration::from_secs(15));
        } else {
            self.lan.until = None;
        }
        self.pump_lan().await
    }

    pub(super) fn join_lan(
        &mut self,
        id: u64,
        epoch: u64,
        host: EndpointId,
        build: String,
    ) -> io::Result<()> {
        if host == self.endpoint.id() {
            self.clear_room();
            return self.error_at(id, epoch, "lan_join_failed");
        }
        let endpoint = self.endpoint.clone();
        self.tasks.spawn(async move {
            let result = match now() {
                Ok(time) => crate::lan::invitation(&endpoint, host, &build, time).await,
                Err(error) => Err(error),
            };
            Completion::LanInvitation(epoch, id, build, result)
        });
        Ok(())
    }

    pub(super) fn completed_lan_invitation(
        &mut self,
        epoch: u64,
        id: u64,
        build: String,
        result: io::Result<String>,
    ) -> io::Result<()> {
        if self.matches(epoch) && self.opening {
            match result {
                Ok(invitation) => self.join_invitation(id, &invitation, build, JoinVia::Pasted { observer: false })?,
                Err(_) => {
                    self.clear_room();
                    self.error_at(id, epoch, "lan_join_failed")?;
                }
            }
        }
        Ok(())
    }

    pub(super) async fn pump_lan(&mut self) -> io::Result<()> {
        if self.lan.online.is_none() {
            return Ok(());
        }
        let leading = self.coordination_writable
            && self
                .recovery
                .as_ref()
                .is_some_and(|r| r.coordinator.current_leader() == Some(r.incarnation));
        if let Some(info) = &mut self.lan.host
            && leading
        {
            // Do not cycle at the discovery library's ten-second cadence:
            // equal metadata can be treated as an unchanged announcement.
            info.pulse = info.pulse.wrapping_add(1);
            self.lan.advertising = crate::lan::advertise(&self.endpoint, Some(info)).is_ok();
        } else if self.lan.advertising {
            let _ = crate::lan::advertise(&self.endpoint, None);
            self.lan.advertising = false;
        }
        if self.lan.until.is_some_and(|until| Instant::now() < until) {
            let rooms = self
                .lan
                .discovery
                .as_ref()
                .map(|d| {
                    d.rooms
                        .lock()
                        .unwrap_or_else(|e| e.into_inner())
                        .snapshot(Instant::now())
                })
                .unwrap_or_default();
            self.emit_bulk(Event::LanRooms {
                view: self.lan.view,
                status: "ready".into(),
                rooms,
            });
        } else {
            self.lan.until = None;
            if self.room.is_none() && !self.opening {
                self.stop_lan().await;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::{endpoint, test_actor};
    use super::*;

    #[tokio::test]
    async fn authority_from_a_control_keeps_lan_coordination_local() {
        let (host, _hosting) = crate::lan::bind().await.unwrap();
        let (guest, _browsing) = crate::lan::bind().await.unwrap();
        // A ticket without a coordination route learns it from the host's
        // authenticated Admission, just as compact online tickets do.
        let invite = Invite::create_lan(host.id(), "test".into(), now().unwrap()).unwrap();
        let recovery = crate::recovery::RecoverySession::host_in(
            invite.room(),
            host.id(),
            crate::transport::Transport::Lan,
            None,
            false,
        )
        .await
        .unwrap();
        let (events, _received) = mpsc::channel(128);
        let mut actor = test_actor(guest.clone(), events);
        actor.epoch = 1;
        actor.transport = transport::Transport::Lan;
        actor.room = Some(invite.room());
        actor.room_invite = Some(invite);
        let payload = serde_json::to_string(&CoordinationControl::Admission {
            admission: recovery.advertise().await,
        })
        .unwrap();
        assert!(
            actor
                .accept_coordination_control(host.id(), &payload)
                .await
                .unwrap()
        );
        let joined = actor.recovery.take().unwrap();
        let advertised = joined.advertise().await;
        assert!(
            advertised
                .coordination_address
                .relay_urls()
                .next()
                .is_none()
        );
        joined.stop().await;
        recovery.stop().await;
        host.close().await;
        guest.close().await;
    }

    #[tokio::test]
    async fn invitation_serving_has_its_own_bounded_pool() {
        let (host, _hosting) = crate::lan::bind().await.unwrap();
        let (guest, _browsing) = crate::lan::bind().await.unwrap();
        let (events, _received) = mpsc::channel(128);
        let mut actor = test_actor(host.clone(), events);
        actor.epoch = 1;
        actor.lan.advertising = true;
        actor.hosted = Some(Invite::create_lan(host.id(), "test".into(), now().unwrap()).unwrap());
        for _ in 0..4 {
            actor.lan.serving.spawn(std::future::pending::<()>());
        }
        let dial = guest.connect(host.addr(), crate::lan::ALPN);
        let accept = async { host.accept().await.unwrap().await.unwrap() };
        let (client, connection) = timeout(Duration::from_secs(10), async {
            tokio::join!(dial, accept)
        })
        .await
        .unwrap();
        let client = client.unwrap();
        actor.completed_incoming(1, Ok(connection)).await.unwrap();
        timeout(Duration::from_secs(2), client.closed())
            .await
            .unwrap();
        assert_eq!(actor.lan.serving.len(), 4);
        assert!(
            actor.tasks.is_empty(),
            "LAN serving used the room's task budget"
        );
        actor.lan.serving.abort_all();
        while actor.lan.serving.join_next().await.is_some() {}
        let dial = guest.connect(host.addr(), crate::lan::ALPN);
        let accept = async { host.accept().await.unwrap().await.unwrap() };
        let (client, connection) = timeout(Duration::from_secs(10), async {
            tokio::join!(dial, accept)
        })
        .await
        .unwrap();
        let client = client.unwrap();
        actor.completed_incoming(1, Ok(connection)).await.unwrap();
        assert_eq!(actor.lan.serving.len(), 1);
        assert!(actor.tasks.is_empty());
        client.close(0u32.into(), b"test complete");
        actor.lan.unadvertise(&host);
        host.close().await;
        guest.close().await;
    }

    #[tokio::test]
    async fn beginning_a_lan_join_retains_the_browsers_endpoint() {
        let endpoint = endpoint().await;
        let (events, _received) = mpsc::channel(128);
        let mut actor = test_actor(endpoint, events);
        actor.lan_watch(1, true).await.unwrap();
        let browser = actor.endpoint.id();
        assert!(actor.begin(1, "test", transport::Transport::Lan).await);
        actor.lan_watch(1, false).await.unwrap();
        assert_eq!(actor.endpoint.id(), browser);
        assert!(actor.opening && actor.lan.discovery.is_some());
        actor.clear_room();
        actor.pump_lan().await.unwrap();
        actor.endpoint.close().await;
    }

    #[tokio::test]
    async fn begin_online_from_browser_restores_policy_once() {
        let endpoint = endpoint().await;
        let online = endpoint.id();
        let (events, _received) = mpsc::channel(128);
        let mut actor = test_actor(endpoint, events);
        actor.lan_watch(1, true).await.unwrap();
        assert_eq!(actor.transport, transport::Transport::Lan);
        assert!(actor.begin(1, "test", transport::Transport::Online).await);
        assert_eq!(actor.endpoint.id(), online);
        assert_eq!(actor.transport, transport::Transport::Online);
        assert!(actor.lan.discovery.is_none());
        assert!(!actor.begin(1, "test", transport::Transport::Lan).await);
        assert_eq!(actor.endpoint.id(), online);
        actor.endpoint.close().await;
    }

    #[tokio::test]
    async fn browser_lease_restores_online_and_ignores_old_views() {
        let endpoint = endpoint().await;
        let online = endpoint.id();
        let (events, mut received) = mpsc::channel(128);
        let mut actor = test_actor(endpoint, events);
        actor.lan_watch(2, true).await.unwrap();
        assert_ne!(actor.endpoint.id(), online);
        assert!(actor.endpoint.addr().relay_urls().next().is_none());
        actor.lan_watch(3, false).await.unwrap();
        assert_eq!(actor.endpoint.id(), online);
        actor.lan_watch(2, true).await.unwrap();
        assert!(actor.lan.online.is_none());
        actor.lan_watch(4, true).await.unwrap();
        actor.lan.until = Some(Instant::now());
        actor.pump_lan().await.unwrap();
        assert_eq!(actor.endpoint.id(), online);
        assert!(actor.lan.discovery.is_none());
        while received.try_recv().is_ok() {}
        actor.endpoint.close().await;
    }

    #[tokio::test]
    async fn rejected_lan_attempt_consumes_epoch_and_can_be_cancelled() {
        let endpoint = endpoint().await;
        let original = endpoint.id();
        let (events, mut received) = mpsc::channel(128);
        let mut actor = test_actor(endpoint, events);
        actor.transport = transport::Transport::RelayOnly;
        assert!(!actor.begin(1, "test", transport::Transport::Lan).await);
        assert_eq!(actor.epoch, 1);
        assert!(!actor.opening);
        assert_eq!(actor.endpoint.id(), original);
        actor
            .command(Request {
                id: 2,
                command: Command::Leave {
                    epoch: 1,
                    abandon: false,
                },
            })
            .await
            .unwrap();
        assert!(matches!(
            received.recv().await.unwrap(),
            Event::RoomClosed { epoch: 1 }
        ));
        assert!(!actor.begin(1, "test", transport::Transport::Lan).await);
        actor.endpoint.close().await;
    }

    #[tokio::test]
    async fn an_online_invitation_cannot_change_a_lan_attempts_policy() {
        let endpoint = endpoint().await;
        let (events, mut received) = mpsc::channel(128);
        let mut actor = test_actor(endpoint, events);
        actor.epoch=1;actor.opening=true;actor.transport=transport::Transport::Lan;
        let invite=Invite::create(iroh::SecretKey::generate().public(), "https://use1-1.relay.n0.iroh.link./".parse().unwrap(), "test".into(), now().unwrap(), 60).unwrap();
        actor.join_invitation(1,&invite.encode().unwrap(),"test".into(),JoinVia::Pasted { observer: false }).unwrap();
        assert!(!actor.opening && actor.tasks.is_empty());
        assert!(matches!(received.recv().await.unwrap(),Event::Error { .. }));
        actor.endpoint.close().await;
    }

    #[tokio::test]
    async fn browsing_cannot_replace_an_online_room() {
        let endpoint = endpoint().await;
        let original = endpoint.id();
        let (events, mut received) = mpsc::channel(128);
        let mut actor = test_actor(endpoint, events);
        actor.room = Some([1; 16]);
        actor.lan_watch(1, true).await.unwrap();
        assert_eq!(actor.endpoint.id(), original);
        assert!(
            matches!(received.recv().await.unwrap(), Event::LanRooms { status, .. } if status == "unavailable")
        );
        actor.endpoint.close().await;
    }
}
