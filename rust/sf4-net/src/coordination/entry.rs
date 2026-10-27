//! What a room replicates and commits: proposals and their receipts, the
//! committed checkpoint, probe reservations and the authority claim.

use super::*;

#[derive(Clone, Serialize, Deserialize)]
pub struct Proposal {
    pub request: String,
    /// Stable Raft deduplication namespace. `request` remains the native
    /// transfer identifier exposed to C++ after commitment; this separate
    /// key prevents a fresh process from colliding with an older incarnation
    /// that happened to reuse the same native request number.
    #[serde(default)]
    pub dedup_id: String,
    pub term: u64,
    pub base: u64,
    /// Shared by the log entry, its clones and the commit. An append
    /// carries it compressed beside its JSON header (`encode_append`); as
    /// JSON it is the same string a `String` would be.
    pub checkpoint: Arc<str>,
    #[serde(default)]
    pub admin: Option<AdminEntry>,
}
impl Proposal {
    /// The bounds every proposal keeps, checked where one is proposed and
    /// again where its entry is applied: a request id and a dedup key of 1 to
    /// 128 bytes, and a checkpoint within `MAX_CHECKPOINT`.
    pub(super) fn well_formed(&self) -> bool {
        !self.request.is_empty()
            && !self.dedup_key().is_empty()
            && self.dedup_key().len() <= 128
            && self.request.len() <= 128
            && self.checkpoint.len() <= MAX_CHECKPOINT
    }

    pub(super) fn dedup_key(&self) -> &str {
        if self.dedup_id.is_empty() {
            &self.request
        } else {
            &self.dedup_id
        }
    }
    pub(super) fn digest(&self) -> [u8; 32] {
        let mut hash = blake3::Hasher::new();
        hash.update(self.dedup_key().as_bytes());
        hash.update(&self.base.to_le_bytes());
        hash.update(self.checkpoint.as_bytes());
        *hash.finalize().as_bytes()
    }
}
impl Debug for Proposal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Proposal")
            .field("term", &self.term)
            .field("base", &self.base)
            .finish_non_exhaustive()
    }
}
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct Receipt {
    pub accepted: bool,
    pub revision: u64,
}
/// The committed checkpoint. The store holds it behind an `Arc` and replaces
/// it whole on each commit, so a reader takes a coherent value without
/// copying the checkpoint, which can weigh a mebibyte.
/// Its fields are private and it has no `Clone`: a value is built whole and
/// never changed, which the cached digest relies on.
#[derive(Default, Serialize, Deserialize)]
pub struct Committed {
    pub(super) revision: u64,
    pub(super) checkpoint: Arc<str>,
    pub(super) request: String,
    pub(super) term: u64,
    /// The checkpoint's SHA-256, hashed on first use.
    #[serde(skip)]
    pub(super) digest: OnceLock<[u8; 32]>,
    /// The parts of the checkpoint the helper reads, read on first use
    /// (`view`).
    #[serde(skip)]
    pub(super) view: OnceLock<Option<Value>>,
}

impl Committed {
    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn checkpoint(&self) -> &str {
        &self.checkpoint
    }

    /// The checkpoint text itself, shared rather than copied.
    pub fn checkpoint_shared(&self) -> Arc<str> {
        self.checkpoint.clone()
    }

    pub fn request(&self) -> &str {
        &self.request
    }

    pub fn term(&self) -> u64 {
        self.term
    }

    pub(super) fn new(revision: u64, checkpoint: Arc<str>, request: String, term: u64) -> Self {
        Self {
            revision,
            checkpoint,
            request,
            term,
            digest: OnceLock::new(),
            view: OnceLock::new(),
        }
    }

    /// The checkpoint's members, room members, tables and tournament
    /// binding, read once per commit (`view::envelope_view`); `None` when
    /// the checkpoint is not JSON. Every reader of the committed roster
    /// takes it from here instead of parsing the whole checkpoint again.
    pub fn view(&self) -> Option<&Value> {
        self.view
            .get_or_init(|| view::envelope_view(&self.checkpoint))
            .as_ref()
    }

    /// The SHA-256 of the checkpoint, all zeroes for none.
    pub fn digest(&self) -> [u8; 32] {
        *self.digest.get_or_init(|| {
            if self.checkpoint.is_empty() {
                [0; 32]
            } else {
                crate::recovery::sha256(self.checkpoint.as_bytes())
            }
        })
    }

    /// The same commit. The store hands out one shared value per commit, so
    /// this is almost always the pointer test; the field comparison covers a
    /// value rebuilt by an installed snapshot.
    pub fn same_commit(&self, other: &Self) -> bool {
        std::ptr::eq(self, other)
            || (self.revision == other.revision
                && self.term == other.term
                && self.request == other.request
                && self.checkpoint == other.checkpoint)
    }
}

/// Small coordination-only entries do not advance the native checkpoint
/// revision. They are still replicated and applied by every admitted helper,
/// which lets a receiver validate a probe against its own committed state.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum AdminEntry {
    ProbeReservation(ProbeReservation),
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct ProbeReservation {
    pub room: [u8; 16],
    /// The authenticated primary/coordination identity that initiated the
    /// reservation and the identity that must accept it. Keeping both ends
    /// explicit prevents a remote member from replaying a reservation for a
    /// different process incarnation.
    pub source_incarnation: u64,
    pub target_incarnation: u64,
    pub request: u64,
    pub pair_revision: u64,
    pub term: u64,
    pub expires: u64,
}

impl ProbeReservation {
    /// The fields a reservation must name, wherever one is applied or
    /// restored: a room, both incarnations, a request and an expiry. Its term
    /// is checked by each of them in its own way.
    pub(super) fn names_its_pair(&self) -> bool {
        self.room != [0; 16]
            && self.source_incarnation != 0
            && self.target_incarnation != 0
            && self.request != 0
            && self.expires != 0
    }
}

/// Small authenticated read-barrier response used when a helper is retiring.
/// It contains no checkpoint bytes: the departing process only needs proof
/// that the successor has the committed term and applied revision.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AuthorityClaim {
    pub incarnation: u64,
    pub term: u64,
    pub leader: Option<u64>,
    pub revision: u64,
    /// The leader's committed voter set.  A departing follower cannot rely
    /// on its local Raft membership after retain=false removes it from the
    /// old quorum, so graceful leave verifies its absence over this
    /// authenticated leader read instead.
    pub voters: BTreeSet<u64>,
    /// The complete applied membership, including learners.  A native
    /// rejection can leave a coordination-only process as a learner, so a
    /// voter-only claim cannot prove that its authenticated route is gone.
    #[serde(default)]
    pub members: BTreeSet<u64>,
}

impl AuthorityClaim {
    /// Whether `id` claims to lead, itself, at `term` or later and with
    /// `revision` or a later one applied.
    pub fn is_leader_at(&self, id: u64, term: u64, revision: u64) -> bool {
        self.incarnation == id
            && self.leader == Some(id)
            && self.term >= term
            && self.revision >= revision
    }
}
