//! One tournament. Each pass reads the bracket once, when a result is
//! waiting or the schedule says so: it settles the results against that
//! read, cancels the Ember matches of sets the organizer changed, and creates
//! one for each set both of whose players have Ember.
//!
//! Only what a set shows is evidence: a set missing from a paged list is
//! asked for on its own before its match is cancelled or its result set
//! aside. The bridge is asked how each live match ended every few minutes,
//! so a result whose delivery was lost is still settled. Outside the window
//! the bracket is read only for results; once it has finished it is not
//! read at all. Passes of one tournament never overlap.
use std::{
    collections::{BTreeMap, HashMap, HashSet},
    fmt,
    sync::Arc,
    time::Duration,
};

use ember_protocol::{
    api::ErrorCode,
    matches::{CreateMatch, Participant, Rules},
    partner::result_id,
    play,
};
use tokio::sync::{Mutex, Notify};

use crate::{
    Clock,
    bridge::{BridgeClient, BridgeError, Found, Player},
    config::{Rounds, Tournament},
    log,
    schedule::{Phase, Retry, Schedule, Window},
    sets::{
        ExternalId, MAX_ATTEMPTS, OnOpen, OnResult, Progress, Received, Seen, Tracked, on_open,
        on_result, seen,
    },
    site::{Purpose, Report, Set, Site, SiteError, Standing},
    store::Store,
};

/// How often a worker wakes when nothing wakes it.
const WAKE_SECS: u64 = 60;
/// How often the bridge is asked about each live match.
const RECONCILE_SECS: u64 = 5 * 60;

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
    mark_underway: bool,
    context: Arc<Context>,
    /// Held for a whole pass.
    state: Mutex<State>,
    pub wake: Notify,
}

/// What a tournament's worker keeps between passes.
struct State {
    schedule: Schedule,
    /// Reads after a failed pass.
    retry: Retry,
    next_reconcile: u64,
    /// Whether the last time the bridge was asked failed, so a run of
    /// failures is logged once.
    reconcile_failing: bool,
    /// The playable sets the last read found, by ID and entries.
    seen: Option<Vec<(String, [String; 2])>>,
    /// Kept in the database too, so a restart does not read it again.
    finished: bool,
    /// Every set looks played but the site does not say it is over: read
    /// at the longest interval, without asking again, until a set opens.
    played_out: bool,
    window_end_logged: bool,
}

/// What a pass's results did.
#[derive(Default)]
struct Settled {
    changed: bool,
    /// Sets reported in this pass, which the read made before it still
    /// shows open.
    reported: HashSet<String>,
    /// Whether a report has changed the bracket since the read, so that the
    /// read is no longer evidence for a later result.
    stale: bool,
}

/// Why a pass stopped early. It is tried again later.
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
        tournament: &Tournament,
        site: Box<dyn Site>,
        context: Arc<Context>,
    ) -> Result<Self, String> {
        let key = tournament.key();
        let finished = context
            .store
            .finished(&key)
            .map_err(|error| format!("cannot read the database: {error}"))?;
        let schedule = Schedule::new(context.poll_secs, context.max_poll_secs);
        Ok(Self {
            key,
            site,
            rounds: tournament.rounds.clone(),
            window: tournament.window()?,
            mark_underway: tournament.mark_underway,
            context,
            state: Mutex::new(State {
                schedule,
                retry: Retry::default(),
                next_reconcile: 0,
                reconcile_failing: false,
                seen: None,
                finished,
                played_out: false,
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

    /// Asks the bridge about live matches when that is due, then reads the
    /// bracket if a result is waiting (and no failure is being waited out),
    /// if `poll`, or if the schedule says so, and acts on what it finds.
    pub async fn pass(&self, poll: bool) {
        let mut state = self.state.lock().await;
        if state.finished {
            return;
        }
        let now = self.context.clock.now();
        if now >= state.next_reconcile {
            state.next_reconcile = now + RECONCILE_SECS;
            match self.reconcile(now).await {
                Ok(()) => state.reconcile_failing = false,
                Err(why) => {
                    if !std::mem::replace(&mut state.reconcile_failing, true) {
                        log!("{}: {why}; asked again every few minutes", self.key);
                    }
                }
            }
        }
        match self.read(&mut state, now, poll).await {
            Ok(false) => {}
            Ok(true) => {
                let failures = state.retry.succeeded();
                if failures > 0 {
                    log!(
                        "{}: reading works again ({failures} failed before)",
                        self.key
                    );
                }
            }
            Err(Trouble::Site(SiteError::BudgetSpent)) => {}
            Err(trouble) => {
                if state.retry.failed(now) {
                    log!(
                        "{}: {trouble}; tried again after a growing wait, up to an hour",
                        self.key
                    );
                }
            }
        }
    }

    fn store(&self) -> &Store {
        &self.context.store
    }

    /// Records the outcome of each live match the bridge says is over, under
    /// the bridge's own `webhook-id`: a delivery that was lost is settled,
    /// and one that arrives later is a repeat.
    async fn reconcile(&self, now: u64) -> Result<(), String> {
        let live = self
            .store()
            .live(&self.key)
            .map_err(|error| format!("database: {error}"))?;
        for (set_id, tracked) in live {
            let Some(match_id) = tracked.match_id else {
                continue;
            };
            let ended = self
                .context
                .bridge
                .ended(&match_id)
                .await
                .map_err(|error| format!("cannot ask the bridge about its matches: {error}"))?;
            let Some((players, ended)) = ended else {
                continue;
            };
            let received = Received {
                id: ExternalId {
                    bracket: self.key.clone(),
                    set: set_id.clone(),
                    attempt: tracked.attempt,
                },
                match_id: match_id.clone(),
                players,
                ended,
            };
            let fresh = self
                .store()
                .receive(&result_id(&match_id), &received, now, false)
                .map_err(|error| format!("database: {error}"))?;
            if fresh {
                log!(
                    "{} set {set_id}: Ember match {match_id} has ended; its result is read from the bridge",
                    self.key
                );
            }
        }
        Ok(())
    }

    /// One read of the bracket and what follows from it. Returns whether a
    /// read was made.
    async fn read(&self, state: &mut State, now: u64, poll: bool) -> Result<bool, Trouble> {
        let phase = self.window.phase(now);
        if phase == Phase::After && !std::mem::replace(&mut state.window_end_logged, true) {
            log!(
                "{}: its window has ended; it is read only to report results, and no match is created",
                self.key
            );
        }
        let in_window = phase == Phase::Open;
        let pending = self.store().pending(&self.key)?;
        let for_results = !pending.is_empty() && state.retry.ready(now);
        let scheduled = in_window && (poll || state.schedule.due(now));
        if !for_results && !scheduled {
            return Ok(false);
        }
        // A result is settled now, whatever the schedule or the budget.
        let purpose = if pending.is_empty() {
            Purpose::Scheduled
        } else {
            Purpose::Result
        };
        state.schedule.reading(now);
        let sets = self.site.open_sets(purpose).await?;
        let seen = fingerprint(&sets);
        let mut changed = state.seen.as_ref() != Some(&seen);
        state.seen = Some(seen);
        let settled = self.settle(&pending, &sets).await?;
        changed |= settled.changed;
        changed |= self.update(&sets, &settled.reported, in_window).await?;
        state.schedule.read(now, changed);
        if !sets.is_empty() {
            state.played_out = false;
        } else if !state.played_out && self.store().has_sets(&self.key)? {
            match self.site.standing().await? {
                Standing::Finished => {
                    self.store().finish(&self.key, now)?;
                    state.finished = true;
                    log!("{}: the tournament is over; it is not read again", self.key);
                }
                Standing::PlayedOut => {
                    state.played_out = true;
                    log!(
                        "{}: every set looks played; read every {} seconds until a set opens",
                        self.key,
                        self.context.max_poll_secs
                    );
                }
                Standing::Running => {}
            }
        }
        if state.played_out {
            state.schedule.slow(now);
        }
        Ok(true)
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
            if playable.get(set_id.as_str()) == Some(&tracked.entry_ids()) {
                continue;
            }
            // Not in the read as it was: the set itself decides.
            let set = self.site.set(&set_id).await?;
            if seen(set.as_ref(), &tracked.entries) != Seen::Unchanged {
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
                (OnOpen::Start(attempt), _) => starts.push((set, attempt)),
                (OnOpen::GiveUp, Some(tracked)) => self.give_up(&set.id, tracked)?,
                (OnOpen::GiveUp, None) | (OnOpen::Leave, _) => {}
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
                self.store().save(&self.key, set_id, &tracked, None)?;
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
        self.store().save(&self.key, set_id, &tracked, None)?;
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
        // one, and that match's set is then judged like any live one.
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
            [Ok(one), Ok(two)] => self.command(set, attempt, [one, two]),
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
            // it is, the set is played as a new attempt.
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

    /// Settles the waiting results in order, against a read made for them.
    async fn settle(
        &self,
        pending: &[(String, Received)],
        sets: &[Set],
    ) -> Result<Settled, Trouble> {
        let mut settled = Settled::default();
        for (webhook_id, received) in pending {
            self.settle_one(received, sets, &mut settled).await?;
            self.store().settled(webhook_id)?;
        }
        Ok(settled)
    }

    async fn settle_one(
        &self,
        received: &Received,
        sets: &[Set],
        settled: &mut Settled,
    ) -> Result<(), Trouble> {
        let set_id = &received.id.set;
        let Some(mut tracked) = self
            .store()
            .tracked(&self.key, set_id)?
            .filter(|tracked| tracked.is_current(received.id.attempt))
        else {
            log!(
                "{} set {set_id}: result of Ember match {} is not for its current match; ignored",
                self.key,
                received.match_id
            );
            return Ok(());
        };
        tracked.match_id = Some(received.match_id.clone());
        // The read shows the set unchanged, and nothing has been reported
        // since it was made: that is evidence enough. Anything else, the set
        // itself is asked.
        let listed = sets
            .iter()
            .find(|set| &set.id == set_id)
            .filter(|set| !settled.stale && seen(Some(set), &tracked.entries) == Seen::Unchanged);
        let set = match listed {
            Some(set) => Some(set.clone()),
            None => self.site.set(set_id).await?,
        };
        settled.changed = true;
        let match_id = &received.match_id;
        match on_result(received, &tracked, seen(set.as_ref(), &tracked.entries)) {
            OnResult::Report { games, winner } => {
                let report = Report {
                    entries: tracked.entries.clone(),
                    games,
                    winner,
                };
                self.report(set_id, tracked, &report, match_id).await?;
                settled.reported.insert(set_id.clone());
                settled.stale = true;
            }
            OnResult::AlreadyReported => {
                tracked.progress = Progress::Reported;
                self.store().save(&self.key, set_id, &tracked, None)?;
                settled.reported.insert(set_id.clone());
                log!(
                    "{} set {set_id}: the site already has the score of Ember match {match_id}",
                    self.key
                );
            }
            // Created by this pass's update, from the same read.
            OnResult::Replay(attempt) => {
                tracked.attempt = attempt;
                tracked.players = None;
                tracked.progress = Progress::Waiting;
                self.store().save(&self.key, set_id, &tracked, None)?;
                log!(
                    "{} set {set_id}: Ember match {match_id} ended without a result; playing it again",
                    self.key
                );
            }
            OnResult::GiveUp => self.give_up(set_id, tracked)?,
            OnResult::Close => {
                tracked.progress = Progress::Closed;
                self.store().save(&self.key, set_id, &tracked, None)?;
                log!(
                    "{} set {set_id}: the organizer already decided or changed it; Ember match {match_id} not reported",
                    self.key
                );
            }
            OnResult::Mismatch => {
                tracked.progress = Progress::Stuck;
                self.store().save(&self.key, set_id, &tracked, None)?;
                log!(
                    "{} set {set_id}: Ember match {match_id} was not played by this attempt's players; the organizer decides this set",
                    self.key
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
        report: &Report,
        match_id: &str,
    ) -> Result<(), Trouble> {
        let score = format!("{}-{}", report.games[0], report.games[1]);
        tracked.progress = match self.site.report(set_id, report).await {
            Ok(()) => {
                log!(
                    "{} set {set_id}: reported {score} from Ember match {match_id}",
                    self.key
                );
                Progress::Reported
            }
            Err(SiteError::Refused(why)) => {
                log!(
                    "{} set {set_id}: the site refused the score {score} of Ember match {match_id} ({why}); the organizer enters it",
                    self.key
                );
                Progress::Stuck
            }
            Err(error) => return Err(error.into()),
        };
        self.store().save(&self.key, set_id, &tracked, None)?;
        Ok(())
    }
}
