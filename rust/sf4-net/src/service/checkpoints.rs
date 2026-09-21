//! Room checkpoint transfer: inbound begin/chunk/end, outbound pump, acknowledgements.
use super::*;

impl Actor {
    pub(super) fn start_outgoing_checkpoint(&mut self, transfer: CheckpointTransfer) {
        // One native receiver owns one reassembly buffer. Never replace an
        // export already in flight; the newly committed revision remains in
        // the replicated state machine and the watcher starts it after the
        // current export completes or times out.
        if self.outgoing_transfer.is_some() {
            return;
        }
        self.outgoing_transfer = Some(OutgoingCheckpoint {
            transfer,
            next_offset: 0,
            in_flight: BTreeSet::new(),
            acked_offset: 0,
            end_sent: false,
            begin_sent: false,
            started: tokio::time::Instant::now(),
        });
        // A completed transfer is not visible to native code until its marker
        // arrives. Preserve revision order when a new commit becomes available
        // while that marker is waiting for IPC capacity.
        self.pump_pending_checkpoint_committed();
        self.pump_outgoing_checkpoint();
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) async fn checkpoint_begin(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    ) -> io::Result<()> {
        if !self.matches(epoch) || self.room != Some(room) {
            return self.error(0, "checkpoint_transfer_busy");
        }
        if self.outgoing_transfer.is_some() {
            return self.error(0, "checkpoint_transfer_busy_outgoing");
        }
        let candidate_digest = IncomingTransfer::validate_begin(
            room,
            transfer,
            term,
            base_revision,
            revision,
            length as usize,
            &digest,
        )?;
        if let Some(pending) = self.pending_checkpoint_proposal.as_ref() {
            let exact_retry = pending.epoch == epoch
                && pending.room == room
                && pending.transfer == transfer
                && pending.term == term
                && pending.base_revision == base_revision
                && pending.revision == revision
                && pending.length == length as usize
                && pending.digest == candidate_digest;
            if !exact_retry {
                return self.error(0, "checkpoint_transfer_busy_proposal");
            }
            // The pending Raft task already owns the validated body. Track
            // only a bounded cursor for an exact native retry, rather than
            // allocating another checkpoint-sized reassembly buffer.
            self.pending_checkpoint_retry = Some((pending.clone(), 0));
            return Ok(());
        }
        if let Some(incoming) = self.incoming_transfer.as_mut() {
            let exact_retry = incoming.room == room
                && incoming.transfer == transfer
                && incoming.term == term
                && incoming.base_revision == base_revision
                && incoming.revision == revision
                && incoming.length == length as usize
                && incoming.digest == candidate_digest;
            if !exact_retry {
                return self.error(0, "checkpoint_transfer_busy_incoming");
            }
            // Retain the validated prefix and renew only an identical native
            // retry. Replayed chunks must byte-match that prefix before they
            // receive fresh sender credit.
            incoming.started = tokio::time::Instant::now();
            return Ok(());
        }
        self.incoming_transfer = Some(IncomingTransfer::begin(
            room,
            transfer,
            term,
            base_revision,
            revision,
            length as usize,
            &digest,
        )?);
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) async fn checkpoint_chunk(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        offset: u32,
        data: String,
    ) -> io::Result<()> {
        let mut retry = false;
        let next_offset = if let Some(incoming) = self.incoming_transfer.as_mut() {
            if incoming.room != room
                || incoming.transfer != transfer
                || incoming.term != term
                || incoming.base_revision != base_revision
                || incoming.revision != revision
            {
                Err(failed("invalid checkpoint identity"))
            } else {
                incoming
                    .push(offset as usize, &data)
                    .map(|acknowledged| acknowledged as u32)
            }
        } else if let Some((pending, cursor)) = self.pending_checkpoint_retry.as_mut() {
            retry = true;
            let decoded = URL_SAFE_NO_PAD
                .decode(data.as_bytes())
                .map_err(|_| failed("invalid checkpoint retry chunk"));
            decoded.and_then(|decoded| {
                let end = (*cursor).saturating_add(decoded.len());
                if pending.epoch != epoch
                    || pending.room != room
                    || pending.transfer != transfer
                    || pending.term != term
                    || pending.base_revision != base_revision
                    || pending.revision != revision
                    || offset as usize != *cursor
                    || decoded.is_empty()
                    || decoded.len() > CHECKPOINT_CHUNK_BYTES
                    || end > pending.length
                {
                    return Err(failed("invalid checkpoint retry chunk"));
                }
                *cursor = end;
                Ok(end as u32)
            })
        } else {
            Err(failed("checkpoint transfer missing"))
        };
        let Ok(next_offset) = next_offset else {
            if retry {
                self.pending_checkpoint_retry = None;
            } else {
                self.incoming_transfer = None;
            }
            return self.error(0, "invalid_checkpoint_chunk");
        };
        let pending_ack = (epoch, room, transfer, next_offset);
        self.pending_checkpoint_ack = None;
        if !self.emit_bulk(Event::CheckpointAck {
            epoch,
            room,
            transfer,
            // The acknowledgement is the next contiguous byte offset. This
            // is the sender's credit and lets a four-chunk window advance.
            offset: next_offset,
        }) {
            // Keep the latest cumulative credit and retry it on the next
            // actor turn. Dropping this event would permanently stall the
            // sender's bounded four-chunk window.
            self.pending_checkpoint_ack = Some(pending_ack);
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) async fn checkpoint_end(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: u32,
        digest: String,
    ) -> io::Result<()> {
        if let Some(pending) = self.pending_checkpoint_proposal.as_ref() {
            let retry_complete = self
                .pending_checkpoint_retry
                .as_ref()
                .is_none_or(|(key, cursor)| key == pending && *cursor == pending.length);
            let exact = pending.epoch == epoch
                && pending.room == room
                && pending.transfer == transfer
                && pending.term == term
                && pending.base_revision == base_revision
                && pending.revision == revision
                && pending.length == length as usize
                && recovery::hex_digest(&pending.digest) == digest;
            if exact && retry_complete {
                self.pending_checkpoint_retry = None;
                return Ok(());
            }
            return self.error(0, "checkpoint_transfer_busy_proposal");
        }
        if self.tasks.len() >= MAX_TASKS {
            return self.error(0, "checkpoint_transfer_busy");
        }
        let Some(incoming) = self.incoming_transfer.take() else {
            return self.error(0, "checkpoint_transfer_missing");
        };
        if !self.matches(epoch)
            || self.room != Some(room)
            || incoming.room != room
            || incoming.transfer != transfer
            || incoming.term != term
            || incoming.base_revision != base_revision
            || incoming.revision != revision
            || incoming.length != length as usize
            || recovery::hex_digest(&incoming.digest) != digest
        {
            return self.error(0, "invalid_checkpoint_end");
        }
        let transfer = incoming.finish()?;
        let Some(recovery) = self.recovery.clone() else {
            return self.error(0, "coordination_unavailable");
        };
        let proposal = recovery.propose(&transfer)?;
        let key = CheckpointProposalKey {
            epoch,
            room,
            incarnation: recovery.incarnation,
            transfer: transfer.transfer,
            term,
            base_revision,
            revision,
            length: transfer.bytes.len(),
            digest: transfer.digest,
        };
        self.pending_checkpoint_proposal = Some(key.clone());
        self.tasks.spawn(async move {
            let result = recovery.coordinator.propose(proposal).await;
            Completion::CheckpointProposal(key, transfer, result)
        });
        Ok(())
    }

    /// Every Raft replica must replay each newly applied committed checkpoint
    /// to its own native process. This watcher is intentionally independent of
    /// leader status: followers become usable replicas after applying the
    /// committed log, while native writes remain gated by `leader_local`.
    pub(super) async fn pump_committed_checkpoint(&mut self) -> io::Result<()> {
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        if self.incoming_transfer.is_some()
            || self.outgoing_transfer.is_some()
            || self.pending_checkpoint_committed.is_some()
        {
            return Ok(());
        }
        let committed = recovery.committed().await;
        if committed.revision == 0 || committed.revision <= self.last_exported_revision {
            return Ok(());
        }
        let Ok(transfer_id) = committed.request.parse::<u64>() else {
            // Only helper proposals created from a native SessionProposal are
            // replayable. Keep the revision pending so an invalid candidate
            // cannot be mistaken for a committed native checkpoint.
            return Ok(());
        };
        if transfer_id == 0 || committed.term == 0 {
            return Ok(());
        }
        let Some(base_revision) = committed.revision.checked_sub(1) else {
            return Ok(());
        };
        let transfer = CheckpointTransfer::new(
            recovery.room,
            transfer_id,
            committed.term,
            base_revision,
            committed.revision,
            committed.checkpoint.into_bytes(),
        )?;
        if let Some(retained) = committed_primary_endpoints(&transfer.bytes) {
            self.schedule_membership_reconciliation(retained, committed.term, committed.revision);
        }
        if self
            .pending_checkpoint_proposal
            .as_ref()
            .is_some_and(|key| {
                key.epoch == self.epoch
                    && key.room == transfer.room
                    && key.incarnation == recovery.incarnation
                    && key.transfer == transfer.transfer
                    && key.term == transfer.term
                    && key.base_revision == transfer.base_revision
                    && key.revision == transfer.revision
                    && key.length == transfer.bytes.len()
                    && key.digest == transfer.digest
            })
        {
            // Durable local application, rather than the client-write waiter,
            // completes proposal ownership. A late worker result is fenced by
            // the missing key and cannot replay the effect twice.
            self.pending_checkpoint_proposal = None;
            self.pending_checkpoint_retry = None;
        }
        self.start_outgoing_checkpoint(transfer);
        Ok(())
    }

    pub(super) fn pump_pending_checkpoint_ack(&mut self) {
        let Some((epoch, room, transfer, offset)) = self.pending_checkpoint_ack else {
            return;
        };
        if self.emit_bulk(Event::CheckpointAck {
            epoch,
            room,
            transfer,
            offset,
        }) {
            self.pending_checkpoint_ack = None;
        }
    }

    pub(super) fn pump_pending_checkpoint_committed(&mut self) {
        let Some(marker) = self.pending_checkpoint_committed.clone() else {
            return;
        };
        if self.emit_bulk(Event::CheckpointCommitted {
            epoch: marker.epoch,
            room: marker.room,
            transfer: marker.transfer,
            term: marker.term,
            base_revision: marker.base_revision,
            revision: marker.revision,
            length: marker.length,
            digest: marker.digest,
        }) {
            self.pending_checkpoint_committed = None;
            self.last_exported_revision = marker.revision;
        }
    }

    pub(super) fn expire_checkpoint_transfers(&mut self) {
        let now = tokio::time::Instant::now();
        if self.incoming_transfer.as_ref().is_some_and(|transfer| {
            now.duration_since(transfer.started) > CHECKPOINT_TRANSFER_TIMEOUT
        }) {
            self.incoming_transfer = None;
            self.pending_checkpoint_ack = None;
            let _ = self.emit_bulk(Event::Error {
                probe_failure: None,
                request_id: 0,
                epoch: self.epoch,
                peer: None,
                code: "checkpoint_receive_timeout".into(),
            });
        }
        if self.outgoing_transfer.as_ref().is_some_and(|transfer| {
            now.duration_since(transfer.started) > CHECKPOINT_TRANSFER_TIMEOUT
        }) {
            let revision = self
                .outgoing_transfer
                .as_ref()
                .map(|transfer| transfer.transfer.revision)
                .unwrap_or_default();
            self.outgoing_transfer = None;
            // A timed-out transfer must be replayed from the applied commit;
            // do not let a previously observed revision suppress retry.
            if revision != 0 && self.last_exported_revision >= revision {
                self.last_exported_revision = revision.saturating_sub(1);
            }
            let _ = self.emit_bulk(Event::Error {
                probe_failure: None,
                request_id: 0,
                epoch: self.epoch,
                peer: None,
                code: "checkpoint_send_timeout".into(),
            });
        }
    }

    pub(super) fn checkpoint_ack(
        &mut self,
        epoch: u64,
        room: [u8; 16],
        transfer: u64,
        offset: u32,
    ) -> io::Result<()> {
        if !self.matches(epoch) || self.room != Some(room) {
            return self.error(0, "stale_checkpoint_ack");
        }
        let Some(outgoing) = self.outgoing_transfer.as_mut() else {
            return self.error(0, "checkpoint_transfer_missing");
        };
        if outgoing.transfer.transfer != transfer {
            return self.error(0, "stale_checkpoint_ack");
        }
        if offset as usize > outgoing.transfer.bytes.len() {
            return self.error(0, "invalid_checkpoint_ack");
        }
        if offset as usize == outgoing.transfer.bytes.len() && outgoing.end_sent {
            let finished = outgoing.transfer.transfer;
            let transfer = outgoing.transfer.clone();
            self.outgoing_transfer = None;
            self.pending_checkpoint_committed = Some(PendingCheckpointCommitted {
                epoch,
                room,
                transfer: finished,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                length: transfer.bytes.len() as u32,
                digest: transfer.digest_hex(),
            });
            self.pump_pending_checkpoint_committed();
            return Ok(());
        }
        if offset <= outgoing.acked_offset {
            return Ok(());
        }
        // Credit advances only to an exact chunk end which this sender has
        // placed in flight. An arbitrary byte offset must not manufacture a
        // fifth credit or skip bytes the receiver never observed.
        if !outgoing.in_flight.contains(&offset) {
            return self.error(0, "invalid_checkpoint_ack");
        }
        outgoing.in_flight.retain(|end| *end > offset);
        outgoing.acked_offset = offset;
        self.pump_outgoing_checkpoint();
        Ok(())
    }

    pub(super) fn pump_outgoing_checkpoint(&mut self) {
        // The receiver cannot make a newer revision visible before the
        // completion marker for the preceding transfer. Keep this guard at
        // the emission seam: IPC capacity can become available after the
        // marker pump failed but before this pump runs again.
        if self.pending_checkpoint_committed.is_some() {
            return;
        }
        let Some(mut outgoing) = self.outgoing_transfer.take() else {
            return;
        };
        let transfer = &outgoing.transfer;
        if !outgoing.begin_sent {
            if !self.emit_bulk(Event::CheckpointBegin {
                epoch: self.epoch,
                room: transfer.room,
                transfer: transfer.transfer,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                length: transfer.bytes.len() as u32,
                digest: transfer.digest_hex(),
            }) {
                self.outgoing_transfer = Some(outgoing);
                return;
            }
            outgoing.begin_sent = true;
        }
        while outgoing.in_flight.len() < CHECKPOINT_WINDOW
            && outgoing.next_offset < transfer.bytes.len()
        {
            let offset = outgoing.next_offset;
            let end = (offset + CHECKPOINT_CHUNK_BYTES).min(transfer.bytes.len());
            let data = URL_SAFE_NO_PAD.encode(&transfer.bytes[offset..end]);
            if !self.emit_bulk(Event::CheckpointChunk {
                epoch: self.epoch,
                room: transfer.room,
                transfer: transfer.transfer,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                offset: offset as u32,
                data,
            }) {
                self.outgoing_transfer = Some(outgoing);
                return;
            }
            // In-flight keys are cumulative end offsets. The receiver ACKs
            // its next contiguous offset, so one ACK may retire several
            // chunks under backpressure.
            outgoing.in_flight.insert(end as u32);
            outgoing.next_offset = end;
        }
        if outgoing.next_offset == transfer.bytes.len()
            && outgoing.in_flight.is_empty()
            && !outgoing.end_sent
            && self.emit_bulk(Event::CheckpointEnd {
                epoch: self.epoch,
                room: transfer.room,
                transfer: transfer.transfer,
                term: transfer.term,
                base_revision: transfer.base_revision,
                revision: transfer.revision,
                length: transfer.bytes.len() as u32,
                digest: transfer.digest_hex(),
            })
        {
            outgoing.end_sent = true;
        }
        self.outgoing_transfer = Some(outgoing);
    }

    pub(super) async fn completed_checkpoint_proposal(
        &mut self,
        key: CheckpointProposalKey,
        transfer: CheckpointTransfer,
        _waiter_result: io::Result<crate::coordination::Receipt>,
    ) -> io::Result<()> {
        if self.pending_checkpoint_proposal.as_ref() != Some(&key) {
            return Ok(());
        }
        self.pending_checkpoint_proposal = None;
        self.pending_checkpoint_retry = None;
        let Some(recovery) = self.recovery.clone() else {
            return Ok(());
        };
        if key.epoch != self.epoch
            || self.room != Some(key.room)
            || recovery.room != key.room
            || recovery.incarnation != key.incarnation
        {
            return Ok(());
        }
        // A waiter error is not proof that a Raft write failed. Only
        // the locally applied committed record authorizes native
        // replay and membership effects; the periodic watcher uses
        // this same durable observation if the waiter never returns.
        let committed = recovery.committed().await;
        let committed_exact = committed.revision == key.revision
            && committed.term == key.term
            && committed.request == key.transfer.to_string()
            && committed.checkpoint.as_bytes() == transfer.bytes
            && transfer.room == key.room
            && transfer.transfer == key.transfer
            && transfer.base_revision == key.base_revision
            && transfer.revision == key.revision
            && transfer.bytes.len() == key.length
            && transfer.digest == key.digest;
        if committed_exact {
            if let Some(retained) = committed_primary_endpoints(&transfer.bytes) {
                self.schedule_membership_reconciliation(
                    retained,
                    committed.term,
                    committed.revision,
                );
            }
            if committed.revision > self.last_exported_revision
                && self.outgoing_transfer.is_none()
                && self.pending_checkpoint_committed.is_none()
            {
                self.start_outgoing_checkpoint(transfer);
            }
        }
        self.last_coordination_state = None;
        self.emit_coordination_state().await?;
        Ok(())
    }
}
