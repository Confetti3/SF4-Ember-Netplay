//! Creating a set's Ember match: finding its players on the bridge, saving
//! the creation before it is sent, and recording the bridge's answer.
use std::{collections::BTreeMap, sync::atomic::Ordering};

use ember_protocol::{
    Error as ProtocolError,
    api::ErrorCode,
    event::rfc3339,
    matches::{CreateMatch, MAX_METADATA_VALUE, Participant, Rules},
    play,
};

use super::Bracket;
use crate::{
    bridge::{BridgeError, Found, Player},
    log,
    rounds::Shape,
    sets::{ExternalId, MAX_ATTEMPTS, Progress, Seen, Tracked},
    site::{Account, Set, SiteError},
};

/// A connected site account older than this is pointed out when its
/// player is given a match: a site such as Challonge lets an account
/// rename, and the bridge knows the name it had when it was connected.
const OLD_CONNECTION_SECS: u64 = 30 * 24 * 60 * 60;

impl Bracket {
    /// Creates the match of each attempt whose two players are found, and
    /// holds the sets assigned to a stream when asked to. Returns whether it
    /// changed anything: created a match or ended an attempt.
    pub(super) async fn start(&self, starts: &[(&Set, u32)]) -> Result<bool, SiteError> {
        // Held sets are settled first and never looked up: a set that waits
        // for the stream has no players to find yet.
        let mut changed = false;
        let mut ready = Vec::new();
        for &(set, attempt) in starts {
            match self.held(set) {
                Some(why) => changed |= self.hold(set, attempt, &why).await?,
                None => ready.push((set, attempt)),
            }
        }
        if ready.is_empty() {
            return Ok(changed);
        }
        let starts = ready.as_slice();
        let accounts: Vec<_> = starts
            .iter()
            .flat_map(|(set, _)| set.entries.iter().flatten())
            .flat_map(|entry| &entry.accounts)
            .collect();
        let now = self.context.clock.now();
        let found = match self.context.bridge.lookup(&accounts, now).await {
            Ok(found) => {
                if self.lookup_failing.swap(false, Ordering::Relaxed) {
                    log!("{}: players can be looked up again", self.key);
                }
                found
            }
            Err(error) => {
                if !self.lookup_failing.swap(true, Ordering::Relaxed) {
                    log!(
                        "{}: cannot look players up: {error}; no match is created until the bridge answers",
                        self.key
                    );
                }
                return Ok(changed);
            }
        };
        for (set, attempt) in starts {
            changed |= self.start_one(set, *attempt, &found).await?;
        }
        Ok(changed)
    }

    /// Why the set waits: the organizer holds it, or the stream it is
    /// assigned to when the tournament holds such sets.
    pub(super) fn held(&self, set: &Set) -> Option<String> {
        if self.desk_held.contains(&set.id) {
            return Some(
                "the organizer holds it on the desk; it is played in Ember once they let it go"
                    .into(),
            );
        }
        let stream = self
            .hold
            .streamed
            .then_some(set.extras.stream.as_deref())
            .flatten()?;
        Some(format!(
            "it is assigned to stream {stream}; it is played in Ember once it is taken off the stream"
        ))
    }

    /// A held set waits, said once. A creation sent for it
    /// and never answered (the bridge was down, or the connector stopped)
    /// must not leave a live match on a set that is not played in Ember: it
    /// is sent again only to learn the match the bridge made, and that match
    /// is cancelled, as for any set the organizer changed. Returns whether
    /// that ended the attempt.
    async fn hold(&self, set: &Set, attempt: u32, why: &str) -> Result<bool, SiteError> {
        let earlier = if attempt > 1 {
            " and its earlier Ember match ended"
        } else {
            ""
        };
        self.note(set, attempt, Some(&format!("held{earlier}: {why}")))?;
        if let Some(tracked) = self.store().tracked(&self.key, &set.id)?
            && tracked.attempt == attempt
            && self.store().command(&self.key, &set.id)?.is_some()
        {
            return self.withdraw(&set.id, tracked, Seen::Changed).await;
        }
        Ok(false)
    }

    async fn start_one(&self, set: &Set, attempt: u32, found: &Found) -> Result<bool, SiteError> {
        // A creation sent for this attempt and never answered is sent again
        // as it was: the bridge answers with the match it made, if it made
        // one, and that match's set is then judged like any live one. If the
        // bridge never had it and the organizer has changed an entry since,
        // the old pair sees a match until the next read cancels it.
        if let (Some(tracked), Some(command)) = (
            self.store().tracked(&self.key, &set.id)?,
            self.store().command(&self.key, &set.id)?,
        ) && tracked.attempt == attempt
        {
            return match command {
                Ok(command) => self.create(set, tracked, &command).await,
                // Not made afresh, which could make a second match: this
                // set alone goes to the organizer.
                Err(why) => {
                    self.end(
                        &set.id,
                        tracked,
                        Progress::Stuck,
                        format_args!(
                            "its saved creation cannot be read ({why}); its Ember match may exist, and the organizer decides this set"
                        ),
                    )?;
                    Ok(false)
                }
            };
        }
        let Some(entries) = &set.entries else {
            return Ok(false);
        };
        let players = entries.each_ref().map(|entry| {
            found.player(entry).ok_or_else(|| {
                // The accounts asked found nobody; the others say why not.
                let (asked, reasons): (Vec<_>, Vec<_>) = entry
                    .accounts
                    .iter()
                    .map(|account| (account, found.not_asked(account)))
                    .partition(|(_, reason)| reason.is_none());
                let reasons: Vec<String> = reasons.into_iter().filter_map(|(_, why)| why).collect();
                let asked: Vec<String> = asked
                    .iter()
                    .map(|(account, _)| account.to_string())
                    .collect();
                match (asked.as_slice(), reasons.as_slice()) {
                    ([], []) => format!(
                        "entry {} has no account to find its player by",
                        entry.participant_id
                    ),
                    ([], reasons) => reasons.join("; "),
                    (asked, []) => format!("{} has not connected in Ember", asked.join(" or ")),
                    (asked, reasons) => format!(
                        "{} has not connected in Ember; {}",
                        asked.join(" or "),
                        reasons.join("; ")
                    ),
                }
            })
        });
        let command = match players {
            [Ok(one), Ok(two)] => {
                self.point_out_old_connections(set, [one, two]).await;
                self.command(set, attempt, [one.1, two.1])
            }
            [Err(why), _] | [_, Err(why)] => return self.note(set, attempt, Some(&why)),
        };
        match command.check() {
            Ok(()) => {}
            Err(ProtocolError::InvalidField("participants")) => {
                return self.note(
                    set,
                    attempt,
                    Some("its two entries are the same Ember player"),
                );
            }
            Err(error) => {
                return self.note(
                    set,
                    attempt,
                    Some(&format!("its match would be refused: {error}")),
                );
            }
        }
        let tracked = Tracked {
            entries: entries.clone().map(|entry| entry.participant_id),
            players: Some(command.by_slot().map(|seat| seat.ember_id.clone())),
            attempt,
            match_id: None,
            progress: Progress::Waiting,
        };
        // Saved before it is sent, so a result for this attempt is
        // recognised, and the creation repeated, even if the connector
        // stops before the bridge's answer is saved.
        self.store()
            .save_creating(&self.key, &set.id, &tracked, &command)?;
        self.create(set, tracked, &command).await
    }

    /// Sends a saved creation and records the bridge's answer.
    async fn create(
        &self,
        set: &Set,
        mut tracked: Tracked,
        command: &CreateMatch,
    ) -> Result<bool, SiteError> {
        let match_id = match self.context.bridge.create(command).await {
            Ok(match_id) => match_id,
            // Kept, and sent again as it is: the bridge may have made it.
            Err(BridgeError::Unavailable) => {
                return self.note(
                    set,
                    tracked.attempt,
                    Some("the bridge could not be reached"),
                );
            }
            // Another request already made this attempt's match: whatever
            // it is, the set is played as a new attempt. That match is not
            // known here, so it stays on the bridge, for its players to see,
            // until it is played or expires. The bridge keeps nothing for a
            // creation it refused, so a refusal does not lead here: a lost
            // or replaced database does, whose saved creation is gone while
            // the bridge still has its match.
            Err(BridgeError::Refused {
                code: ErrorCode::IdempotencyConflict,
                ..
            }) => {
                let attempt = tracked.attempt + 1;
                tracked.players = None;
                if attempt > MAX_ATTEMPTS {
                    self.give_up(&set.id, tracked)?;
                    return Ok(false);
                }
                tracked.attempt = attempt;
                self.store().save(&self.key, &set.id, &tracked)?;
                return self.note(
                    set,
                    attempt,
                    Some("its match ID was already taken; playing it as a new attempt"),
                );
            }
            // Nothing was made: the next read builds the creation afresh.
            Err(error) => {
                tracked.players = None;
                self.store().save(&self.key, &set.id, &tracked)?;
                return self.note(
                    set,
                    tracked.attempt,
                    Some(&format!("the bridge refused the match: {error}")),
                );
            }
        };
        tracked.match_id = Some(match_id.clone());
        tracked.progress = Progress::Live;
        self.store().save(&self.key, &set.id, &tracked)?;
        self.store().note(&self.key, &set.id, None)?;
        log!(
            "{} set {}: Ember match {match_id} created ({}, first to {}, attempt {})",
            self.key,
            set.id,
            set.round_label,
            command.rules.games_to_win,
            tracked.attempt
        );
        // An earlier attempt already marked it, when the read says so.
        if self.mark_underway && !set.underway {
            match self.site.mark_underway(&set.id).await {
                Ok(()) | Err(SiteError::BudgetSpent) => {}
                Err(error) => log!("{} set {}: not marked underway: {error}", self.key, set.id),
            }
        }
        Ok(true)
    }

    /// Records why a set waits, logging each reason once. A set seen for
    /// the first time is saved, so its reason has a row to go on. Nothing
    /// was created.
    fn note(&self, set: &Set, attempt: u32, why: Option<&str>) -> Result<bool, SiteError> {
        if self.store().tracked(&self.key, &set.id)?.is_none()
            && let Some(entries) = set.entry_ids()
        {
            let tracked = Tracked {
                entries: entries.map(str::to_owned),
                players: None,
                attempt,
                match_id: None,
                progress: Progress::Waiting,
            };
            self.store().save(&self.key, &set.id, &tracked)?;
        }
        if self.store().note(&self.key, &set.id, why)?
            && let Some(why) = why
        {
            log!("{} set {}: waiting: {why}", self.key, set.id);
        }
        Ok(false)
    }

    fn command(&self, set: &Set, attempt: u32, players: [&Player; 2]) -> CreateMatch {
        let external = ExternalId {
            bracket: self.key.clone(),
            set: set.id.clone(),
            attempt,
        };
        CreateMatch {
            external_match_id: external.to_string(),
            game: "usf4".into(),
            participants: [0, 1]
                .map(|slot| Participant {
                    participant_id: players[usize::from(slot)].participant_id.clone(),
                    ember_id: players[usize::from(slot)].ember_id.clone(),
                    slot,
                })
                .into(),
            rules: Rules::standard(self.rounds.length(set, self.shape(set)), play::PROFILE),
            observer_policy: "authorized_only".into(),
            result_policy: "two_player_agreement_or_review".into(),
            required_build_id: self.context.build_id.clone(),
            metadata: placement(set),
        }
    }

    /// Says once per player, while the connector runs, that the site
    /// account they were found by was connected long ago, when that site
    /// looks accounts up by username (the bridge's `lookup_by`), which an
    /// account can change.
    async fn point_out_old_connections(&self, set: &Set, players: [(&Account, &Player); 2]) {
        let now = self.context.clock.now();
        let old = |player: &Player| {
            player
                .connected_at
                .is_some_and(|at| now.saturating_sub(at) > OLD_CONNECTION_SECS)
        };
        if !players.iter().any(|(_, player)| old(player)) {
            return;
        }
        let Some(sites) = self.context.bridge.sign_in_sites(now).await else {
            return;
        };
        let mut pointed_out = self
            .context
            .pointed_out
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        for (account, player) in players {
            if let Account::Site { site, .. } = account
                && sites.get(site) == Some(&true)
                && let Some(at) = player.connected_at
                && old(player)
                && pointed_out.insert(player.ember_id.clone())
            {
                log!(
                    "{} set {}: {account} was connected in Ember on {}; if it was renamed since, check that its player is the entry's",
                    self.key,
                    set.id,
                    rfc3339(at)
                );
            }
        }
    }
}

/// Where the set sits in the bracket, as the match's metadata: its label,
/// its round as the site numbers it (below zero in a losers bracket) and
/// its phase, the last two when the site says them. A replay of the match
/// shows them.
fn placement(set: &Set) -> BTreeMap<String, String> {
    let mut metadata = BTreeMap::from([(
        "round_label".to_owned(),
        round_label(&set.round_label).to_owned(),
    )]);
    if set.round != 0 {
        metadata.insert("round".to_owned(), set.round.to_string());
    }
    if let Some(phase) = &set.extras.phase {
        metadata.insert(
            "phase".to_owned(),
            round_label(&phase.to_string()).to_owned(),
        );
    }
    metadata
}

/// A round label as match metadata takes it: cut to the bridge's limit on
/// a metadata value, at a character boundary.
fn round_label(label: &str) -> &str {
    let mut end = label.len().min(MAX_METADATA_VALUE);
    while !label.is_char_boundary(end) {
        end -= 1;
    }
    &label[..end]
}

impl Bracket {
    /// The shape of `set`'s bracket, as the last read showed it.
    pub(super) fn shape(&self, set: &Set) -> Option<Shape> {
        self.shapes
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .iter()
            .find(|(phase, _)| *phase == set.extras.phase)
            .map(|&(_, shape)| shape)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::site::{Extras, Phase, Status};

    #[test]
    fn placement_says_the_losers_round_and_phase_when_the_site_does() {
        let mut set = Set {
            id: "s".into(),
            round: -3,
            round_label: "Losers Quarter-Final".into(),
            entries: None,
            status: Status::Open,
            underway: false,
            winner: None,
            extras: Extras {
                phase: Phase::new(Some("Top 8"), Some("p8".into())),
                ..Extras::default()
            },
        };
        let placed = placement(&set);
        assert_eq!(placed["round_label"], "Losers Quarter-Final");
        assert_eq!(placed["round"], "-3");
        assert_eq!(placed["phase"], "Top 8");

        set.round = 0;
        set.extras = Extras::default();
        assert_eq!(placement(&set).len(), 1);
    }

    #[test]
    fn a_long_round_label_is_cut_where_a_character_ends() {
        assert_eq!(round_label("Winners Final"), "Winners Final");
        let long = "\u{fc}".repeat(MAX_METADATA_VALUE);
        let cut = round_label(&long);
        assert_eq!(cut.len(), MAX_METADATA_VALUE);
        assert!(cut.chars().all(|c| c == '\u{fc}'));
        let odd = format!("x{long}");
        assert_eq!(round_label(&odd).len(), MAX_METADATA_VALUE - 1);
    }
}
