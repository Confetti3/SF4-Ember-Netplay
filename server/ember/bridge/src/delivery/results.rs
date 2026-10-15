//! Finished matches' results, sent to the platform that created the match: to
//! BluMint's API in BluMint's format (`routes::blumint::submission`), or to a
//! connection's `results_url` as a signed `partner::MatchResult`.
//!
//! Only enabled connections with what they need to be sent (BluMint's API key,
//! or a result secret) are leased, so a connection still waiting for one holds
//! nobody else's back, and its results go out once it has it. Leasing marks a
//! result `delivering`; from then on it cannot be corrected
//! (`ledger::settle`), so what is sent is what the match keeps.
use std::collections::BTreeMap;

use ember_protocol::{
    matches::MatchState,
    partner::{MatchResult, Outcome as Ended, RESULT_TYPE, ResultParticipant, result_id},
    webhook::{self, Secret},
};
use rusqlite::{Transaction, params};
use serde_json::json;
use zeroize::Zeroizing;

use super::{
    BATCH, Leased, Outcome, Queue, Record, Settled,
    webhooks::{Answer, answer, post_signed},
};
use crate::{
    AppState,
    audit::audit,
    config::BLUMINT,
    error::Result,
    routes::{
        blumint,
        ledger::{Match, load, participants, scores},
        policy,
    },
    util::outbound_client,
};

/// Where one connection's results go.
enum Destination {
    /// BluMint's submit endpoint, with BluMint's API key for the connection.
    BluMint {
        submit_url: String,
        key: Zeroizing<String>,
    },
    /// The connection's `results_url`, signed with its result secret.
    Signed { url: String, secret: Secret },
}

impl Destination {
    /// Who the audit log names as sending.
    fn actor(&self) -> &'static str {
        match self {
            Self::BluMint { .. } => BLUMINT,
            Self::Signed { .. } => "results",
        }
    }

    fn format(&self) -> Format {
        match self {
            Self::BluMint { .. } => Format::BluMint,
            Self::Signed { .. } => Format::Signed,
        }
    }

    async fn send(
        &self,
        state: &AppState,
        match_id: &str,
        body: Vec<u8>,
    ) -> std::result::Result<Answer, Outcome> {
        match self {
            Self::BluMint { submit_url, key } => {
                let retry = |error: &str| Outcome::Retry {
                    error: error.to_owned(),
                    retry_after: None,
                };
                let Ok(client) = outbound_client().build() else {
                    return Err(retry("client"));
                };
                let response = client
                    .post(submit_url)
                    .header("x-api-key", key.as_str())
                    .header("content-type", "application/json")
                    .body(body)
                    .send()
                    .await
                    .map_err(|error| {
                        retry(if error.is_timeout() {
                            "timeout"
                        } else {
                            "connection failed"
                        })
                    })?;
                Ok(answer(response).await)
            }
            Self::Signed { url, secret } => {
                let Ok(headers) =
                    webhook::sign(&[secret], &result_id(match_id), state.now(), &body)
                else {
                    return Err(Outcome::Retry {
                        error: "cannot sign".into(),
                        retry_after: None,
                    });
                };
                post_signed(state, url, &headers, body, async { true }).await
            }
        }
    }
}

/// The body a destination takes.
#[derive(Clone, Copy)]
enum Format {
    BluMint,
    Signed,
}

impl Format {
    /// The body for a finished match, or None while it is still on.
    fn body(self, tx: &Transaction<'_>, bridge_id: &str, found: &Match) -> Result<Option<Vec<u8>>> {
        Ok(match self {
            Self::BluMint => {
                blumint::submission(tx, found)?.map(|body| body.to_string().into_bytes())
            }
            Self::Signed => result(tx, bridge_id, found)?
                .map(|result| serde_json::to_vec(&result).unwrap_or_default()),
        })
    }
}

/// A finished match's result for a connection's `results_url`, or None while
/// it is still on.
fn result(tx: &Transaction<'_>, bridge_id: &str, found: &Match) -> Result<Option<MatchResult>> {
    let outcome = match found.state {
        MatchState::Completed => Ended::Completed,
        MatchState::Cancelled | MatchState::Failed => Ended::Restart,
        MatchState::Expired => Ended::Expired,
        _ => return Ok(None),
    };
    let (wins, _) = scores(tx, &found.id)?;
    let participants: Vec<ResultParticipant> = participants(tx, &found.id, found.generation)?
        .into_iter()
        .map(|p| ResultParticipant {
            score: wins[usize::from(p.slot)],
            participant_id: p.participant_id,
            ember_id: p.ember_id,
            slot: p.slot,
        })
        .collect();
    let winner_participant_id = (outcome == Ended::Completed)
        .then(|| {
            participants
                .iter()
                .find(|p| p.score >= found.games_to_win)
                .map(|p| p.participant_id.clone())
        })
        .flatten();
    Ok(Some(MatchResult {
        kind: RESULT_TYPE.into(),
        bridge_id: bridge_id.to_owned(),
        connection_id: found.connection_id.clone(),
        match_id: found.id.clone(),
        external_match_id: found.external_match_id.clone(),
        outcome,
        revision: found.revision.to_string(),
        participants,
        winner_participant_id,
    }))
}

/// The results queue (a `delivery::Queue`).
pub struct Results {
    destinations: BTreeMap<String, Destination>,
}

impl Results {
    pub fn new(state: &AppState) -> Self {
        let destinations = state
            .config
            .tenants
            .iter()
            .flat_map(|tenant| &tenant.connections)
            .filter(|connection| connection.enabled)
            .filter_map(|connection| {
                let destination = if connection.is_blumint() {
                    Destination::BluMint {
                        submit_url: format!(
                            "{}/tournaments/match/submit",
                            blumint::api_base(connection)
                        ),
                        key: state.integrations.api_keys.get(&connection.id)?.clone(),
                    }
                } else {
                    Destination::Signed {
                        url: connection.results_url.clone()?,
                        secret: state
                            .integrations
                            .result_secrets
                            .get(&connection.id)?
                            .clone(),
                    }
                };
                Some((connection.id.clone(), destination))
            })
            .collect();
        Self { destinations }
    }
}

pub struct Due {
    match_id: String,
    connection_id: String,
}

impl Queue for Results {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let connections: Vec<&String> = self.destinations.keys().collect();
        let rows = tx
            .prepare(
                "SELECT m.id, m.connection_id, m.delivery_attempts, m.delivery_first_at
                 FROM matches m JOIN provider_connections c ON c.id = m.connection_id
                 WHERE m.delivery_state IN ('queued', 'retrying', 'delivering') AND m.delivery_next_at <= ?1
                   AND m.state IN ('completed', 'cancelled', 'failed', 'expired')
                   AND m.connection_id IN (SELECT value FROM json_each(?2)) AND c.enabled = 1
                 ORDER BY m.delivery_next_at, m.id LIMIT ?3",
            )?
            .query_map(params![now, json!(connections).to_string(), BATCH], |row| {
                Ok(Leased {
                    item: Due {
                        match_id: row.get(0)?,
                        connection_id: row.get(1)?,
                    },
                    attempt: row.get::<_, u32>(2)? + 1,
                    first_attempt_at: row.get(3)?,
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE matches SET delivery_state = 'delivering', delivery_next_at = ?1, delivery_attempts = ?2
                 WHERE id = ?3",
                params![until, row.attempt, row.item.match_id],
            )?;
        }
        Ok(rows)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let retry = |error: String, retry_after: Option<u64>| Outcome::Retry { error, retry_after };
        let Some(destination) = self.destinations.get(&item.connection_id) else {
            return Outcome::Cancelled;
        };
        let (id, format) = (item.match_id.clone(), destination.format());
        let bridge_id = state.config.bridge_id.clone();
        // Checked again as late as possible, as webhooks are.
        let body = state
            .db
            .read(move |tx| match load(tx, &id)? {
                Some(found) if policy::enabled(tx, &found.connection_id)? => {
                    format.body(tx, &bridge_id, &found)
                }
                _ => Ok(None),
            })
            .await;
        let body = match body {
            Ok(Some(body)) => body,
            // Its connection was disabled since the lease; it is sent once the
            // connection is enabled again, its attempts one higher.
            Ok(None) => return Outcome::Cancelled,
            Err(_) => return retry("database unavailable".into(), None),
        };
        let answered = match destination.send(state, &item.match_id, body).await {
            Ok(answered) => answered,
            Err(outcome) => return outcome,
        };
        let status = answered.status;
        match status {
            // 409: the platform already has this match's result.
            200..=299 | 409 => Outcome::Delivered,
            429 => retry(format!("http {status}"), answered.retry_after),
            // Any other refusal is the same on a retry.
            400..=499 => Outcome::Refused(format!("http {status}")),
            _ => retry(format!("http {status}"), answered.retry_after),
        }
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (state, next, error) = match &record.settled {
            Settled::Delivered => ("delivered", record.at, None),
            Settled::Retry { at, error } => ("retrying", *at, Some(error.as_str())),
            Settled::Refused(error) | Settled::Expired(error) => {
                ("failed", record.at, Some(error.as_str()))
            }
        };
        let changed = tx.execute(
            "UPDATE matches SET delivery_state = ?1, delivery_next_at = ?2,
                delivery_first_at = COALESCE(delivery_first_at, ?3)
             WHERE id = ?4 AND delivery_state = 'delivering' AND delivery_attempts = ?5",
            params![
                state,
                next,
                record.first_attempt_at,
                leased.item.match_id,
                leased.attempt
            ],
        )?;
        if changed == 1 {
            let actor = self
                .destinations
                .get(&leased.item.connection_id)
                .map_or("results", Destination::actor);
            audit(
                tx,
                record.at,
                ("bridge", actor.into()),
                &format!("{actor}.submit"),
                &leased.item.match_id,
                state,
                error,
            )?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn only_the_latest_lease_records_its_outcome() {
        let path = std::env::temp_dir().join(format!(
            "ember-bridge-lease-{}.sqlite3",
            ember_protocol::encoding::b64u(&crate::util::random::<12>())
        ));
        let db = crate::Db::open(&path).unwrap();
        db.write(|tx| {
            tx.execute_batch(
                "INSERT INTO tenants (id, name) VALUES ('bm', 'BluMint');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('bm-partner', 'bm', 'blumint', 'staging', 'BluMint', 1);
                 INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
                   assignment_generation, state, games_to_win, rules, required_build_id, metadata,
                   delivery_state, created_at, updated_at)
                   VALUES ('emt_1', 'bm', 'bm-partner', 'blumint_1', '', 1, 1, 'completed', 2, '{}', '', '{}',
                   'queued', 0, 0);",
            )?;
            Ok(())
        })
        .await
        .unwrap();
        let results = std::sync::Arc::new(Results {
            destinations: [(
                "bm-partner".to_owned(),
                Destination::BluMint {
                    submit_url: String::new(),
                    key: Zeroizing::new(String::new()),
                },
            )]
            .into(),
        });
        let lease = |now: u64| {
            let results = results.clone();
            db.write(move |tx| results.lease(tx, now, now + 60))
        };
        let record = |leased: Leased<Due>, settled: Settled| {
            let results = results.clone();
            db.write(move |tx| {
                results.record(
                    tx,
                    &leased,
                    &Record {
                        at: 100,
                        first_attempt_at: 0,
                        settled,
                    },
                )
            })
        };
        let first = lease(0).await.unwrap().pop().unwrap();
        assert!(lease(30).await.unwrap().is_empty(), "leased twice");
        let marked: String = db
            .read(|tx| {
                Ok(tx.query_row(
                    "SELECT delivery_state FROM matches WHERE id = 'emt_1'",
                    [],
                    |row| row.get(0),
                )?)
            })
            .await
            .unwrap();
        assert_eq!(marked, "delivering");
        // The first attempt outlives its lease and is leased again.
        let second = lease(61).await.unwrap().pop().unwrap();
        assert_eq!((first.attempt, second.attempt), (1, 2));
        record(
            first,
            Settled::Retry {
                at: 101,
                error: "timeout".into(),
            },
        )
        .await
        .unwrap();
        record(second, Settled::Delivered).await.unwrap();
        let stored: (String, u32) = db
            .read(|tx| {
                Ok(tx.query_row(
                    "SELECT delivery_state, delivery_attempts FROM matches WHERE id = 'emt_1'",
                    [],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )?)
            })
            .await
            .unwrap();
        assert_eq!(stored, ("delivered".to_owned(), 2));
        drop(db);
        for suffix in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(format!("{}{suffix}", path.display()));
        }
    }
}
