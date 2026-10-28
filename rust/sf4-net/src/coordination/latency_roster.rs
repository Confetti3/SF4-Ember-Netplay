//! Who a latency connection may be opened with: the active members of the
//! committed native checkpoint, each with its process incarnation, primary
//! endpoint and the latency endpoint it was admitted with. Like
//! `committed_probe_pair_bound`, this is a small JSON boundary rather than a
//! native model dependency.

use std::collections::{BTreeMap, BTreeSet};

use iroh::EndpointId;
use serde_json::Value;

use super::MAX_MEMBERS;

/// `room::MemberStatus` and `room::TablePhase` values.
const STATUS_PLAYING: u64 = 4;
const STATUS_WATCHING: u64 = 5;
const PHASE_PLAYING: u64 = 3;
const PHASE_PAUSED: u64 = 4;

/// One active member of the committed room.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LatencyRow {
    pub member: u64,
    pub incarnation: u64,
    pub primary: EndpointId,
    /// The member's latency endpoint, when it was admitted with a usable one.
    pub latency: Option<EndpointId>,
    /// The region code of that endpoint's relay, when it named a known one.
    pub relay: Option<String>,
    /// Playing, or watching a table whose match is running.
    pub in_match: bool,
}

/// The committed checkpoint could not be read as a room roster.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RosterInvalid;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LatencyRoster {
    rows: Vec<LatencyRow>,
}

impl LatencyRoster {
    pub fn rows(&self) -> &[LatencyRow] {
        &self.rows
    }

    /// The row of `member`. A roster never holds two.
    pub fn row(&self, member: u64) -> Option<&LatencyRow> {
        self.rows.iter().find(|row| row.member == member)
    }

    /// The row whose latency endpoint is `latency`. A roster never holds two.
    pub fn by_latency(&self, latency: EndpointId) -> Option<&LatencyRow> {
        self.rows.iter().find(|row| row.latency == Some(latency))
    }
}

/// 64 lowercase hexadecimal characters, the form the native side writes
/// endpoint IDs in (`EndpointIdentity.hxx`).
fn endpoint_text(text: &str) -> Option<EndpointId> {
    let canonical = text.len() == 64
        && text
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte));
    canonical.then(|| text.parse().ok()).flatten()
}

/// The roster of the committed native `SessionProposal` envelope: every row
/// that is not frozen and whose member is in the room snapshot. A latency ID
/// that is malformed, equals a primary endpoint or is held by two rows is
/// dropped from its rows; the members stay.
pub fn committed_latency_roster(checkpoint: &str) -> Result<LatencyRoster, RosterInvalid> {
    latency_roster_in(super::envelope_view(checkpoint).as_ref())
}

/// `committed_latency_roster` of a commit, from its view.
pub fn latency_roster_of(committed: &super::Committed) -> Result<LatencyRoster, RosterInvalid> {
    latency_roster_in(committed.view())
}

/// `committed_latency_roster` of a parsed envelope or its view, `None` for a
/// checkpoint that is not JSON.
pub(crate) fn latency_roster_in(envelope: Option<&Value>) -> Result<LatencyRoster, RosterInvalid> {
    let envelope = envelope.ok_or(RosterInvalid)?;
    let root = envelope.get("checkpoint").ok_or(RosterInvalid)?;
    let rows = root
        .get("members")
        .and_then(Value::as_array)
        .ok_or(RosterInvalid)?;
    let snapshot = root
        .get("room")
        .and_then(|room| room.get("snapshot"))
        .ok_or(RosterInvalid)?;
    let snapshot_members = snapshot
        .get("members")
        .and_then(Value::as_array)
        .ok_or(RosterInvalid)?;
    let tables = snapshot
        .get("tables")
        .and_then(Value::as_array)
        .map(Vec::as_slice)
        .unwrap_or_default();
    let running = |table: i64| {
        tables.iter().any(|row| {
            row.get("id").and_then(Value::as_i64) == Some(table)
                && matches!(
                    row.get("phase").and_then(Value::as_u64),
                    Some(PHASE_PLAYING | PHASE_PAUSED)
                )
        })
    };
    let in_match = |member: u64| {
        snapshot_members.iter().find_map(|row| {
            (row.get("id").and_then(Value::as_u64) == Some(member)).then(|| {
                match row.get("status").and_then(Value::as_u64) {
                    Some(STATUS_PLAYING) => true,
                    Some(STATUS_WATCHING) => row
                        .get("table")
                        .and_then(Value::as_i64)
                        .is_some_and(running),
                    _ => false,
                }
            })
        })
    };
    let mut active = 0usize;
    let mut members = BTreeSet::new();
    let mut incarnations = BTreeSet::new();
    let mut roster = Vec::new();
    for row in rows {
        if row.get("frozen").and_then(Value::as_bool) == Some(true) {
            continue;
        }
        active += 1;
        if active > MAX_MEMBERS {
            return Err(RosterInvalid);
        }
        let member = row
            .get("member")
            .and_then(Value::as_u64)
            .ok_or(RosterInvalid)?;
        let data = row.get("data").ok_or(RosterInvalid)?;
        let incarnation = row
            .get("incarnation")
            .and_then(Value::as_u64)
            .or_else(|| data.get("incarnation")?.as_u64())
            .ok_or(RosterInvalid)?;
        if incarnation == 0 || !incarnations.insert(incarnation) || !members.insert(member) {
            return Err(RosterInvalid);
        }
        let Some(playing) = in_match(member) else {
            continue;
        };
        // A row without a usable primary endpoint can never be authorized.
        let Some(primary) = data
            .get("authenticatedEndpoint")
            .or_else(|| data.get("authenticated_endpoint"))
            .and_then(Value::as_str)
            .and_then(|text| text.parse::<EndpointId>().ok())
        else {
            continue;
        };
        let route = data.get("latency").filter(|route| route.is_object());
        let latency = route
            .and_then(|route| route.get("endpoint"))
            .and_then(Value::as_str)
            .and_then(endpoint_text);
        let relay = route
            .and_then(|route| route.get("relay"))
            .and_then(Value::as_str)
            .filter(|code| ember_protocol::relay::is_region(code))
            .map(str::to_owned);
        roster.push(LatencyRow {
            member,
            incarnation,
            primary,
            relay: latency.and(relay),
            latency,
            in_match: playing,
        });
    }
    let primaries: BTreeSet<_> = roster.iter().map(|row| row.primary).collect();
    let mut holders = BTreeMap::<EndpointId, usize>::new();
    for latency in roster.iter().filter_map(|row| row.latency) {
        *holders.entry(latency).or_default() += 1;
    }
    for row in &mut roster {
        if row
            .latency
            .is_some_and(|latency| primaries.contains(&latency) || holders[&latency] > 1)
        {
            row.latency = None;
            row.relay = None;
        }
    }
    Ok(LatencyRoster { rows: roster })
}
