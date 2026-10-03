//! Bounded nonblocking handoff between the IPC actor and one room connection.
//! A slow peer cannot grow process memory or block other peers/gameplay.
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

use ember_protocol::EmberId;
use iroh::endpoint::Connection;
use tokio::{
    sync::mpsc,
    task::JoinHandle,
    time::{Instant, timeout_at},
};

use crate::{
    transport::ControlChannel,
    wire::{ControlFrame, MAX_CONTROL_PAYLOAD},
};

pub const CONTROL_QUEUE_CAPACITY: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum QueueError {
    Invalid,
    Full,
    Closed,
}

impl QueueError {
    /// The reason a refused native send reports to the native side.
    pub fn label(self) -> &'static str {
        match self {
            Self::Invalid => "invalid",
            Self::Full => "queue_full",
            Self::Closed => "queue_closed",
        }
    }
}

/// Whether the endpoint behind a control has an accepted room session on it.
/// Native messages are read from a control only while it is not `Pending`:
/// the native side must learn of a session before any message from it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Session {
    /// No Admission received yet.
    Unbound,
    /// An Admission for this incarnation is being applied; that operation's
    /// outcome binds or closes the control. A completion for any other
    /// incarnation, such as a replaced control's, leaves it untouched.
    Pending(u64),
    /// Accepted for this process incarnation.
    Bound(u64),
}

pub struct ControlWorker {
    connection: Connection,
    /// Dropped by `finish`, which ends the writer's queue.
    outgoing: Option<mpsc::Sender<ControlFrame>>,
    incoming: mpsc::Receiver<ControlFrame>,
    tasks: [JoinHandle<()>; 2],
    rejected: Arc<AtomicU64>,
    session: Session,
    account: Option<EmberId>,
}

impl ControlWorker {
    pub fn start(channel: ControlChannel) -> Self {
        let (outgoing, mut send_queue) = mpsc::channel(CONTROL_QUEUE_CAPACITY);
        let (recv_queue, incoming) = mpsc::channel(CONTROL_QUEUE_CAPACITY);
        let rejected = Arc::new(AtomicU64::new(0));
        let account = channel.account;
        let connection = channel.connection;
        let mut sender = channel.sender;
        let mut receiver = channel.receiver;
        let send_connection = connection.clone();
        let recv_connection = connection.clone();
        let tasks = [
            tokio::spawn(async move {
                let mut delivered = true;
                while let Some(frame) = send_queue.recv().await {
                    if sender.send(&frame).await.is_err() {
                        delivered = false;
                        break;
                    }
                }
                // The queue ends only when `finish` drops its sender. The
                // stream is then finished and acknowledged before the
                // connection closes, so the last frames are not lost with it.
                if delivered {
                    sender.finish().await;
                }
                send_connection.close(1u32.into(), b"control writer ended");
            }),
            tokio::spawn(async move {
                while let Ok(frame) = receiver.receive().await {
                    // The bounded queue is also the QUIC receive credit. Pause
                    // this stream when the actor is busy so ordered lifecycle
                    // events remain on the authenticated connection instead of
                    // turning ordinary bulk backpressure into room recovery.
                    if recv_queue.send(frame).await.is_err() {
                        break;
                    }
                }
                recv_connection.close(1u32.into(), b"control reader ended");
            }),
        ];
        Self {
            connection,
            outgoing: Some(outgoing),
            incoming,
            tasks,
            rejected,
            session: Session::Unbound,
            account,
        }
    }

    /// The Ember ID a public room's ticket admitted this connection under.
    pub fn account(&self) -> Option<&EmberId> {
        self.account.as_ref()
    }

    pub fn session(&self) -> Session {
        self.session
    }

    pub fn set_session(&mut self, session: Session) {
        self.session = session;
    }

    /// Stable for the life of this QUIC connection. A close reported for a
    /// superseded connection can then be told from one for the current one.
    pub fn id(&self) -> u64 {
        self.connection.stable_id() as u64
    }

    pub fn try_send(&self, frame: ControlFrame) -> Result<(), QueueError> {
        if frame.payload.is_empty()
            || frame.payload.len() > MAX_CONTROL_PAYLOAD
            || frame.message_id <= 1
            || frame.message_id > i64::MAX as u64
        {
            self.rejected.fetch_add(1, Ordering::Relaxed);
            return Err(QueueError::Invalid);
        }
        if self.is_closed() {
            return Err(QueueError::Closed);
        }
        let Some(outgoing) = self.outgoing.as_ref() else {
            return Err(QueueError::Closed);
        };
        outgoing.try_send(frame).map_err(|error| {
            self.rejected.fetch_add(1, Ordering::Relaxed);
            match error {
                mpsc::error::TrySendError::Full(_) => QueueError::Full,
                mpsc::error::TrySendError::Closed(_) => QueueError::Closed,
            }
        })
    }

    /// Stop accepting frames. The writer delivers what is already queued and
    /// then finishes the stream; `finished` waits for that.
    pub fn finish(&mut self) {
        self.outgoing = None;
    }

    /// Wait until the writer has delivered everything queued before `finish`
    /// and the peer has acknowledged it, or until `deadline`. Dropping the
    /// worker afterwards closes the connection either way.
    pub async fn finished(&mut self, deadline: Instant) {
        let _ = timeout_at(deadline, &mut self.tasks[0]).await;
    }

    pub fn try_receive(&mut self) -> Option<ControlFrame> {
        self.incoming.try_recv().ok()
    }
    /// Close the connection now. Frames the reader already queued stay
    /// receivable, so a worker being replaced is closed and then drained.
    pub fn close(&self) {
        self.connection.close(1u32.into(), b"control replaced");
    }

    /// The transport is gone. Frames it delivered before closing may still be
    /// queued for the actor; `is_drained` says when none remain.
    pub fn is_closed(&self) -> bool {
        self.connection.close_reason().is_some()
    }

    /// The reader has ended, so nothing more will be queued, and the actor has
    /// taken every frame it queued. A finished and acknowledged stream only
    /// means the frames reached the reader, so a closed worker is removed
    /// once this holds, not when the transport closes.
    pub fn is_drained(&self) -> bool {
        // The reader is checked first: after it has ended the queue can only
        // shrink, so an empty queue then is final.
        self.tasks[1].is_finished() && self.incoming.is_empty()
    }
    pub fn rejected(&self) -> u64 {
        self.rejected.load(Ordering::Relaxed)
    }
}

impl Drop for ControlWorker {
    fn drop(&mut self) {
        self.connection.close(0u32.into(), b"room ended");
        for task in &self.tasks {
            task.abort();
        }
    }
}
