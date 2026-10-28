//! The OpenRaft network: the RPC methods, the transport they cross and
//! each follower's peer, with the encoded appends they share.

use super::*;

/// A coordination RPC. Its discriminant is the method byte on the wire.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Method {
    Append = 1,
    Vote = 2,
    Snapshot = 3,
    Authority = 4,
    Propose = 5,
    Elect = 6,
    Remove = 7,
}

impl Method {
    /// The method a wire byte names, if any.
    pub fn from_byte(byte: u8) -> Option<Self> {
        Some(match byte {
            1 => Self::Append,
            2 => Self::Vote,
            3 => Self::Snapshot,
            4 => Self::Authority,
            5 => Self::Propose,
            6 => Self::Elect,
            7 => Self::Remove,
            _ => return None,
        })
    }

    /// The method's wire byte.
    pub fn byte(self) -> u8 {
        self as u8
    }

    /// The method's name, for tests and diagnostics.
    pub fn name(self) -> &'static str {
        match self {
            Self::Append => "append",
            Self::Vote => "vote",
            Self::Snapshot => "snapshot",
            Self::Authority => "authority",
            Self::Propose => "propose",
            Self::Elect => "elect",
            Self::Remove => "remove",
        }
    }
}

// Both the Iroh adapter and deterministic partition tests cross this interface.
// RPC serialization errors deliberately do not expose private checkpoint data.
pub trait RpcTransport: Send + Sync + 'static {
    fn call(
        &self,
        target: u64,
        address: String,
        method: Method,
        body: Vec<u8>,
    ) -> Pin<Box<dyn Future<Output = io::Result<Vec<u8>>> + Send + '_>>;
}
#[derive(Clone)]
pub struct Network {
    pub(super) transport: Arc<dyn RpcTransport>,
    /// Shared by every follower's `Peer`.
    pub(super) appends: Arc<AppendBodies>,
}
impl Network {
    pub fn new(transport: Arc<dyn RpcTransport>) -> Self {
        Self {
            transport,
            appends: Arc::default(),
        }
    }
}

/// An append that carries entries, named by what fixes its content: the
/// leader's vote, the entry before them, the leader's commit and the first
/// and last entry. An entry never changes under its LogId, so two requests
/// with one key encode to the same bytes.
pub(super) type AppendKey = (
    Vote<u64>,
    Option<LogId<u64>>,
    Option<LogId<u64>>,
    LogId<u64>,
    LogId<u64>,
);
/// How many encoded appends are kept: the newest commit and the one before
/// it, for a follower a step behind.
pub(super) const APPEND_BODIES: usize = 2;
pub(super) type AppendBody = Arc<std::sync::OnceLock<Option<Arc<Vec<u8>>>>>;

/// The RPC bodies of the last appends, encoded once for every follower that
/// is sent the same request. Each replication stream runs on its own task, so
/// a stream that finds the key encoding waits for it rather than encoding a
/// second time.
#[derive(Default)]
pub(super) struct AppendBodies {
    pub(super) slots: std::sync::Mutex<VecDeque<(AppendKey, AppendBody)>>,
    /// The checkpoints the bodies carry, compressed.
    pub(super) checkpoints: CompressedCheckpoints,
    #[cfg(test)]
    pub(super) encodes: std::sync::atomic::AtomicU64,
}
impl AppendBodies {
    /// The encoded body of `request`, `None` when it cannot be encoded.
    pub(super) fn body(&self, request: &AppendEntriesRequest<RoomTypes>) -> Option<Arc<Vec<u8>>> {
        let encode = || {
            #[cfg(test)]
            self.encodes
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            encode_append(request, &self.checkpoints).ok().map(Arc::new)
        };
        // A heartbeat is small; keep the slots for the bodies that are not.
        let (Some(first), Some(last)) = (request.entries.first(), request.entries.last()) else {
            return encode();
        };
        let key = (
            request.vote,
            request.prev_log_id,
            request.leader_commit,
            first.log_id,
            last.log_id,
        );
        let body = {
            let mut slots = self
                .slots
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            match slots.iter().find(|(slot, _)| *slot == key) {
                Some((_, body)) => body.clone(),
                None => {
                    let body = AppendBody::default();
                    slots.push_back((key, body.clone()));
                    if slots.len() > APPEND_BODIES {
                        slots.pop_front();
                    }
                    body
                }
            }
        };
        body.get_or_init(encode).clone()
    }
}

pub struct Peer {
    pub(super) network: Network,
    pub(super) id: u64,
    pub(super) address: String,
}
impl RaftNetworkFactory<RoomTypes> for Network {
    type Network = Peer;
    async fn new_client(&mut self, id: u64, node: &BasicNode) -> Peer {
        Peer {
            network: self.clone(),
            id,
            address: node.addr.clone(),
        }
    }
}
// This error shape is required by OpenRaft's network trait.
pub(super) fn rpc_failed<E: std::error::Error>() -> RPCError<u64, BasicNode, E> {
    RPCError::Network(NetworkError::new(&io::Error::other(
        "room coordination RPC failed",
    )))
}
impl Peer {
    #[allow(clippy::result_large_err)]
    pub(super) async fn request<T: Serialize, R: DeserializeOwned, E: std::error::Error>(
        &self,
        method: Method,
        value: T,
    ) -> Result<R, RPCError<u64, BasicNode, E>> {
        let bytes = serde_json::to_vec(&value).map_err(|_| rpc_failed())?;
        self.send(method, bytes).await
    }

    #[allow(clippy::result_large_err)]
    pub(super) async fn send<R: DeserializeOwned, E: std::error::Error>(
        &self,
        method: Method,
        bytes: Vec<u8>,
    ) -> Result<R, RPCError<u64, BasicNode, E>> {
        let response = self
            .network
            .transport
            .call(self.id, self.address.clone(), method, bytes)
            .await
            .map_err(|_| rpc_failed())?;
        serde_json::from_slice(&response).map_err(|_| rpc_failed())
    }
}
impl RaftNetwork<RoomTypes> for Peer {
    async fn append_entries(
        &mut self,
        request: AppendEntriesRequest<RoomTypes>,
        _: RPCOption,
    ) -> Result<AppendEntriesResponse<u64>, RPCError<u64, BasicNode, RaftError<u64>>> {
        // Every follower sent this request is sent the same bytes.
        let body = self.network.appends.body(&request).ok_or_else(rpc_failed)?;
        self.send(Method::Append, body.as_ref().clone()).await
    }
    async fn vote(
        &mut self,
        request: VoteRequest<u64>,
        _: RPCOption,
    ) -> Result<VoteResponse<u64>, RPCError<u64, BasicNode, RaftError<u64>>> {
        self.request(Method::Vote, request).await
    }
    async fn install_snapshot(
        &mut self,
        request: InstallSnapshotRequest<RoomTypes>,
        _: RPCOption,
    ) -> Result<
        InstallSnapshotResponse<u64>,
        RPCError<u64, BasicNode, RaftError<u64, InstallSnapshotError>>,
    > {
        let body = encode_install_snapshot(request).map_err(|_| rpc_failed())?;
        self.send(Method::Snapshot, body).await
    }
}
