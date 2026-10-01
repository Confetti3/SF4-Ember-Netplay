//! The event log (spec 20). An event is committed in the same transaction as
//! the state change it describes, together with one outbox row per matching
//! webhook subscription. Its canonical bytes are stored once and sent
//! unchanged on every delivery attempt and replay.
use ember_protocol::{
    EmberId,
    encoding::Counter,
    event::{CONTENT_TYPE, Event, Kind, SCHEMA_PREFIX, SPEC_VERSION, rfc3339},
    json,
};
use rusqlite::{OptionalExtension, Transaction, params};

use crate::{
    config::Config,
    error::{ApiFailure, Result},
    util::new_id,
};

pub struct NewEvent<'a> {
    pub kind: Kind,
    pub tenant_id: &'a str,
    pub connection_id: Option<&'a str>,
    pub subject: String,
    pub match_id: Option<&'a str>,
    /// Set only for identity events, which are private to this identity and
    /// its provider connection.
    pub ember_id: Option<&'a EmberId>,
    /// The lobby an event belongs to. Everyone who has joined it may read it.
    pub lobby_id: Option<&'a str>,
    /// The tournament an event belongs to. Its entrants may read it.
    pub tournament_id: Option<&'a str>,
    pub data: serde_json::Value,
}

pub fn emit(tx: &Transaction<'_>, config: &Config, now: u64, event: NewEvent<'_>) -> Result<i64> {
    let serde_json::Value::Object(data) = &event.data else {
        return Err(ApiFailure::unavailable());
    };
    let data = data.clone();
    let seq: i64 = tx.query_row("SELECT COALESCE(MAX(seq), 0) + 1 FROM events", [], |row| {
        row.get(0)
    })?;
    let id = new_id("evt");
    let envelope = Event {
        specversion: SPEC_VERSION.into(),
        id: id.clone(),
        source: config.origin.clone(),
        kind: event.kind.full(),
        subject: event.subject.clone(),
        time: rfc3339(now),
        datacontenttype: CONTENT_TYPE.into(),
        dataschema: format!("{SCHEMA_PREFIX}{}", event.kind.name()),
        emberseq: Counter(seq as u64),
        data,
    };
    envelope
        .check(config.policy())
        .map_err(|_| ApiFailure::unavailable())?;
    let body = json::canonical(&envelope).map_err(|_| ApiFailure::unavailable())?;
    tx.execute(
        "INSERT INTO events (seq, id, tenant_id, connection_id, type, subject, match_id, ember_id, body, created_at, lobby_id,
            tournament_id)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
        params![
            seq,
            id,
            event.tenant_id,
            event.connection_id,
            envelope.kind,
            event.subject,
            event.match_id,
            event.ember_id.map(EmberId::as_str),
            body,
            now,
            event.lobby_id,
            event.tournament_id
        ],
    )?;
    fan_out(tx, seq, &event, &envelope.kind, now)?;
    Ok(seq)
}

fn fan_out(
    tx: &Transaction<'_>,
    seq: i64,
    event: &NewEvent<'_>,
    kind: &str,
    now: u64,
) -> Result<()> {
    let mut statement = tx.prepare(
        "SELECT id, connection_id, event_types FROM webhook_subscriptions WHERE tenant_id = ?1 AND enabled = 1",
    )?;
    let subscriptions = statement
        .query_map([event.tenant_id], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, Option<String>>(1)?,
                row.get::<_, String>(2)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (id, connection, types) in subscriptions {
        let wanted: Vec<String> = serde_json::from_str(&types).unwrap_or_default();
        if !wanted.iter().any(|wanted| wanted == kind) {
            continue;
        }
        let visible = match (&connection, event.connection_id) {
            (Some(subscriber), Some(source)) => subscriber == source,
            // Organizer subscriptions see tenant match events, never identity.
            (None, _) => !event.kind.is_identity(),
            (Some(_), None) => false,
        };
        if visible {
            tx.execute(
                "INSERT OR IGNORE INTO delivery_outbox (event_seq, subscription_id, state, next_attempt_at)
                 VALUES (?1, ?2, 'pending', ?3)",
                params![seq, id, now],
            )?;
        }
    }
    Ok(())
}

/// Who is reading the log, and so which events they may see (WEB-06).
#[derive(Clone, Debug)]
pub enum Viewer {
    Provider {
        tenant_id: String,
        connection_id: String,
    },
    Organizer {
        tenant_id: String,
    },
    Player {
        ember_id: EmberId,
    },
}

pub const MAX_PAGE: usize = 200;

/// Events after `after` visible to `viewer`, oldest first.
pub fn list(
    tx: &Transaction<'_>,
    viewer: &Viewer,
    after: i64,
    limit: usize,
) -> Result<Vec<(i64, Vec<u8>)>> {
    let oldest: Option<i64> = tx
        .query_row("SELECT MIN(seq) FROM events", [], |row| row.get(0))
        .optional()?
        .flatten();
    if after > 0 && oldest.is_some_and(|oldest| after < oldest - 1) {
        return Err(ApiFailure::new(
            ember_protocol::api::ErrorCode::ResyncRequired,
            "The cursor is older than the retained log. Fetch a snapshot and continue from its cursor.",
        ));
    }
    let limit = limit.clamp(1, MAX_PAGE) as i64;
    let map = |row: &rusqlite::Row<'_>| Ok((row.get::<_, i64>(0)?, row.get::<_, Vec<u8>>(1)?));
    let rows = match viewer {
        Viewer::Provider {
            tenant_id,
            connection_id,
        } => tx
            .prepare(
                "SELECT seq, body FROM events WHERE tenant_id = ?1 AND connection_id = ?2 AND seq > ?3
                 ORDER BY seq LIMIT ?4",
            )?
            .query_map(params![tenant_id, connection_id, after, limit], map)?
            .collect::<rusqlite::Result<Vec<_>>>()?,
        Viewer::Organizer { tenant_id } => tx
            .prepare(
                "SELECT seq, body FROM events WHERE tenant_id = ?1 AND ember_id IS NULL AND seq > ?2
                 ORDER BY seq LIMIT ?3",
            )?
            .query_map(params![tenant_id, after, limit], map)?
            .collect::<rusqlite::Result<Vec<_>>>()?,
        Viewer::Player { ember_id } => tx
            .prepare(
                "SELECT seq, body FROM events WHERE seq > ?1 AND (ember_id = ?2 OR match_id IN
                    (SELECT match_id FROM match_participants WHERE ember_id = ?2) OR lobby_id IN
                    (SELECT lobby_id FROM lobby_entries WHERE ember_id = ?2) OR tournament_id IN
                    (SELECT tournament_id FROM tournament_entrants WHERE ember_id = ?2))
                 ORDER BY seq LIMIT ?3",
            )?
            .query_map(params![after, ember_id.as_str(), limit], map)?
            .collect::<rusqlite::Result<Vec<_>>>()?,
    };
    Ok(rows)
}

/// `{"events":[...],"next_cursor":"N"}` with each event's stored bytes.
pub fn page_body(rows: &[(i64, Vec<u8>)], after: i64) -> Vec<u8> {
    let mut body = b"{\"events\":[".to_vec();
    for (index, (_, event)) in rows.iter().enumerate() {
        if index > 0 {
            body.push(b',');
        }
        body.extend_from_slice(event);
    }
    let cursor = rows.last().map_or(after, |(seq, _)| *seq);
    body.extend_from_slice(format!("],\"next_cursor\":\"{cursor}\"}}").as_bytes());
    body
}

/// The current head of the log, for snapshots.
pub fn head(tx: &Transaction<'_>) -> Result<i64> {
    Ok(
        tx.query_row("SELECT COALESCE(MAX(seq), 0) FROM events", [], |row| {
            row.get(0)
        })?,
    )
}
