//! One tournament. Each pass reads the bracket once, when a result is
//! waiting or the schedule says so: it settles the results against that
//! read, cancels the Ember matches of sets the organizer changed, and creates
//! one for each set both of whose players have Ember.
//!
//! Only what a set shows is evidence: a set missing from a paged list is
//! asked for on its own before its match is cancelled or its result set
//! aside. A failed pass is not followed by another read, of any kind, until
//! a growing wait has passed. Outside the window the bracket is read only
//! for results; once it has finished it is not read at all. Passes of one
//! tournament never overlap.
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
    schedule::{Phase, Retry, Schedule, Window},
    sets::{
        MAX_ATTEMPTS, OnOpen, OnResult, Progress, Received, Seen, Tracked, adds_up, on_open,
        on_result, seen,
    },
    site::{Purpose, Report, Reported, Set, Site, SiteError, Standing},
    store::Store,
};

/// How often a worker wakes when nothing wakes it.
const WAKE_SECS: u64 = 60;
/// Tries at a report the site defers (`SiteError::Deferred`) before its set
/// is left to the organizer. The result waits a minute after the first,
/// doubling up to an hour: 12 tries span 1 + 2 + 4 + 8 + 16 + 32 minutes and
/// five hours, about 6 hours. Counted while the connector runs.
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
    /// Held for a whole pass.
    state: Mutex<State>,
    pub wake: Notify,
}

/// What a tournament's worker keeps between passes.
struct State {
    schedule: Schedule,
    /// Reads after a failed pass.
    retry: Retry,
    /// Results whose report the site deferred, by webhook ID: each waits
    /// on its own, while the rest of the tournament carries on.
    deferred: HashMap<String, Retry>,
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
    /// Its report waits, on its own, for a later pass.
    Deferred,
}

/// Why a pass stopped early. It is tried again later.
enum Trouble {
    Site(SiteError),
    Bridge(BridgeError),
    Store(rusqlite::Error),
}

impl From<BridgeError> for Trouble {
    fn from(error: BridgeError) -> Self {
        Self::Bridge(error)
    }
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
            Self::Bridge(error) => error.fmt(f),
            Self::Store(error) => write!(f, "database: {error}"),
        }
    }
}

/// The playable sets of a read, in a form that compares.
fn fingerprint(sets: &[Set]) -> Vec<(String, [String; 2])> {
    let mut read: Vec<(String, [String; 2])> = sets
        .iter()
        .filter_map(|set| Some((set.id.clone(), set.playable_ids()?.map(str::to_owned))))
        .collect();
    read.sort();
    read
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
            lookup_failing: AtomicBool::new(false),
            state: Mutex::new(State {
                schedule,
                retry: Retry::default(),
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
        let scheduled = in_window && (poll || state.schedule.due(now));
        if pending.is_empty() && !scheduled {
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
        let read = fingerprint(&sets);
        let mut changed = state.last_read.as_ref() != Some(&read);
        state.last_read = Some(read);
        let settled = self
            .settle(&pending, &sets, &mut state.deferred, now)
            .await?;
        changed |= settled.changed;
        changed |= self
            .update(&sets, &settled.reported, purpose, in_window)
            .await?;
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
        purpose: Purpose,
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
            // Not in the read as it was: the set itself decides. Asking can
            // wait for the budget like the read it follows; the match goes
            // on meanwhile.
            let set = match self.site.set(&set_id, purpose).await {
                Err(SiteError::BudgetSpent) => continue,
                set => set?,
            };
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

    /// Settles the waiting results in order, against a read made for them.
    /// A result the site defers waits on its own; the others go on.
    async fn settle(
        &self,
        pending: &[(String, Received)],
        sets: &[Set],
        deferred: &mut HashMap<String, Retry>,
        now: u64,
    ) -> Result<Settled, Trouble> {
        let mut settled = Settled::default();
        for (webhook_id, received) in pending {
            let tries = deferred.get(webhook_id).map_or(0, Retry::failures);
            match self.settle_one(received, sets, &mut settled, tries).await? {
                Settle::Done => {
                    deferred.remove(webhook_id);
                    self.store().settled(webhook_id)?;
                }
                Settle::Deferred => {
                    deferred.entry(webhook_id.clone()).or_default().failed(now);
                }
            }
        }
        Ok(settled)
    }

    /// `tries`: how often this result's report was deferred before.
    async fn settle_one(
        &self,
        received: &Received,
        sets: &[Set],
        settled: &mut Settled,
        tries: u32,
    ) -> Result<Settle, Trouble> {
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
            return Ok(Settle::Done);
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
            None => self.site.set(set_id, Purpose::Result).await?,
        };
        settled.changed = true;
        let match_id = &received.match_id;
        match on_result(received, &tracked, seen(set.as_ref(), &tracked.entries)) {
            OnResult::Report { games, winner } => {
                // The games one by one, for a site that takes them, which
                // must add up to the score.
                let sequence = if self.site.wants_games() {
                    self.context.bridge.sequence(match_id).await?
                } else {
                    Vec::new()
                };
                if self.site.wants_games() && !adds_up(&sequence, games) {
                    tracked.progress = Progress::Stuck;
                    self.store().save(&self.key, set_id, &tracked, None)?;
                    log!(
                        "{} set {set_id}: the games of Ember match {match_id} do not add up to its score; the organizer decides this set",
                        self.key
                    );
                    return Ok(Settle::Done);
                }
                let report = Report {
                    entries: tracked.entries.clone(),
                    games,
                    winner,
                    sequence,
                    key: match_id.clone(),
                };
                if self
                    .report(set_id, tracked, &report, match_id, tries)
                    .await?
                    == Settle::Deferred
                {
                    return Ok(Settle::Deferred);
                }
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
        Ok(Settle::Done)
    }

    /// Reports the score. A score the site refuses is left to the
    /// organizer; one it defers waits, up to `MAX_DEFERRALS` tries.
    async fn report(
        &self,
        set_id: &str,
        mut tracked: Tracked,
        report: &Report,
        match_id: &str,
        tries: u32,
    ) -> Result<Settle, Trouble> {
        let score = format!("{}-{}", report.games[0], report.games[1]);
        tracked.progress = match self.site.report(set_id, report).await {
            Ok(Reported::Sent) => {
                log!(
                    "{} set {set_id}: reported {score} from Ember match {match_id}",
                    self.key
                );
                Progress::Reported
            }
            Ok(Reported::AlreadyHeld) => {
                log!(
                    "{} set {set_id}: the site already had the winner of Ember match {match_id}; its score there is the site's",
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
            Err(SiteError::Deferred(why)) if tries + 1 < MAX_DEFERRALS => {
                log!(
                    "{} set {set_id}: {why}; the score {score} of Ember match {match_id} is sent again later (try {} of {MAX_DEFERRALS})",
                    self.key,
                    tries + 1
                );
                return Ok(Settle::Deferred);
            }
            Err(SiteError::Deferred(why)) => {
                log!(
                    "{} set {set_id}: {why}; the score {score} of Ember match {match_id} was refused {MAX_DEFERRALS} times, and the organizer enters it",
                    self.key
                );
                Progress::Stuck
            }
            Err(error) => return Err(error.into()),
        };
        self.store().save(&self.key, set_id, &tracked, None)?;
        Ok(Settle::Done)
    }
}
