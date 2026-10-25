//! The replay channel for the native side (docs/design/REPLAY_CHANNEL.md).
//! The game names files, never sends their bytes: an upload reads an
//! `.emberreplay` the game archived, and a download writes one into a folder
//! the game names, so nothing near the size of a replay crosses the IPC.
//! Like the ranking routes these use the player session, except the file
//! route, which anyone may read. A bridge without the routes is told apart as
//! `replay_channel_unavailable`.
use std::{
    path::{Path, PathBuf},
    sync::Arc,
};

use ember_protocol::{
    replays::{
        ListQuery, MAX_EXPORT, NOT_YOUR_GAME, REPLAY_INVALID, REPLAY_NOT_FOUND,
        REPLAYS_PATH, ReplayPage, ReplaySharing, ReplayUpload, SHARING_PATH, SharingChoice,
        UPLOAD_LIMIT, UploadAnswer, file_path, is_replay_id, read_export,
    },
    skill::NO_ACCOUNT,
};
use serde::de::DeserializeOwned;

use super::{Failure, Outcome, Shared, check_path, client};

/// Refusals of the replay routes that the game words apart.
const REASONS: [&str; 5] = [
    NO_ACCOUNT,
    REPLAY_INVALID,
    NOT_YOUR_GAME,
    UPLOAD_LIMIT,
    REPLAY_NOT_FOUND,
];
const UNAVAILABLE: &str = "replay_channel_unavailable";

/// What the game says of a game it archived, beside the file.
pub struct Upload {
    pub file: String,
    pub room_id: String,
    pub table: u8,
    pub generation: u64,
    pub side: u8,
    pub name: String,
    pub country: Option<String>,
}

/// Uploads one archived replay and answers what became of it.
pub async fn upload(shared: &Arc<Shared>, bridge_id: &str, upload: Upload) -> Outcome {
    let path = check_path(&upload.file)?.to_owned();
    let export = tokio::task::spawn_blocking(move || read_replay(&path))
        .await
        .map_err(|_| Failure::new("internal"))??;
    let request = ReplayUpload::new(
        upload.room_id,
        upload.table,
        upload.generation,
        upload.side,
        upload.name,
        upload.country,
        &export,
    );
    request
        .check()
        .map_err(|_| Failure::new("invalid_request"))?;
    let body = serde_json::to_vec(&request).map_err(|_| Failure::new("internal"))?;
    let answer: UploadAnswer = call(
        shared,
        bridge_id,
        reqwest::Method::POST,
        REPLAYS_PATH.into(),
        Some(body),
    )
    .await?;
    answer.check().map_err(|_| invalid_response())?;
    to_json(&answer)
}

/// The player's choice to share, or with `choice` stores it first.
pub async fn sharing(
    shared: &Arc<Shared>,
    bridge_id: &str,
    choice: Option<SharingChoice>,
) -> Outcome {
    let (method, body) = match choice {
        Some(choice) => (
            reqwest::Method::PUT,
            Some(serde_json::to_vec(&choice).map_err(|_| Failure::new("internal"))?),
        ),
        None => (reqwest::Method::GET, None),
    };
    let sharing: ReplaySharing = call(shared, bridge_id, method, SHARING_PATH.into(), body).await?;
    to_json(&sharing)
}

/// A page of the channel.
pub async fn list(shared: &Arc<Shared>, bridge_id: &str, query: ListQuery) -> Outcome {
    query
        .check()
        .map_err(|_| Failure::new("invalid_request"))?;
    let path = format!("{REPLAYS_PATH}{}", query_string(&query));
    let page: ReplayPage = call(shared, bridge_id, reqwest::Method::GET, path, None).await?;
    page.check().map_err(|_| invalid_response())?;
    to_json(&page)
}

/// Downloads entry `id` into `folder` as `<id>.emberreplay` and answers the
/// file's path. A file already there with the same bytes is kept.
pub async fn download(shared: &Arc<Shared>, bridge_id: &str, id: &str, folder: &str) -> Outcome {
    if !is_replay_id(id) {
        return Err(Failure::new("invalid_request"));
    }
    let folder = check_path(folder)?.to_owned();
    let bridge = client::approved(shared, bridge_id)?;
    let url = format!("{}{}", bridge.origin, file_path(id));
    let (status, bytes) = shared
        .client
        .call(reqwest::Method::GET, &url, None, None)
        .await?;
    if status != 200 {
        return Err(match client::feature_answer(status, &bytes, 200, &REASONS, UNAVAILABLE) {
            Err(failure) => failure,
            Ok(_) => invalid_response(),
        });
    }
    if bytes.len() > MAX_EXPORT || read_export(&bytes).is_err() {
        return Err(invalid_response());
    }
    let target = folder.join(format!("{id}.emberreplay"));
    let written = target.clone();
    tokio::task::spawn_blocking(move || write_once(&written, &bytes))
        .await
        .map_err(|_| Failure::new("internal"))??;
    Ok(Some(serde_json::json!({ "file": target.to_string_lossy() })))
}

fn read_replay(path: &Path) -> Result<Vec<u8>, Failure> {
    if path.extension().is_none_or(|extension| extension != "emberreplay") {
        return Err(Failure::new("invalid_path"));
    }
    let metadata = std::fs::metadata(path).map_err(|_| Failure::new("file_unreadable"))?;
    if !metadata.is_file() || metadata.len() > MAX_EXPORT as u64 {
        return Err(Failure::new("replay_unreadable"));
    }
    let bytes = std::fs::read(path).map_err(|_| Failure::new("file_unreadable"))?;
    read_export(&bytes).map_err(|_| Failure::new("replay_unreadable"))?;
    Ok(bytes)
}

/// Writes `bytes` at `path` unless a file is there: through a part file
/// linked into place, so a reader never sees half a replay and an existing
/// file is never replaced. One there with the same bytes counts as written.
fn write_once(path: &Path, bytes: &[u8]) -> Result<(), Failure> {
    let failed = || Failure::new("file_unwritable");
    if let Ok(existing) = std::fs::read(path) {
        return if existing == bytes { Ok(()) } else { Err(failed()) };
    }
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|_| failed())?;
    }
    let mut part = PathBuf::from(path);
    part.set_extension("part");
    std::fs::write(&part, bytes).map_err(|_| failed())?;
    let linked = std::fs::hard_link(&part, path);
    let _ = std::fs::remove_file(&part);
    match linked {
        Ok(()) => Ok(()),
        Err(_) if std::fs::read(path).is_ok_and(|existing| existing == bytes) => Ok(()),
        Err(_) => Err(failed()),
    }
}

/// `query` as a URL query, empty when it has no filter.
fn query_string(query: &ListQuery) -> String {
    let mut pairs = Vec::new();
    if let Some(fighter) = query.fighter {
        pairs.push(format!("fighter={fighter}"));
    }
    if let Some(band) = query.band {
        pairs.push(format!("band={band}"));
    }
    if let Some(name) = &query.name {
        pairs.push(format!("name={}", percent_encode(name)));
    }
    if let Some(before) = &query.before {
        pairs.push(format!("before={before}"));
    }
    if pairs.is_empty() {
        String::new()
    } else {
        format!("?{}", pairs.join("&"))
    }
}

fn percent_encode(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for byte in text.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'~') {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}

/// One replay route under the player session, read as `T`.
async fn call<T: DeserializeOwned>(
    shared: &Arc<Shared>,
    bridge_id: &str,
    method: reqwest::Method,
    path: String,
    body: Option<Vec<u8>>,
) -> Result<T, Failure> {
    let answered = client::with_session(shared, bridge_id, |bridge, token| {
        let (method, url, body) = (
            method.clone(),
            format!("{}{path}", bridge.origin),
            body.clone(),
        );
        async move {
            let (status, response) = shared.client.call(method, &url, Some(&token), body).await?;
            client::feature_answer(status, &response, 200, &REASONS, UNAVAILABLE).map(Some)
        }
    })
    .await?;
    serde_json::from_value(answered.ok_or_else(invalid_response)?).map_err(|_| invalid_response())
}

fn invalid_response() -> Failure {
    Failure::new("bridge_invalid_response")
}

fn to_json<T: serde::Serialize>(value: &T) -> Outcome {
    serde_json::to_value(value)
        .map(Some)
        .map_err(|_| Failure::new("internal"))
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;
    use crate::tournament::Request;

    #[test]
    fn requests_have_the_shapes_the_game_sends() {
        let upload = json!({ "op": "replay_upload", "bridge_id": "brg_x",
            "file": r"C:\replays\a.emberreplay", "room_id": "ab".repeat(16), "table": 1,
            "generation": 4, "side": 0, "name": "Player 1", "country": "GB" });
        let sharing = json!({ "op": "replay_sharing", "bridge_id": "brg_x" });
        let share = json!({ "op": "replay_share", "bridge_id": "brg_x", "choice": { "sharing": false } });
        let list = json!({ "op": "replay_list", "bridge_id": "brg_x", "query": { "fighter": 23 } });
        let download = json!({ "op": "replay_download", "bridge_id": "brg_x",
            "id": "cd".repeat(16), "folder": r"C:\replays" });
        for (value, op) in [
            (upload, "replay_upload"),
            (sharing, "replay_sharing"),
            (share, "replay_share"),
            (list, "replay_list"),
            (download, "replay_download"),
        ] {
            let parsed: Request = serde_json::from_value(value.clone()).expect("a request");
            assert_eq!(parsed.op(), op);
            assert_eq!(serde_json::to_value(&parsed).unwrap(), value);
        }
        for bad in [
            json!({ "op": "replay_sharing" }),
            json!({ "op": "replay_share", "bridge_id": "brg_x", "choice": { "sharing": "no" } }),
            json!({ "op": "replay_list", "bridge_id": "brg_x", "query": { "fighter": 1, "x": 1 } }),
        ] {
            assert!(serde_json::from_value::<Request>(bad.clone()).is_err(), "{bad}");
        }
    }

    #[test]
    fn a_bridge_without_the_channel_is_told_apart() {
        let refusal = |reason: &str| {
            serde_json::to_vec(&json!({ "error": {
                "code": "forbidden", "message": "m", "retryable": false, "request_id": "req_1",
                "details": { "reason": reason },
            }}))
            .unwrap()
        };
        let reply = |status, body: &[u8]| client::feature_answer(status, body, 200, &REASONS, UNAVAILABLE);
        assert_eq!(reply(404, b"").unwrap_err().0, UNAVAILABLE);
        for reason in REASONS {
            assert_eq!(reply(403, &refusal(reason)).unwrap_err().0, reason);
        }
        assert_eq!(reply(404, &refusal(REPLAY_NOT_FOUND)).unwrap_err().0, REPLAY_NOT_FOUND);
    }

    #[test]
    fn a_query_names_only_its_filters() {
        assert_eq!(query_string(&ListQuery::default()), "");
        let query = ListQuery {
            fighter: Some(23),
            band: Some(9),
            name: Some("Player 1&x".into()),
            before: Some("ab".repeat(16)),
        };
        assert_eq!(
            query_string(&query),
            format!("?fighter=23&band=9&name=Player%201%26x&before={}", "ab".repeat(16))
        );
    }

    #[test]
    fn a_download_never_replaces_another_file() {
        let dir = std::env::temp_dir().join(format!("sf4e-replays-{}", std::process::id()));
        let path = dir.join("one.emberreplay");
        let _ = std::fs::remove_dir_all(&dir);
        assert!(write_once(&path, b"first").is_ok());
        assert!(write_once(&path, b"first").is_ok());
        assert!(write_once(&path, b"other").is_err());
        assert_eq!(std::fs::read(&path).unwrap(), b"first");
        assert!(!dir.join("one.part").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn only_an_export_is_uploaded() {
        let dir = std::env::temp_dir().join(format!("sf4e-replay-read-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let wrong_kind = dir.join("notes.txt");
        std::fs::write(&wrong_kind, b"EMBRPLY1").unwrap();
        assert_eq!(read_replay(&wrong_kind).unwrap_err().0, "invalid_path");
        let not_a_replay = dir.join("x.emberreplay");
        std::fs::write(&not_a_replay, b"EMBRPLY1").unwrap();
        assert_eq!(read_replay(&not_a_replay).unwrap_err().0, "replay_unreadable");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
