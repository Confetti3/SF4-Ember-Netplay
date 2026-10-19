//! One tournament (README, "For each configured tournament"). Each pass
//! reads the bracket once, when a result is waiting or the schedule says
//! so: it settles the results against that read, cancels the Ember matches
//! of sets the organizer changed, and creates one for each set both of
//! whose players have Ember. Passes of one tournament never overlap.
//!
//! Creating a set's match is in `start`, and reading back from the bridge a
//! result whose delivery is failing in `reconcile`.
mod reconcile;
mod start;

use std::{
    collections::{HashMap, HashSet},
    fmt,
    sync::{Arc, atomic::AtomicBool},
    time::Duration,
};

use ember_protocol::EmberId;
use tokio::sync::{Mutex, Notify};

use crate::{
    Clock,
    bridge::{BridgeClient, BridgeError},
    config::{Rounds, Tournament},
    log,
    schedule::{Backoff, Phase, Window},
    sets::{
        MAX_ATTEMPTS, OnOpen, OnResult, Progress, Received, Seen, Tracked, adds_up, on_open,
        on_result, seen,
    },
    site::{Purpose, Report, Reported, Set, Site, SiteError, Standing},
    store::Store,
};

/// How often a worker wakes when nothing wakes it.
const WAKE_SECS: u64 = 60;
/// Tries at a report that waits on its own (the site deferred it, or failed
/// on it after a read that worked) before its set is left to the organizer.
/// The result waits a minute after the first, doubling up to an hour: 12
/// tries span 1 + 2 + 4 + 8 + 16 + 32 minutes and five hours, about 6
/// hours. Counted while the connector runs.
const MAX_DEFERRALS: u32 = 12;

/// What every tournament shares.
pub(crate) struct Context {
    pub store: Arc<Store>,
    pub bridge: BridgeClient,
    pub clock: Clock,
    pub build_id: String,
    pub poll_secs: u64,
    pub max_poll_secs: u64,
    /// Players whose old account connection has been pointed out since the
    /// connector started, so each is pointed out once.
    pub pointed_out: std::sync::Mutex<HashSet<EmberId>>,
}

pub(crate) struct Bracket {
    /// `<site>:<tournament>`.
    pub key: String,
    site: Box<dyn Site>,
    rounds: Rounds,
    window: Window,
    mark_underway: bool,
    context: Arc<Context>,
    /// Whether the last lookup of players failed, so a failure is logged
    /// once until lookups work again.
    lookup_failing: AtomicBool,
    /// Ember matches whose cancel the bridge has refused, each logged once
    /// until it is cancelled.
    cancel_failing: std::sync::Mutex<HashSet<String>>,
    /// Held for a whole pass.
    state: Mutex<State>,
    pub wake: Notify,
}

/// What a tournament's worker keeps between passes.
struct State {
    /// When the bracket is next read on a schedule: `poll_secs`, doubling
    /// while reads find nothing new, up to `max_poll_secs`.
    schedule: Backoff,
    /// Reads after a failed pass: a minute, doubling up to an hour.
    retry: Backoff,
    /// Results whose report waits on its own, by webhook ID, while the rest
    /// of the tournament carries on.
    deferred: HashMap<String, Backoff>,
    /// The bridge's read-back: when it is next due, and what it remembers.
    reconcile: reconcile::Memory,
    /// The playable sets the last read found, by ID and entries.
    last_read: Option<Vec<(String, [String; 2])>>,
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

/// What became of one result in a pass.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Settle {
    /// Settled for good: reported, set aside, or played again.
    Done,
    /// Not for its set's current match: dropped, and nothing changed.
    Ignored,
    /// Its report waits, on its own, for a later pass.
    Deferred,
}

impl From<BridgeError> for SiteError {
    fn from(error: BridgeError) -> Self {
        Self::Unavailable(match error {
            BridgeError::Unavailable => error.to_string(),
            BridgeError::Refused { .. } => format!("the bridge refused: {error}"),
        })
    }
}

impl From<rusqlite::Error> for SiteError {
    fn from(error: rusqlite::Error) -> Self {
        Self::Unavailable(format!("database: {error}"))
    }
}

/// The playable sets of a read, in a form that compares.
fn fingerprint(sets: &[Set]) -> Vec<(String, [String; 2])> {
    sets.iter()
        .filter_map(|set| Some((set.id.clone(), set.playable_ids()?.map(str::to_owned))))
        .collect()
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
        let schedule = Backoff::new(context.poll_secs, context.max_poll_secs);
        Ok(Self {
            key,
            site,
            rounds: tournament.rounds.clone(),
            window: tournament.window()?,
            mark_underway: tournament.mark_underway,
            context,
            lookup_failing: AtomicBool::new(false),
            cancel_failing: std::sync::Mutex::default(),
            state: Mutex::new(State {
                schedule,
                retry: Backoff::retry(),
                deferred: HashMap::new(),
                reconcile: reconcile::Memory::default(),
                last_read: None,
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

    /// Asks the bridge about live matches when that is due, then, unless a
    /// failure is being waited out, reads the bracket if a result is
    /// waiting, if `poll`, or if the schedule says so, and acts on what it
    /// finds.
    pub async fn pass(&self, poll: bool) {
        let mut state = self.state.lock().await;
        if state.finished {
            return;
        }
        let now = self.context.clock.now();
        self.reconcile(&mut state.reconcile, now).await;
        match self.read(&mut state, now, poll).await {
            Ok(false) => {}
            Ok(true) => {
                let failures = state.retry.reset();
                if failures > 0 {
                    log!(
                        "{}: reading works again ({failures} failed before)",
                        self.key
                    );
                }
            }
            Err(SiteError::BudgetSpent) => {}
            Err(error) => {
                if state.retry.grow(now) == 1 {
                    log!(
                        "{}: {error}; tried again after a growing wait, up to an hour",
                        self.key
                    );
                }
            }
        }
    }

    fn store(&self) -> &Store {
        &self.context.store
    }

    /// Ends the set's current attempt as `progress`, saying why.
    fn end(
        &self,
        set_id: &str,
        mut tracked: Tracked,
        progress: Progress,
        why: fmt::Arguments<'_>,
    ) -> Result<(), SiteError> {
        tracked.progress = progress;
        self.store().save(&self.key, set_id, &tracked)?;
        log!("{} set {set_id}: {why}", self.key);
        Ok(())
    }

    /// Ends the attempt of a creation never answered, whose set the
    /// organizer decided or changed (`seen`). The creation is sent again
    /// as it was: the bridge answers with the match it made for it, or, if
    /// it never had it, makes it now. Either way that match is cancelled,
    /// as a live one would be. Returns whether the attempt ended.
    async fn withdraw(
        &self,
        set_id: &str,
        mut tracked: Tracked,
        seen: Seen,
    ) -> Result<bool, SiteError> {
        let command = match self.store().command(&self.key, set_id)? {
            Some(Ok(command)) => command,
            Some(Err(why)) => {
                self.end(
                    set_id,
                    tracked,
                    Progress::Stuck,
                    format_args!(
                        "its saved creation cannot be read ({why}); its Ember match may exist, and the organizer decides this set"
                    ),
                )?;
                return Ok(true);
            }
            None => return Ok(false),
        };
        match self.context.bridge.create(&command).await {
            Ok(match_id) => {
                tracked.match_id = Some(match_id);
                self.close(set_id, tracked, seen).await
            }
            // Tried again on the next read.
            Err(BridgeError::Unavailable) => Ok(false),
            // No match of this attempt is known to cancel: the bridge made
            // none, or holds another under its ID.
            Err(error) => {
                let why = format!(
                    "its creation was never answered, and the bridge now refuses it ({error})"
                );
                match seen {
                    Seen::Decided { .. } => {
                        self.end(set_id, tracked, Progress::Closed, format_args!("{why}"))?
                    }
                    _ => self.play_again_later(set_id, tracked, format_args!("{why}"))?,
                }
                Ok(true)
            }
        }
    }

    /// Ends an attempt whose set changed, or went away, before it was
    /// decided: the set is played as a new attempt when it opens again, for
    /// whichever players, until its last attempt.
    fn play_again_later(
        &self,
        set_id: &str,
        mut tracked: Tracked,
        why: fmt::Arguments<'_>,
    ) -> Result<(), SiteError> {
        if tracked.attempt >= MAX_ATTEMPTS {
            log!("{} set {set_id}: {why}", self.key);
            return self.give_up(set_id, tracked);
        }
        tracked.attempt += 1;
        tracked.players = None;
        tracked.match_id = None;
        self.end(
            set_id,
            tracked,
            Progress::Waiting,
            format_args!("{why}; a new attempt is played when it opens"),
        )
    }

    fn give_up(&self, set_id: &str, tracked: Tracked) -> Result<(), SiteError> {
        self.end(
            set_id,
            tracked,
            Progress::Stuck,
            format_args!(
                "{MAX_ATTEMPTS} Ember matches ended without a result; the organizer decides this set"
            ),
        )
    }

    /// One read of the bracket and what follows from it. Returns whether a
    /// read was made.
    async fn read(&self, state: &mut State, now: u64, poll: bool) -> Result<bool, SiteError> {
        let phase = self.window.phase(now);
        if phase == Phase::After && !std::mem::replace(&mut state.window_end_logged, true) {
            log!(
                "{}: its window has ended; it is read only to report results, and no match is created",
                self.key
            );
        }
        // After a failure nothing is read, whatever asks for it, until the
        // wait is over: a site that keeps failing costs a few requests an hour.
        if !state.retry.ready(now) {
            return Ok(false);
        }
        let in_window = phase == Phase::Open;
        let mut pending = self.store().pending(&self.key)?;
        state
            .deferred
            .retain(|webhook_id, _| pending.iter().any(|(id, _)| id == webhook_id));
        pending.retain(|(webhook_id, _)| {
            state
                .deferred
                .get(webhook_id)
                .is_none_or(|wait| wait.ready(now))
        });
        let scheduled = in_window && (poll || state.schedule.ready(now));
        if pending.is_empty() && !scheduled {
            return Ok(false);
        }
        // A result is settled now, whatever the schedule or the budget.
        let purpose = if pending.is_empty() {
            Purpose::Scheduled
        } else {
            Purpose::Result
        };
        state.schedule.hold(now);
        let mut sets = self.site.open_sets(purpose).await?;
        // A paged list can show a set twice, when a set ahead of it opens
        // between two pages.
        sets.sort_by(|a, b| a.id.cmp(&b.id));
        sets.dedup_by(|a, b| a.id == b.id);
        let read = fingerprint(&sets);
        let mut changed = state.last_read.as_ref() != Some(&read);
        state.last_read = Some(read);
        // The matches are brought in line with the read even when a result
        // could not be settled: a failure is waited out after that.
        let (settled, failure) = self
            .settle(&pending, &sets, &mut state.deferred, now, phase)
            .await;
        let updated = self
            .update(&sets, &settled.reported, purpose, in_window)
            .await;
        if let Some(error) = failure {
            return Err(error);
        }
        changed |= settled.changed || updated?;
        state.schedule.read(now, changed);
        if !sets.is_empty() {
            state.played_out = false;
        } else if !state.played_out && self.store().has_sets(&self.key)? {
            // A budget too spent to ask is no failure: the read worked.
            let standing = match self.site.standing().await {
                Err(SiteError::BudgetSpent) => Standing::Running,
                standing => standing?,
            };
            match standing {
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
            state.schedule.longest(now);
        }
        Ok(true)
    }

    /// Brings the Ember matches in line with a read: cancels those whose set
    /// changed and, if `create`, creates those whose set is ready. Returns
    /// whether it did either. A live set that cannot be asked about holds
    /// back only itself: the failure is returned after the rest is done.
    async fn update(
        &self,
        sets: &[Set],
        reported: &HashSet<String>,
        purpose: Purpose,
        create: bool,
    ) -> Result<bool, SiteError> {
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
        let mut failure = None;
        for (set_id, tracked) in self.store().live(&self.key)? {
            if playable.get(set_id.as_str()) == Some(&tracked.entry_ids()) {
                continue;
            }
            // Not in the read as it was: the set itself decides. Asking can
            // wait for the budget like the read it follows; the match goes
            // on meanwhile.
            let set = match self.site.set(&set_id, purpose).await {
                Ok(set) => set,
                Err(SiteError::BudgetSpent) => continue,
                Err(error) => {
                    failure.get_or_insert(error);
                    continue;
                }
            };
            match seen(set.as_ref(), &tracked.entries) {
                Seen::Unchanged => {}
                seen => changed |= self.close(&set_id, tracked, seen).await?,
            }
        }
        // A creation never answered, whose set is not open as it was: the
        // bridge may have made its match, which is ended here rather than
        // left until it expires. One whose set is listed as it was is sent
        // again below.
        for (set_id, tracked) in self.store().unanswered(&self.key)? {
            if playable.get(set_id.as_str()) == Some(&tracked.entry_ids()) {
                continue;
            }
            let set = match self.site.set(&set_id, purpose).await {
                Ok(set) => set,
                Err(SiteError::BudgetSpent) => continue,
                Err(error) => {
                    failure.get_or_insert(error);
                    continue;
                }
            };
            match seen(set.as_ref(), &tracked.entries) {
                Seen::Unchanged => {}
                seen => changed |= self.withdraw(&set_id, tracked, seen).await?,
            }
        }
        if create {
            changed |= self.create_ready(&open).await?;
        }
        match failure {
            Some(error) => Err(error),
            None => Ok(changed),
        }
    }

    /// Creates the matches of the sets that are ready.
    async fn create_ready(&self, open: &[(&Set, [&str; 2])]) -> Result<bool, SiteError> {
        let mut starts = Vec::new();
        for &(set, entries) in open {
            let tracked = self.store().tracked(&self.key, &set.id)?;
            match (on_open(tracked.as_ref(), entries), tracked) {
                (OnOpen::Start(attempt), _) => starts.push((set, attempt)),
                (OnOpen::GiveUp, Some(tracked)) => self.give_up(&set.id, tracked)?,
                (OnOpen::GiveUp, None) | (OnOpen::Leave, _) => {}
            }
        }
        self.start(&starts).await
    }

    /// Cancels the Ember match of a set the organizer decided or changed
    /// (`seen`). A cancel that fails is tried again on the next read, and
    /// logged once. Returns whether it ended.
    async fn close(&self, set_id: &str, tracked: Tracked, seen: Seen) -> Result<bool, SiteError> {
        let Some(match_id) = tracked.match_id.clone() else {
            return Ok(false);
        };
        let cancelled = self
            .context
            .bridge
            .cancel(&match_id, "The organizer changed this set.")
            .await;
        let first_failure = {
            let mut failing = self
                .cancel_failing
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            match &cancelled {
                Ok(()) => {
                    failing.remove(&match_id);
                    false
                }
                Err(_) => failing.insert(match_id.clone()),
            }
        };
        match cancelled {
            Ok(()) => {
                match seen {
                    Seen::Decided { .. } => self.end(
                        set_id,
                        tracked,
                        Progress::Closed,
                        format_args!("decided by the organizer; Ember match {match_id} ended"),
                    )?,
                    _ => self.play_again_later(
                        set_id,
                        tracked,
                        format_args!("changed by the organizer; Ember match {match_id} ended"),
                    )?,
                }
                Ok(true)
            }
            Err(error) => {
                if first_failure {
                    log!(
                        "{} set {set_id}: cannot cancel Ember match {match_id}: {error}; tried again on each read",
                        self.key
                    );
                }
                Ok(false)
            }
        }
    }

    /// Settles the waiting results in order, against a read made for them.
    /// A result whose report waits does so on its own, and the others go
    /// on. A failure stops it, and is returned beside what was settled; a
    /// refused token or version is returned once every result was tried.
    async fn settle(
        &self,
        pending: &[(String, Received)],
        sets: &[Set],
        deferred: &mut HashMap<String, Backoff>,
        now: u64,
        phase: Phase,
    ) -> (Settled, Option<SiteError>) {
        let mut settled = Settled::default();
        // A refused token or contract version stops nothing here: every
        // result is still tried (each is refused the same way, or goes in),
        // and the refusal is returned at the end, so the tournament waits.
        let mut denied = None;
        for (webhook_id, received) in pending {
            let tries = deferred.get(webhook_id).map_or(0, Backoff::count);
            let outcome = self
                .settle_one(received, sets, &mut settled, tries, phase)
                .await;
            let done = match outcome {
                Ok(Settle::Done) => {
                    // A result settled changes the bracket, so the schedule
                    // starts again from its shortest interval.
                    settled.changed = true;
                    true
                }
                Ok(Settle::Ignored) => true,
                Ok(Settle::Deferred) => {
                    deferred
                        .entry(webhook_id.clone())
                        .or_insert_with(Backoff::retry)
                        .grow(now);
                    false
                }
                Err(SiteError::Denied(why)) => {
                    denied.get_or_insert(SiteError::Denied(why));
                    false
                }
                Err(error) => return (settled, Some(error)),
            };
            if done {
                deferred.remove(webhook_id);
                if let Err(error) = self.store().settled(webhook_id) {
                    return (settled, Some(error.into()));
                }
            }
        }
        (settled, denied)
    }

    /// `tries`: how often this result's report waited before.
    async fn settle_one(
        &self,
        received: &Received,
        sets: &[Set],
        settled: &mut Settled,
        tries: u32,
        phase: Phase,
    ) -> Result<Settle, SiteError> {
        let set_id = &received.id.set;
        let match_id = &received.match_id;
        let Some(mut tracked) = self
            .store()
            .tracked(&self.key, set_id)?
            .filter(|tracked| tracked.is_current(received.id.attempt))
        else {
            log!(
                "{} set {set_id}: result of Ember match {match_id} is not for its current match; ignored",
                self.key
            );
            return Ok(Settle::Ignored);
        };
        tracked.match_id = Some(match_id.clone());
        // The read shows the set unchanged, and nothing has been reported
        // since it was made: that is evidence enough. Anything else, the set
        // itself is asked.
        let listed = sets
            .iter()
            .find(|set| &set.id == set_id)
            .filter(|set| !settled.stale && seen(Some(set), &tracked.entries) == Seen::Unchanged);
        let set = match listed {
            Some(set) => Some(set.clone()),
            None => self.site.set(set_id, Purpose::Result).await?,
        };
        let seen = seen(set.as_ref(), &tracked.entries);
        match on_result(received, &tracked, seen) {
            OnResult::Report { games, winner } => {
                // The games one by one, for a site that takes them, which
                // must add up to the score.
                let sequence = if self.site.wants_games() {
                    self.context.bridge.sequence(match_id).await?
                } else {
                    Vec::new()
                };
                if self.site.wants_games() && !adds_up(&sequence, games) {
                    self.end(
                        set_id,
                        tracked,
                        Progress::Stuck,
                        format_args!(
                            "the games of Ember match {match_id} do not add up to its score; the organizer decides this set"
                        ),
                    )?;
                    return Ok(Settle::Done);
                }
                let report = Report {
                    entries: tracked.entries.clone(),
                    games,
                    winner,
                    sequence,
                    key: match_id.clone(),
                };
                let outcome = self.report(set_id, tracked, &report, tries).await?;
                // Even a report that waits may have gone in: the read is no
                // longer evidence for a later result.
                settled.stale = true;
                if outcome == Settle::Done {
                    settled.reported.insert(set_id.clone());
                }
                return Ok(outcome);
            }
            OnResult::AlreadyReported => {
                settled.reported.insert(set_id.clone());
                self.end(
                    set_id,
                    tracked,
                    Progress::Reported,
                    format_args!("the site already has the score of Ember match {match_id}"),
                )?;
            }
            // Created by this pass's update, from the same read.
            OnResult::Replay(attempt) => {
                tracked.attempt = attempt;
                tracked.players = None;
                let next = if phase == Phase::After {
                    "its window has ended, so no match is created, and the organizer decides this set unless the window is extended"
                } else {
                    "playing it again"
                };
                self.end(
                    set_id,
                    tracked,
                    Progress::Waiting,
                    format_args!("Ember match {match_id} ended without a result; {next}"),
                )?;
            }
            OnResult::GiveUp => self.give_up(set_id, tracked)?,
            OnResult::Close if matches!(seen, Seen::Decided { .. }) => self.end(
                set_id,
                tracked,
                Progress::Closed,
                format_args!(
                    "the organizer already decided it; Ember match {match_id} not reported"
                ),
            )?,
            OnResult::Close => self.play_again_later(
                set_id,
                tracked,
                format_args!(
                    "the organizer changed it before its result; Ember match {match_id} not reported"
                ),
            )?,
            OnResult::Mismatch => self.end(
                set_id,
                tracked,
                Progress::Stuck,
                format_args!(
                    "Ember match {match_id} was not played by this attempt's players; the organizer decides this set"
                ),
            )?,
        }
        Ok(Settle::Done)
    }

    /// Reports the score. A score the site refuses is left to the
    /// organizer. One it defers, or fails on although the read before it
    /// worked, waits on its own, up to `MAX_DEFERRALS` tries.
    async fn report(
        &self,
        set_id: &str,
        tracked: Tracked,
        report: &Report,
        tries: u32,
    ) -> Result<Settle, SiteError> {
        let match_id = &report.key;
        let score = format!("{}-{}", report.games[0], report.games[1]);
        let (progress, why) = match self.site.report(set_id, report).await {
            Ok(Reported::Sent) => (
                Progress::Reported,
                format!("reported {score} from Ember match {match_id}"),
            ),
            Ok(Reported::AlreadyHeld) => (
                Progress::Reported,
                format!(
                    "the site already had the winner of Ember match {match_id}; its score there is the site's"
                ),
            ),
            Err(SiteError::Refused(why)) => (
                Progress::Stuck,
                format!(
                    "the site refused the score {score} of Ember match {match_id} ({why}); the organizer enters it"
                ),
            ),
            Err(SiteError::Deferred(why) | SiteError::Unavailable(why))
                if tries + 1 < MAX_DEFERRALS =>
            {
                log!(
                    "{} set {set_id}: {why}; the score {score} of Ember match {match_id} is sent again later (try {} of {MAX_DEFERRALS})",
                    self.key,
                    tries + 1
                );
                return Ok(Settle::Deferred);
            }
            Err(SiteError::Deferred(why) | SiteError::Unavailable(why)) => (
                Progress::Stuck,
                format!(
                    "{why}; the score {score} of Ember match {match_id} could not be sent in {MAX_DEFERRALS} tries, and the organizer enters it"
                ),
            ),
            Err(error) => return Err(error),
        };
        self.end(set_id, tracked, progress, format_args!("{why}"))?;
        Ok(Settle::Done)
    }
}
