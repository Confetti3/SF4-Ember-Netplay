//! Who may watch a match: the organizer's staff list for its connection, the
//! connection's open-to-link-holders setting, and the match's denials.
use std::collections::HashSet;

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

pub struct Policy {
    link_holders: bool,
    staff: HashSet<String>,
    denied: HashSet<String>,
}

fn column(tx: &Transaction<'_>, sql: &str, key: &str) -> Result<HashSet<String>> {
    Ok(tx
        .prepare(sql)?
        .query_map(params![key], |row| row.get(0))?
        .collect::<rusqlite::Result<_>>()?)
}

impl Policy {
    pub fn load(tx: &Transaction<'_>, connection_id: &str, match_id: &str) -> Result<Self> {
        Ok(Self {
            link_holders: tx.query_row(
                "SELECT watch_link_holders FROM provider_connections WHERE id = ?1",
                [connection_id],
                |row| row.get(0),
            )?,
            staff: column(
                tx,
                "SELECT ember_id FROM connection_watch_staff WHERE connection_id = ?1",
                connection_id,
            )?,
            denied: column(
                tx,
                "SELECT ember_id FROM match_watch_denials WHERE match_id = ?1",
                match_id,
            )?,
        })
    }

    pub fn link_holders(&self) -> bool {
        self.link_holders
    }

    /// A denial overrides staff, and staff overrides the link-holder setting.
    pub fn admission(&self, ember_id: &EmberId) -> Admission {
        let id = ember_id.as_str();
        if self.denied.contains(id) {
            Admission::Denied
        } else if self.staff.contains(id) {
            Admission::Staff
        } else if self.link_holders {
            Admission::LinkHolder
        } else {
            Admission::Closed
        }
    }
}
