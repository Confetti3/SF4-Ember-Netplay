//! Tests only: drive the state machine as Raft would, so the latency tests
//! in the helper can apply batches and install snapshots on a live room's
//! store.
use super::*;

impl Store {
    /// Applies `checkpoints` as one batch of committed native entries, each
    /// on the revision before it. Returns how many were accepted.
    pub async fn apply_checkpoints(&self, checkpoints: &[String]) -> usize {
        use openraft::{CommittedLeaderId, Entry, EntryPayload, LogId, RaftStorage};
        let (mut index, term, mut base) = {
            let memory = self.0.lock().await;
            let machine = &memory.machine;
            (
                machine.applied.map_or(0, |id| id.index),
                machine.committed.term().max(1),
                machine.committed.revision(),
            )
        };
        let entries: Vec<Entry<RoomTypes>> = checkpoints
            .iter()
            .enumerate()
            .map(|(offset, checkpoint)| {
                index += 1;
                let proposal = Proposal {
                    request: format!("latency-test-{index}"),
                    dedup_id: format!("latency-test-{index}-{offset}"),
                    term,
                    base,
                    checkpoint: checkpoint.as_str().into(),
                    admin: None,
                };
                base += 1;
                Entry {
                    log_id: LogId::new(CommittedLeaderId::new(term, 0), index),
                    payload: EntryPayload::Normal(proposal),
                }
            })
            .collect();
        let receipts = self
            .clone()
            .apply_to_state_machine(&entries)
            .await
            .expect("apply");
        receipts.iter().filter(|receipt| receipt.accepted).count()
    }

    /// Holds the store lock until the returned guard is dropped, as a long
    /// apply or snapshot build would.
    pub async fn hold_lock(&self) -> impl Sized + '_ {
        self.0.lock().await
    }

    /// Installs a snapshot of the current state machine whose committed
    /// checkpoint is `checkpoint`, at the same revision and term.
    pub async fn install_checkpoint_snapshot(&self, checkpoint: &str) {
        use openraft::{RaftStorage, SnapshotMeta};
        let (meta, bytes) = {
            let memory = self.0.lock().await;
            let mut machine = memory.machine.clone();
            let committed = &memory.machine.committed;
            machine.committed = std::sync::Arc::new(Committed::new(
                committed.revision(),
                checkpoint.into(),
                committed.request().to_owned(),
                committed.term(),
            ));
            let meta = SnapshotMeta {
                last_log_id: machine.applied,
                last_membership: machine.membership.clone(),
                snapshot_id: format!("latency-test-{}", memory.observations),
            };
            (meta, encode_snapshot(&machine).expect("snapshot"))
        };
        self.clone()
            .install_snapshot(&meta, Box::new(std::io::Cursor::new(bytes)))
            .await
            .expect("install");
    }
}
