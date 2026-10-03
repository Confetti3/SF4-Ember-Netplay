//! Authenticated, bounded room-consensus transport. This endpoint is separate
//! from the gameplay endpoint: shutting down consensus cannot retire GGPO links.
use crate::coordination::{
    AuthorityClaim, Coordinator, MAX_PROPOSE_REQUEST, MAX_RPC_RESPONSE, MAX_SNAPSHOT,
    MAX_SNAPSHOT_REQUEST, MAX_VOTE_REQUEST, Ownership, RpcTransport, SNAPSHOT_FRAGMENT_BYTES,
};
use retired::RetiredFilter;
use iroh::{
    Endpoint, EndpointAddr, EndpointId,
    endpoint::{Connection, PortmapperConfig, presets},
};
use std::{
    collections::BTreeMap,
    future::Future,
    io,
    pin::Pin,
    sync::Arc,
    time::{Duration, Instant},
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    sync::{RwLock, Semaphore},
    task::JoinSet,
    time::{sleep, timeout},
};

const ALPN: &[u8] = b"sf4e/coordination/1";
const MAX_RPC: usize = MAX_SNAPSHOT;
const RPC_TIMEOUT: Duration = Duration::from_secs(10);
const CONNECTION_IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const MAX_CONNECTION_STREAMS: usize = 8;
/// A caller keeps one cached connection per target and redials after a
/// canceled RPC closes it, so two cover the old connection winding down.
const MAX_PEER_CONNECTIONS: usize = 2;
/// Request bytes held at once, per authenticated endpoint and in total. One
/// endpoint's allowance fits a full append beside its smaller RPCs, and the
/// total leaves the same room for the rest of the room.
const PEER_BODY_BUDGET: usize = 2 * MAX_RPC;
const BODY_BUDGET: usize = 4 * MAX_RPC;
const RELAY_READY_TIMEOUT: Duration = Duration::from_secs(10);
/// How long the address handed to a peer waits for this endpoint's relay
/// route. A new endpoint registers with its relay about a second after it
/// binds; an address taken before then can leave a peer with no path that
/// works, and its first Raft RPC to us then waits out OpenRaft's deadline.
/// Past this the address goes out with what it has (its direct addresses).
const ADVERTISE_READY_TIMEOUT: Duration = Duration::from_secs(3);
const CHUNK: usize = SNAPSHOT_FRAGMENT_BYTES;
mod retired;
fn failure() -> io::Error {
    io::Error::other("room coordination transport unavailable")
}

pub struct IrohRpc {
    endpoint: Endpoint,
    room: [u8; 16],
    incarnation: u64,
    // Populated by authenticated admission, never by an incoming RPC. Routes
    // bind the Raft process incarnation to the exact endpoint public key.
    members: RwLock<BTreeMap<u64, MemberBinding>>,
    // A fixed-size tombstone filter lets expired route records be evicted
    // without ever making the exact retired incarnation admissible again.
    // False positives fail closed; the filter has no false negatives.
    retired_filter: RwLock<RetiredFilter>,
    /// The room's size limit applies to the connections and bindings this
    /// endpoint holds; see `Ownership::max_nodes`.
    limit: usize,
    connections: RwLock<BTreeMap<u64, Connection>>,
    /// One dial gate per target. Only its holder opens the target's
    /// connection, so concurrent RPCs share one cached connection instead of
    /// each dialing its own. The gate is held only while dialing.
    dials: std::sync::Mutex<BTreeMap<u64, Arc<tokio::sync::Mutex<()>>>>,
    in_flight: Semaphore,
    /// Served connections and the request byte allowance of each remote
    /// endpoint. An entry lives while that endpoint has a served connection.
    peers: std::sync::Mutex<BTreeMap<EndpointId, PeerLoad>>,
    body_budget: Semaphore,
}

struct PeerLoad {
    connections: usize,
    bytes: Arc<Semaphore>,
}

/// One served connection of a remote endpoint, counted until it ends.
struct PeerSlot {
    owner: Arc<IrohRpc>,
    peer: EndpointId,
    bytes: Arc<Semaphore>,
}

impl PeerSlot {
    fn claim(owner: &Arc<IrohRpc>, peer: EndpointId) -> Option<Self> {
        let mut peers = owner.peers.lock().unwrap_or_else(|e| e.into_inner());
        let load = peers.entry(peer).or_insert_with(|| PeerLoad {
            connections: 0,
            bytes: Arc::new(Semaphore::new(PEER_BODY_BUDGET)),
        });
        if load.connections >= MAX_PEER_CONNECTIONS {
            return None;
        }
        load.connections += 1;
        Some(Self {
            owner: owner.clone(),
            peer,
            bytes: load.bytes.clone(),
        })
    }
}

impl Drop for PeerSlot {
    fn drop(&mut self) {
        let mut peers = self.owner.peers.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(load) = peers.get_mut(&self.peer) {
            load.connections = load.connections.saturating_sub(1);
            if load.connections == 0 {
                peers.remove(&self.peer);
            }
        }
    }
}

/// The largest request body each method can carry. Only an append holds a
/// checkpoint; the single-byte methods carry a fixed marker.
fn request_ceiling(method: &str) -> usize {
    match method {
        "append" => MAX_RPC,
        "vote" => MAX_VOTE_REQUEST,
        "snapshot" => MAX_SNAPSHOT_REQUEST,
        "propose" => MAX_PROPOSE_REQUEST,
        _ => 1,
    }
}

#[derive(Clone, Debug)]
struct MemberBinding {
    coordination: EndpointAddr,
    primary: Option<EndpointId>,
    /// A committed departure keeps a short-lived, read-only route so the
    /// departing helper can query the successor's voter set after retain=false
    /// stops replicating membership to it. All Raft-mutating methods remain
    /// rejected while this marker is present.
    retired_until: Option<Instant>,
}

struct ConnectionUseGuard(Option<Connection>);

impl ConnectionUseGuard {
    fn new(connection: Connection) -> Self {
        Self(Some(connection))
    }

    fn succeeded(&mut self) {
        self.0 = None;
    }
}

impl Drop for ConnectionUseGuard {
    fn drop(&mut self) {
        if let Some(connection) = self.0.take() {
            connection.close(1u32.into(), b"room RPC canceled");
        }
    }
}
impl IrohRpc {
    pub async fn bind(room: [u8; 16], incarnation: u64, relay_only: bool) -> io::Result<Arc<Self>> {
        Self::bind_on(room, incarnation, relay_only, None).await
    }

    /// `bind` with the IPv4 socket on exactly `port`, with no fallback to
    /// another one. A headless room host is given the port it must hold.
    pub async fn bind_on(
        room: [u8; 16],
        incarnation: u64,
        relay_only: bool,
        port: Option<u16>,
    ) -> io::Result<Arc<Self>> {
        Self::bind_owned(room, incarnation, relay_only, port, Ownership::Private).await
    }

    /// `bind_on` for a node that takes part in its room as `ownership`.
    pub async fn bind_owned(
        room: [u8; 16],
        incarnation: u64,
        relay_only: bool,
        port: Option<u16>,
        ownership: Ownership,
    ) -> io::Result<Arc<Self>> {
        if incarnation == 0 || room == [0; 16] {
            return Err(failure());
        }
        // Same reasoning as the gameplay endpoint in transport.rs: take iroh's
        // default port mapping so coordination can also form a direct path
        // behind NATs that need a gateway mapping.
        let builder = Endpoint::builder(presets::N0).alpns(vec![ALPN.to_vec()]);
        let builder = if relay_only {
            builder
                .clear_ip_transports()
                .portmapper_config(PortmapperConfig::Disabled)
        } else {
            builder
        };
        let builder = match port {
            Some(port) => builder
                .bind_addr((std::net::Ipv4Addr::UNSPECIFIED, port))
                .map_err(|_| failure())?,
            None => builder,
        };
        let endpoint = builder.bind().await.map_err(|_| failure())?;
        // Binding only allocates the local socket. In relay-only mode an
        // immediately advertised address has no relay route yet, so a new
        // learner's first Raft RPC pays relay selection, registration, and
        // QUIC setup inside OpenRaft's heartbeat deadline. Wait until Iroh has
        // completed a relay handshake before publishing this endpoint.
        if relay_only
            && timeout(RELAY_READY_TIMEOUT, endpoint.online())
                .await
                .is_err()
        {
            endpoint.close().await;
            return Err(failure());
        }
        Ok(Arc::new(Self {
            endpoint,
            room,
            incarnation,
            members: RwLock::new(BTreeMap::new()),
            retired_filter: RwLock::new(RetiredFilter::new(room, ownership)),
            limit: ownership.max_nodes(),
            connections: RwLock::new(BTreeMap::new()),
            dials: std::sync::Mutex::new(BTreeMap::new()),
            in_flight: Semaphore::new(ownership.max_nodes()),
            peers: std::sync::Mutex::new(BTreeMap::new()),
            body_budget: Semaphore::new(BODY_BUDGET),
        }))
    }
    pub fn address(&self) -> EndpointAddr {
        self.endpoint.addr()
    }
    /// This endpoint's address for a peer to dial: as it is once the relay
    /// route is in, or after `ADVERTISE_READY_TIMEOUT` as it is then.
    pub async fn advertised_address(&self) -> EndpointAddr {
        let _ = timeout(ADVERTISE_READY_TIMEOUT, self.endpoint.online()).await;
        self.endpoint.addr()
    }
    pub fn identity(&self) -> EndpointId {
        self.endpoint.id()
    }
    pub async fn admit(&self, incarnation: u64, address: EndpointAddr) -> io::Result<()> {
        self.admit_bound(incarnation, address, None).await
    }

    /// Admission is the only route mutation.  The control stream supplies
    /// the primary game endpoint binding; incoming coordination RPCs never
    /// get to populate this table themselves.
    pub async fn admit_bound(
        &self,
        incarnation: u64,
        address: EndpointAddr,
        primary: impl Into<Option<EndpointId>>,
    ) -> io::Result<()> {
        let primary = primary.into();
        // This process never routes RPCs to itself, so its own incarnation
        // and coordination key are never bound to a route.
        if incarnation == self.incarnation
            || address.id == self.endpoint.id()
            || self.retired_filter_contains(incarnation).await
        {
            return Err(failure());
        }
        let now = Instant::now();
        let expired: Vec<u64> = {
            let mut members = self.members.write().await;
            let expired: Vec<_> = members
                .iter()
                .filter_map(|(id, known)| {
                    known
                        .retired_until
                        .is_some_and(|retired_until| retired_until <= now)
                        .then_some(*id)
                })
                .collect();
            for id in &expired {
                members.remove(id);
            }
            expired
        };
        for id in expired {
            self.forget_dial(id);
            if let Some(connection) = self.connections.write().await.remove(&id) {
                connection.close(1u32.into(), b"retired room route expired");
            }
        }
        let mut members = self.members.write().await;
        if incarnation == 0
            || (members
                .values()
                .filter(|known| known.retired_until.is_none())
                .count()
                >= self.limit
                && !members.contains_key(&incarnation))
            || members
                .iter()
                .any(|(id, known)| {
                    *id != incarnation
                        && known.retired_until.is_none()
                        && known.coordination.id == address.id
                })
            // A primary game endpoint is an authenticated, process-local
            // binding.  Reusing it for a second live incarnation would let
            // a stale process receive the new member's gameplay/control
            // routing, so the binding must remain one-to-one until the old
            // incarnation is explicitly revoked.
            || primary.as_ref().is_some_and(|candidate| {
                members.iter().any(|(id, known)| {
                    *id != incarnation
                        && known.retired_until.is_none()
                        && known.primary.as_ref() == Some(candidate)
                })
            })
        {
            return Err(failure());
        }
        if let Some(known) = members.get_mut(&incarnation) {
            // A retired incarnation is a permanent tombstone. A restarted
            // process receives a fresh random incarnation; replaying an old
            // Admission must never reopen its Raft mutation route.
            if known.retired_until.is_some() {
                return Err(failure());
            }
            if known.coordination.id != address.id
                || primary.is_some_and(|candidate| {
                    known.primary.is_some_and(|existing| existing != candidate)
                })
            {
                return Err(failure());
            }
            known.coordination = address;
            if primary.is_some() {
                known.primary = primary;
            }
            return Ok(());
        }
        members.insert(
            incarnation,
            MemberBinding {
                coordination: address,
                primary,
                retired_until: None,
            },
        );
        Ok(())
    }
    /// Preserve an authenticated route for a bounded departure proof. The
    /// route is accepted only for the authority read in `serve`; vote,
    /// append, snapshot, and proposal RPCs remain fenced immediately.
    pub async fn retire(&self, incarnation: u64) {
        self.retired_filter_insert(incarnation).await;
        if let Some(binding) = self.members.write().await.get_mut(&incarnation) {
            binding
                .retired_until
                .get_or_insert(Instant::now() + Duration::from_secs(30));
        }
    }
    pub async fn revoke(&self, incarnation: u64) {
        self.members.write().await.remove(&incarnation);
        self.forget_dial(incarnation);
        if let Some(connection) = self.connections.write().await.remove(&incarnation) {
            connection.close(1u32.into(), b"room membership ended");
        }
    }

    fn forget_dial(&self, target: u64) {
        self.dials
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&target);
    }

    async fn cached_connection(&self, target: u64) -> Option<Connection> {
        self.connections
            .read()
            .await
            .get(&target)
            .filter(|c| c.close_reason().is_none())
            .cloned()
    }

    /// The open cached connection to `target`, dialed once if there is none.
    /// Callers that arrive while a dial is in progress wait for it and then
    /// share its connection. A caller canceled while dialing releases the
    /// gate, and the next caller dials in its place.
    async fn route(&self, target: u64, address: EndpointAddr) -> io::Result<Connection> {
        if let Some(connection) = self.cached_connection(target).await {
            return Ok(connection);
        }
        let gate = self
            .dials
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .entry(target)
            .or_default()
            .clone();
        let _dialing = gate.lock().await;
        if let Some(connection) = self.cached_connection(target).await {
            return Ok(connection);
        }
        let connection = self
            .endpoint
            .connect(address, ALPN)
            .await
            .map_err(|_| failure())?;
        self.connections
            .write()
            .await
            .insert(target, connection.clone());
        Ok(connection)
    }

    async fn retired_filter_contains(&self, incarnation: u64) -> bool {
        self.retired_filter.read().await.contains(incarnation)
    }

    async fn retired_filter_insert(&self, incarnation: u64) {
        self.retired_filter.write().await.insert(incarnation);
    }

    pub async fn primary_members(&self) -> Vec<EndpointId> {
        self.members
            .read()
            .await
            .values()
            .filter(|binding| binding.retired_until.is_none())
            .filter_map(|binding| binding.primary)
            .collect()
    }

    pub async fn is_retired(&self, incarnation: u64) -> bool {
        self.members
            .read()
            .await
            .get(&incarnation)
            .is_some_and(|binding| binding.retired_until.is_some())
    }

    /// Whether this process has an authenticated coordination binding for the
    /// incarnation.  A successor uses this provenance when it receives a
    /// replayed Admission after the Raft membership has already excluded the
    /// old process but the former leader died before broadcasting its local
    /// retirement tombstone.
    pub async fn has_binding(&self, incarnation: u64) -> bool {
        self.members.read().await.contains_key(&incarnation)
    }

    /// Query a successor over the already authenticated incarnation binding.
    /// The peer performs a linearizable read before returning its claim, so a
    /// stale term advertisement cannot satisfy a retiring helper.
    pub async fn authority_claim(&self, target: u64) -> io::Result<AuthorityClaim> {
        let expected = self
            .members
            .read()
            .await
            .get(&target)
            .map(|binding| binding.coordination.id.to_string())
            .ok_or_else(failure)?;
        let bytes =
            <Self as RpcTransport>::call(self, target, expected, "authority", vec![0]).await?;
        serde_json::from_slice(&bytes).map_err(|_| failure())
    }

    /// Ask an admitted voter to trigger the committed handoff election. The
    /// receiver authenticates that this route is still its current leader.
    pub async fn request_election(&self, target: u64) -> io::Result<()> {
        let expected = self
            .members
            .read()
            .await
            .get(&target)
            .map(|binding| binding.coordination.id.to_string())
            .ok_or_else(failure)?;
        let bytes = <Self as RpcTransport>::call(self, target, expected, "elect", vec![0]).await?;
        if bytes != [1] {
            return Err(failure());
        }
        Ok(())
    }

    /// Ask the current leader to commit removal of this authenticated source
    /// from the voter set.  This is used only by a coordination learner that
    /// joined the room but never entered the native room roster; native
    /// roster departures continue to use the checkpoint reconciliation path.
    pub async fn request_self_removal(&self, target: u64) -> io::Result<()> {
        let expected = self
            .members
            .read()
            .await
            .get(&target)
            .map(|binding| binding.coordination.id.to_string())
            .ok_or_else(failure)?;
        let bytes = <Self as RpcTransport>::call(self, target, expected, "remove", vec![0]).await?;
        if bytes != [1] {
            return Err(failure());
        }
        Ok(())
    }
    pub async fn close(&self) {
        self.endpoint.close().await;
    }

    pub async fn serve(self: Arc<Self>, coordinator: Arc<Coordinator>) -> io::Result<()> {
        let mut tasks = JoinSet::new();
        loop {
            tokio::select! {
                incoming = self.endpoint.accept() => {
                    let Some(incoming) = incoming else { return Ok(()); };
                    if tasks.len() >= self.limit { incoming.refuse(); continue; }
                    let owner = self.clone(); let coordinator = coordinator.clone();
                    tasks.spawn(async move {
                        let Ok(Ok(connection)) = timeout(RPC_TIMEOUT, incoming).await else { return; };
                        let now = Instant::now();
                        if connection.alpn() != ALPN || !owner.members.read().await.values().any(|a| {
                            a.coordination.id == connection.remote_id()
                                && !a.retired_until.is_some_and(|until| until <= now)
                        }) {
                            connection.close(1u32.into(), b"room member required"); return;
                        }
                        // Each endpoint holds at most a couple of served
                        // connections, so one endpoint never takes the shared
                        // connection limit from the other members.
                        let Some(slot) = PeerSlot::claim(&owner, connection.remote_id()) else {
                            connection.close(1u32.into(), b"room RPC connection limit"); return;
                        };
                        // Serve the streams of one connection concurrently. A large
                        // checkpoint append must not queue the authority reads and
                        // heartbeats behind it: both sides would then report quorum
                        // lost while the entry is still replicating.
                        let mut streams = JoinSet::new();
                        loop {
                            // The idle window only runs while nothing is in flight:
                            // a checkpoint append that outlives it must not lose its
                            // route, and the response stream it is writing to.
                            let idle = sleep(CONNECTION_IDLE_TIMEOUT);
                            tokio::pin!(idle);
                            let (mut send, mut recv) = tokio::select! {
                                _ = &mut idle, if streams.is_empty() => {
                                    connection.close(1u32.into(), b"room RPC connection idle");
                                    return;
                                },
                                accepted = connection.accept_bi(),
                                    if streams.len() < MAX_CONNECTION_STREAMS => {
                                    let Ok(stream) = accepted else {
                                        connection.close(1u32.into(), b"room RPC connection idle");
                                        return;
                                    };
                                    stream
                                },
                                // A rejected or abandoned stream fails alone; the
                                // caller sees its reset. Closing here would also
                                // kill the healthy streams beside it.
                                _ = streams.join_next(), if !streams.is_empty() => continue,
                            };
                            let owner = owner.clone(); let coordinator = coordinator.clone();
                            let connection = connection.clone();
                            let peer_bytes = slot.bytes.clone();
                            streams.spawn(timeout(RPC_TIMEOUT, async move {
                                let mut room = [0; 16]; recv.read_exact(&mut room).await.map_err(|_| failure())?;
                                let source = recv.read_u64().await?;
                                let target = recv.read_u64().await?;
                                let method = recv.read_u8().await?;
                                let retired = owner.members.read().await.get(&source).and_then(|a| {
                                    (a.coordination.id == connection.remote_id()).then_some(a.retired_until)
                                });
                                let Some(retired) = retired else { return Err(failure()); };
                                if retired.is_some_and(|until| until <= Instant::now()) {
                                    return Err(failure());
                                }
                                if room != owner.room || target != owner.incarnation { return Err(failure()); }
                                let method = match method { 1 => "append", 2 => "vote", 3 => "snapshot", 4 => "authority", 5 => "propose", 6 => "elect", 7 => "remove", _ => return Err(failure()) };
                                if retired.is_some() && method != "authority" { return Err(failure()); }
                                // The announced size is held against the endpoint's
                                // and the shared allowance before any of it is read,
                                // and the body grows only as its bytes arrive.
                                let size = read_size(&mut recv, request_ceiling(method)).await?;
                                let _peer_bytes = peer_bytes.acquire_many(size as u32).await.map_err(|_| failure())?;
                                let _bytes = owner.body_budget.acquire_many(size as u32).await.map_err(|_| failure())?;
                                let body = read_sized(&mut recv, size).await?;
                                let response = coordinator.dispatch(source, method, &body).await?;
                                write_body(&mut send, &response).await?;
                                send.finish().map_err(|_| failure())?;
                                if method == "remove" && response == [1] {
                                    owner.retire(source).await;
                                }
                                Ok::<(), io::Error>(())
                            }));
                        }
                    });
                },
                result = tasks.join_next(), if !tasks.is_empty() => {
                    if result.is_some_and(|result| result.is_err()) { return Err(failure()); }
                },
            }
        }
    }
}
async fn read_size(
    reader: &mut (impl tokio::io::AsyncRead + Unpin),
    ceiling: usize,
) -> io::Result<usize> {
    let size = reader.read_u32().await? as usize;
    if size == 0 || size > ceiling.min(MAX_RPC) {
        return Err(failure());
    }
    Ok(size)
}
/// Read a body of the announced size one chunk at a time, so memory follows
/// the bytes actually received rather than the size a header announced.
async fn read_sized(
    reader: &mut (impl tokio::io::AsyncRead + Unpin),
    size: usize,
) -> io::Result<Vec<u8>> {
    let mut data = Vec::with_capacity(size.min(CHUNK));
    let mut chunk = vec![0; size.min(CHUNK)];
    while data.len() < size {
        let part = &mut chunk[..(size - data.len()).min(CHUNK)];
        reader.read_exact(part).await?;
        data.extend_from_slice(part);
    }
    Ok(data)
}
async fn read_body(
    reader: &mut (impl tokio::io::AsyncRead + Unpin),
    ceiling: usize,
) -> io::Result<Vec<u8>> {
    let size = read_size(reader, ceiling).await?;
    read_sized(reader, size).await
}
async fn write_body(
    writer: &mut (impl tokio::io::AsyncWrite + Unpin),
    data: &[u8],
) -> io::Result<()> {
    if data.is_empty() || data.len() > MAX_RPC {
        return Err(failure());
    }
    writer.write_u32(data.len() as u32).await?;
    for chunk in data.chunks(CHUNK) {
        writer.write_all(chunk).await?;
    }
    Ok(())
}
impl RpcTransport for IrohRpc {
    fn call(
        &self,
        target: u64,
        expected: String,
        method: &'static str,
        body: Vec<u8>,
    ) -> Pin<Box<dyn Future<Output = io::Result<Vec<u8>>> + Send + '_>> {
        Box::pin(async move {
            let _permit = self.in_flight.try_acquire().map_err(|_| failure())?;
            match timeout(RPC_TIMEOUT, async {
                let address = self
                    .members
                    .read()
                    .await
                    .get(&target)
                    .map(|binding| binding.coordination.clone())
                    .ok_or_else(failure)?;
                if address.id.to_string() != expected {
                    return Err(failure());
                }
                let connection = self.route(target, address).await?;
                // If this future is canceled by OpenRaft before the transport
                // timeout, Drop still closes the cached route. A retry must
                // never reuse a connection whose response stream was abandoned.
                // The authority read is the exception: it is polled every second
                // under a two-second deadline and shares this connection, so
                // closing on its cancellation would tear down a checkpoint
                // append still in flight beside it. Dropping its stream halves
                // resets that stream alone.
                let mut connection_guard = ConnectionUseGuard::new(connection.clone());
                if method == "authority" {
                    connection_guard.succeeded();
                }
                let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failure())?;
                send.write_all(&self.room).await.map_err(|_| failure())?;
                send.write_u64(self.incarnation).await?;
                send.write_u64(target).await?;
                send.write_u8(match method {
                    "append" => 1,
                    "vote" => 2,
                    "snapshot" => 3,
                    "authority" => 4,
                    "propose" => 5,
                    "elect" => 6,
                    "remove" => 7,
                    _ => return Err(failure()),
                })
                .await?;
                write_body(&mut send, &body).await?;
                send.finish().map_err(|_| failure())?;
                // Every response is a small Raft reply, receipt or claim.
                let result = read_body(&mut recv, MAX_RPC_RESPONSE).await;
                if result.is_ok() {
                    connection_guard.succeeded();
                }
                result
            })
            .await
            {
                Ok(result) => result,
                Err(_) => Err(failure()),
            }
        })
    }
}

#[cfg(test)]
mod tests;
