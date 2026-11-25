//! Reads the games the archive kept before migration 067 back from the
//! bucket for their round scores (`archive::rounds`): at most
//! `GAMES_PER_PASS` games a pass, `READS_AT_ONCE` at a time, a pass every
//! `PASS_SECS`, and what they say is recorded in one short write once the
//! pass's reads are over. The production archive held about 260,000 games
//! when this came, about 9 KB each: about three small reads a second take
//! them in about a day, and once none is left a pass is one indexed query.
//!
//! Not a queue: a game is read once. One the bucket no longer holds, or holds
//! something else under its name, is read as having no rounds; one the
//! bucket did not answer for (unreachable, refused) is left as it was, and a
//! later pass reads it.
use std::{sync::Arc, time::Duration};

use ember_protocol::replays::{MAX_REPLAY, Replay};
use futures_util::StreamExt;

use super::archive::key;
use crate::{AppState, archive::rounds, s3::Bucket};

const GAMES_PER_PASS: i64 = 32;
const READS_AT_ONCE: usize = 4;
const PASS_SECS: u64 = 10;

/// Runs a pass every `PASS_SECS`, forever.
pub async fn run(state: AppState, bucket: Arc<Bucket>) {
    loop {
        pass(&state, &bucket).await;
        tokio::time::sleep(Duration::from_secs(PASS_SECS)).await;
    }
}

/// One pass: reads the newest `GAMES_PER_PASS` stored games whose rounds
/// were never read back from `bucket`, then records what each says.
pub async fn pass(state: &AppState, bucket: &Bucket) {
    let Some(secret) = state.integrations.archive_secret_key.as_deref() else {
        return;
    };
    let unread = match state.db.read(|tx| rounds::unread(tx, GAMES_PER_PASS)).await {
        Ok(unread) => unread,
        Err(error) => {
            eprintln!("archive: could not list the games whose rounds are unread: {error:?}");
            return;
        }
    };
    let read: Vec<(String, Option<[u8; 2]>)> = futures_util::stream::iter(unread)
        .map(|digest| async move {
            match rounds_of(state, bucket, secret, &digest).await {
                Ok(rounds) => Some((digest, rounds)),
                Err(error) => {
                    eprintln!("archive: the rounds of {digest} are not read yet: {error}");
                    None
                }
            }
        })
        .buffer_unordered(READS_AT_ONCE)
        .filter_map(std::future::ready)
        .collect()
        .await;
    if read.is_empty() {
        return;
    }
    let now = state.now();
    let recorded = state
        .db
        .write(move |tx| {
            for (digest, rounds) in &read {
                rounds::read(tx, digest, *rounds, now)?;
            }
            Ok(())
        })
        .await;
    if let Err(error) = recorded {
        eprintln!("archive: could not record the rounds read: {error:?}");
    }
}

/// The rounds game `digest`'s file in `bucket` says each side won: none when
/// the bucket no longer holds the replay the digest names or the replay does
/// not say, which no later read changes; an error when the bucket did not
/// answer for it.
async fn rounds_of(
    state: &AppState,
    bucket: &Bucket,
    secret: &str,
    digest: &str,
) -> Result<Option<[u8; 2]>, String> {
    let file = bucket
        .get(secret, &key(digest), MAX_REPLAY, state.now())
        .await?;
    Ok(file
        .as_deref()
        .and_then(Replay::parse)
        .filter(|replay| replay.digest_hex() == digest)
        .and_then(|replay| replay.rounds))
}
