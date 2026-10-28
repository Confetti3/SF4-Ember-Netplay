//! Lobbies: an opt-in queue on a provider connection that plays one
//! first-to-N set after another (an extension beyond EMBER-TB-001).
//!
//! Each set is an ordinary match, so scoring, adjudication and the busy check
//! are the match code's. When a set completes the lobby rotates its seats in
//! the same transaction: the players the rotation sends away join the back of
//! the queue and the next set starts with whoever is first. Only players the
//! provider queued at their own request are ever in the queue.
mod model;
mod rotation;
mod routes;

pub use rotation::{on_players_free, on_set_completed, on_unlink};
pub use routes::{close, create, get, join, leave};

/// `external_match_id` prefix of lobby sets; providers cannot create one.
pub const MATCH_PREFIX: &str = "lobby:";
/// Players queued or seated in one lobby at most.
pub const MAX_PLAYERS: i64 = 64;
