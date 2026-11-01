//! Stores each archived set's pack in the archive's bucket as
//! `sets/<match id>.emberpack` (`crate::archive::sets`). A set's
//! `archive_sets` version counts the games it gained in the archive; a lease
//! builds the pack from the set's games as they stand at that version, and
//! a stored pack records the version it was built at. A set that gained a
//! game meanwhile stays due, and is not leased again while a lease of it
//! runs, so an older pack never replaces a newer one. A set with no
//! replays left is never stored as an empty pack.
use std::sync::Arc;

use rusqlite::{Transaction, params};

use super::{BATCH, Leased, Outcome, Queue, Record, archive::settled, archive::store};
use crate::{
    AppState,
    error::Result,
    routes::{ledger::load, replays::packed},
    s3::Bucket,
};

pub struct Due {
    match_id: String,
    version: i64,
    /// None when the set holds no replay.
    pack: Option<Vec<u8>>,
}

/// Sets whose pack the bucket does not hold at their latest version.
pub struct Packs(pub Arc<Bucket>);

/// Records that set `match_id` gained a game in the archive, in the same
/// transaction. A set whose pack is stored is due now; one already due or
/// leased keeps its time, so its lease settles before it is leased again.
pub fn changed(tx: &Transaction<'_>, match_id: &str, now: u64) -> Result<()> {
    tx.execute(
        "INSERT INTO archive_sets (match_id, version, next_attempt_at) VALUES (?1, 1, ?2)
         ON CONFLICT (match_id) DO UPDATE SET version = version + 1,
            next_attempt_at = CASE WHEN uploaded_version = version THEN excluded.next_attempt_at
                                   ELSE next_attempt_at END",
        params![match_id, now],
    )?;
    crate::db::queued();
    Ok(())
}

impl Queue for Packs {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let sets = tx
            .prepare(
                "SELECT match_id, version, attempts, first_attempt_at FROM archive_sets
                 WHERE uploaded_version < version AND next_attempt_at <= ?1
                 ORDER BY next_attempt_at LIMIT ?2",
            )?
            .query_map(params![now, BATCH], |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, u32>(2)? + 1,
                    row.get::<_, Option<u64>>(3)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        let mut leased = Vec::new();
        for (match_id, version, attempt, first_attempt_at) in sets {
            tx.execute(
                "UPDATE archive_sets SET next_attempt_at = ?1, attempts = ?2 WHERE match_id = ?3",
                params![until, attempt, match_id],
            )?;
            let held: bool = tx.query_row(
                "SELECT EXISTS (SELECT 1 FROM match_replays WHERE match_id = ?1)",
                [&match_id],
                |row| row.get(0),
            )?;
            let pack = match load(tx, &match_id)? {
                Some(found) if held => Some(packed(tx, &found)?),
                _ => None,
            };
            leased.push(Leased {
                item: Due {
                    match_id,
                    version,
                    pack,
                },
                attempt,
                first_attempt_at,
            });
        }
        Ok(leased)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let Some(pack) = &item.pack else {
            return Outcome::Refused("the set holds no replays".into());
        };
        let key = format!("sets/{}.emberpack", item.match_id);
        store(state, &self.0, &key, pack.clone()).await
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (stored, next, error) = settled(record);
        if let Some(error) = error {
            eprintln!(
                "archive: the pack of {} not stored yet: {error}",
                leased.item.match_id
            );
        }
        // A stored pack starts the next version's retries afresh.
        tx.execute(
            "UPDATE archive_sets SET
                uploaded_version = CASE WHEN ?1 THEN MAX(uploaded_version, ?2) ELSE uploaded_version END,
                attempts = CASE WHEN ?1 THEN 0 ELSE attempts END,
                first_attempt_at = CASE WHEN ?1 THEN NULL ELSE COALESCE(first_attempt_at, ?3) END,
                next_attempt_at = ?4, last_error = ?5
             WHERE match_id = ?6 AND attempts = ?7",
            params![
                stored,
                leased.item.version,
                record.first_attempt_at,
                next,
                error,
                leased.item.match_id,
                leased.attempt
            ],
        )?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Db, delivery::Settled};

    fn record(settled: Settled) -> Record {
        Record {
            at: 100,
            first_attempt_at: 90,
            settled,
        }
    }

    async fn state_of(db: &Db) -> (i64, i64, u64, i64) {
        db.read(|tx| {
            Ok(tx.query_row(
                "SELECT version, uploaded_version, next_attempt_at, attempts FROM archive_sets WHERE match_id = 'emt_1'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )?)
        })
        .await
        .unwrap()
    }

    /// A game the set gains while its pack is being stored keeps the set
    /// due, but not leasable before that lease settles; the newer pack is
    /// leased only after it.
    #[tokio::test]
    async fn a_set_that_changed_while_its_pack_was_stored_is_stored_again() {
        let db = Db::open(std::path::Path::new(":memory:")).unwrap();
        let bucket = Bucket::new(&crate::config::Archive {
            endpoint: "http://127.0.0.1:9".into(),
            bucket: "replays".into(),
            region: "us-east-1".into(),
            access_key: "key".into(),
            public_base: "https://s3.example/replays".into(),
        })
        .unwrap();
        let packs = Arc::new(Packs(Arc::new(bucket)));
        db.write(|tx| {
            tx.execute_batch(
                "INSERT INTO tenants (id, name) VALUES ('t', 'Tenant');
                 INSERT INTO provider_connections (id, tenant_id, kind, environment, display_name, enabled)
                   VALUES ('c', 't', 'direct', 'staging', 'Site', 1);
                 INSERT INTO matches (id, tenant_id, connection_id, external_match_id, create_digest, revision,
                   assignment_generation, state, games_to_win, rules, required_build_id, metadata,
                   delivery_state, created_at, updated_at)
                   VALUES ('emt_1', 't', 'c', 'one', '', 1, 1, 'ready', 2, '{}', '', '{}', 'not_required', 1, 1);",
            )?;
            changed(tx, "emt_1", 10)
        })
        .await
        .unwrap();
        let leasing = packs.clone();
        let first = db
            .write(move |tx| leasing.lease(tx, 10, 70))
            .await
            .unwrap()
            .pop()
            .unwrap();
        assert_eq!(first.item.version, 1);
        // The set holds no replays: its pack is never stored empty.
        assert!(first.item.pack.is_none());
        db.write(|tx| changed(tx, "emt_1", 20)).await.unwrap();
        assert_eq!(state_of(&db).await, (2, 0, 70, 1));
        let leasing = packs.clone();
        let again = db.write(move |tx| leasing.lease(tx, 20, 80)).await.unwrap();
        assert!(again.is_empty(), "leased while the first lease runs");
        // The first lease stores version 1; the set stays due for version 2.
        let recording = packs.clone();
        db.write(move |tx| recording.record(tx, &first, &record(Settled::Delivered)))
            .await
            .unwrap();
        assert_eq!(state_of(&db).await, (2, 1, 100, 0));
        let leasing = packs.clone();
        let second = db
            .write(move |tx| leasing.lease(tx, 100, 160))
            .await
            .unwrap()
            .pop()
            .unwrap();
        assert_eq!(second.item.version, 2);
        // A late outcome of a lease that was replaced changes nothing.
        let stale = Leased {
            item: Due {
                match_id: "emt_1".into(),
                version: 1,
                pack: None,
            },
            attempt: 0,
            first_attempt_at: None,
        };
        let recording = packs.clone();
        db.write(move |tx| recording.record(tx, &stale, &record(Settled::Delivered)))
            .await
            .unwrap();
        assert_eq!(state_of(&db).await, (2, 1, 160, 1));
        let recording = packs.clone();
        db.write(move |tx| recording.record(tx, &second, &record(Settled::Delivered)))
            .await
            .unwrap();
        assert_eq!(state_of(&db).await, (2, 2, 100, 0));
        let leasing = packs.clone();
        assert!(
            db.write(move |tx| leasing.lease(tx, 1000, 1060))
                .await
                .unwrap()
                .is_empty()
        );
        // A set whose pack is stored is due again as soon as it changes.
        db.write(|tx| changed(tx, "emt_1", 2000)).await.unwrap();
        assert_eq!(state_of(&db).await, (3, 2, 2000, 0));
    }
}
