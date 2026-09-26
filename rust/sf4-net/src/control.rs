//! Bounded nonblocking handoff between the IPC actor and one room connection.
//! A slow peer cannot grow process memory or block other peers/gameplay.
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

use iroh::endpoint::Connection;
use tokio::{sync::mpsc, task::JoinHandle};

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
    outgoing: mpsc::Sender<ControlFrame>,
    incoming: mpsc::Receiver<ControlFrame>,
    tasks: [JoinHandle<()>; 2],
    rejected: Arc<AtomicU64>,
    session: Session,
}

impl ControlWorker {
    pub fn start(channel: ControlChannel) -> Self {
        let (outgoing, mut send_queue) = mpsc::channel(CONTROL_QUEUE_CAPACITY);
        let (recv_queue, incoming) = mpsc::channel(CONTROL_QUEUE_CAPACITY);
        let rejected = Arc::new(AtomicU64::new(0));
        let connection = channel.connection;
        let mut sender = channel.sender;
        let mut receiver = channel.receiver;
        let send_connection = connection.clone();
        let recv_connection = connection.clone();
        let tasks = [
            tokio::spawn(async move {
                while let Some(frame) = send_queue.recv().await {
                    if sender.send(&frame).await.is_err() {
                        break;
                    }
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
            outgoing,
            incoming,
            tasks,
            rejected,
            session: Session::Unbound,
        }
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
        self.outgoing.try_send(frame).map_err(|error| {
            self.rejected.fetch_add(1, Ordering::Relaxed);
            match error {
                mpsc::error::TrySendError::Full(_) => QueueError::Full,
                mpsc::error::TrySendError::Closed(_) => QueueError::Closed,
            }
        })
    }

    pub fn try_receive(&mut self) -> Option<ControlFrame> {
        self.incoming.try_recv().ok()
    }
    pub fn is_closed(&self) -> bool {
        self.connection.close_reason().is_some()
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
