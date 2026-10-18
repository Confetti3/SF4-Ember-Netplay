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
    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, ()>;

    /// Whether the site accounts this adapter gives are names an account
    /// can change, so one connected long ago may now be someone else's.
    fn names_can_change(&self) -> bool {
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
/// winning entry's index, and the winning entry of each game in the order
/// they were played, for a site that takes games one by one.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Report {
    pub entries: [String; 2],
    pub games: [u8; 2],
    pub winner: usize,
    pub sequence: Vec<usize>,
}

#[derive(Debug)]
pub enum SiteError {
    /// Not reached, or failing on its side (including rate limits and a
    /// credential it refuses): try again later.
    Unavailable(String),
    /// The site refused the request as it stands.
    Refused(String),
    /// A scheduled read would go past the site's request budget.
    BudgetSpent,
}

impl fmt::Display for SiteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Unavailable(message) | Self::Refused(message) => f.write_str(message),
            Self::BudgetSpent => f.write_str("this month's request budget is spent"),
        }
    }
}
