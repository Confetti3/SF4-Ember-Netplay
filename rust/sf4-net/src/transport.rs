//! Iroh connections with bounded framing and explicit application admission.
//! The C++ room owner decides membership/roles; this module checks credentials
//! before handing it a connection. Control and gameplay close independently.
use std::{
    io,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use iroh::{
    Endpoint, EndpointAddr, EndpointId,
    endpoint::{Connection, ConnectionError, PortmapperConfig, RecvStream, SendStream, presets},
};
use serde::{Deserialize, Serialize};
use subtle::ConstantTimeEq;
use tokio::{
    sync::watch,
    time::{Instant, sleep_until, timeout},
};

use crate::{
    invite::{Invite, RoomProof},
    wire::{self, CONTROL_ALPN, ControlFrame, GAME_ALPN, GAME_HEADER, MatchKey, VERSION},
};

pub const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(10);
/// A prepared listener precedes the separately committed game-connect phase.
/// Keep that authorization bounded, while allowing the room journal enough
/// time to replicate every participant's prepared acknowledgement.
pub const PREPARED_GAME_TIMEOUT: Duration = Duration::from_secs(60);
/// The first bytes on a GAME_ALPN stream identify its purpose. This lets a
/// probe and gameplay handshake share one authenticated QUIC connection.
pub const GAME_PROBE_MAGIC: [u8; 4] = *b"PRB2";
pub const GAME_PLAY_MAGIC: [u8; 4] = *b"GME1";

fn failed() -> io::Error {
    io::Error::new(
        io::ErrorKind::ConnectionAborted,
        "peer connection or admission failed",
    )
}
fn now() -> io::Result<u64> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .map_err(|_| failed())
}

/// Relay-only is a diagnostic/test policy, never an alternate gameplay protocol.
pub async fn bind_endpoint_with_policy(relay_only: bool) -> io::Result<Endpoint> {
    // Port mapping was switched off to demonstrate the transport did not
    // depend on it. Leaving it off in the shipping endpoint also removed a
    // NAT-traversal aid: iroh documents the cost of Disabled as "potentially
    // worse direct connectivity behind some NATs", which is what players who
    // cannot reach each other directly are hitting. Take iroh's default
    // (enabled) for gameplay; relay-only keeps it off, where it is moot
    // because that diagnostic clears the IP transports anyway.
    let builder =
        Endpoint::builder(presets::N0).alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()]);
    let builder = if relay_only {
        builder
            .clear_ip_transports()
            .portmapper_config(PortmapperConfig::Disabled)
    } else {
        builder
    };
    builder.bind().await.map_err(|_| failed())
}

pub struct ControlChannel {
    pub connection: Connection,
    pub sender: ControlSender,
    pub receiver: ControlReceiver,
}

pub struct ControlSender {
    stream: SendStream,
    last_id: u64,
    poisoned: bool,
}
pub struct ControlReceiver {
    stream: RecvStream,
    last_id: u64,
    poisoned: bool,
}

impl ControlSender {
    pub async fn send(&mut self, frame: &ControlFrame) -> io::Result<()> {
        if self.poisoned || frame.message_id <= self.last_id {
            return Err(failed());
        }
        // Any interrupted write invalidates this stream. Callers close the
        // channel instead of retrying a partially written frame on it.
        self.poisoned = true;
        wire::write_control(&mut self.stream, frame).await?;
        self.last_id = frame.message_id;
        self.poisoned = false;
        Ok(())
    }
}

impl ControlReceiver {
    pub async fn receive(&mut self) -> io::Result<ControlFrame> {
        if self.poisoned {
            return Err(failed());
        }
        self.poisoned = true;
        let frame = wire::read_control(&mut self.stream).await?;
        if frame.message_id <= self.last_id {
            return Err(failed());
        }
        self.last_id = frame.message_id;
        self.poisoned = false;
        Ok(frame)
    }
}

fn control_channel(connection: Connection, stream: (SendStream, RecvStream)) -> ControlChannel {
    ControlChannel {
        connection,
        sender: ControlSender {
            stream: stream.0,
            last_id: 1,
            poisoned: false,
        },
        receiver: ControlReceiver {
            stream: stream.1,
            last_id: 1,
            poisoned: false,
        },
    }
}

async fn send_handshake<T: Serialize>(send: &mut SendStream, value: &T) -> io::Result<()> {
    let payload = serde_json::to_vec(value).map_err(|_| failed())?;
    wire::write_control(
        send,
        &ControlFrame {
            message_id: 1,
            payload,
        },
    )
    .await
}

async fn read_handshake<T: serde::de::DeserializeOwned>(recv: &mut RecvStream) -> io::Result<T> {
    let frame = wire::read_control(recv).await?;
    if frame.message_id != 1 {
        return Err(failed());
    }
    serde_json::from_slice(&frame.payload).map_err(|_| failed())
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Accepted {
    version: u16,
}

/// Drops an in-flight connection on timeout, cancellation, or admission failure.
/// A successful operation explicitly disarms this guard.
struct PendingConnection(Option<Connection>);
impl Drop for PendingConnection {
    fn drop(&mut self) {
        if let Some(connection) = &self.0 {
            connection.close(1u32.into(), b"admission ended");
        }
    }
}

pub async fn connect_control(endpoint: &Endpoint, invite: &Invite) -> io::Result<ControlChannel> {
    timeout(HANDSHAKE_TIMEOUT, async {
        let connection = endpoint
            .connect(invite.address(), CONTROL_ALPN)
            .await
            .map_err(|_| failed())?;
        let mut pending = PendingConnection(Some(connection.clone()));
        let result = connect_control_on(connection, invite).await;
        if result.is_ok() {
            pending.0 = None;
        }
        result
    })
    .await
    .map_err(|_| failed())?
}

// Also used by the deterministic local harness with explicit loopback routing.
pub async fn connect_control_on(
    connection: Connection,
    invite: &Invite,
) -> io::Result<ControlChannel> {
    connect_control_on_expected(connection, invite, Some(invite.endpoint())).await
}

/// Rebind a control route to an already admitted room member. The invitation
/// authenticates the room capability; the expected endpoint binds the new
/// socket to the member selected by the committed coordination record.
pub async fn connect_control_to(
    endpoint: &Endpoint,
    address: EndpointAddr,
    invite: &Invite,
) -> io::Result<ControlChannel> {
    timeout(HANDSHAKE_TIMEOUT, async {
        let connection = endpoint
            .connect(address.clone(), CONTROL_ALPN)
            .await
            .map_err(|_| failed())?;
        connect_control_on_expected(connection, invite, Some(address.id)).await
    })
    .await
    .map_err(|_| failed())?
}

async fn connect_control_on_expected(
    connection: Connection,
    invite: &Invite,
    expected: Option<EndpointId>,
) -> io::Result<ControlChannel> {
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = timeout(HANDSHAKE_TIMEOUT, async {
        if connection.alpn() != CONTROL_ALPN
            || expected.is_some_and(|expected| connection.remote_id() != expected)
        {
            return Err(failed());
        }
        let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failed())?;
        send_handshake(&mut send, &invite.proof()).await?;
        let accepted: Accepted = read_handshake(&mut recv).await?;
        if accepted.version != VERSION {
            return Err(failed());
        }
        Ok(control_channel(connection, (send, recv)))
    })
    .await
    .map_err(|_| failed())?;
    if result.is_ok() {
        pending.0 = None;
    }
    result
}

pub async fn accept_control(connection: Connection, room: &Invite) -> io::Result<ControlChannel> {
    accept_control_policy(connection, room, None, now()?).await
}

pub(crate) async fn accept_control_member(
    connection: Connection,
    room: &Invite,
    member: EndpointId,
) -> io::Result<ControlChannel> {
    accept_control_policy(connection, room, Some(member), now()?).await
}

async fn accept_control_policy(
    connection: Connection,
    room: &Invite,
    member: Option<EndpointId>,
    clock: u64,
) -> io::Result<ControlChannel> {
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = timeout(HANDSHAKE_TIMEOUT, async {
        if connection.alpn() != CONTROL_ALPN {
            return Err(failed());
        }
        let (mut send, mut recv) = connection.accept_bi().await.map_err(|_| failed())?;
        let proof: RoomProof = read_handshake(&mut recv).await?;
        if let Some(member) = member {
            if connection.remote_id() != member {
                return Err(failed());
            }
            room.admit_member(&proof)?;
        } else {
            room.admit(&proof, clock)?;
        }
        send_handshake(&mut send, &Accepted { version: VERSION }).await?;
        Ok(control_channel(connection, (send, recv)))
    })
    .await
    .map_err(|_| failed())?;
    if result.is_ok() {
        pending.0 = None;
    }
    result
}

/// Supplied over authenticated control IPC for a single authorized peer/match.
/// A new capability and generation must be allocated on every rematch.
#[derive(Clone)]
pub struct GameAuthorization {
    pub peer: EndpointId,
    pub key: MatchKey,
    pub capability: [u8; 32],
    pub max_packet: usize,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct GameProof {
    version: u16,
    room: [u8; 16],
    generation: u64,
    capability: [u8; 32],
    max_packet: usize,
}

impl GameAuthorization {
    fn proof(&self) -> GameProof {
        GameProof {
            version: VERSION,
            room: self.key.room,
            generation: self.key.generation,
            capability: self.capability,
            max_packet: self.max_packet,
        }
    }
    fn validate(&self, connection: &Connection, proof: &GameProof) -> io::Result<()> {
        if connection.alpn() != GAME_ALPN
            || connection.remote_id() != self.peer
            || proof.version != VERSION
            || proof.room != self.key.room
            || proof.generation != self.key.generation
            || proof.max_packet != self.max_packet
            || self.capability == [0; 32]
            || !bool::from(proof.capability.ct_eq(&self.capability))
            || self.key.room == [0; 16]
            || self.key.generation == 0
            || self.max_packet == 0
            || self.max_packet > wire::MAX_UDP_PAYLOAD
            || connection.max_datagram_size().unwrap_or(0) < self.max_packet + GAME_HEADER
        {
            return Err(failed());
        }
        Ok(())
    }
}

pub struct GameConnection {
    pub(crate) connection: Connection,
    pub(crate) authorization: GameAuthorization,
}
impl GameConnection {
    pub fn max_packet(&self) -> usize {
        self.authorization.max_packet
    }
    pub fn close(&self) {
        self.connection.close(0u32.into(), b"match ended");
    }
}
impl Drop for GameConnection {
    fn drop(&mut self) {
        self.close();
    }
}

pub async fn connect_game(
    endpoint: &Endpoint,
    address: EndpointAddr,
    auth: GameAuthorization,
) -> io::Result<GameConnection> {
    if address.id != auth.peer {
        return Err(failed());
    }
    let deadline = Instant::now() + HANDSHAKE_TIMEOUT;
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(failed());
        }
        let Ok((result, connection)) = timeout(
            remaining,
            connect_game_once(endpoint, address.clone(), auth.clone()),
        )
        .await
        else {
            return Err(failed());
        };
        match result {
            Ok(game) => return Ok(game),
            Err(error) => {
                let retry = if let Some(connection) = &connection {
                    game_listener_pending(connection).await
                } else {
                    Instant::now() < deadline
                };
                if !retry {
                    if let Some(connection) = &connection {
                        connection.close(1u32.into(), b"admission ended");
                    }
                    return Err(error);
                }
            }
        }
        // Listener and dial commands are delivered to separate helper
        // processes. A dial may reach GAME_ALPN before the peer installs its
        // matching generation; retry only within the original handshake bound.
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
}

async fn connect_game_once(
    endpoint: &Endpoint,
    address: EndpointAddr,
    auth: GameAuthorization,
) -> (io::Result<GameConnection>, Option<Connection>) {
    let connection = match endpoint.connect(address, GAME_ALPN).await {
        Ok(connection) => connection,
        Err(_) => return (Err(failed()), None),
    };
    let observed = connection.clone();
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = async {
        let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failed())?;
        auth.validate(&connection, &auth.proof())?;
        send.write_all(&GAME_PLAY_MAGIC)
            .await
            .map_err(|_| failed())?;
        send_handshake(&mut send, &auth.proof()).await?;
        let proof: GameProof = read_handshake(&mut recv).await?;
        auth.validate(&connection, &proof)?;
        send.finish().map_err(|_| failed())?;
        Ok(GameConnection {
            connection,
            authorization: auth,
        })
    }
    .await;
    // The caller owns a failed connection long enough to distinguish the
    // peer's explicit listener-not-ready close from an authentication error.
    // Cancellation before this point still runs the guard and closes it.
    pending.0 = None;
    (result, Some(observed))
}

async fn game_listener_pending(connection: &Connection) -> bool {
    let reason = if let Some(reason) = connection.close_reason() {
        reason
    } else {
        let Ok(reason) = timeout(Duration::from_millis(100), connection.closed()).await else {
            return false;
        };
        reason
    };
    matches!(reason,
        ConnectionError::ApplicationClosed(close)
            if close.reason.as_ref() == b"gameplay not authorized")
}

pub enum GameStream {
    Probe(SendStream, RecvStream),
    Gameplay(SendStream, RecvStream),
}

/// Consume the purpose marker while leaving the application handshake in the
/// stream for the caller that owns that mode.
pub async fn accept_game_stream(connection: &Connection) -> io::Result<GameStream> {
    accept_game_stream_until(connection, Instant::now() + HANDSHAKE_TIMEOUT).await
}

pub async fn accept_game_stream_until(
    connection: &Connection,
    deadline: Instant,
) -> io::Result<GameStream> {
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = tokio::time::timeout_at(deadline, async {
        if connection.alpn() != GAME_ALPN {
            return Err(failed());
        }
        let (send, recv) = connection.accept_bi().await.map_err(|_| failed())?;
        classify_game_stream(send, recv).await
    })
    .await
    .map_err(|_| failed())?;
    if result.is_ok() {
        pending.0 = None;
    }
    result
}

async fn classify_game_stream(send: SendStream, mut recv: RecvStream) -> io::Result<GameStream> {
    let mut marker = [0u8; GAME_PROBE_MAGIC.len()];
    recv.read_exact(&mut marker).await.map_err(|_| failed())?;
    if marker == GAME_PROBE_MAGIC {
        Ok(GameStream::Probe(send, recv))
    } else if marker == GAME_PLAY_MAGIC {
        Ok(GameStream::Gameplay(send, recv))
    } else {
        Err(failed())
    }
}

async fn accept_prepared_stream(
    connection: &Connection,
    mut deadline: watch::Receiver<Instant>,
) -> io::Result<(SendStream, RecvStream)> {
    loop {
        let expires = *deadline.borrow_and_update();
        if expires <= Instant::now() {
            return Err(failed());
        }
        tokio::select! {
            result = connection.accept_bi() => return result.map_err(|_| failed()),
            _ = sleep_until(expires) => {
                if *deadline.borrow() <= Instant::now() {
                    return Err(failed());
                }
            }
            changed = deadline.changed() => {
                if changed.is_err() {
                    return Err(failed());
                }
            }
        }
    }
}

/// Finish the authenticated game handshake on a connection reserved by the
/// same-future-game route probe.  The reservation carries no gameplay
/// capability; this call is the only transition that makes a datagram bridge
/// possible.
pub async fn connect_game_on(
    connection: Connection,
    auth: GameAuthorization,
) -> io::Result<GameConnection> {
    timeout(HANDSHAKE_TIMEOUT, async {
        if connection.alpn() != GAME_ALPN || connection.remote_id() != auth.peer {
            return Err(failed());
        }
        let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failed())?;
        auth.validate(&connection, &auth.proof())?;
        send.write_all(&GAME_PLAY_MAGIC)
            .await
            .map_err(|_| failed())?;
        send_handshake(&mut send, &auth.proof()).await?;
        let proof: GameProof = read_handshake(&mut recv).await?;
        auth.validate(&connection, &proof)?;
        send.finish().map_err(|_| failed())?;
        Ok(GameConnection {
            connection,
            authorization: auth,
        })
    })
    .await
    .map_err(|_| failed())?
}

pub async fn accept_game(
    connection: Connection,
    auth: GameAuthorization,
) -> io::Result<GameConnection> {
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = timeout(HANDSHAKE_TIMEOUT, async {
        if connection.alpn() != GAME_ALPN || connection.remote_id() != auth.peer {
            return Err(failed());
        }
        match accept_game_stream(&connection).await? {
            GameStream::Gameplay(send, recv) => {
                accept_game_stream_with(connection, auth, send, recv).await
            }
            GameStream::Probe(_, _) => Err(failed()),
        }
    })
    .await
    .map_err(|_| failed())?;
    if result.is_ok() {
        pending.0 = None;
    }
    result
}

pub async fn accept_game_on(
    connection: Connection,
    auth: GameAuthorization,
    deadline: watch::Receiver<Instant>,
) -> io::Result<GameConnection> {
    if connection.alpn() != GAME_ALPN || connection.remote_id() != auth.peer {
        return Err(failed());
    }
    let (send, recv) = accept_prepared_stream(&connection, deadline).await?;
    timeout(HANDSHAKE_TIMEOUT, async {
        match classify_game_stream(send, recv).await? {
            GameStream::Gameplay(send, recv) => {
                accept_game_stream_with(connection, auth, send, recv).await
            }
            GameStream::Probe(_, _) => Err(failed()),
        }
    })
    .await
    .map_err(|_| failed())?
}

pub async fn accept_game_stream_with(
    connection: Connection,
    auth: GameAuthorization,
    send: SendStream,
    recv: RecvStream,
) -> io::Result<GameConnection> {
    accept_game_stream_with_until(
        connection,
        auth,
        send,
        recv,
        Instant::now() + HANDSHAKE_TIMEOUT,
    )
    .await
}

pub async fn accept_game_stream_with_until(
    connection: Connection,
    auth: GameAuthorization,
    mut send: SendStream,
    mut recv: RecvStream,
    deadline: Instant,
) -> io::Result<GameConnection> {
    let mut pending = PendingConnection(Some(connection.clone()));
    let result = tokio::time::timeout_at(deadline, async {
        let proof: GameProof = read_handshake(&mut recv).await?;
        auth.validate(&connection, &proof)?;
        send_handshake(&mut send, &auth.proof()).await?;
        send.finish().map_err(|_| failed())?;
        Ok(GameConnection {
            connection,
            authorization: auth,
        })
    })
    .await
    .map_err(|_| failed())?;
    if result.is_ok() {
        pending.0 = None;
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bridge::Bridge;
    use std::{net::Ipv4Addr, sync::atomic::Ordering};
    use tokio::{net::UdpSocket, sync::watch};

    async fn local_endpoint() -> Endpoint {
        Endpoint::builder(presets::Minimal)
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .unwrap()
            .alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()])
            .portmapper_config(PortmapperConfig::Disabled)
            .bind()
            .await
            .unwrap()
    }

    fn address(endpoint: &Endpoint) -> EndpointAddr {
        EndpointAddr::new(endpoint.id()).with_ip_addr(endpoint.bound_sockets()[0])
    }

    fn room(endpoint: &Endpoint) -> Invite {
        let relay = iroh::defaults::prod::default_relay_map()
            .urls::<Vec<_>>()
            .remove(0);
        Invite::create(
            endpoint.id(),
            relay,
            "same-sidecar-build".into(),
            now().unwrap(),
            3600,
        )
        .unwrap()
    }

    async fn control_pair(a: &Endpoint, b: &Endpoint) -> (ControlChannel, ControlChannel) {
        let invite = room(b);
        let (client, server) = tokio::join!(
            async {
                let connection = a.connect(address(b), CONTROL_ALPN).await.unwrap();
                connect_control_on(connection, &invite).await.unwrap()
            },
            async {
                let connection = b.accept().await.unwrap().await.unwrap();
                accept_control(connection, &invite).await.unwrap()
            }
        );
        (client, server)
    }

    async fn game_pair(
        a: &Endpoint,
        b: &Endpoint,
        generation: u64,
    ) -> (GameConnection, GameConnection) {
        let key = MatchKey {
            room: [13; 16],
            generation,
        };
        let a_auth = GameAuthorization {
            peer: b.id(),
            key,
            capability: [91; 32],
            max_packet: 1024,
        };
        let b_auth = GameAuthorization {
            peer: a.id(),
            ..a_auth.clone()
        };
        let (client, server) = tokio::join!(connect_game(a, address(b), a_auth), async {
            let connection = b.accept().await.unwrap().await.unwrap();
            accept_game(connection, b_auth).await
        });
        (client.unwrap(), server.unwrap())
    }

    #[tokio::test]
    async fn expired_invite_reauthenticates_only_the_admitted_endpoint_after_handoff() {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let successor = local_endpoint().await;
        let invite = room(&b);
        let clock = now().unwrap() + 7200;
        for (server, member, allowed) in [
            (&b, None, false),
            (&b, Some(a.id()), true),
            (&successor, Some(a.id()), true),
            (&b, Some(b.id()), false),
        ] {
            let (client, accepted) = tokio::join!(
                async {
                    let connection = a.connect(address(server), CONTROL_ALPN).await.unwrap();
                    connect_control_on_expected(connection, &invite, Some(server.id())).await
                },
                async {
                    let connection = server.accept().await.unwrap().await.unwrap();
                    accept_control_policy(connection, &invite, member, clock).await
                }
            );
            assert_eq!(client.is_ok(), allowed);
            assert_eq!(accepted.is_ok(), allowed);
        }
        a.close().await;
        b.close().await;
        successor.close().await;
    }

    #[tokio::test]
    async fn partial_game_proof_uses_remaining_marker_deadline_and_releases_connection() {
        let a = local_endpoint().await;
        let b = local_endpoint().await;
        let (client, server) = tokio::join!(a.connect(address(&b), GAME_ALPN), async {
            b.accept().await.unwrap().await
        });
        let client = client.unwrap();
        let server = server.unwrap();
        let (mut send, _recv) = client.open_bi().await.unwrap();
        send.write_all(&GAME_PLAY_MAGIC).await.unwrap();
        let started = Instant::now();
        let deadline = started + Duration::from_millis(250);
        let GameStream::Gameplay(tx, rx) =
            accept_game_stream_until(&server, deadline).await.unwrap()
        else {
            panic!("wrong purpose")
        };
        tokio::time::sleep(Duration::from_millis(150)).await;
        let auth = GameAuthorization {
            peer: a.id(),
            key: MatchKey {
                room: [39; 16],
                generation: 1,
            },
            capability: [11; 32],
            max_packet: 1024,
        };
        assert!(
            timeout(
                Duration::from_millis(300),
                accept_game_stream_with_until(server, auth, tx, rx, deadline)
            )
            .await
            .unwrap()
            .is_err()
        );
        assert!(started.elapsed() < Duration::from_millis(500));
        timeout(Duration::from_secs(1), client.closed())
            .await
            .unwrap();
        let _new_generation = game_pair(&a, &b, 2).await;
        a.close().await;
        b.close().await;
    }

    #[tokio::test]
    async fn game_dial_retries_until_matching_listener_is_installed() {
        timeout(Duration::from_secs(15), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let key = MatchKey {
                room: [27; 16],
                generation: 4,
            };
            let a_auth = GameAuthorization {
                peer: b.id(),
                key,
                capability: [71; 32],
                max_packet: 1024,
            };
            let b_auth = GameAuthorization {
                peer: a.id(),
                ..a_auth.clone()
            };
            let (client, server) = tokio::join!(connect_game(&a, address(&b), a_auth), async {
                // Model the remote helper receiving the GAME_ALPN dial
                // before its prepare_game command. The next bounded dial
                // must use the listener installed immediately afterward.
                let premature = b.accept().await.unwrap().await.unwrap();
                premature.close(1u32.into(), b"gameplay not authorized");
                let prepared = b.accept().await.unwrap().await.unwrap();
                accept_game(prepared, b_auth).await
            });
            assert!(client.is_ok());
            assert!(server.is_ok());
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn prepared_probe_connection_waits_past_handshake_timeout_for_game_connect() {
        timeout(Duration::from_secs(20), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let (client_connection, server_connection) =
                tokio::join!(a.connect(address(&b), GAME_ALPN), async {
                    b.accept().await.unwrap().await
                });
            let client_connection = client_connection.unwrap();
            let server_connection = server_connection.unwrap();
            let key = MatchKey {
                room: [39; 16],
                generation: 7,
            };
            let client_auth = GameAuthorization {
                peer: b.id(),
                key,
                capability: [53; 32],
                max_packet: 1024,
            };
            let server_auth = GameAuthorization {
                peer: a.id(),
                ..client_auth.clone()
            };
            let (_deadline_sender, deadline) =
                watch::channel(Instant::now() + PREPARED_GAME_TIMEOUT);
            let server = tokio::spawn(accept_game_on(server_connection, server_auth, deadline));

            // The room journal commits game_connect after every participant's
            // game_prepared acknowledgement. This intentional pre-stream gap
            // is not part of the on-wire handshake timeout.
            tokio::time::sleep(HANDSHAKE_TIMEOUT + Duration::from_secs(1)).await;
            let client = connect_game_on(client_connection, client_auth).await;
            assert!(client.is_ok());
            assert!(server.await.unwrap().is_ok());
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn prepared_stream_deadline_extends_during_recovery_pause() {
        timeout(Duration::from_secs(2), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let (client_connection, server_connection) =
                tokio::join!(a.connect(address(&b), GAME_ALPN), async {
                    b.accept().await.unwrap().await
                });
            let client_connection = client_connection.unwrap();
            let server_connection = server_connection.unwrap();
            let (deadline_sender, deadline) =
                watch::channel(Instant::now() + Duration::from_millis(25));
            let server =
                tokio::spawn(
                    async move { accept_prepared_stream(&server_connection, deadline).await },
                );
            tokio::time::sleep(Duration::from_millis(10)).await;
            deadline_sender.send_replace(Instant::now() + Duration::from_secs(1));
            tokio::time::sleep(Duration::from_millis(40)).await;
            let (mut send, _recv) = client_connection.open_bi().await.unwrap();
            send.write_all(&[0]).await.unwrap();
            assert!(server.await.unwrap().is_ok());
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn actual_quic_control_preserves_ready_ids_and_rejects_replays() {
        timeout(Duration::from_secs(15), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let (mut client, mut server) = control_pair(&a, &b).await;
            let frame = ControlFrame {
                message_id: 1234,
                payload: br#"{"type":"lobby_ready"}"#.to_vec(),
            };
            client.sender.send(&frame).await.unwrap();
            assert_eq!(server.receiver.receive().await.unwrap(), frame);
            assert!(client.sender.send(&frame).await.is_err());
            client.connection.close(0u32.into(), b"test finished");
            assert!(server.receiver.receive().await.is_err());
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn wrong_room_capability_is_rejected_before_control_delivery() {
        timeout(Duration::from_secs(15), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let expected = room(&b);
            let wrong = room(&b);
            let (client, server) = tokio::join!(
                async {
                    let connection = a.connect(address(&b), CONTROL_ALPN).await.unwrap();
                    connect_control_on(connection, &wrong).await
                },
                async {
                    let connection = b.accept().await.unwrap().await.unwrap();
                    accept_control(connection, &expected).await
                }
            );
            assert!(client.is_err());
            assert!(server.is_err());
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn control_workers_bound_both_directions_and_backpressure_the_consumer() {
        use crate::control::{CONTROL_QUEUE_CAPACITY, ControlWorker, QueueError};
        timeout(Duration::from_secs(15), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let (client, server) = control_pair(&a, &b).await;
            let sender = ControlWorker::start(client);
            let mut receiver = ControlWorker::start(server);
            // This current-thread executor cannot drain the queue until yield.
            for id in 2..(CONTROL_QUEUE_CAPACITY as u64 + 2) {
                sender
                    .try_send(ControlFrame {
                        message_id: id,
                        payload: vec![1],
                    })
                    .unwrap();
            }
            assert_eq!(
                sender.try_send(ControlFrame {
                    message_id: 999,
                    payload: vec![1]
                }),
                Err(QueueError::Full)
            );
            assert_eq!(
                sender.try_send(ControlFrame {
                    message_id: 1000,
                    payload: vec![1; wire::MAX_CONTROL_PAYLOAD + 1]
                }),
                Err(QueueError::Invalid)
            );
            let mut id = CONTROL_QUEUE_CAPACITY as u64 + 2;
            let end = CONTROL_QUEUE_CAPACITY as u64 * 3 + 2;
            while id < end {
                match sender.try_send(ControlFrame {
                    message_id: id,
                    payload: vec![1],
                }) {
                    Ok(()) => id += 1,
                    Err(QueueError::Full) => (),
                    Err(QueueError::Closed) => panic!("backpressure closed the control route"),
                    Err(QueueError::Invalid) => panic!("valid frame rejected"),
                }
                tokio::task::yield_now().await;
            }
            assert!(!receiver.is_closed());
            let mut expected = 2;
            while expected < end {
                if let Some(frame) = receiver.try_receive() {
                    assert_eq!(frame.message_id, expected);
                    expected += 1;
                } else {
                    tokio::task::yield_now().await;
                }
            }
            assert!(!receiver.is_closed());
            assert_eq!(receiver.rejected(), 0);
            drop(sender);
            drop(receiver);
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn interrupted_control_read_cannot_resume_at_a_corrupted_boundary() {
        timeout(Duration::from_secs(15), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let (client, mut server) = control_pair(&a, &b).await;
            assert!(
                timeout(Duration::from_millis(10), server.receiver.receive())
                    .await
                    .is_err()
            );
            assert!(server.receiver.receive().await.is_err());
            client.connection.close(0u32.into(), b"test ended");
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn wrong_match_capability_and_peer_identity_are_rejected() {
        timeout(Duration::from_secs(15), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            for wrong_identity in [false, true] {
                let key = MatchKey {
                    room: [13; 16],
                    generation: 1,
                };
                let a_auth = GameAuthorization {
                    peer: b.id(),
                    key,
                    capability: [1; 32],
                    max_packet: 1024,
                };
                let b_auth = GameAuthorization {
                    peer: if wrong_identity { b.id() } else { a.id() },
                    capability: if wrong_identity { [1; 32] } else { [2; 32] },
                    ..a_auth.clone()
                };
                let (client, server) = tokio::join!(connect_game(&a, address(&b), a_auth), async {
                    accept_game(b.accept().await.unwrap().await.unwrap(), b_auth).await
                });
                assert!(client.is_err());
                assert!(server.is_err());
            }
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn raw_udp_survives_control_close_and_filters_stale_remote_and_local_packets() {
        timeout(Duration::from_secs(20), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let (control_a, control_b) = control_pair(&a, &b).await;
            let (game_a, game_b) = game_pair(&a, &b, 1).await;
            let raw_sender = game_a.connection.clone();
            let local_a = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            let local_b = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            let bridge_a = Bridge::bind(game_a, local_a.local_addr().unwrap())
                .await
                .unwrap();
            let bridge_b = Bridge::bind(game_b, local_b.local_addr().unwrap())
                .await
                .unwrap();
            let virtual_a = bridge_a.local_addr().unwrap();
            let virtual_b = bridge_b.local_addr().unwrap();
            let stats_b = bridge_b.stats.clone();
            let (stop, stop_rx) = watch::channel(false);
            let a_task = tokio::spawn(bridge_a.run(stop_rx.clone()));
            let b_task = tokio::spawn(bridge_b.run(stop_rx));
            control_a
                .connection
                .close(0u32.into(), b"room channel test");
            control_b.connection.closed().await;

            let stale = MatchKey {
                room: [13; 16],
                generation: 2,
            }
            .encode(&[88; 16], 1200)
            .unwrap();
            raw_sender.send_datagram(stale.into()).unwrap();
            let intruder = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            intruder
                .send_to(b"wrong local port", virtual_a)
                .await
                .unwrap();

            // With no rendering or game-frame tick, packets still flow in both
            // directions and arrive from the registered virtual endpoint.
            let mut buffer = [0; 2048];
            for index in 0..100u32 {
                let mut payload = vec![0xA5; 1024];
                payload[..4].copy_from_slice(&index.to_be_bytes());
                local_a.send_to(&payload, virtual_a).await.unwrap();
                let (size, source) = local_b.recv_from(&mut buffer).await.unwrap();
                assert_eq!(source, virtual_b);
                assert_eq!(buffer[..size], payload);
                local_b.send_to(&payload, virtual_b).await.unwrap();
                let (size, source) = local_a.recv_from(&mut buffer).await.unwrap();
                assert_eq!(source, virtual_a);
                assert_eq!(buffer[..size], payload);
            }
            assert_eq!(stats_b.received_packets.load(Ordering::Relaxed), 100);
            assert!(stats_b.rejected_packets.load(Ordering::Relaxed) >= 1);
            stop.send(true).unwrap();
            a_task.await.unwrap().unwrap();
            b_task.await.unwrap().unwrap();
            assert!(raw_sender.close_reason().is_some());
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn fifty_match_generations_reuse_endpoints_and_close_each_mapping() {
        timeout(Duration::from_secs(45), async {
            let a = local_endpoint().await;
            let b = local_endpoint().await;
            let (mut control_a, mut control_b) = control_pair(&a, &b).await;
            for generation in 1..=50 {
                let (game_a, game_b) = game_pair(&a, &b, generation).await;
                let local_a = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
                let local_b = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
                let bridge_a = Bridge::bind(game_a, local_a.local_addr().unwrap())
                    .await
                    .unwrap();
                let bridge_b = Bridge::bind(game_b, local_b.local_addr().unwrap())
                    .await
                    .unwrap();
                let virtual_a = bridge_a.local_addr().unwrap();
                let (stop, rx) = watch::channel(false);
                let a_task = tokio::spawn(bridge_a.run(rx.clone()));
                let b_task = tokio::spawn(bridge_b.run(rx));
                local_a
                    .send_to(&generation.to_be_bytes(), virtual_a)
                    .await
                    .unwrap();
                let mut bytes = [0; 64];
                let count = local_b.recv(&mut bytes).await.unwrap();
                assert_eq!(bytes[..count], generation.to_be_bytes());
                stop.send(true).unwrap();
                // Either side can observe its peer's close before local stop.
                let _ = a_task.await.unwrap();
                let _ = b_task.await.unwrap();
            }
            let frame = ControlFrame {
                message_id: 500,
                payload: b"room still alive".to_vec(),
            };
            control_a.sender.send(&frame).await.unwrap();
            assert_eq!(control_b.receiver.receive().await.unwrap(), frame);
            a.close().await;
            b.close().await;
        })
        .await
        .unwrap();
    }
}
