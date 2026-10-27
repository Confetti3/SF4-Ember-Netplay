//! The proofs a departing helper waits for before it closes its routes:
//! a successor's committed claim, or its own removal from the quorum.
use std::{collections::BTreeSet, io};

use super::RecoverySession;

impl RecoverySession {
    /// Confirm that a successor has installed the committed authority claim
    /// before this process closes its coordination and primary routes.
    pub async fn confirm_successor(
        &self,
        successor: u64,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let claim = self.rpc.authority_claim(successor).await?;
        if !claim.is_leader_at(successor, minimum_term, minimum_revision) {
            return Err(io::Error::other("successor authority claim not committed"));
        }
        Ok(())
    }

    /// A removed leader can retain a stale leader view. Query every remaining
    /// admitted voter so a slow follower cannot hide the elected successor.
    pub async fn confirm_available_successor(
        &self,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let mut candidates = self.coordinator.store().applied_voters().await;
        candidates.remove(&self.incarnation);
        let recovery = self.clone();
        first_confirmed_successor(candidates, move |successor| {
            let recovery = recovery.clone();
            async move {
                recovery
                    .confirm_successor(successor, minimum_term, minimum_revision)
                    .await
            }
        })
        .await
    }

    /// Confirm a follower's committed removal through the current leader.
    /// OpenRaft's retain=false membership change may stop replicating to the
    /// departing process before its local metrics learn that it is no longer
    /// a voter.  The authenticated leader claim is therefore the final proof
    /// used by a healthy non-leader departure.
    pub async fn confirm_departure(
        &self,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let successor = self
            .coordinator
            .current_leader()
            .filter(|id| *id != self.incarnation)
            .ok_or_else(|| io::Error::other("successor authority unavailable"))?;
        let claim = self.rpc.authority_claim(successor).await?;
        if !claim.is_leader_at(successor, minimum_term, minimum_revision)
            // An empty set is also rejected so a pre-membership/legacy
            // authority response cannot be mistaken for learner exclusion.
            || claim.members.is_empty()
            || claim.members.contains(&self.incarnation)
            || claim.voters.contains(&self.incarnation)
        {
            return Err(io::Error::other("departure remains in committed quorum"));
        }
        Ok(())
    }

    /// Confirm that this helper is the sole committed voter after its
    /// successor has already retired. This covers an explicit final room
    /// teardown where two healthy helpers issue Leave together: the former
    /// leader remains available for a short authority-read grace window, but
    /// there is no successor quorum left to confirm a second departure.
    pub async fn confirm_singleton_departure(
        &self,
        minimum_term: u64,
        minimum_revision: u64,
    ) -> io::Result<()> {
        let successor = self
            .coordinator
            .current_leader()
            .filter(|id| *id != self.incarnation)
            .or_else(|| {
                self.coordinator
                    .voter_ids()
                    .into_iter()
                    .find(|id| *id != self.incarnation)
            })
            .ok_or_else(|| io::Error::other("former authority route unavailable"))?;
        let claim = self.rpc.authority_claim(successor).await?;
        if claim.term < minimum_term
            || claim.revision < minimum_revision
            || claim.leader != Some(self.incarnation)
            || claim.voters.len() != 1
            || !claim.voters.contains(&self.incarnation)
        {
            return Err(io::Error::other("singleton authority not committed"));
        }
        Ok(())
    }
}

async fn first_confirmed_successor<F, Fut>(candidates: BTreeSet<u64>, confirm: F) -> io::Result<()>
where
    F: Fn(u64) -> Fut,
    Fut: std::future::Future<Output = io::Result<()>> + Send + 'static,
{
    let mut claims = tokio::task::JoinSet::new();
    for candidate in candidates {
        claims.spawn(confirm(candidate));
    }
    while let Some(result) = claims.join_next().await {
        if matches!(result, Ok(Ok(()))) {
            return Ok(());
        }
    }
    Err(io::Error::other("successor authority unavailable"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[tokio::test]
    async fn elected_successor_is_not_hidden_by_a_stalled_first_voter() {
        tokio::time::timeout(
            Duration::from_millis(200),
            first_confirmed_successor(BTreeSet::from([1, 2, 3]), |candidate| async move {
                match candidate {
                    1 => std::future::pending().await,
                    2 => Err(io::Error::other("not the elected leader")),
                    _ => Ok(()),
                }
            }),
        )
        .await
        .expect("first voter blocked the elected successor")
        .unwrap();
        assert!(
            first_confirmed_successor(BTreeSet::from([1, 2]), |_| async {
                Err(io::Error::other("no quorum claim"))
            })
            .await
            .is_err()
        );
        assert!(
            first_confirmed_successor(BTreeSet::new(), |_| async { Ok(()) })
                .await
                .is_err()
        );
    }
}
