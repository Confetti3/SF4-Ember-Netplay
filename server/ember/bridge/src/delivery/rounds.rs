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
//! bucket did not answer for (unreachable, refused, too long) is left as it
//! was. The passes sweep the unread games newest first, each going on below
//! the last game the one before took, back to the newest once a pass finds
//! fewer than it takes: a game that keeps failing is tried again once a
//! sweep, and never holds up the games after it.
use std::{sync::Arc, time::Duration};

use futures_util::StreamExt;

use super::archive::read_back;
use crate::{
    AppState,
    archive::rounds::{self, Unread},
    s3::Bucket,
};

pub(crate) const GAMES_PER_PASS: i64 = 32;
const READS_AT_ONCE: usize = 4;
const PASS_SECS: u64 = 10;

/// Runs a pass every `PASS_SECS`, forever.
pub async fn run(state: AppState, bucket: Arc<Bucket>) {
    let mut below = None;
    loop {
        below = pass(&state, &bucket, below.as_ref()).await;
        tokio::time::sleep(Duration::from_secs(PASS_SECS)).await;
    }
}

/// One sweep from the newest unread game: passes until one finds fewer
/// games than it takes. Tests call it.
pub async fn sweep(state: &AppState, bucket: &Bucket) {
    let mut below = pass(state, bucket, None).await;
    while below.is_some() {
        below = pass(state, bucket, below.as_ref()).await;
    }
}

/// One pass: reads the next `GAMES_PER_PASS` unread games below `below`
/// (from the newest when none) back from `bucket`, then records what each
/// says. Where the next pass goes on from: below the last game this one
/// took, or the newest again when it took fewer than it may.
pub async fn pass(state: &AppState, bucket: &Bucket, below: Option<&Unread>) -> Option<Unread> {
    let secret = state.integrations.archive_secret_key.as_deref()?;
    let below = below.cloned();
    let unread = state
        .db
        .read(move |tx| rounds::unread(tx, below.as_ref(), GAMES_PER_PASS))
        .await
        .map_err(|error| {
            eprintln!("archive: could not list the games whose rounds are unread: {error:?}")
        })
        .ok()?;
    let next = (unread.len() as i64 == GAMES_PER_PASS)
        .then(|| unread.last().cloned())
        .flatten();
    let attempts: Vec<_> = futures_util::stream::iter(unread)
        .map(|game| async move {
            let read = read_back(state, bucket, secret, &game.digest).await;
            (
                game.digest,
                read.map(|replay| replay.and_then(|replay| replay.rounds)),
            )
        })
        .buffer_unordered(READS_AT_ONCE)
        .collect()
        .await;
    let mut read = Vec::new();
    let mut failed = Vec::new();
    for (digest, rounds) in attempts {
        match rounds {
            Ok(rounds) => read.push((digest, rounds)),
            Err(error) => failed.push(error),
        }
    }
    // One line a pass, however many failed.
    if let Some(error) = failed.first() {
        eprintln!(
            "archive: the rounds of {} games are not read yet; the first: {error}",
            failed.len()
        );
    }
    if !read.is_empty() {
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
    next
}
