//! Authenticated, bounded room-consensus transport. This endpoint is separate
//! from the gameplay endpoint: shutting down consensus cannot retire GGPO links.
use crate::coordination::{
    AuthorityClaim, Coordinator, MAX_MEMBERS, MAX_PROPOSE_REQUEST, MAX_RPC_RESPONSE, MAX_SNAPSHOT,
    MAX_SNAPSHOT_REQUEST, MAX_VOTE_REQUEST, RpcTransport, SNAPSHOT_FRAGMENT_BYTES,
};
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
const CHUNK: usize = SNAPSHOT_FRAGMENT_BYTES;
const RETIRED_FILTER_WORDS: usize = 128;
const RETIRED_FILTER_HASHES: u64 = 3;
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
    retired_filter: RwLock<[u64; RETIRED_FILTER_WORDS]>,
    connections: RwLock<BTreeMap<u64, Connection>>,
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
            retired_filter: RwLock::new([0; RETIRED_FILTER_WORDS]),
            connections: RwLock::new(BTreeMap::new()),
            in_flight: Semaphore::new(MAX_MEMBERS),
            peers: std::sync::Mutex::new(BTreeMap::new()),
            body_budget: Semaphore::new(BODY_BUDGET),
        }))
    }
    pub fn address(&self) -> EndpointAddr {
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
                >= MAX_MEMBERS
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
        if let Some(connection) = self.connections.write().await.remove(&incarnation) {
            connection.close(1u32.into(), b"room membership ended");
        }
    }

    fn retired_filter_index(&self, incarnation: u64, round: u64) -> usize {
        let mut value = incarnation ^ round.wrapping_mul(0x9e37_79b9_7f4a_7c15);
        value ^= u64::from_le_bytes(self.room[..8].try_into().unwrap()).rotate_left(17);
        value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9).rotate_left(23);
        value ^= u64::from_le_bytes(self.room[8..].try_into().unwrap()).rotate_left(17);
        value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9).rotate_left(23);
        (value as usize) % (RETIRED_FILTER_WORDS * u64::BITS as usize)
    }

    async fn retired_filter_contains(&self, incarnation: u64) -> bool {
        let filter = self.retired_filter.read().await;
        (0..RETIRED_FILTER_HASHES).all(|round| {
            let bit = self.retired_filter_index(incarnation, round);
            filter[bit / u64::BITS as usize] & (1u64 << (bit % u64::BITS as usize)) != 0
        })
    }

    async fn retired_filter_insert(&self, incarnation: u64) {
        let mut filter = self.retired_filter.write().await;
        for round in 0..RETIRED_FILTER_HASHES {
            let bit = self.retired_filter_index(incarnation, round);
            filter[bit / u64::BITS as usize] |= 1u64 << (bit % u64::BITS as usize);
        }
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
                    if tasks.len() >= MAX_MEMBERS { incoming.refuse(); continue; }
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
                let cached = self
                    .connections
                    .read()
                    .await
                    .get(&target)
                    .filter(|c| c.close_reason().is_none())
                    .cloned();
                let connection = if let Some(connection) = cached {
                    connection
                } else {
                    let connection = self
                        .endpoint
                        .connect(address, ALPN)
                        .await
                        .map_err(|_| failure())?;
                    self.connections
                        .write()
                        .await
                        .insert(target, connection.clone());
                    connection
                };
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
mod tests {
    use super::*;
    use crate::coordination::Proposal;
    use openraft::BasicNode;
    #[tokio::test]
    async fn authenticated_iroh_commit_and_host_loss() {
        let mut transports = Vec::new();
        let mut nodes = Vec::new();
        let mut tasks = JoinSet::new();
        for id in 1..=3 {
            let transport = IrohRpc::bind([7; 16], id, false).await.unwrap();
            let node = Arc::new(Coordinator::new(id, transport.clone()).await.unwrap());
            transports.push(transport);
            nodes.push(node);
        }
        for transport in &transports {
            for (i, peer) in transports.iter().enumerate() {
                if !Arc::ptr_eq(transport, peer) {
                    transport.admit(i as u64 + 1, peer.address()).await.unwrap();
                }
            }
        }
        for i in 0..3 {
            tasks.spawn(transports[i].clone().serve(nodes[i].clone()));
        }
        nodes[0]
            .raft()
            .initialize(
                transports
                    .iter()
                    .enumerate()
                    .map(|(i, t)| (i as u64 + 1, BasicNode::new(t.identity().to_string())))
                    .collect::<BTreeMap<_, _>>(),
            )
            .await
            .unwrap();
        nodes[0]
            .raft()
            .wait(Some(Duration::from_secs(15)))
            .current_leader(1, "Iroh leader")
            .await
            .unwrap();
        let proposal = Proposal {
            request: "first".into(),
            dedup_id: "test:first".into(),
            term: nodes[0].raft().metrics().borrow().current_term,
            base: 0,
            checkpoint: "active generation 42".into(),
            admin: None,
        };
        assert!(nodes[0].propose(proposal).await.unwrap().accepted);
        tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                if nodes[1].committed().await.revision == 1
                    && nodes[2].committed().await.revision == 1
                {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(25)).await;
            }
        })
        .await
        .unwrap();
        nodes[0].raft().shutdown().await.unwrap();
        transports[0].close().await;
        nodes[1].raft().trigger().elect().await.unwrap();
        tokio::time::timeout(Duration::from_secs(15), async {
            loop {
                if nodes[1].raft().metrics().borrow().current_leader.is_some()
                    || nodes[2].raft().metrics().borrow().current_leader.is_some()
                {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
        })
        .await
        .unwrap();
        let successor = if nodes[1].raft().metrics().borrow().current_leader == Some(2) {
            &nodes[1]
        } else {
            &nodes[2]
        };
        assert_eq!(
            successor.committed().await.checkpoint,
            "active generation 42"
        );
        for i in 1..3 {
            nodes[i].raft().shutdown().await.unwrap();
            transports[i].close().await;
        }
        tasks.abort_all();
        while tasks.join_next().await.is_some() {}
    }

    #[tokio::test]
    async fn primary_binding_is_unique_until_incarnation_revoke() {
        let router = IrohRpc::bind([8; 16], 1, false).await.unwrap();
        let first = IrohRpc::bind([8; 16], 2, false).await.unwrap();
        let second = IrohRpc::bind([8; 16], 3, false).await.unwrap();
        let primary = router.identity();

        router
            .admit_bound(2, first.address(), Some(primary))
            .await
            .unwrap();
        assert!(
            router
                .admit_bound(3, second.address(), Some(primary))
                .await
                .is_err()
        );
        router.admit_bound(3, second.address(), None).await.unwrap();
        router.revoke(2).await;
        router
            .admit_bound(3, second.address(), Some(primary))
            .await
            .unwrap();

        router.close().await;
        first.close().await;
        second.close().await;
    }

    #[tokio::test]
    async fn own_incarnation_and_coordination_key_are_never_bound() {
        let router = IrohRpc::bind([10; 16], 1, false).await.unwrap();
        let peer = IrohRpc::bind([10; 16], 2, false).await.unwrap();

        assert!(router.admit(1, peer.address()).await.is_err());
        assert!(router.admit(2, router.address()).await.is_err());
        assert!(!router.has_binding(1).await);
        router.admit(2, peer.address()).await.unwrap();

        router.close().await;
        peer.close().await;
    }

    /// One RPC written by hand, so a test chooses the method byte and body.
    async fn raw_rpc(connection: &Connection, method: u8, body: &[u8]) -> io::Result<Vec<u8>> {
        let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failure())?;
        send.write_all(&[12; 16]).await?;
        send.write_u64(2).await?;
        send.write_u64(1).await?;
        send.write_u8(method).await?;
        send.write_u32(body.len() as u32).await?;
        send.write_all(body).await?;
        send.finish().map_err(|_| failure())?;
        read_body(&mut recv, MAX_RPC_RESPONSE).await
    }

    #[tokio::test]
    async fn served_connections_and_bodies_stay_within_their_bounds() {
        let room = [12; 16];
        let server = IrohRpc::bind(room, 1, false).await.unwrap();
        let client = IrohRpc::bind(room, 2, false).await.unwrap();
        server.admit(2, client.address()).await.unwrap();
        let coordinator = Arc::new(
            Coordinator::new_for_room(room, 1, server.clone())
                .await
                .unwrap(),
        );
        coordinator
            .raft()
            .initialize(BTreeMap::from([(
                1,
                BasicNode::new(server.identity().to_string()),
            )]))
            .await
            .unwrap();
        coordinator
            .raft()
            .wait(Some(Duration::from_secs(5)))
            .current_leader(1, "single leader")
            .await
            .unwrap();
        let serving = tokio::spawn(server.clone().serve(coordinator.clone()));
        let connect = || client.endpoint.connect(server.address(), ALPN);

        let first = connect().await.unwrap();
        let second = connect().await.unwrap();
        assert!(raw_rpc(&first, 4, &[0]).await.is_ok());
        assert!(raw_rpc(&second, 4, &[0]).await.is_ok());
        // A third connection from the same endpoint is closed at once.
        let third = connect().await.unwrap();
        assert!(
            timeout(Duration::from_secs(10), third.closed())
                .await
                .is_ok()
        );

        // A size beyond its method's limit fails the stream from the header
        // alone, before any body byte arrives.
        let (mut send, mut recv) = first.open_bi().await.unwrap();
        send.write_all(&room).await.unwrap();
        send.write_u64(2).await.unwrap();
        send.write_u64(1).await.unwrap();
        send.write_u8(4).await.unwrap();
        send.write_u32(2).await.unwrap();
        assert!(matches!(
            timeout(
                Duration::from_secs(3),
                read_body(&mut recv, MAX_RPC_RESPONSE)
            )
            .await,
            Ok(Err(_))
        ));
        drop(send);

        // A body beyond its method's size fails its own stream only.
        assert!(raw_rpc(&first, 4, &[0, 0]).await.is_err());
        assert!(
            raw_rpc(&first, 2, &vec![b' '; MAX_VOTE_REQUEST + 1])
                .await
                .is_err()
        );
        assert!(raw_rpc(&first, 4, &[0]).await.is_ok());

        // A closed connection frees its place for a new one.
        second.close(0u32.into(), b"done");
        timeout(Duration::from_secs(10), async {
            loop {
                if let Ok(next) = connect().await
                    && raw_rpc(&next, 4, &[0]).await.is_ok()
                {
                    break;
                }
                sleep(Duration::from_millis(50)).await;
            }
        })
        .await
        .unwrap();

        serving.abort();
        coordinator.raft().shutdown().await.unwrap();
        server.close().await;
        client.close().await;
    }

    /// Drops `call` once the receiver reports that the RPC's stream reached it,
    /// so the cancellation lands on a call that is waiting for its response
    /// whatever the connection setup cost.
    async fn cancel_once_received<F: Future>(
        call: F,
        received: &mut tokio::sync::mpsc::UnboundedReceiver<()>,
    ) {
        tokio::select! {
            _ = call => panic!("the stalled receiver answered the RPC"),
            arrived = timeout(Duration::from_secs(30), received.recv()) => {
                assert!(matches!(arrived, Ok(Some(()))), "the RPC never reached the receiver");
            }
        }
    }

    #[tokio::test]
    async fn canceled_rpc_closes_the_cached_connection_before_retry() {
        let room = [11; 16];
        let caller = IrohRpc::bind(room, 1, false).await.unwrap();
        let receiver = IrohRpc::bind(room, 2, false).await.unwrap();
        caller.admit(2, receiver.address()).await.unwrap();
        let receiver_endpoint = receiver.endpoint.clone();
        let (received_tx, mut received) = tokio::sync::mpsc::unbounded_channel();
        let stalled = tokio::spawn(async move {
            let incoming = receiver_endpoint.accept().await.unwrap();
            let connection = incoming.await.unwrap();
            let _read = connection.accept_bi().await.unwrap();
            received_tx.send(()).unwrap();
            let _append = connection.accept_bi().await.unwrap();
            received_tx.send(()).unwrap();
            // Hold the connection and both streams open, never answering,
            // until the test aborts this task.
            std::future::pending::<()>().await;
        });

        // A canceled authority read shares the route with raft traffic and
        // must leave it open.
        let read = <IrohRpc as RpcTransport>::call(
            caller.as_ref(),
            2,
            receiver.identity().to_string(),
            "authority",
            vec![0],
        );
        cancel_once_received(read, &mut received).await;
        let cached = caller.connections.read().await.get(&2).cloned().unwrap();
        assert!(
            cached.close_reason().is_none(),
            "canceled authority read closed the shared route"
        );

        let call = <IrohRpc as RpcTransport>::call(
            caller.as_ref(),
            2,
            receiver.identity().to_string(),
            "append",
            vec![0],
        );
        cancel_once_received(call, &mut received).await;
        let cached = caller.connections.read().await.get(&2).cloned().unwrap();
        assert!(
            cached.close_reason().is_some(),
            "canceled RPC left a dead cached connection reusable"
        );
        stalled.abort();
        caller.close().await;
        receiver.close().await;
    }

    #[tokio::test]
    async fn retired_incarnation_cannot_replay_admission() {
        let router = IrohRpc::bind([9; 16], 1, false).await.unwrap();
        let departing = IrohRpc::bind([9; 16], 2, false).await.unwrap();
        let replacement = IrohRpc::bind([9; 16], 3, false).await.unwrap();
        let primary = router.identity();

        router
            .admit_bound(2, departing.address(), Some(primary))
            .await
            .unwrap();
        router.retire(2).await;

        // A disconnected member may retain an authenticated transport long
        // enough to finish its Leave proof, but replaying its old admission
        // must never restore append/vote authority or evict a fresh process.
        assert!(
            router
                .admit_bound(2, departing.address(), Some(primary))
                .await
                .is_err()
        );
        router
            .admit_bound(3, replacement.address(), Some(primary))
            .await
            .unwrap();

        router.close().await;
        departing.close().await;
        replacement.close().await;
    }
}
