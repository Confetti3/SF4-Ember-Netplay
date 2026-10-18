//! Creating a set's Ember match: finding its players on the bridge, saving
//! the creation before it is sent, and recording the bridge's answer.
use std::collections::BTreeMap;

use ember_protocol::{
    api::ErrorCode,
    event::rfc3339,
    matches::{CreateMatch, Participant, Rules},
    play,
};

use super::{Bracket, Trouble};
use crate::{
    bridge::{BridgeError, Found, Player},
    log,
    sets::{ExternalId, MAX_ATTEMPTS, Progress, Tracked},
    site::{Entry, Set, SiteError},
};

/// A connected account older than this is pointed out when its player is
/// given a match: Challonge lets an account rename, and the bridge knows
/// the username it had when it was connected.
const OLD_CONNECTION_SECS: u64 = 30 * 24 * 60 * 60;

impl Bracket {
    /// Creates the match of each attempt whose two players are found.
    /// Returns whether it created any.
    pub(super) async fn start(&self, starts: &[(&Set, u32)]) -> Result<bool, Trouble> {
        if starts.is_empty() {
            return Ok(false);
        }
        let accounts: Vec<_> = starts
            .iter()
            .flat_map(|(set, _)| set.entries.iter().flatten())
            .filter_map(|entry| entry.account.as_ref())
            .collect();
        let found = match self.context.bridge.lookup(&accounts).await {
            Ok(found) => found,
            Err(error) => {
                log!("{}: cannot look players up: {error}", self.key);
                return Ok(false);
            }
        };
        let mut created = false;
        for (set, attempt) in starts {
            created |= self.start_one(set, *attempt, &found).await?;
        }
        Ok(created)
    }

    async fn start_one(&self, set: &Set, attempt: u32, found: &Found) -> Result<bool, Trouble> {
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
            return self.create(set, tracked, &command).await;
        }
        let Some(entries) = &set.entries else {
            return Ok(false);
        };
        let players = entries.each_ref().map(|entry| match &entry.account {
            None => Err(format!(
                "entry {} has no account to find its player by",
                entry.participant_id
            )),
            Some(account) => found
                .get(account)
                .ok_or_else(|| format!("{account} has not connected that account in Ember")),
        });
        let command = match players {
            [Ok(one), Ok(two)] => {
                self.point_out_old_connections(set, entries, [one, two]);
                self.command(set, attempt, [one, two])
            }
            [Err(why), _] | [_, Err(why)] => return self.note(set, attempt, Some(&why)),
        };
        if command.check().is_err() {
            return self.note(
                set,
                attempt,
                Some("its two entries are the same Ember player"),
            );
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
            .save(&self.key, &set.id, &tracked, Some(&command))?;
        self.create(set, tracked, &command).await
    }

    /// Sends a saved creation and records the bridge's answer.
    async fn create(
        &self,
        set: &Set,
        mut tracked: Tracked,
        command: &CreateMatch,
    ) -> Result<bool, Trouble> {
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
            // until it is played or expires. It takes a refused creation
            // followed by a different one (the configuration or a lookup
            // changed in between) to get here.
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
                self.store().save(&self.key, &set.id, &tracked, None)?;
                return self.note(
                    set,
                    attempt,
                    Some("its match ID was already taken; playing it as a new attempt"),
                );
            }
            // Nothing was made: the next read builds the creation afresh.
            Err(error) => {
                tracked.players = None;
                self.store().save(&self.key, &set.id, &tracked, None)?;
                return self.note(
                    set,
                    tracked.attempt,
                    Some(&format!("the bridge refused the match: {error}")),
                );
            }
        };
        tracked.match_id = Some(match_id.clone());
        tracked.progress = Progress::Live;
        self.store().save(&self.key, &set.id, &tracked, None)?;
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
    fn note(&self, set: &Set, attempt: u32, why: Option<&str>) -> Result<bool, Trouble> {
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
            self.store().save(&self.key, &set.id, &tracked, None)?;
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
            rules: Rules::standard(self.rounds.games_to_win(set.round), play::PROFILE),
            observer_policy: "authorized_only".into(),
            result_policy: "two_player_agreement_or_review".into(),
            required_build_id: self.context.build_id.clone(),
            metadata: BTreeMap::from([("round_label".to_owned(), set.round_label.clone())]),
        }
    }

    /// Says once per player, while the connector runs, that their account
    /// was connected long ago.
    fn point_out_old_connections(&self, set: &Set, entries: &[Entry; 2], players: [&Player; 2]) {
        let now = self.context.clock.now();
        let mut pointed_out = self
            .context
            .pointed_out
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        for (entry, player) in entries.iter().zip(players) {
            if let (Some(account), Some(at)) = (&entry.account, player.connected_at)
                && now.saturating_sub(at) > OLD_CONNECTION_SECS
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
