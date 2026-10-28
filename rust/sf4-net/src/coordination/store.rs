//! The room state machine and its in-memory Raft storage.

use super::*;

#[derive(Clone, Default, Serialize, Deserialize)]
pub(super) struct Machine {
    pub(super) applied: Option<LogId<u64>>,
    pub(super) membership: StoredMembership<u64, BasicNode>,
    /// Replicated proof that an incarnation previously reached applied Raft
    /// membership. A successor can therefore recognize its replay after a
    /// remove entry even if no actor tick observed the intermediate state.
    #[serde(default)]
    pub(super) member_history: BTreeSet<u64>,
    /// The departed members of `member_history`, oldest departure first: the
    /// order they left the applied membership. Eviction drops from the front,
    /// so the newest departures are the ones kept, and a member that is still
    /// live, or that just left, is never dropped for an older departure.
    /// Members of `member_history` missing from this list were live in the
    /// last applied membership. A snapshot without the field lists none as
    /// departed, so `remember_members` treats all of them as leaving at the
    /// next membership, in ascending incarnation order.
    #[serde(default)]
    pub(super) departed_order: Vec<u64>,
    pub(super) committed: Arc<Committed>,
    pub(super) recent: Vec<(String, [u8; 32], Receipt)>,
    #[serde(default)]
    pub(super) probes: Vec<ProbeReservation>,
}
/// What applying a checkpoint proposal did: its receipt, and whether it
/// replaced the committed checkpoint.
pub(super) struct AppliedCheckpoint {
    pub(super) receipt: Receipt,
    pub(super) replaced: bool,
}

impl Machine {
    pub(super) fn apply_membership(
        &mut self,
        log_id: LogId<u64>,
        membership: &openraft::Membership<u64, BasicNode>,
    ) {
        let live: BTreeSet<u64> = membership.nodes().map(|(id, _)| *id).collect();
        self.remember_members(&live);
        self.membership = StoredMembership::new(Some(log_id), membership.clone());
    }

    /// Applies a probe reservation proposed in `term`; whether it was kept.
    pub(super) fn apply_probe(
        &mut self,
        proposal: &Proposal,
        reservation: &ProbeReservation,
        term: u64,
    ) -> bool {
        if !(proposal.request.len() <= 128
            && proposal.checkpoint.is_empty()
            && proposal.term == term
            && reservation.term == proposal.term
            && reservation.names_its_pair())
        {
            return false;
        }
        let members: Vec<_> = self.membership.nodes().collect();
        let source_member = members
            .iter()
            .any(|(id, _)| **id == reservation.source_incarnation);
        let target_member = members
            .iter()
            .any(|(id, _)| **id == reservation.target_incarnation);
        if !source_member || !target_member {
            return false;
        }
        // `expires` is a logical watermark carried by the committed entry.
        // Pruning against it keeps apply deterministic on every replica while
        // bounding old process incarnations.
        self.probes.retain(|previous| {
            previous.room != reservation.room
                || previous.source_incarnation != reservation.source_incarnation
                || previous.target_incarnation != reservation.target_incarnation
                || previous.expires >= reservation.expires
        });
        if self.probes.len() >= MAX_MEMBERS {
            self.probes.remove(0);
        }
        self.probes.push(reservation.clone());
        true
    }

    /// Applies a checkpoint proposed in `term`. None leaves the entry's
    /// receipt as it is: not accepted, at the committed revision.
    pub(super) fn apply_checkpoint(
        &mut self,
        proposal: &Proposal,
        term: u64,
    ) -> Option<AppliedCheckpoint> {
        if let Some((_, digest, previous)) = self
            .recent
            .iter()
            .find(|(id, _, _)| id == proposal.dedup_key())
        {
            // A lost acknowledgement can be retried, but reusing a request ID
            // for different state must never acknowledge that different
            // candidate as if it were committed.
            return (*digest == proposal.digest()).then(|| AppliedCheckpoint {
                receipt: previous.clone(),
                replaced: false,
            });
        }
        if !(proposal.well_formed()
            && proposal.term == term
            && proposal.base == self.committed.revision
            && proposal.base < u64::MAX)
        {
            return None;
        }
        let receipt = Receipt {
            accepted: true,
            revision: proposal.base + 1,
        };
        self.committed = Arc::new(Committed::new(
            receipt.revision,
            proposal.checkpoint.clone(),
            proposal.request.clone(),
            proposal.term,
        ));
        self.recent.push((
            proposal.dedup_key().to_owned(),
            proposal.digest(),
            receipt.clone(),
        ));
        if self.recent.len() > RECENT_REQUESTS {
            self.recent.remove(0);
        }
        Some(AppliedCheckpoint {
            receipt,
            replaced: true,
        })
    }

    /// Apply a new membership to the history. Incarnations of the previous
    /// membership that it no longer contains join the departed order, then the
    /// oldest departures beyond the retained window are dropped. The current
    /// members are never dropped. This runs on every replica in log order, so
    /// all of them keep the same window.
    pub(super) fn remember_members(&mut self, live: &BTreeSet<u64>) {
        // Keep only listed departures that are still remembered, once each,
        // and forget one that is live again.
        let mut listed = BTreeSet::new();
        let history = &self.member_history;
        self.departed_order
            .retain(|id| history.contains(id) && !live.contains(id) && listed.insert(*id));
        // Whatever is not listed as departed was live until now.
        for id in &self.member_history {
            if !live.contains(id) && listed.insert(*id) {
                self.departed_order.push(*id);
            }
        }
        self.member_history.extend(live.iter().copied());
        let excess = self
            .departed_order
            .len()
            .saturating_sub(RETAINED_RETIRED_MEMBER_HISTORY);
        for id in self.departed_order.drain(..excess) {
            self.member_history.remove(&id);
        }
    }
}
pub(super) struct Memory {
    pub(super) vote: Option<Vote<u64>>,
    pub(super) purged: Option<LogId<u64>>,
    pub(super) log: BTreeMap<u64, Entry<RoomTypes>>,
    pub(super) machine: Machine,
    pub(super) snapshot: Option<(SnapshotMeta<u64, BasicNode>, Vec<u8>)>,
    /// The most nodes an installed snapshot's membership may hold.
    pub(super) max_nodes: usize,
    /// The committed revision, published for `Store::committed_changes`.
    pub(super) commits: watch::Sender<u64>,
    /// How many times `machine.committed` was replaced: once per accepted
    /// native checkpoint and once per installed snapshot, whatever either
    /// holds. A reader that sees it move by more than one missed a commit.
    pub(super) observations: u64,
    /// `observations`, published for `Store::observed_changes`.
    pub(super) observed: watch::Sender<u64>,
}
impl Memory {
    pub(super) fn new(max_nodes: usize) -> Self {
        Self {
            vote: None,
            purged: None,
            log: BTreeMap::new(),
            machine: Machine::default(),
            snapshot: None,
            max_nodes,
            commits: watch::Sender::new(0),
            observations: 0,
            observed: watch::Sender::new(0),
        }
    }

    /// Tells `committed_changes` readers when the committed revision moved,
    /// and `observed_changes` readers when the committed value was replaced,
    /// which a snapshot at the same revision also does.
    pub(super) fn publish_commit(&self) {
        let revision = self.machine.committed.revision;
        self.commits.send_if_modified(|published| {
            let moved = *published != revision;
            *published = revision;
            moved
        });
        let observations = self.observations;
        self.observed.send_if_modified(|published| {
            let moved = *published != observations;
            *published = observations;
            moved
        });
    }
}

#[derive(Clone)]
pub struct Store(pub(super) Arc<Mutex<Memory>>);
impl Default for Store {
    fn default() -> Self {
        Self::with_node_limit(MAX_MEMBERS)
    }
}
impl Store {
    pub(super) fn with_node_limit(max_nodes: usize) -> Self {
        Self(Arc::new(Mutex::new(Memory::new(max_nodes))))
    }

    /// The committed checkpoint, shared: taking it copies nothing, so the
    /// actor can look on every tick.
    pub async fn committed(&self) -> Arc<Committed> {
        self.0.lock().await.machine.committed.clone()
    }

    /// Follows the committed revision: it changes when an entry or an
    /// installed snapshot moves it, so a reader can wait for that instead of
    /// asking on a timer.
    pub async fn committed_changes(&self) -> watch::Receiver<u64> {
        self.0.lock().await.commits.subscribe()
    }

    /// The committed checkpoint and how many times it has been replaced,
    /// read together under the one lock.
    pub async fn committed_observed(&self) -> (Arc<Committed>, u64) {
        let memory = self.0.lock().await;
        (memory.machine.committed.clone(), memory.observations)
    }

    /// Follows `committed_observed`'s count.
    pub async fn observed_changes(&self) -> watch::Receiver<u64> {
        self.0.lock().await.observed.subscribe()
    }

    /// The membership entry applied by the state machine.  OpenRaft's
    /// metrics expose the effective (possibly joint, merely appended)
    /// configuration; authority and retirement proofs must use this durable
    /// applied view instead.
    pub async fn applied_voters(&self) -> BTreeSet<u64> {
        self.0.lock().await.machine.membership.voter_ids().collect()
    }

    /// The voter set an applied joint configuration is heading to, if the
    /// applied membership is joint. OpenRaft proposes the uniform half only
    /// from the caller of the change; a caller that gave up leaves the group
    /// joint, and the old half then keeps every one of its voters needed.
    pub async fn applied_joint_goal(&self) -> Option<BTreeSet<u64>> {
        let memory = self.0.lock().await;
        let configs = memory.machine.membership.membership().get_joint_config();
        (configs.len() > 1)
            .then(|| configs.last().cloned())
            .flatten()
    }

    /// Return the durable applied membership, including learners.  Raft's
    /// effective metrics can contain an appended joint configuration; route
    /// retirement must wait for this state-machine view instead.
    pub async fn applied_members(&self) -> BTreeSet<u64> {
        self.0
            .lock()
            .await
            .machine
            .membership
            .nodes()
            .map(|(id, _)| *id)
            .collect()
    }

    /// Read current membership and durable membership history from one state
    /// machine snapshot. Separate reads could straddle an AddLearner commit
    /// and misclassify the newly applied member as retired.
    pub async fn applied_membership_provenance(&self) -> (BTreeSet<u64>, BTreeSet<u64>) {
        let memory = self.0.lock().await;
        (
            memory
                .machine
                .membership
                .nodes()
                .map(|(id, _)| *id)
                .collect(),
            memory.machine.member_history.clone(),
        )
    }

    /// Whether `reservation` is applied, every field equal.
    pub async fn probe_reserved(&self, reservation: &ProbeReservation) -> bool {
        self.0.lock().await.machine.probes.contains(reservation)
    }

    /// Check a probe against the last native checkpoint applied by this
    /// replica.  A Raft membership pair alone is insufficient: a reservation
    /// must name the two members seated at the same table revision, and both
    /// members must still have a committed fighter selection.  The endpoint
    /// strings are checked here as well so a stale endpoint cannot reuse a
    /// valid member id after an incarnation rebind.
    pub async fn probe_pair_bound(
        &self,
        source_incarnation: u64,
        target_incarnation: u64,
        source_endpoint: &str,
        target_endpoint: &str,
        pair_revision: u64,
    ) -> bool {
        let committed = self.committed().await;
        probe_pair_identities_valid(
            source_incarnation,
            target_incarnation,
            source_endpoint,
            target_endpoint,
            pair_revision,
        ) && committed.view().is_some_and(|envelope| {
            probe_pair_bound_in(
                envelope,
                source_incarnation,
                target_incarnation,
                source_endpoint,
                target_endpoint,
                pair_revision,
            )
        })
    }
}

impl RaftLogReader<RoomTypes> for Store {
    async fn try_get_log_entries<R: RangeBounds<u64> + Clone + Debug + OptionalSend>(
        &mut self,
        range: R,
    ) -> Result<Vec<Entry<RoomTypes>>, StoreError> {
        Ok(self
            .0
            .lock()
            .await
            .log
            .range(range)
            .map(|(_, e)| e.clone())
            .collect())
    }
}
impl Store {
    /// The state machine a snapshot is built from, copied under the lock:
    /// cheap, since the checkpoint itself is shared.
    pub(super) async fn snapshot_source(&self) -> Machine {
        self.0.lock().await.machine.clone()
    }

    /// Keeps a built snapshot as the current one unless the stored one is
    /// newer. OpenRaft builds snapshots on a task of their own, so an
    /// installed snapshot can land between `snapshot_source` and this.
    /// Returns whether it was kept.
    pub(super) async fn keep_snapshot(
        &self,
        meta: &SnapshotMeta<u64, BasicNode>,
        data: &[u8],
    ) -> bool {
        let mut memory = self.0.lock().await;
        if memory
            .snapshot
            .as_ref()
            .is_some_and(|(stored, _)| stored.last_log_id > meta.last_log_id)
        {
            return false;
        }
        memory.snapshot = Some((meta.clone(), data.to_vec()));
        true
    }
}

/// The snapshot of `machine`: its metadata and its encoded bytes.
pub(super) fn snapshot_of(
    machine: &Machine,
) -> io::Result<(SnapshotMeta<u64, BasicNode>, Vec<u8>)> {
    let data = encode_snapshot(machine)?;
    let meta = SnapshotMeta {
        last_log_id: machine.applied,
        last_membership: machine.membership.clone(),
        snapshot_id: format!("{:?}", machine.applied),
    };
    Ok((meta, data))
}

impl RaftSnapshotBuilder<RoomTypes> for Store {
    /// Encodes outside the store lock, so apply and the actor's reads do not
    /// wait for it.
    async fn build_snapshot(&mut self) -> Result<Snapshot<RoomTypes>, StoreError> {
        let machine = self.snapshot_source().await;
        let (meta, data) =
            snapshot_of(&machine).map_err(|e| StorageIOError::read_state_machine(&e))?;
        self.keep_snapshot(&meta, &data).await;
        Ok(Snapshot {
            meta,
            snapshot: Box::new(Cursor::new(data)),
        })
    }
}
impl RaftStorage<RoomTypes> for Store {
    type LogReader = Self;
    type SnapshotBuilder = Self;
    async fn save_vote(&mut self, vote: &Vote<u64>) -> Result<(), StoreError> {
        self.0.lock().await.vote = Some(*vote);
        Ok(())
    }
    async fn read_vote(&mut self) -> Result<Option<Vote<u64>>, StoreError> {
        Ok(self.0.lock().await.vote)
    }
    async fn get_log_state(&mut self) -> Result<LogState<RoomTypes>, StoreError> {
        let memory = self.0.lock().await;
        Ok(LogState {
            last_purged_log_id: memory.purged,
            last_log_id: memory
                .log
                .last_key_value()
                .map(|(_, e)| e.log_id)
                .or(memory.purged),
        })
    }
    async fn get_log_reader(&mut self) -> Self {
        self.clone()
    }
    async fn append_to_log<I>(&mut self, entries: I) -> Result<(), StoreError>
    where
        I: IntoIterator<Item = Entry<RoomTypes>> + OptionalSend,
    {
        let mut memory = self.0.lock().await;
        for entry in entries {
            memory.log.insert(entry.log_id.index, entry);
        }
        Ok(())
    }
    async fn delete_conflict_logs_since(&mut self, id: LogId<u64>) -> Result<(), StoreError> {
        self.0.lock().await.log.retain(|index, _| *index < id.index);
        Ok(())
    }
    async fn purge_logs_upto(&mut self, id: LogId<u64>) -> Result<(), StoreError> {
        let mut memory = self.0.lock().await;
        memory.purged = Some(id);
        memory.log.retain(|index, _| *index > id.index);
        Ok(())
    }
    async fn last_applied_state(
        &mut self,
    ) -> Result<(Option<LogId<u64>>, StoredMembership<u64, BasicNode>), StoreError> {
        let memory = self.0.lock().await;
        Ok((memory.machine.applied, memory.machine.membership.clone()))
    }
    async fn apply_to_state_machine(
        &mut self,
        entries: &[Entry<RoomTypes>],
    ) -> Result<Vec<Receipt>, StoreError> {
        let mut memory = self.0.lock().await;
        let machine = &mut memory.machine;
        let mut receipts = Vec::with_capacity(entries.len());
        let mut replaced = 0u64;
        for entry in entries {
            machine.applied = Some(entry.log_id);
            let mut receipt = Receipt {
                accepted: false,
                revision: machine.committed.revision,
            };
            match &entry.payload {
                EntryPayload::Blank => {}
                EntryPayload::Membership(membership) => {
                    machine.apply_membership(entry.log_id, membership);
                }
                EntryPayload::Normal(proposal) => {
                    let term = entry.log_id.leader_id.term;
                    if let Some(AdminEntry::ProbeReservation(reservation)) = &proposal.admin {
                        receipt.accepted = machine.apply_probe(proposal, reservation, term);
                    } else if let Some(applied) = machine.apply_checkpoint(proposal, term) {
                        replaced += u64::from(applied.replaced);
                        receipt = applied.receipt;
                    }
                }
            }
            receipts.push(receipt);
        }
        memory.observations = memory.observations.saturating_add(replaced);
        memory.publish_commit();
        Ok(receipts)
    }
    async fn get_snapshot_builder(&mut self) -> Self {
        self.clone()
    }
    async fn begin_receiving_snapshot(&mut self) -> Result<Box<Cursor<Vec<u8>>>, StoreError> {
        Ok(Box::new(Cursor::new(Vec::new())))
    }
    async fn install_snapshot(
        &mut self,
        meta: &SnapshotMeta<u64, BasicNode>,
        data: Box<Cursor<Vec<u8>>>,
    ) -> Result<(), StoreError> {
        let bytes = data.into_inner();
        let max_nodes = self.0.lock().await.max_nodes;
        if bytes.len() > MAX_SNAPSHOT {
            return Err(StorageIOError::write_state_machine(&io::Error::other(
                "room snapshot too large",
            ))
            .into());
        }
        let machine =
            decode_snapshot(&bytes).map_err(|e| StorageIOError::write_state_machine(&e))?;
        if machine.applied != meta.last_log_id
            || machine.membership != meta.last_membership
            || machine.committed.checkpoint.len() > MAX_CHECKPOINT
            || machine.recent.len() > RECENT_REQUESTS
            || machine.probes.len() > MAX_MEMBERS
            || machine.membership.nodes().count() > max_nodes
            || machine.member_history.len() > MAX_MEMBER_HISTORY
            || machine.departed_order.len() > MAX_RETIRED_MEMBER_HISTORY
            || machine
                .departed_order
                .iter()
                .any(|id| !machine.member_history.contains(id))
            || machine.member_history.contains(&0)
            || machine.recent.iter().any(|(id, _, receipt)| {
                id.is_empty()
                    || id.len() > 128
                    || !receipt.accepted
                    || receipt.revision > machine.committed.revision
            })
            || machine
                .recent
                .iter()
                .enumerate()
                .any(|(index, (id, _, _))| {
                    machine.recent[..index]
                        .iter()
                        .any(|(other, _, _)| other == id)
                })
            || machine
                .probes
                .iter()
                .any(|reservation| !reservation.names_its_pair() || reservation.term == 0)
        {
            return Err(StorageIOError::write_state_machine(&io::Error::other(
                "invalid room checkpoint",
            ))
            .into());
        }
        let mut memory = self.0.lock().await;
        memory.machine = machine;
        memory.snapshot = Some((meta.clone(), bytes));
        memory.observations = memory.observations.saturating_add(1);
        memory.publish_commit();
        Ok(())
    }
    async fn get_current_snapshot(&mut self) -> Result<Option<Snapshot<RoomTypes>>, StoreError> {
        Ok(self
            .0
            .lock()
            .await
            .snapshot
            .as_ref()
            .map(|(meta, data)| Snapshot {
                meta: meta.clone(),
                snapshot: Box::new(Cursor::new(data.clone())),
            }))
    }
}
