//! What the connector needs from a bracket site. Each site's adapter keeps
//! its own wire types; the connector sees only these.
use std::{fmt, future::Future, pin::Pin};

pub type SiteFuture<'a, T> = Pin<Box<dyn Future<Output = Result<T, SiteError>> + Send + 'a>>;

/// Whether a request can wait for next month's budget.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Need {
    /// Reading the bracket on its schedule, and marking sets underway:
    /// stops at 100%.
    Poll,
    /// Reading the bracket for a result received, and reporting it: always
    /// made.
    Result,
}

/// One tournament on one site, with the organizer's access to it.
pub trait Site: Send + Sync {
    /// The sets the site lists as open, with their entries' usernames.
    fn open_sets(&self, need: Need) -> SiteFuture<'_, Vec<Set>>;
    /// Whether the bracket has been played out, asked when no set is open:
    /// one that has not started yet is not finished.
    fn finished(&self, need: Need) -> SiteFuture<'_, bool>;
    fn mark_underway<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()>;
    fn report<'a>(&'a self, id: &'a str, report: &'a Report) -> SiteFuture<'a, ()>;
    fn reopen<'a>(&'a self, id: &'a str) -> SiteFuture<'a, ()>;
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
    /// The site account behind the entry, when it has one.
    pub username: Option<String>,
}

impl Set {
    /// The two entries' participant IDs, when the set can be played now.
    pub fn playable_ids(&self) -> Option<[&str; 2]> {
        let [a, b] = self
            .entries
            .as_ref()
            .filter(|_| self.status == Status::Open)?;
        Some([&a.participant_id, &b.participant_id])
    }
}

/// A finished set's score: each entry's games won, player 1 first, and the
/// winning entry's index.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Report {
    pub entries: [String; 2],
    pub games: [u8; 2],
    pub winner: usize,
}

#[derive(Debug)]
pub enum SiteError {
    /// Not reached, or failing on its side (including rate limits and a
    /// credential it refuses): try again later.
    Unavailable(String),
    /// The set or tournament does not exist (any more).
    Gone,
    /// The site refused the request as it stands.
    Refused(String),
    /// This month's request budget is spent, and the request can wait.
    BudgetSpent,
}

impl fmt::Display for SiteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Unavailable(message) | Self::Refused(message) => f.write_str(message),
            Self::Gone => f.write_str("not found on the site"),
            Self::BudgetSpent => f.write_str("this month's request budget is spent"),
        }
    }
}
