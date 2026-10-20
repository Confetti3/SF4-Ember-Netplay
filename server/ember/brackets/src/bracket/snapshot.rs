//! What a desk event's bracket looks like to its organizer (docs/design/
//! TOURNAMENT_DESK.md): every set of the bracket, with what the connector is
//! doing about it, plus every set it has tracked. Sent to the bridge after
//! each read, which adds each Ember match's score and state and serves it to
//! the organizer's desk and, if they make it public, to anyone.
//!
//! ponytail: the whole bracket is read every `WHOLE_SECS` (the open sets
//! every poll), so a set decided between two whole reads shows its last
//! read state until the next; shorten it if desks need it sooner and the
//! sites' budgets allow.
use std::collections::BTreeMap;

use serde_json::{Value, json};

use super::Bracket;
use crate::{
    sets::{Progress, Tracked},
    site::{Account, Entry, Set, Status},
};

/// The most sets one snapshot carries; the bridge takes no more.
pub const MAX_SETS: usize = 1024;

impl Bracket {
    /// The snapshot of the sets seen open and those tracked, by set ID.
    pub(super) fn snapshot(
        &self,
        seen: &BTreeMap<String, Set>,
        tracked: &[(String, Tracked)],
    ) -> Value {
        let mut sets: BTreeMap<&str, Value> = seen
            .iter()
            .map(|(id, set)| {
                let status = match set.status {
                    Status::Pending => "pending",
                    Status::Open => "open",
                    Status::Decided => "decided",
                };
                let entry = json!({
                    "id": id,
                    "round": set.round,
                    "round_label": set.round_label,
                    "phase": set.extras.phase.as_ref().map(ToString::to_string),
                    "entries": set.entry_ids(),
                    "names": set.entries.as_ref().map(|entries| entries.each_ref().map(|entry| entry.name.as_deref())),
                    // The Discord account each entrant linked on the site
                    // (start.gg's), pinged when their Ember ID has none.
                    "discord": set.entries.as_ref().map(|entries| entries.each_ref().map(discord)),
                    "status": status,
                    "winner": set.winner,
                    "held": self.held(set).is_some(),
                });
                (id.as_str(), entry)
            })
            .collect();
        for (id, tracked) in tracked {
            let entry = sets
                .entry(id.as_str())
                .or_insert_with(|| json!({ "id": id, "entries": tracked.entries, "held": false }));
            // Reported or closed on the site: no longer open, whatever the
            // last read that showed it said.
            if matches!(tracked.progress, Progress::Reported | Progress::Closed) {
                entry["status"] = json!("decided");
            }
            entry["progress"] = json!(tracked.progress.as_str());
            entry["match_id"] = json!(tracked.match_id);
        }
        json!({ "sets": sets.into_values().take(MAX_SETS).collect::<Vec<_>>() })
    }
}

fn discord(entry: &Entry) -> Option<&str> {
    entry.accounts.iter().find_map(|account| match account {
        Account::Discord { user_id } => Some(user_id.as_str()),
        _ => None,
    })
}
