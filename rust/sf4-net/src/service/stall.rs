//! Names what the helper actor is doing when it stops turning over. Every
//! game command waits behind the actor, so a field log that only shows a
//! missing reply cannot tell a stalled actor from a lost event (F-008).
use super::*;
use std::sync::{Mutex, atomic::AtomicU64};

/// How long one actor step may run before it is reported.
pub(super) const STALL_REPORT: Duration = Duration::from_secs(1);
/// While a step stays stuck, it is reported again this often.
const STALL_REPEAT: Duration = Duration::from_secs(5);

/// The actor step in progress, shared with the watchdog task.
pub(super) struct Busy {
    origin: Instant,
    // Milliseconds after `origin` at which the current step began, plus one;
    // zero while the actor waits in `select!`.
    since: AtomicU64,
    stage: Mutex<&'static str>,
    events: mpsc::Sender<Event>,
}

/// Ends the step when dropped, on every exit path of a `select!` arm.
pub(super) struct Step<'a>(&'a Busy);

impl Busy {
    pub(super) fn new(events: mpsc::Sender<Event>) -> Arc<Self> {
        Arc::new(Self { origin: Instant::now(), since: AtomicU64::new(0), stage: Mutex::new(""), events })
    }

    pub(super) fn enter(&self, stage: &'static str) -> Step<'_> {
        self.stage(stage);
        self.since.store(self.now_ms() + 1, Ordering::Relaxed);
        Step(self)
    }

    /// Renames the running step without restarting its clock.
    pub(super) fn stage(&self, stage: &'static str) {
        *self.stage.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = stage;
    }

    /// Reports a step stuck past `STALL_REPORT`, while it is still stuck.
    pub(super) fn watch(self: &Arc<Self>) -> AbortHandle {
        let busy = self.clone();
        tokio::spawn(async move {
            let mut check = interval(Duration::from_millis(250));
            let (mut episode, mut next) = (0, STALL_REPORT);
            loop {
                check.tick().await;
                let since = busy.since.load(Ordering::Relaxed);
                if since == 0 {
                    continue;
                }
                if since != episode {
                    (episode, next) = (since, STALL_REPORT);
                }
                let stalled = busy.elapsed(since);
                if stalled >= next {
                    busy.report(stalled, false);
                    next = stalled + STALL_REPEAT;
                }
            }
        })
        .abort_handle()
    }

    fn now_ms(&self) -> u64 {
        u64::try_from(self.origin.elapsed().as_millis()).unwrap_or(u64::MAX - 1)
    }

    fn elapsed(&self, since: u64) -> Duration {
        Duration::from_millis(self.now_ms().saturating_sub(since - 1))
    }

    // Diagnostics never take the slots reserved for lifecycle events, and a
    // full queue drops the report rather than failing the helper.
    fn report(&self, stalled: Duration, ended: bool) {
        if self.events.capacity() <= LIFECYCLE_EVENT_RESERVE {
            return;
        }
        let stage = *self.stage.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let _ = self.events.try_send(Event::HelperStall {
            stage: stage.into(),
            stalled_ms: u64::try_from(stalled.as_millis()).unwrap_or(u64::MAX),
            ended,
        });
    }
}

impl Drop for Step<'_> {
    fn drop(&mut self) {
        let since = self.0.since.swap(0, Ordering::Relaxed);
        if since == 0 {
            return;
        }
        let stalled = self.0.elapsed(since);
        if stalled >= STALL_REPORT {
            self.0.report(stalled, true);
        }
    }
}

pub(super) fn command_stage(command: &Command) -> &'static str {
    match command {
        Command::Status => "command:status",
        Command::Host { .. } => "command:host",
        Command::Join { .. } => "command:join",
        Command::Leave { .. } => "command:leave",
        Command::Send { .. } => "command:send",
        Command::CloseControl { .. } => "command:close_control",
        Command::PrepareGame { .. } => "command:prepare_game",
        Command::EndMatch { .. } => "command:end_match",
        Command::EndPeer { .. } => "command:end_peer",
        Command::CheckpointBegin { .. } => "command:checkpoint_begin",
        Command::CheckpointChunk { .. } => "command:checkpoint_chunk",
        Command::CheckpointEnd { .. } => "command:checkpoint_end",
        Command::CheckpointAck { .. } => "command:checkpoint_ack",
        Command::ProbeRequest { .. } => "command:probe_request",
        Command::Shutdown => "command:shutdown",
    }
}

pub(super) fn completion_stage(completion: &Completion) -> &'static str {
    match completion {
        Completion::GuestControl(..) => "task:guest_control",
        Completion::Incoming(..) => "task:incoming",
        Completion::ClassifiedGame(..) => "task:classified_game",
        Completion::Hosted(..) => "task:hosted",
        Completion::Control(..) => "task:control",
        Completion::MemberControl(..) => "task:member_control",
        Completion::Reconnect(..) => "task:reconnect",
        Completion::Game(..) => "task:game",
        Completion::BridgeEnded(..) => "task:bridge_ended",
        Completion::CheckpointProposal(..) => "task:checkpoint_proposal",
        Completion::CoordinationRefresh(..) => "task:coordination_refresh",
        Completion::MembershipReconciliation(..) => "task:membership_reconciliation",
        Completion::Admission(..) => "task:admission",
        Completion::ProbeAuthorization(..) => "task:probe_authorization",
        Completion::Probe(..) => "task:probe",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn stuck_step_is_reported_while_stuck_and_when_it_ends() {
        let (events, mut receiver) = mpsc::channel(IPC_QUEUE_CAPACITY);
        let busy = Busy::new(events);
        let _watchdog = TaskScope(vec![busy.watch()]);
        {
            let _step = busy.enter("tick");
            busy.stage("tick:poll_controls");
            tokio::time::sleep(STALL_REPORT + Duration::from_millis(400)).await;
        }
        let mut seen = Vec::new();
        while let Ok(event) = receiver.try_recv() {
            if let Event::HelperStall { stage, stalled_ms, ended } = event {
                seen.push((stage, stalled_ms, ended));
            }
        }
        assert!(seen.iter().any(|(stage, ms, ended)| stage == "tick:poll_controls" && !ended && *ms >= 1000));
        assert!(matches!(seen.last(), Some((stage, ms, true)) if stage == "tick:poll_controls" && *ms >= 1000));
        // A quick step says nothing.
        drop(busy.enter("statistics"));
        tokio::time::sleep(Duration::from_millis(600)).await;
        assert!(receiver.try_recv().is_err());
    }
}
