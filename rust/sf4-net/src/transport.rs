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

pub mod fixed_port;

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
        || Endpoint::builder(presets::N0).alpns(vec![CONTROL_ALPN.to_vec(), GAME_ALPN.to_vec()]);
    let bound = if relay_only {
        builder()
            .clear_ip_transports()
            .portmapper_config(PortmapperConfig::Disabled)
            .bind()
            .await
    } else {
        fixed_port::bind(builder, &fixed_port::PORTS).await
    };
    bound.map_err(|_| failed())
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
#[cfg(test)]
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

#[cfg(test)]
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
mod tests;
