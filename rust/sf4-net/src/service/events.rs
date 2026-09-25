//! The actor's event queue to the native side, with a backlog for the
//! events that must not be lost.
//!
//! The IPC queue holds `IPC_QUEUE_CAPACITY` events and the native side drains
//! eight per game tick. A lifecycle burst, such as an `end_match` closing
//! fifteen links or a room update fanning out while a member joins, can
//! overrun it for a few ticks. Every such event used to be `try_send` with
//! `?`, so a full queue ended the actor and every gameplay link with it
//! (F-017). Lifecycle events now wait here, in order, and move into the
//! queue as it drains; only a closed queue or a backlog that keeps growing
//! is fatal. Droppable events (statistics, load) are still sent only when
//! they cost nothing.

use std::{collections::VecDeque, io, sync::Mutex};

use tokio::sync::mpsc::{self, error::TrySendError};

use super::{Event, LIFECYCLE_EVENT_RESERVE, failed};

/// Held lifecycle events beyond this mean the native side stopped reading.
pub(super) const MAX_EVENT_BACKLOG: usize = 256;

pub(super) struct EventOutbox {
    sender: mpsc::Sender<Event>,
    // The actor emits through `&self` from one task; the lock is never held
    // across an await.
    backlog: Mutex<VecDeque<Event>>,
}

impl EventOutbox {
    pub(super) fn new(sender: mpsc::Sender<Event>) -> Self {
        Self {
            sender,
            backlog: Mutex::new(VecDeque::new()),
        }
    }

    /// A sender for a task that reports on its own, such as the stall watchdog.
    pub(super) fn sender(&self) -> mpsc::Sender<Event> {
        self.sender.clone()
    }

    /// Free queue slots right now.
    pub(super) fn capacity(&self) -> usize {
        self.sender.capacity()
    }

    /// True while more work may be taken on: the queue has room beyond the
    /// lifecycle reserve and no lifecycle event is waiting for it.
    pub(super) fn has_headroom(&self) -> bool {
        self.backlog().is_empty() && self.sender.capacity() > LIFECYCLE_EVENT_RESERVE
    }

    /// A lifecycle event: queued now, or held in order until the queue drains.
    pub(super) fn emit(&self, event: Event) -> io::Result<()> {
        let mut backlog = self.backlog();
        if backlog.is_empty() {
            match self.sender.try_send(event) {
                Ok(()) => return Ok(()),
                Err(TrySendError::Closed(_)) => return Err(failed("IPC event queue closed")),
                Err(TrySendError::Full(event)) => backlog.push_back(event),
            }
        } else {
            backlog.push_back(event);
        }
        if backlog.len() > MAX_EVENT_BACKLOG {
            return Err(failed("IPC event backlog exceeded"));
        }
        Ok(())
    }

    /// A droppable event, sent only when it takes nothing from lifecycle events.
    pub(super) fn emit_bulk(&self, event: Event) -> bool {
        self.has_headroom() && self.sender.try_send(event).is_ok()
    }

    /// Moves held events into the queue, oldest first, as far as it has room.
    pub(super) fn flush(&self) -> io::Result<()> {
        let mut backlog = self.backlog();
        while let Some(event) = backlog.pop_front() {
            match self.sender.try_send(event) {
                Ok(()) => (),
                Err(TrySendError::Closed(_)) => return Err(failed("IPC event queue closed")),
                Err(TrySendError::Full(event)) => {
                    backlog.push_front(event);
                    break;
                }
            }
        }
        Ok(())
    }

    fn backlog(&self) -> std::sync::MutexGuard<'_, VecDeque<Event>> {
        self.backlog
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

#[cfg(test)]
mod tests;
