//! The replay channel routes (docs/design/REPLAY_CHANNEL.md): a player's
//! upload of a room game, their choice to share, and the list and files for
//! players and (without a credential) the website. Only where public rooms
//! are on, since only their hosts can tell who played.
use std::{
    collections::HashSet,
    io::Write,
    path::{Path as FsPath, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, SystemTime},
};

use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Response},
};
use ember_protocol::replays::{
    ListQuery, MAX_UPLOAD_BODY, REPLAY_INVALID, REPLAY_NOT_FOUND, ReplayPage, ReplayUpload,
    SharingChoice, UPLOAD_LIMIT, UploadAnswer, archive_name, is_replay_id,
};
use sha2::{Digest, Sha256};

use crate::{
    AppState,
    auth::Player,
    db::Db,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::{
        play::playing,
        rooms::{self, classes_shown, developers},
    },
    skill::{
        self,
        channel::{self, MAX_BYTES, MAX_ROWS, Show},
    },
    util::{create_private, hex, random},
};

/// Requests one player may make per window to the routes other than the
/// upload, all together.
const REQUESTS_PER_WINDOW: usize = 60;
const WINDOW_SECS: u64 = 10 * 60;
/// Uploads one Discord account may make a day.
const UPLOADS_PER_DAY: usize = 300;
const DAY_SECS: u64 = 24 * 60 * 60;
/// How long a public page is answered again before it is read again.
const PUBLIC_PAGE_SECS: u64 = 15;
/// How often the replay folder is checked for files no replay names, and how
/// old such a file must be: a file is written just before its row.
const SWEEP_SECS: u64 = 60 * 60;
const ORPHAN_AGE: Duration = Duration::from_secs(60 * 60);

/// Per-process state of the channel: the last public page, when the replay
/// folder was last checked, and the lock that owns the files (below).
#[derive(Default)]
pub struct Shared {
    page: tokio::sync::Mutex<Option<(u64, Arc<ReplayPage>)>>,
    swept_at: AtomicU64,
    /// Whoever holds it as a reader is putting a file in the folder and
    /// recording the replay that names it (an upload, from before the file is
    /// stored until its row is committed); the sweep holds it as the writer
    /// while it reads which files are named and removes the others. So the
    /// sweep never judges a file by a view that an upload has since made
    /// stale, and never removes a file an upload has just stored or touched.
    files: tokio::sync::RwLock<()>,
}

/// `POST /v1/replays`.
pub async fn upload(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<MAX_UPLOAD_BODY>,
) -> Result<Response> {
    if !rooms::enabled(&state) {
        return Err(ApiFailure::not_found());
    }
    let player = playing(&state, &headers).await?;
    let request: ReplayUpload = body.parse()?;
    let ember_id = player.ember_id.clone();
    let Some(account) = state
        .db
        .read(move |tx| skill::account(tx, &ember_id))
        .await?
    else {
        // Without an account there is nowhere to keep a choice to share.
        return Ok(ok(&UploadAnswer::NOT_SHARED));
    };
    let now = state.now();
    state
        .rate
        .check(
            &format!("replays:{account}"),
            UPLOADS_PER_DAY,
            DAY_SECS,
            now,
        )
        .map_err(|retry| ApiFailure::rate_limited(retry).detail("reason", UPLOAD_LIMIT))?;
    let (export, info) = request.check().map_err(invalid)?;
    let size = u32::try_from(export.len()).map_err(|_| ApiFailure::unavailable())?;
    let sha256 = hex(&Sha256::digest(&export));
    let (folder, name) = (folder(&state), sha256.clone());
    // Held until the row is committed: the sweep waits for it (`Shared::files`).
    let _storing = state.replays.files.read().await;
    tokio::task::spawn_blocking(move || store(&folder, &name, &export))
        .await
        .map_err(|_| ApiFailure::unavailable())?
        .map_err(|error| {
            eprintln!("replays: could not store a file: {error}");
            ApiFailure::unavailable()
        })?;
    let (classes_shown, developers) = (classes_shown(&state), developers(&state));
    let answer = state
        .db
        .write(move |tx| {
            let show = Show {
                classes_shown,
                developers: &developers,
            };
            let upload = channel::Upload {
                ember_id: &player.ember_id,
                request: &request,
                info,
                sha256: &sha256,
                size,
            };
            channel::upload(tx, show, &upload, now)
        })
        .await?;
    Ok(ok(&answer))
}

/// A refusal of the upload's own fields; a replay the game would not import
/// says so by reason.
fn invalid(error: ember_protocol::Error) -> ApiFailure {
    use ember_protocol::Error;
    let replay = matches!(
        error,
        Error::InvalidField("replay") | Error::InvalidEncoding("replay")
    );
    let failure = ApiFailure::from(error);
    if replay {
        failure.detail("reason", REPLAY_INVALID)
    } else {
        failure
    }
}

/// `GET /v1/replays/me`.
pub async fn my_sharing(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let (player, _) = admit(&state, &headers).await?;
    let sharing = state
        .db
        .read(move |tx| channel::sharing(tx, &player.ember_id))
        .await?;
    Ok(ok(&sharing))
}

/// `PUT /v1/replays/me`, answered with the choice after it. A page already
/// made for the website is forgotten, so a player who stops sharing is off it
/// at once.
pub async fn choose_sharing(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let (player, now) = admit(&state, &headers).await?;
    let chosen: SharingChoice = body.parse()?;
    let sharing = state
        .db
        .write(move |tx| channel::choose(tx, &player.ember_id, chosen.sharing, now))
        .await?;
    state.replays.page.lock().await.take();
    Ok(ok(&sharing))
}

/// `GET /v1/replays`: a page of the channel for a player, with their own
/// sides marked.
pub async fn list(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(query): Query<ListQuery>,
) -> Result<Response> {
    let (player, _) = admit(&state, &headers).await?;
    query.check()?;
    let page = state
        .db
        .read(move |tx| channel::list(tx, Some(&player.ember_id), &query))
        .await?;
    Ok(ok(&page))
}

/// `GET /v1/replays/public`: the same page for the website, with no
/// credential. Anyone can ask for it, directly or through the website, so the
/// newest page, which most ask for, is read at most every 15 s, one read at a
/// time.
pub async fn public_list(
    State(state): State<AppState>,
    Query(query): Query<ListQuery>,
) -> Result<Response> {
    if !rooms::enabled(&state) {
        return Err(ApiFailure::not_found());
    }
    query.check()?;
    let now = state.now();
    if query != ListQuery::default() {
        let page = state
            .db
            .read(move |tx| channel::list(tx, None, &query))
            .await?;
        return Ok(ok(&page));
    }
    let mut cached = state.replays.page.lock().await;
    if let Some((listed_at, page)) = cached.as_ref()
        && now.saturating_sub(*listed_at) < PUBLIC_PAGE_SECS
    {
        return Ok(ok(&**page));
    }
    let page = Arc::new(
        state
            .db
            .read(move |tx| channel::list(tx, None, &query))
            .await?,
    );
    *cached = Some((now, Arc::clone(&page)));
    Ok(ok(&*page))
}

/// `GET /v1/replays/{id}/file`: the `.emberreplay` of a listed replay, with
/// no credential.
pub async fn file(State(state): State<AppState>, Path(id): Path<String>) -> Result<Response> {
    if !rooms::enabled(&state) {
        return Err(ApiFailure::not_found());
    }
    let missing = || ApiFailure::not_found().detail("reason", REPLAY_NOT_FOUND);
    if !is_replay_id(&id) {
        return Err(missing());
    }
    let listed = id.clone();
    let sha256 = state
        .db
        .read(move |tx| channel::file_of(tx, &listed))
        .await?
        .ok_or_else(missing)?;
    let path = folder(&state).join(format!("{sha256}.emberreplay"));
    let bytes = tokio::task::spawn_blocking(move || std::fs::read(path))
        .await
        .map_err(|_| ApiFailure::unavailable())?
        .map_err(|error| {
            eprintln!("replays: a listed file could not be read: {error}");
            missing()
        })?;
    // Named as Ember's archive names it, so a download put in the replay
    // folder shows in the game's Replays list.
    let name = archive_name(&bytes).unwrap_or_else(|| format!("{id}.emberreplay"));
    Ok((
        StatusCode::OK,
        [
            (header::CONTENT_TYPE, "application/octet-stream".to_owned()),
            (
                header::CONTENT_DISPOSITION,
                format!("attachment; filename=\"{name}\""),
            ),
            (header::CACHE_CONTROL, "public, max-age=300".to_owned()),
        ],
        bytes,
    )
        .into_response())
}

/// The calling player, within their request budget, and the time.
async fn admit(state: &AppState, headers: &HeaderMap) -> Result<(Player, u64)> {
    if !rooms::enabled(state) {
        return Err(ApiFailure::not_found());
    }
    let player = playing(state, headers).await?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("replays-me:{}", player.ember_id.as_str()),
            REQUESTS_PER_WINDOW,
            WINDOW_SECS,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    Ok((player, now))
}

/// Where the replay files are: next to the database.
fn folder(state: &AppState) -> PathBuf {
    state
        .config
        .database
        .parent()
        .unwrap_or(FsPath::new(""))
        .join("replays")
}

/// Writes `bytes` as the file named `sha256` unless that file exists. A file
/// is complete before it has its name, and one that exists is never
/// replaced.
fn store(folder: &FsPath, sha256: &str, bytes: &[u8]) -> std::io::Result<()> {
    let target = folder.join(format!("{sha256}.emberreplay"));
    if target.exists() {
        // Still wanted: not a file to sweep.
        if let Ok(file) = std::fs::OpenOptions::new().write(true).open(&target) {
            let _ = file.set_modified(SystemTime::now());
        }
        return Ok(());
    }
    std::fs::create_dir_all(folder)?;
    let temp = folder.join(format!("{}.tmp", hex(&random::<8>())));
    let written = create_private(&temp).and_then(|mut file| {
        file.write_all(bytes)?;
        file.sync_all()
    });
    let linked = written.and_then(|()| match std::fs::hard_link(&temp, &target) {
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => Ok(()),
        other => other,
    });
    let _ = std::fs::remove_file(&temp);
    linked
}

/// Removes files in `folder` that no replay names (`kept`) and temporary
/// files, once they are `min_age` old. Answers how many it removed.
fn sweep(folder: &FsPath, kept: &HashSet<String>, min_age: Duration) -> usize {
    let Ok(entries) = std::fs::read_dir(folder) else {
        return 0;
    };
    let mut removed = 0;
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let stray = match name.strip_suffix(".emberreplay") {
            Some(sha256) => !kept.contains(sha256),
            None => name.ends_with(".tmp"),
        };
        let old = entry
            .metadata()
            .and_then(|meta| meta.modified())
            .is_ok_and(|modified| modified.elapsed().is_ok_and(|age| age >= min_age));
        if stray && old && std::fs::remove_file(entry.path()).is_ok() {
            removed += 1;
        }
    }
    removed
}

/// The channel's part of the maintenance pass: old records go, and now and
/// then so do the files nothing names.
pub async fn maintain(state: &AppState, now: u64) {
    if !rooms::enabled(state) {
        return;
    }
    let pruned = state
        .db
        .write(move |tx| channel::prune(tx, MAX_ROWS, MAX_BYTES, now))
        .await;
    if let Err(error) = pruned {
        eprintln!("maintenance: could not prune the replay channel: {error:?}");
    }
    let swept_at = state.replays.swept_at.load(Ordering::Relaxed);
    if now.saturating_sub(swept_at) < SWEEP_SECS {
        return;
    }
    sweep_files(&state.replays, &state.db, folder(state), now).await;
}

/// Removes the stray files from `folder` while no upload is between storing
/// a file and recording it (`Shared::files`), judging them by the replays
/// recorded once it has the lock.
async fn sweep_files(shared: &Shared, db: &Db, folder: PathBuf, now: u64) {
    let _sweeping = shared.files.write().await;
    shared.swept_at.store(now, Ordering::Relaxed);
    let Ok(kept) = db.read(channel::files_kept).await else {
        return;
    };
    let _ = tokio::task::spawn_blocking(move || sweep(&folder, &kept, ORPHAN_AGE)).await;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_file_is_written_once_and_a_stray_one_is_swept() {
        let folder = std::env::temp_dir().join(format!("ember-replays-{}", hex(&random::<8>())));
        let (kept, stray) = ("a".repeat(64), "b".repeat(64));
        store(&folder, &kept, b"first").unwrap();
        store(&folder, &kept, b"second").unwrap();
        store(&folder, &stray, b"stray").unwrap();
        let read = |sha256: &str| std::fs::read(folder.join(format!("{sha256}.emberreplay")));
        assert_eq!(read(&kept).unwrap(), b"first");
        // Only the two files remain: no temporary file is left behind.
        assert_eq!(std::fs::read_dir(&folder).unwrap().count(), 2);

        let names = HashSet::from([kept.clone()]);
        assert_eq!(sweep(&folder, &names, Duration::from_secs(3600)), 0);
        assert_eq!(sweep(&folder, &names, Duration::ZERO), 1);
        assert!(read(&kept).is_ok() && read(&stray).is_err());
        let _ = std::fs::remove_dir_all(&folder);
    }

    #[tokio::test]
    async fn the_sweep_waits_for_an_upload_to_record_its_file() {
        let folder = std::env::temp_dir().join(format!("ember-replays-{}", hex(&random::<8>())));
        let db = Db::open(FsPath::new(":memory:")).unwrap();
        let shared = Shared::default();
        let sha256 = "c".repeat(64);
        store(&folder, &sha256, b"upload").unwrap();
        let path = folder.join(format!("{sha256}.emberreplay"));
        // Old enough to be swept if no replay names it.
        let old = SystemTime::now() - 2 * ORPHAN_AGE;
        let file = std::fs::OpenOptions::new().write(true).open(&path).unwrap();
        file.set_modified(old).unwrap();
        drop(file);

        // The upload has stored its file and not yet recorded it: the sweep
        // does not go ahead on a view without the row.
        let storing = shared.files.read().await;
        let waiting = tokio::time::timeout(
            Duration::from_millis(200),
            sweep_files(&shared, &db, folder.clone(), 1),
        )
        .await;
        assert!(waiting.is_err());
        assert!(path.exists());
        assert_eq!(shared.swept_at.load(Ordering::Relaxed), 0);

        // The upload records its replay and lets go: the file is named.
        let named = sha256.clone();
        db.write(move |tx| {
            tx.execute(
                "INSERT INTO channel_replays
                     (id, room_id, table_id, generation, room_kind, uploaded0, uploaded1,
                      fighter0, fighter1, played_at, sha256, size, published, created_at)
                 VALUES ('1', 'room', 0, 1, 'public', 0, 0, 1, 2, 5, ?1, 6, 1, 5)",
                [named],
            )?;
            Ok(())
        })
        .await
        .unwrap();
        drop(storing);
        sweep_files(&shared, &db, folder.clone(), 1).await;
        assert!(path.exists());
        assert_eq!(shared.swept_at.load(Ordering::Relaxed), 1);

        // Once no replay names it, the next sweep removes it.
        db.write(|tx| {
            tx.execute("DELETE FROM channel_replays", [])?;
            Ok(())
        })
        .await
        .unwrap();
        sweep_files(&shared, &db, folder.clone(), 2).await;
        assert!(!path.exists());
        let _ = std::fs::remove_dir_all(&folder);
    }
}
