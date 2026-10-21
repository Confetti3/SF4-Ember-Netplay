//! Stores the replay archive's games in its bucket (`routes::archive`):
//! tournament games first, each as `games/<crc>.emberreplay`, and with a
//! set's game its whole pack again as `sets/<match id>.emberpack`. A stored
//! game's replay leaves the bridge. Nothing is given up: the game marks a
//! replay sent once the bridge has it, so this is the archive's only copy,
//! and one the bucket did not take is tried again, hourly once a day has
//! passed.
use rusqlite::{Transaction, params};

use super::{BATCH, Leased, Outcome, Queue, Record, Settled};
use crate::{
    AppState,
    error::Result,
    routes::{ledger::load, replays::packed},
    s3,
};

pub struct Due {
    crc: String,
    replay: Vec<u8>,
    set: Option<String>,
}

/// Games waiting for the archive's bucket.
pub struct Archive;

impl Queue for Archive {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let rows = tx
            .prepare(
                "SELECT crc, attempts, first_attempt_at, replay, match_id FROM archive_games
                 WHERE state = 'pending' AND next_attempt_at <= ?1
                 ORDER BY priority, next_attempt_at, received_at LIMIT ?2",
            )?
            .query_map(params![now, BATCH], |row| {
                Ok(Leased {
                    attempt: row.get::<_, u32>(1)? + 1,
                    first_attempt_at: row.get(2)?,
                    item: Due {
                        crc: row.get(0)?,
                        replay: row.get(3)?,
                        set: row.get(4)?,
                    },
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE archive_games SET next_attempt_at = ?1, attempts = ?2 WHERE crc = ?3",
                params![until, row.attempt, row.item.crc],
            )?;
        }
        Ok(rows)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let (Some(archive), Some(secret)) = (
            &state.config.archive,
            &state.integrations.archive_secret_key,
        ) else {
            return retry("no archive secret key".into());
        };
        let Ok(client) = crate::util::outbound_client().build() else {
            return retry("client".into());
        };
        let put = |key: String, body: Vec<u8>| {
            let headers = s3::signed_put(archive, secret, &key, &body, state.now());
            let mut request = client
                .put(format!(
                    "{}/{}/{key}",
                    archive.endpoint.trim_end_matches('/'),
                    archive.bucket
                ))
                .header("content-type", "application/octet-stream");
            for (name, value) in headers.into_iter().filter(|(name, _)| *name != "host") {
                request = request.header(name, value);
            }
            async move {
                match request.body(body).send().await {
                    Ok(response) if response.status().is_success() => None,
                    Ok(response) => Some(format!("{key}: status {}", response.status().as_u16())),
                    Err(_) => Some(format!("{key}: unreachable")),
                }
            }
        };
        if let Some(error) = put(
            format!("games/{}.emberreplay", item.crc),
            item.replay.clone(),
        )
        .await
        {
            return retry(error);
        }
        if let Some(id) = &item.set {
            let found = id.clone();
            let pack = state
                .db
                .read(move |tx| match load(tx, &found)? {
                    Some(found) => packed(tx, &found).map(Some),
                    None => Ok(None),
                })
                .await;
            match pack {
                Ok(Some(pack)) => {
                    if let Some(error) = put(
                        format!("sets/{id}.emberpack"),
                        pack,
                    )
                    .await
                    {
                        return retry(error);
                    }
                }
                Ok(None) => {}
                Err(_) => return retry("pack".into()),
            }
        }
        Outcome::Delivered
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (stored, next, error) = match &record.settled {
            Settled::Delivered => (true, record.at, None),
            Settled::Retry { at, error } => (false, *at, Some(error)),
            Settled::Refused(error) | Settled::Expired(error) => {
                (false, record.at + 3600, Some(error))
            }
        };
        if let Some(error) = error {
            eprintln!("archive: {} not stored yet: {error}", leased.item.crc);
        }
        tx.execute(
            "UPDATE archive_games SET
                state = CASE WHEN ?1 THEN 'stored' ELSE state END,
                replay = CASE WHEN ?1 THEN NULL ELSE replay END,
                stored_at = CASE WHEN ?1 THEN ?2 ELSE stored_at END,
                next_attempt_at = ?3, last_error = ?4,
                first_attempt_at = CASE WHEN ?1 THEN first_attempt_at ELSE COALESCE(first_attempt_at, ?5) END
             WHERE crc = ?6 AND state = 'pending' AND attempts = ?7",
            params![
                stored,
                record.at,
                next,
                error,
                record.first_attempt_at,
                leased.item.crc,
                leased.attempt
            ],
        )?;
        Ok(())
    }
}

fn retry(error: String) -> Outcome {
    Outcome::Retry {
        error,
        retry_after: None,
    }
}
