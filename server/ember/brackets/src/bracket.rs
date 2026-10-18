//! One tournament. Each pass reads the bracket once, when a result is
//! waiting or the schedule says so: it settles the results against that
//! read, cancels the Ember matches of sets the organizer changed, and creates
//! one for each set both of whose players have Ember. Outside the
//! tournament's window it is read only for results, and no match is created;
//! once it has finished it is not read at all. Passes of one tournament
//! never overlap.
use std::{
    collections::{BTreeMap, HashMap, HashSet},
    fmt,
    sync::Arc,
    time::Duration,
};

use ember_protocol::{
    challonge::{is_username, username_key},
    json,
    matches::{CreateMatch, Participant, Rules},
    partner::{FoundChallongePlayer, MatchResult},
    play,
};
use tokio::sync::{Mutex, Notify};

use crate::{
    Clock,
    bridge::BridgeClient,
    config::Rounds,
    log,
    schedule::{Phase, Schedule, Window},
    sets::{ExternalId, MAX_ATTEMPTS, Progress, Step, Tracked, on_open, on_result, report_of},
    site::{Need, Set, Site, SiteError},
    store::Store,
};

/// How often a worker wakes when nothing wakes it: results that could not
/// be settled are tried again then.
const WAKE_SECS: u64 = 60;
const MAX_RESULT: usize = 64 * 1024;

/// What every tournament shares.
pub(crate) struct Context {
    pub store: Arc<Store>,
    pub bridge: BridgeClient,
    pub clock: Clock,
    pub build_id: String,
    pub poll_secs: u64,
    pub max_poll_secs: u64,
}

pub(crate) struct Bracket {
    /// `<site>:<tournament>`.
    pub key: String,
    site: Box<dyn Site>,
    rounds: Rounds,
    window: Window,
    context: Arc<Context>,
    /// Held for a whole pass.
    state: Mutex<State>,
    pub wake: Notify,
}

/// What a tournament's worker keeps between passes.
struct State {
    schedule: Schedule,
    /// The playable sets the last read found, by ID and entries.
    seen: Option<Vec<(String, [String; 2])>>,
    /// Kept in the database too, so a restart does not read it again.
    finished: bool,
    window_end_logged: bool,
}

/// What a pass's results did.
#[derive(Default)]
struct Settled {
    changed: bool,
    /// Sets reported in this pass, which the read made before it still
    /// shows open.
    reported: HashSet<String>,
}

/// Why a pass stopped early. It is tried again on the next one.
enum Trouble {
    Site(SiteError),
    Store(rusqlite::Error),
}

impl From<SiteError> for Trouble {
    fn from(error: SiteError) -> Self {
        Self::Site(error)
    }
}

impl From<rusqlite::Error> for Trouble {
    fn from(error: rusqlite::Error) -> Self {
        Self::Store(error)
    }
}

impl fmt::Display for Trouble {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Site(error) => error.fmt(f),
            Self::Store(error) => write!(f, "database: {error}"),
        }
    }
}

/// The playable sets of a read, in a form that compares.
fn fingerprint(sets: &[Set]) -> Vec<(String, [String; 2])> {
    let mut seen: Vec<(String, [String; 2])> = sets
        .iter()
        .filter_map(|set| Some((set.id.clone(), set.playable_ids()?.map(str::to_owned))))
        .collect();
    seen.sort();
    seen
}

impl Bracket {
    pub fn new(
        key: String,
        site: Box<dyn Site>,
        rounds: Rounds,
        window: Window,
        context: Arc<Context>,
    ) -> Result<Self, String> {
        let finished = context
            .store
            .finished(&key)
            .map_err(|error| format!("cannot read the database: {error}"))?;
        let schedule = Schedule::new(context.poll_secs, context.max_poll_secs);
        Ok(Self {
            key,
            site,
            rounds,
            window,
            context,
            state: Mutex::new(State {
                schedule,
                seen: None,
                finished,
                window_end_logged: false,
            }),
            wake: Notify::new(),
        })
    }

    pub async fn run(self: Arc<Self>) {
        loop {
            self.pass(false).await;
            let wait = Duration::from_secs(WAKE_SECS);
            let _ = tokio::time::timeout(wait, self.wake.notified()).await;
        }
    }

    /// Reads the bracket if a result is waiting, if `poll`, or if the
    /// schedule says it is due, and acts on what it finds.
    pub async fn pass(&self, poll: bool) {
        let mut state = self.state.lock().await;
        let now = self.context.clock.now();
        match self.pass_at(&mut state, now, poll).await {
            Ok(()) | Err(Trouble::Site(SiteError::BudgetSpent)) => {}
            Err(trouble) => log!("{}: {trouble}; tried again later", self.key),
        }
    }

    fn store(&self) -> &Store {
        &self.context.store
    }

    async fn pass_at(&self, state: &mut State, now: u64, poll: bool) -> Result<(), Trouble> {
        if state.finished {
            return Ok(());
        }
        let phase = self.window.phase(now);
        if phase == Phase::After && !std::mem::replace(&mut state.window_end_logged, true) {
            log!(
                "{}: its window has ended; it is read only to report results, and no match is created",
                self.key
            );
        }
        let in_window = phase == Phase::Open;
        let pending = self.store().pending(&self.key)?;
        let scheduled = in_window && (poll || state.schedule.due(now));
        if pending.is_empty() && !scheduled {
            return Ok(());
        }
        // A result is settled now, whatever the schedule or the budget.
        let need = if pending.is_empty() {
            Need::Poll
        } else {
            Need::Result
        };
        state.schedule.reading(now);
        let sets = self.site.open_sets(need).await?;
        let seen = fingerprint(&sets);
        let mut changed = state.seen.as_ref() != Some(&seen);
        state.seen = Some(seen);
        let settled = self.settle(&pending, &sets).await?;
        changed |= settled.changed;
        changed |= self.update(&sets, &settled.reported, in_window).await?;
        state.schedule.read(now, changed);
        if sets.is_empty() && self.site.finished(need).await? {
            self.store().finish(&self.key, now)?;
            state.finished = true;
            log!(
                "{}: the bracket is finished; it is not read again",
                self.key
            );
        }
        Ok(())
    }

    /// Brings the Ember matches in line with a read: cancels those whose set
    /// changed and, if `create`, creates those whose set is ready. Returns
    /// whether it did either.
    async fn update(
        &self,
        sets: &[Set],
        reported: &HashSet<String>,
        create: bool,
    ) -> Result<bool, Trouble> {
        let open: Vec<(&Set, [&str; 2])> = sets
            .iter()
            .filter(|set| !reported.contains(&set.id))
            .filter_map(|set| Some((set, set.playable_ids()?)))
            .collect();
        let playable: HashMap<&str, [&str; 2]> = open
            .iter()
            .map(|(set, entries)| (set.id.as_str(), *entries))
            .collect();
        let mut changed = false;
        for (set_id, tracked) in self.store().live(&self.key)? {
            if playable.get(set_id.as_str()) != Some(&tracked.entry_ids()) {
                changed |= self.close(&set_id, tracked).await?;
            }
        }
        if !create {
            return Ok(changed);
        }
        let mut starts = Vec::new();
        for (set, _) in open {
            let tracked = self.store().tracked(&self.key, &set.id)?;
            match (on_open(tracked.as_ref()), tracked) {
                (Step::Start(attempt), _) => starts.push((set, attempt)),
                (Step::GiveUp, Some(tracked)) => self.give_up(&set.id, tracked)?,
                _ => {}
            }
        }
        Ok(self.start(&starts).await? || changed)
    }

    /// Cancels the Ember match of a set the organizer changed. A cancel that
    /// fails is tried again on the next read. Returns whether it ended.
    async fn close(&self, set_id: &str, mut tracked: Tracked) -> Result<bool, Trouble> {
        let Some(match_id) = tracked.match_id.clone() else {
            return Ok(false);
        };
        match self
            .context
            .bridge
            .cancel(&match_id, "The organizer changed this set.")
            .await
        {
            Ok(()) => {
                tracked.progress = Progress::Closed;
                self.store().save(&self.key, set_id, &tracked)?;
                log!(
                    "{} set {set_id}: changed by the organizer; Ember match {match_id} ended",
                    self.key
                );
                Ok(true)
            }
            Err(error) => {
                log!(
                    "{} set {set_id}: cannot cancel Ember match {match_id}: {error}",
                    self.key
                );
                Ok(false)
            }
        }
    }

    fn give_up(&self, set_id: &str, mut tracked: Tracked) -> Result<(), Trouble> {
        tracked.progress = Progress::Stuck;
        self.store().save(&self.key, set_id, &tracked)?;
        log!(
            "{} set {set_id}: {MAX_ATTEMPTS} Ember matches ended without a result; the organizer decides this set",
            self.key
        );
        Ok(())
    }

    /// Creates the match of each attempt whose two players are found.
    /// Returns whether it created any.
    async fn start(&self, starts: &[(&Set, u32)]) -> Result<bool, Trouble> {
        if starts.is_empty() {
            return Ok(false);
        }
        let mut usernames: Vec<String> = starts
            .iter()
            .flat_map(|(set, _)| set.entries.iter().flatten())
            .filter_map(|entry| entry.username.clone())
            .filter(|username| is_username(username))
            .collect();
        usernames.sort_by_key(|username| username_key(username));
        usernames.dedup_by_key(|username| username_key(username));
        let found: HashMap<String, FoundChallongePlayer> =
            match self.context.bridge.lookup(&usernames).await {
                Ok(players) => players
                    .into_iter()
                    .map(|player| (username_key(&player.username), player))
                    .collect(),
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

    async fn start_one(
        &self,
        set: &Set,
        attempt: u32,
        found: &HashMap<String, FoundChallongePlayer>,
    ) -> Result<bool, Trouble> {
        let Some(entries) = &set.entries else {
            return Ok(false);
        };
        let mut tracked = Tracked {
            entries: entries.clone().map(|entry| entry.participant_id),
            attempt,
            match_id: None,
            progress: Progress::Waiting,
        };
        // Saved before the match exists, so a result for this attempt is
        // recognised even if the connector stops before it saves the match.
        self.store().save(&self.key, &set.id, &tracked)?;
        let players = entries.each_ref().map(|entry| match &entry.username {
            None => Err(format!(
                "entry {} has no Challonge account",
                entry.participant_id
            )),
            Some(username) => found
                .get(&username_key(username))
                .ok_or_else(|| format!("{username} has not connected Challonge in Ember")),
        });
        let command = match players {
            [Ok(one), Ok(two)] => self.command(set, attempt, [one, two]),
            [Err(why), _] | [_, Err(why)] => return self.note(&set.id, Some(&why)),
        };
        if command.check().is_err() {
            return self.note(&set.id, Some("its two entries are the same Ember player"));
        }
        let match_id = match self.context.bridge.create(&command).await {
            Ok(match_id) => match_id,
            Err(error) => {
                return self.note(
                    &set.id,
                    Some(&format!("the bridge refused the match: {error}")),
                );
            }
        };
        tracked.match_id = Some(match_id.clone());
        tracked.progress = Progress::Live;
        self.store().save(&self.key, &set.id, &tracked)?;
        self.store().note(&self.key, &set.id, None)?;
        log!(
            "{} set {}: Ember match {match_id} created ({}, first to {}, attempt {attempt})",
            self.key,
            set.id,
            set.round_label,
            command.rules.games_to_win
        );
        // An earlier attempt already marked it, when the read says so.
        if !set.underway {
            match self.site.mark_underway(&set.id).await {
                Ok(()) | Err(SiteError::BudgetSpent) => {}
                Err(error) => log!("{} set {}: not marked underway: {error}", self.key, set.id),
            }
        }
        Ok(true)
    }

    /// Records why a set waits, logging each reason once. Nothing was
    /// created.
    fn note(&self, set_id: &str, why: Option<&str>) -> Result<bool, Trouble> {
        if self.store().note(&self.key, set_id, why)?
            && let Some(why) = why
        {
            log!("{} set {set_id}: waiting: {why}", self.key);
        }
        Ok(false)
    }

    fn command(&self, set: &Set, attempt: u32, players: [&FoundChallongePlayer; 2]) -> CreateMatch {
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

    /// Settles the waiting results in order, against a read made for them.
    async fn settle(
        &self,
        pending: &[(String, Vec<u8>)],
        sets: &[Set],
    ) -> Result<Settled, Trouble> {
        let mut settled = Settled::default();
        for (webhook_id, body) in pending {
            self.settle_one(body, sets, &mut settled).await?;
            self.store().settled(webhook_id)?;
        }
        Ok(settled)
    }

    async fn settle_one(
        &self,
        body: &[u8],
        sets: &[Set],
        settled: &mut Settled,
    ) -> Result<(), Trouble> {
        // Both were checked when the result was received.
        let Ok(result) = json::parse_as::<MatchResult>(body, MAX_RESULT) else {
            return Ok(());
        };
        let Some(id) = ExternalId::parse(&result.external_match_id) else {
            return Ok(());
        };
        let Some(mut tracked) = self
            .store()
            .tracked(&self.key, &id.set)?
            .filter(|tracked| tracked.is_current(id.attempt))
        else {
            log!(
                "{} set {}: result of Ember match {} is not for its current match; ignored",
                self.key,
                id.set,
                result.match_id
            );
            return Ok(());
        };
        tracked.match_id = Some(result.match_id.clone());
        let unchanged = sets
            .iter()
            .find(|set| set.id == id.set)
            .and_then(Set::playable_ids)
            == Some(tracked.entry_ids());
        settled.changed = true;
        match on_result(tracked.attempt, result.outcome, unchanged) {
            Step::Report => {
                self.report(&id.set, tracked, &result).await?;
                settled.reported.insert(id.set);
            }
            // Created by this pass's update, from the same read.
            Step::Start(attempt) => {
                tracked.attempt = attempt;
                tracked.progress = Progress::Waiting;
                self.store().save(&self.key, &id.set, &tracked)?;
                log!(
                    "{} set {}: Ember match {} ended without a result; playing it again",
                    self.key,
                    id.set,
                    result.match_id
                );
            }
            Step::GiveUp => self.give_up(&id.set, tracked)?,
            Step::Close | Step::Leave => {
                tracked.progress = Progress::Closed;
                self.store().save(&self.key, &id.set, &tracked)?;
                log!(
                    "{} set {}: the organizer already decided or changed it; Ember match {} not reported",
                    self.key,
                    id.set,
                    result.match_id
                );
            }
        }
        Ok(())
    }

    /// Reports the score. A score the site refuses is left to the organizer.
    async fn report(
        &self,
        set_id: &str,
        mut tracked: Tracked,
        result: &MatchResult,
    ) -> Result<(), Trouble> {
        let Some(report) = report_of(&tracked.entries, result) else {
            return Ok(());
        };
        tracked.progress = match self.site.report(set_id, &report).await {
            Ok(()) => {
                log!(
                    "{} set {set_id}: reported {}-{} from Ember match {}",
                    self.key,
                    report.games[0],
                    report.games[1],
                    result.match_id
                );
                Progress::Reported
            }
            Err(error @ (SiteError::Refused(_) | SiteError::Gone)) => {
                log!(
                    "{} set {set_id}: the site refused the score {}-{} of Ember match {} ({error}); the organizer enters it",
                    self.key,
                    report.games[0],
                    report.games[1],
                    result.match_id
                );
                Progress::Stuck
            }
            Err(error) => return Err(error.into()),
        };
        self.store().save(&self.key, set_id, &tracked)?;
        Ok(())
    }
}
