//! What the connector needs from a bracket site. Each site's adapter keeps
//! its own wire types and its own request limits; the connector sees only
//! these.
use std::{fmt, future::Future, pin::Pin};

pub type SiteFuture<'a, T> = Pin<Box<dyn Future<Output = Result<T, SiteError>> + Send + 'a>>;

/// Why the bracket is read. A site with a request budget keeps scheduled
/// reads within it; a read for a result received is always made.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Purpose {
    Scheduled,
    Result,
}

/// One tournament on one site, with the organizer's access to it.
pub trait Site: Send + Sync {
    /// The sets the site lists as open. A list read in pages is no snapshot
    /// and can miss a set, so a set's absence proves nothing.
    fn open_sets(&self, purpose: Purpose) -> SiteFuture<'_, Vec<Set>>;
    /// One set as it is now, or `None` when the site says it does not
    /// exist: the evidence asked for before a match is cancelled or a
    /// result is set aside.
    fn set<'a>(&'a self, id: &'a str, purpose: Purpose) -> SiteFuture<'a, Option<Set>>;
    /// Whether the bracket is over, asked when a read finds no open set.
    fn standing(&self) -> SiteFuture<'_, Standing>;
    fn mark_underway<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()>;
    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, Reported>;

    /// Whether `report` uses each game's winner (`Report::sequence`), which
    /// the connector then reads from the bridge before reporting.
    fn wants_games(&self) -> bool {
        false
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Standing {
    /// The site says the tournament is over.
    Finished,
    /// Every set looks played, but the site does not say it is over: a
    /// later stage or round may still be added, or a set reopened.
    PlayedOut,
    Running,
}

/// An account an entry stands for, by which the bridge finds its player.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Account {
    /// An account on a bridge sign-in site, by the ID that site's lookup
    /// matches (a Challonge username, a start.gg user ID).
    Site { site: String, id: String },
    /// A Discord account the platform shows linked to the entry.
    Discord { user_id: String },
}

impl fmt::Display for Account {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Site { site, id } => write!(f, "{site} account {id}"),
            Self::Discord { user_id } => write!(f, "Discord account {user_id}"),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Set {
    pub id: String,
    /// As the site numbers it: below zero in a losers bracket.
    pub round: i32,
    pub round_label: String,
    /// Player 1 and player 2, once both are known.
    pub entries: Option<[Entry; 2]>,
    pub status: Status,
    /// Marked underway on the site, when the site says.
    pub underway: bool,
    /// The winning entry's participant ID, once decided.
    pub winner: Option<String>,
    /// The stage of the event the set is in, when the site says: a start.gg
    /// phase, Challonge's group or final stage, an http service's `phase`.
    /// Round rules can name it, since stages number their rounds afresh.
    pub phase: Option<Phase>,
    /// The stream the set is assigned to, when the site says (start.gg).
    pub stream: Option<String>,
}

/// A stage of an event, by the name the organizer sees and, where the site
/// has one, its ID.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Phase {
    pub name: String,
    pub id: Option<String>,
}

impl Phase {
    pub fn named(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            id: None,
        }
    }

    /// Whether a round rule's `phase` names this one: its name, ignoring
    /// case and the spaces around it, or its ID.
    pub fn is(&self, wanted: &str) -> bool {
        let wanted = wanted.trim();
        self.name.trim().to_lowercase() == wanted.to_lowercase()
            || self.id.as_deref() == Some(wanted)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// Waiting for an earlier set.
    Pending,
    /// Can be played, and has no result.
    Open,
    /// Has a result.
    Decided,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub participant_id: String,
    /// The accounts behind the entry, the one to try first first: its site
    /// account, then a linked Discord account.
    pub accounts: Vec<Account>,
}

impl Set {
    /// The two entries' participant IDs, when both are known.
    pub fn entry_ids(&self) -> Option<[&str; 2]> {
        let [a, b] = self.entries.as_ref()?;
        Some([&a.participant_id, &b.participant_id])
    }

    /// The two entries' participant IDs, when the set can be played now.
    pub fn playable_ids(&self) -> Option<[&str; 2]> {
        self.entry_ids().filter(|_| self.status == Status::Open)
    }
}

/// A finished set's score: each entry's games won, player 1 first, the
/// winning entry's index, and, for a site that `wants_games`, the winning
/// entry of each game in the order they were played.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Report {
    pub entries: [String; 2],
    pub games: [u8; 2],
    pub winner: usize,
    pub sequence: Vec<usize>,
    /// The same on every delivery of this result (the Ember match's ID), so
    /// a site can tell a repeat from a different result.
    pub key: String,
}

/// How a report ended up on the site.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reported {
    /// Written now.
    Sent,
    /// The site already held this winner: an earlier send whose answer was
    /// lost, or the organizer entering the same winner by hand.
    AlreadyHeld,
}

#[derive(Debug)]
pub enum SiteError {
    /// Not reached, or failing on its side for a moment (a timeout, a rate
    /// limit, a 5xx): try again later. A report that fails so after a read
    /// that worked waits on its own, like `Deferred`.
    Unavailable(String),
    /// The site refused the request as it stands. A refused read is tried
    /// again under the same growing wait as `Unavailable`; a refused report
    /// is final, and the organizer enters that set's score.
    Refused(String),
    /// This one report cannot be taken now, for a reason of its own set
    /// (the site refused it, and the set is still open and unchanged): that
    /// result alone waits and is tried again, while the rest of the
    /// tournament carries on. Only `report` returns it.
    Deferred(String),
    /// The site refuses the credential, or the contract version: no one set
    /// is at fault, so none is given up for it. Whatever asked, the pass
    /// stops and the whole tournament waits under the growing wait until
    /// the operator or organizer fixes it.
    Denied(String),
    /// A scheduled read would go past the site's request budget.
    BudgetSpent,
}

impl fmt::Display for SiteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Unavailable(message)
            | Self::Refused(message)
            | Self::Deferred(message)
            | Self::Denied(message) => f.write_str(message),
            Self::BudgetSpent => f.write_str("this month's request budget is spent"),
        }
    }
}
