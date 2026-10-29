//! Ping to each member (MEMBER_LATENCY.md). Every room member binds a latency
//! endpoint for its room incarnation, admitted into its committed row. While
//! the room board is open, the actor keeps one authenticated latency
//! connection to each other committed member and reports what each costs.
//!
//! Established connections live in actor-owned slots, one per direction per
//! peer; the only tasks are bounded opening tasks. One authority snapshot
//! authorizes every step, and it is revoked whenever the local binding, the
//! room or the committed state it was read from cannot be trusted any more.
use std::{
    collections::BTreeMap,
    sync::{Arc, Mutex},
    time::Duration,
};

use iroh::{
    EndpointId,
    endpoint::{Connection, Incoming},
};
use tokio::{
    sync::{Semaphore, watch},
    task::{AbortHandle, JoinError, JoinSet},
    time::Instant,
};

use super::{
    Actor, Event, MAX_CONTROL_PEERS,
    protocol::{LatencyRoute, LatencyState, LatencyUnavailable, MemberLatencyRow},
};
use crate::coordination::Committed;

mod authority;
mod endpoint;
mod opening;
#[cfg(test)]
mod tests;

use authority::{Inputs, LatencyAuthority, Observer, PeerBinding};
pub(super) use endpoint::bind_for_room;
pub(super) use endpoint::{LatencyEndpoint, LatencyNet};
use opening::{
    ACCEPT_DEADLINE, AcceptContext, AdmissionTable, DIAL_DEADLINE, DialFailure, DialRequest,
    InboundOpened, MAX_DIALS, MAX_PENDING, OutboundOpened, SlotGuard, accept_opening, dial_opening,
};

/// The first wait before a member is dialled again, doubling to `REDIAL_MAX`.
const REDIAL_FIRST: Duration = Duration::from_secs(5);
const REDIAL_MAX: Duration = Duration::from_secs(60);
/// How long the last value of a member who is playing is still shown.
const VALUE_TTL: Duration = Duration::from_secs(30);
/// A board subscription lapses this long after its last renewal.
const LEASE: Duration = Duration::from_secs(15);

/// A connection another member's board opened to us.
pub(super) struct InboundSlot {
    pub(super) binding: PeerBinding,
    pub(super) guard: SlotGuard,
}

/// A connection our board opened. Only these produce values.
pub(super) struct OutboundSlot {
    pub(super) binding: PeerBinding,
    /// Counts this member's outbound replacements.
    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) generation: u64,
    pub(super) guard: SlotGuard,
    pub(super) value: Option<Sample>,
}

/// The dialer's state for one member of the active view.
struct Dial {
    binding: PeerBinding,
    /// The current attempt; only its completion is installed.
    attempt: u64,
    task: Option<tokio::task::Id>,
    failure: Option<DialFailure>,
    backoff: Duration,
    next: Instant,
    generation: u64,
}

/// The board subscription the native side holds.
struct View {
    epoch: u64,
    room: [u8; 16],
    view: u64,
    lease: Instant,
    /// The last value of each member, kept for `VALUE_TTL` once it plays.
    last: BTreeMap<u64, (u64, Sample, Instant)>,
    /// The table last sent.
    sent: Option<Vec<MemberLatencyRow>>,
}

pub(super) struct Latency {
    pub(super) net: LatencyNet,
    /// The room incarnation's latency endpoint.
    pub(super) endpoint: Option<LatencyEndpoint>,
    observer: Observer,
    /// The authority as opening tasks read it.
    authority: watch::Sender<Arc<LatencyAuthority>>,
    /// The store last read, kept so a change of games alone re-evaluates the
    /// authority without reading it again.
    store: Option<(u64, Arc<Committed>, u64)>,
    /// Signals a replaced commit in the current room's store.
    observed: Option<(u64, watch::Receiver<u64>)>,
    had_game: bool,
    permits: Arc<Semaphore>,
    pub(super) admissions: Arc<Mutex<AdmissionTable>>,
    pub(super) inbound: BTreeMap<EndpointId, InboundSlot>,
    pub(super) outbound: BTreeMap<u64, OutboundSlot>,
    pub(super) openings_in: JoinSet<Option<InboundOpened>>,
    pub(super) openings_out: JoinSet<Result<OutboundOpened, DialFailure>>,
    /// Every opening task until it is reaped.
    pub(super) handles: BTreeMap<tokio::task::Id, AbortHandle>,
    dials: BTreeMap<u64, Dial>,
    next_attempt: u64,
    view: Option<View>,
}

impl Latency {
    /// Tests only: an actor that binds no latency endpoint, for the tests
    /// that are not about latency.
    #[cfg(test)]
    pub(super) fn offline() -> Self {
        Self {
            net: LatencyNet::off(),
            ..Self::default()
        }
    }
}

impl Default for Latency {
    fn default() -> Self {
        let observer = Observer::default();
        let (authority, _) = watch::channel(observer.current());
        Self {
            net: LatencyNet::default(),
            endpoint: None,
            observer,
            authority,
            store: None,
            observed: None,
            had_game: false,
            permits: Arc::new(Semaphore::new(MAX_PENDING)),
            admissions: Arc::default(),
            inbound: BTreeMap::new(),
            outbound: BTreeMap::new(),
            openings_in: JoinSet::new(),
            openings_out: JoinSet::new(),
            handles: BTreeMap::new(),
            dials: BTreeMap::new(),
            next_attempt: 0,
            view: None,
        }
    }
}

/// Runs `work` while the latency endpoint `binding` asks for is bound, as
/// the join and host tasks do.
/// The work never waits on the bind: when it is done, a bind still running is
/// dropped, and the room has no latency route. The route is sent once, in the
/// join request, so there is nothing to publish later.
pub(super) async fn beside<T>(
    binding: Option<(LatencyNet, crate::transport::Transport)>,
    work: impl std::future::Future<Output = T>,
) -> (T, Option<LatencyEndpoint>) {
    let Some((net, policy)) = binding else {
        return (work.await, None);
    };
    // LAN admission may finish immediately, before its latency endpoint binds.
    // Await both bounded operations so the one admission carries that route.
    if policy == crate::transport::Transport::Lan {
        return tokio::join!(work, bind_for_room(net, policy));
    }
    let bind = bind_for_room(net, policy);
    tokio::pin!(work, bind);
    let mut bound = None;
    let mut binding = true;
    loop {
        tokio::select! {
            biased;
            result = &mut work => return (result, bound),
            endpoint = &mut bind, if binding => {
                binding = false;
                bound = endpoint;
            }
        }
    }
}

/// The next incoming latency connection, or never without an endpoint.
pub(super) async fn accept(endpoint: Option<&LatencyEndpoint>) -> Option<Incoming> {
    match endpoint {
        Some(endpoint) => endpoint.endpoint().accept().await,
        None => std::future::pending().await,
    }
}

/// Whole milliseconds, 1 to 999.
fn milliseconds(rtt: Duration) -> u16 {
    ((rtt.as_micros() + 500) / 1000).clamp(1, 999) as u16
}

/// A value: whole milliseconds and the path they were measured over.
pub(super) type Sample = (u16, LatencyRoute);

/// The route of the connection's selected path; a relay while none is selected.
fn route(connection: &Connection) -> LatencyRoute {
    let direct = connection
        .paths()
        .iter()
        .any(|path| path.is_selected() && !path.is_relay());
    if direct {
        LatencyRoute::Direct
    } else {
        LatencyRoute::Relay
    }
}

fn row(member: u64, incarnation: u64, state: LatencyState) -> MemberLatencyRow {
    MemberLatencyRow {
        member,
        incarnation,
        state,
    }
}

fn unavailable(member: u64, incarnation: u64, reason: LatencyUnavailable) -> MemberLatencyRow {
    row(member, incarnation, LatencyState::Unavailable { reason })
}

fn value(member: u64, incarnation: u64, (rtt_ms, route): Sample) -> MemberLatencyRow {
    row(member, incarnation, LatencyState::Value { rtt_ms, route })
}

impl Latency {
    pub(super) fn authority(&self) -> Arc<LatencyAuthority> {
        self.observer.current()
    }

    fn publish(&self) {
        self.authority.send_replace(self.observer.current());
    }

    /// Drops every slot and every opening: what a revocation does.
    fn drop_everything(&mut self) {
        self.inbound.clear();
        self.outbound.clear();
        self.openings_in.abort_all();
        self.openings_out.abort_all();
        for dial in self.dials.values_mut() {
            dial.task = None;
        }
    }

    /// Ends the board subscription: its outbound slots, dials and table go;
    /// inbound slots serve other members' boards and stay.
    fn stop_view(&mut self) {
        self.view = None;
        self.outbound.clear();
        self.openings_out.abort_all();
        self.dials.clear();
    }

    /// Applies what an observation changed.
    fn apply(&mut self, change: authority::Change) {
        if change.revoked {
            self.drop_everything();
        } else if change.roster {
            let authority = self.observer.current();
            if let (Some(local), Some(roster)) = (authority.local(), authority.roster()) {
                self.inbound
                    .retain(|_, slot| authority.authorizes(&local, &slot.binding));
                self.outbound
                    .retain(|_, slot| authority.authorizes(&local, &slot.binding));
                let mut table = opening::lock(&self.admissions);
                for pending in table.pending.values() {
                    if !authority.authorizes(&local, &pending.binding)
                        && let Some(handle) = self.handles.get(&pending.task)
                    {
                        handle.abort();
                    }
                }
                table.retain(|(latency, incarnation)| {
                    roster
                        .rows()
                        .iter()
                        .any(|row| row.latency == Some(*latency) && row.incarnation == *incarnation)
                });
                drop(table);
                let handles = &self.handles;
                self.dials.retain(|_, dial| {
                    let keep = authority.authorizes(&local, &dial.binding);
                    if !keep && let Some(handle) = dial.task.and_then(|id| handles.get(&id)) {
                        handle.abort();
                    }
                    keep
                });
            }
        }
        self.publish();
    }

    fn observe(&mut self, inputs: Inputs) {
        let change = self.observer.observe(inputs);
        self.apply(change);
    }

    /// The table of every other member, as the native board shows it.
    fn rows(&self, primary: EndpointId) -> Vec<MemberLatencyRow> {
        let authority = self.observer.current();
        let now = Instant::now();
        let Some(roster) = authority.roster() else {
            // No authority: whoever the last commit named is unavailable.
            return self.observer.last_roster().map_or_else(Vec::new, |roster| {
                roster
                    .rows()
                    .iter()
                    .filter(|entry| entry.primary != primary)
                    .take(MAX_CONTROL_PEERS)
                    .map(|entry| {
                        unavailable(
                            entry.member,
                            entry.incarnation,
                            LatencyUnavailable::RoomUnavailable,
                        )
                    })
                    .collect()
            });
        };
        let me = authority.local_member();
        let kept = |member: u64, incarnation: u64| {
            self.view
                .as_ref()
                .and_then(|view| view.last.get(&member))
                .filter(|(at_incarnation, _, at)| {
                    *at_incarnation == incarnation && now.saturating_duration_since(*at) < VALUE_TTL
                })
                .map(|(_, rtt, _)| *rtt)
        };
        roster
            .rows()
            .iter()
            .filter(|entry| Some(entry.member) != me && entry.primary != primary)
            .take(MAX_CONTROL_PEERS)
            .map(|entry| {
                let (member, incarnation) = (entry.member, entry.incarnation);
                if entry.latency.is_none() {
                    return unavailable(member, incarnation, LatencyUnavailable::NoLatencyRoute);
                }
                if entry.in_match {
                    return kept(member, incarnation).map_or_else(
                        || unavailable(member, incarnation, LatencyUnavailable::InMatch),
                        |rtt| value(member, incarnation, rtt),
                    );
                }
                if let Some(rtt) = self
                    .outbound
                    .get(&member)
                    .filter(|slot| slot.binding.incarnation == incarnation)
                    .and_then(|slot| slot.value)
                {
                    return value(member, incarnation, rtt);
                }
                match self
                    .dials
                    .get(&member)
                    .filter(|dial| dial.binding.incarnation == incarnation)
                    .and_then(|dial| dial.failure)
                {
                    Some(DialFailure::Refused) => {
                        unavailable(member, incarnation, LatencyUnavailable::Refused)
                    }
                    Some(DialFailure::Unreachable) => {
                        unavailable(member, incarnation, LatencyUnavailable::Unreachable)
                    }
                    None => row(member, incarnation, LatencyState::Measuring),
                }
            })
            .collect()
    }
}

impl Actor {
    /// A public room's members measure over relays only, as the diagnostic
    /// relay-only policy does.
    pub(super) fn latency_binding(&self) -> Option<(LatencyNet, crate::transport::Transport)> {
        let policy = if self.public.is_some() {
            crate::transport::Transport::RelayOnly
        } else {
            self.transport
        };
        (!self.is_public_host()).then(|| (self.latency.net.clone(), policy))
    }

    /// Our latency endpoint and its relay region, for `hosted` and
    /// `connected`.
    pub(super) fn latency_route(&self) -> (Option<EndpointId>, Option<String>) {
        let Some(latency) = &self.latency.endpoint else {
            return (None, None);
        };
        let region = super::network::known_home_region(latency.endpoint()).map(str::to_owned);
        (Some(latency.endpoint().id()), region)
    }

    /// Keeps the endpoint a join or host task bound, when it is for the room
    /// being opened; otherwise it is dropped, which closes it.
    pub(super) fn latency_adopt(&mut self, epoch: u64, endpoint: Option<LatencyEndpoint>) {
        if epoch == self.epoch && self.latency.endpoint.is_none() {
            self.latency.endpoint = endpoint;
        }
    }

    /// Ends latency for the room: the endpoint, the subscription, every slot
    /// and opening.
    pub(super) fn latency_clear(&mut self) {
        self.latency.stop_view();
        self.latency.endpoint = None;
        self.latency.store = None;
        self.latency.observed = None;
        self.latency.observe(Inputs {
            room: None,
            epoch: self.epoch,
            endpoints: None,
            store: None,
            has_game: false,
        });
        self.latency.observer.forget();
        self.latency.drop_everything();
        // Attempt numbers carry on, so a mark of an opening still being torn
        // down can never be taken for a new one's.
        let mut admissions = opening::lock(&self.latency.admissions);
        admissions.charged.clear();
        admissions.pending.clear();
    }

    fn latency_inputs(&self) -> Inputs {
        Inputs {
            room: self.room,
            epoch: self.epoch,
            endpoints: self
                .latency
                .endpoint
                .as_ref()
                .map(|latency| (self.endpoint.id(), latency.endpoint().id())),
            store: self.latency.store.clone(),
            has_game: !self.games.is_empty(),
        }
    }

    /// Reads the store and applies the observation rules (3.3).
    pub(super) async fn latency_refresh(&mut self) {
        if self.latency.endpoint.is_none() {
            return;
        }
        self.latency.store = match &self.recovery {
            Some(recovery) => {
                let owner = recovery.incarnation;
                if self.latency.observed.as_ref().map(|(id, _)| *id) != Some(owner) {
                    let changes = recovery.coordinator.store().observed_changes().await;
                    self.latency.observed = Some((owner, changes));
                }
                if let Some((_, changes)) = &mut self.latency.observed {
                    changes.mark_unchanged();
                }
                let (committed, count) = recovery.coordinator.store().committed_observed().await;
                Some((owner, committed, count))
            }
            None => {
                self.latency.observed = None;
                None
            }
        };
        self.latency.had_game = !self.games.is_empty();
        let inputs = self.latency_inputs();
        self.latency.observe(inputs);
    }

    /// A game slot was added or removed: preparation counts as a match.
    pub(super) fn latency_games_changed(&mut self) {
        let has_game = !self.games.is_empty();
        if self.latency.endpoint.is_none() || has_game == self.latency.had_game {
            return;
        }
        self.latency.had_game = has_game;
        let inputs = self.latency_inputs();
        self.latency.observe(inputs);
    }

    /// The actor's tick: a replaced commit or a change of games is applied
    /// at once.
    pub(super) async fn latency_tick(&mut self) {
        if self.latency.endpoint.is_none() {
            return;
        }
        let replaced = self
            .latency
            .observed
            .as_ref()
            .is_some_and(|(_, changes)| changes.has_changed().unwrap_or(true));
        if replaced {
            self.latency_refresh().await;
        } else {
            self.latency_games_changed();
        }
    }

    /// `latency_watch`: start, renew or stop the board subscription.
    pub(super) fn latency_watch(&mut self, epoch: u64, room: [u8; 16], view: u64, on: bool) {
        if !self.matches(epoch) || self.room != Some(room) || view == 0 {
            return;
        }
        let current = self.latency.view.as_ref().map(|active| active.view);
        if !on {
            if current == Some(view) {
                self.latency.stop_view();
            }
            return;
        }
        let lease = Instant::now() + LEASE;
        if current == Some(view)
            && let Some(active) = &mut self.latency.view
        {
            active.lease = lease;
            return;
        }
        self.latency.stop_view();
        self.latency.view = Some(View {
            epoch,
            room,
            view,
            lease,
            last: BTreeMap::new(),
            sent: None,
        });
        self.latency_dial();
    }

    /// The statistics arm, once a second: refresh, retire dead slots, sample,
    /// report, dial.
    pub(super) async fn latency_second(&mut self) {
        if self.latency.endpoint.is_none() {
            return;
        }
        self.latency_refresh().await;
        let now = Instant::now();
        if self.latency.view.as_ref().is_some_and(|view| {
            now >= view.lease || view.epoch != self.epoch || Some(view.room) != self.room
        }) {
            self.latency.stop_view();
        }
        self.latency
            .inbound
            .retain(|_, slot| slot.guard.connection.close_reason().is_none());
        let dead: Vec<u64> = self
            .latency
            .outbound
            .iter()
            .filter(|(_, slot)| slot.guard.connection.close_reason().is_some())
            .map(|(member, _)| *member)
            .collect();
        for member in dead {
            self.latency.outbound.remove(&member);
            if let Some(dial) = self.latency.dials.get_mut(&member) {
                dial.next = now + dial.backoff;
            }
        }
        self.latency_sample(now);
        self.latency_report();
        self.latency_dial();
    }

    /// Reads each outbound slot's selected path RTT.
    fn latency_sample(&mut self, now: Instant) {
        for (member, slot) in &mut self.latency.outbound {
            let paths = slot.guard.connection.paths();
            let Some(rtt) = paths
                .iter()
                .find(|path| path.is_selected())
                .map(|path| path.rtt())
            else {
                continue;
            };
            let rtt = (milliseconds(rtt), route(&slot.guard.connection));
            slot.value = Some(rtt);
            if let Some(view) = &mut self.latency.view {
                view.last
                    .insert(*member, (slot.binding.incarnation, rtt, now));
            }
        }
    }

    /// Sends the whole table when it changed. It is droppable: a full queue
    /// leaves it to the next second.
    fn latency_report(&mut self) {
        let Some(view) = &self.latency.view else {
            return;
        };
        let rows = self.latency.rows(self.endpoint.id());
        if view.sent.as_ref() == Some(&rows) {
            return;
        }
        let event = Event::MemberLatency {
            epoch: view.epoch,
            room: view.room,
            view: view.view,
            members: rows.clone(),
        };
        if self.events.emit_bulk(event)
            && let Some(view) = &mut self.latency.view
        {
            view.sent = Some(rows);
        }
    }

    /// Dials every member of the view that has no outbound slot, none in
    /// flight and no backoff left, up to `MAX_DIALS` at once.
    pub(super) fn latency_dial(&mut self) {
        let Some(latency) = &self.latency.endpoint else {
            return;
        };
        let authority = self.latency.authority();
        let (Some(roster), Some(_)) = (authority.roster(), self.latency.view.as_ref()) else {
            return;
        };
        let now = Instant::now();
        let endpoint = latency.endpoint().clone();
        for entry in roster.rows() {
            if self.latency.openings_out.len() >= MAX_DIALS {
                break;
            }
            if entry.in_match {
                // The member's helper has closed its side; its last value is kept.
                self.latency.outbound.remove(&entry.member);
                continue;
            }
            let Some((local, binding)) = authority.binding_of(entry.member) else {
                continue;
            };
            if self.latency.outbound.contains_key(&entry.member) {
                continue;
            }
            if let Some(dial) = self.latency.dials.get(&entry.member)
                && dial.binding == binding
                && (dial.task.is_some() || now < dial.next)
            {
                continue;
            }
            self.latency.next_attempt += 1;
            let attempt = self.latency.next_attempt;
            let request = DialRequest {
                endpoint: endpoint.clone(),
                binding,
                local,
                relay: entry
                    .relay
                    .as_deref()
                    .and_then(crate::invite::relay_for_region),
                revocation: authority.revocation,
                attempt,
            };
            let handle = self
                .latency
                .openings_out
                .spawn(dial_opening(request, now + DIAL_DEADLINE));
            let task = handle.id();
            self.latency.handles.insert(task, handle);
            let dial = self.latency.dials.entry(entry.member).or_insert(Dial {
                binding,
                attempt,
                task: None,
                failure: None,
                backoff: REDIAL_FIRST,
                next: now,
                generation: 0,
            });
            if dial.binding != binding {
                *dial = Dial {
                    binding,
                    attempt,
                    task: None,
                    failure: None,
                    backoff: REDIAL_FIRST,
                    next: now,
                    generation: dial.generation,
                };
            }
            dial.attempt = attempt;
            dial.task = Some(task);
        }
    }

    /// An incoming latency connection: a permit or a refusal before TLS.
    pub(super) fn latency_incoming(&mut self, incoming: Option<Incoming>) {
        let Some(incoming) = incoming else {
            // The endpoint closed under us: the room has no latency any more.
            self.latency_clear();
            return;
        };
        if self.latency.openings_in.len() >= MAX_PENDING {
            return incoming.refuse();
        }
        let Ok(permit) = self.latency.permits.clone().try_acquire_owned() else {
            return incoming.refuse();
        };
        let context = AcceptContext {
            authority: self.latency.authority.subscribe(),
            admissions: self.latency.admissions.clone(),
        };
        let deadline = Instant::now() + ACCEPT_DEADLINE;
        let handle = self
            .latency
            .openings_in
            .spawn(accept_opening(incoming, permit, context, deadline));
        self.latency.handles.insert(handle.id(), handle);
    }

    /// Reaps one inbound opening task. Its handle goes first, whatever the
    /// outcome; a failure has already cleaned up through its guards.
    pub(super) async fn latency_inbound_done(
        &mut self,
        result: Result<(tokio::task::Id, Option<InboundOpened>), JoinError>,
    ) {
        let id = match &result {
            Ok((id, _)) => *id,
            Err(error) => error.id(),
        };
        self.latency.handles.remove(&id);
        if let Ok((_, Some(opened))) = result {
            self.latency_install_inbound(opened).await;
        }
    }

    /// Installs an inbound opening if the refreshed authority still
    /// authorizes it. Otherwise it is dropped, and its guards close the
    /// connection, clear the pending mark and release the permit.
    async fn latency_install_inbound(&mut self, opened: InboundOpened) {
        self.latency_refresh().await;
        let authority = self.latency.authority();
        if opened.revocation != authority.revocation
            || !authority.authorizes(&opened.local, &opened.binding)
        {
            return;
        }
        let InboundOpened {
            binding,
            connection,
            send,
            recv,
            reservation,
            permit,
            ..
        } = opened;
        self.latency.inbound.insert(
            binding.latency,
            InboundSlot {
                binding,
                guard: SlotGuard::new(connection, send, recv),
            },
        );
        // Released only once the slot exists.
        drop(reservation);
        drop(permit);
    }

    /// Reaps one outbound opening task.
    pub(super) async fn latency_outbound_done(
        &mut self,
        result: Result<(tokio::task::Id, Result<OutboundOpened, DialFailure>), JoinError>,
    ) {
        let id = match &result {
            Ok((id, _)) => *id,
            Err(error) => error.id(),
        };
        self.latency.handles.remove(&id);
        let dial = self
            .latency
            .dials
            .values_mut()
            .find(|dial| dial.task == Some(id));
        if let Some(dial) = dial {
            dial.task = None;
            if let Ok((_, Err(failure))) = &result {
                dial.failure = Some(*failure);
                dial.next = Instant::now() + dial.backoff;
                dial.backoff = (dial.backoff * 2).min(REDIAL_MAX);
            }
        }
        if let Ok((_, Ok(opened))) = result {
            self.latency_install_outbound(opened).await;
        }
    }

    async fn latency_install_outbound(&mut self, opened: OutboundOpened) {
        self.latency_refresh().await;
        let authority = self.latency.authority();
        let member = opened.binding.member;
        let current = self.latency.view.is_some()
            && self
                .latency
                .dials
                .get(&member)
                .is_some_and(|dial| dial.attempt == opened.attempt);
        if !current
            || opened.revocation != authority.revocation
            || !authority.authorizes(&opened.local, &opened.binding)
        {
            return;
        }
        let Some(dial) = self.latency.dials.get_mut(&member) else {
            return;
        };
        dial.generation += 1;
        dial.failure = None;
        dial.backoff = REDIAL_FIRST;
        let generation = dial.generation;
        let OutboundOpened {
            binding,
            connection,
            send,
            recv,
            rtt,
            ..
        } = opened;
        let guard = SlotGuard::new(connection, send, recv);
        let rtt = (milliseconds(rtt), route(&guard.connection));
        if let Some(view) = &mut self.latency.view {
            view.last
                .insert(member, (binding.incarnation, rtt, Instant::now()));
        }
        self.latency.outbound.insert(
            member,
            OutboundSlot {
                binding,
                generation,
                guard,
                value: Some(rtt),
            },
        );
    }
}
