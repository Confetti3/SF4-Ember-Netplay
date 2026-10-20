//! Who may watch a match: the organizer's staff list for its connection, the
//! connection's open-to-link-holders setting, and the match's denials.
use std::collections::{BTreeMap, HashSet};

use ember_protocol::EmberId;
use rusqlite::{Transaction, params};

use crate::error::Result;

/// Where an Ember ID stands for one match. Staff sort before link holders,
/// which is the order places are given in.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Admission {
    Staff,
    LinkHolder,
    Denied,
    Closed,
}

impl Admission {
    /// The name the organizer's view gives it.
    pub fn via(self) -> &'static str {
        match self {
            Self::Staff => "staff",
            Self::LinkHolder => "link_holders",
            Self::Denied => "denied",
            Self::Closed => "none",
        }
    }
}

/// What the organizer set for a connection: whether anyone holding a match
/// link may watch, and who is always approved.
pub struct ConnectionWatch {
    pub link_holders: bool,
    /// Staff by Ember ID, with when each was added.
    pub staff: BTreeMap<String, u64>,
}

impl ConnectionWatch {
    pub fn load(tx: &Transaction<'_>, connection_id: &str) -> Result<Self> {
        Ok(Self {
            link_holders: tx.query_row(
                "SELECT watch_link_holders FROM provider_connections WHERE id = ?1",
                [connection_id],
                |row| row.get(0),
            )?,
            staff: tx
                .prepare(
                    "SELECT ember_id, added_at FROM connection_watch_staff WHERE connection_id = ?1",
                )?
                .query_map([connection_id], |row| Ok((row.get(0)?, row.get(1)?)))?
                .collect::<rusqlite::Result<_>>()?,
        })
    }
}

/// Who may watch one match: its connection's settings and its own denials.
pub struct WatchPolicy {
    connection: ConnectionWatch,
    denied: HashSet<String>,
}

impl WatchPolicy {
    pub fn load(tx: &Transaction<'_>, connection_id: &str, match_id: &str) -> Result<Self> {
        Ok(Self {
            connection: ConnectionWatch::load(tx, connection_id)?,
            denied: tx
                .prepare("SELECT ember_id FROM match_watch_denials WHERE match_id = ?1")?
                .query_map(params![match_id], |row| row.get(0))?
                .collect::<rusqlite::Result<_>>()?,
        })
    }

    pub fn link_holders(&self) -> bool {
        self.connection.link_holders
    }

    /// A denial overrides staff, and staff overrides the link-holder setting.
    pub fn admission(&self, ember_id: &EmberId) -> Admission {
        let id = ember_id.as_str();
        if self.denied.contains(id) {
            Admission::Denied
        } else if self.connection.staff.contains_key(id) {
            Admission::Staff
        } else if self.connection.link_holders {
            Admission::LinkHolder
        } else {
            Admission::Closed
        }
    }
}
