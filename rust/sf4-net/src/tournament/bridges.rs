//! Bridges the user approved (spec 9.1): `bridges.json` holds public
//! profile data and the bridge's public signing keys only, never a token.
//! Discovery documents and deep links cannot add an entry; only an explicit
//! approval from the native UI does. `removed.json` remembers the bridges the
//! player deliberately forgot, so nothing re-trusts one without a fresh
//! approval.
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use ember_protocol::{
    api::{BridgeProfile, SigningKey},
    encoding::{OriginPolicy, b64u, check_origin, is_prefixed_id},
    json,
};
use serde::{Deserialize, Serialize};

use super::Failure;

const FILE: &str = "bridges.json";
const FORMAT: &str = "ember-bridges";
const MAX_BRIDGES: usize = 16;
const MAX_FILE: usize = 64 * 1024;
// A separate file, so `bridges.json` keeps the format older builds parse.
const REMOVED_FILE: &str = "removed.json";
const REMOVED_FORMAT: &str = "ember-bridges-removed";
const MAX_REMOVED: usize = 64;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Approved {
    pub bridge_id: String,
    pub origin: String,
    pub display_name: String,
    pub approved_at: u64,
    /// The bridge's public keys, fetched from its approved origin. They sign
    /// the room bindings and game permits this helper accepts.
    #[serde(default)]
    pub keys: Vec<SigningKey>,
}

impl Approved {
    /// The bridge key `kid` names, if the bridge published it.
    pub fn key(&self, kid: &str) -> Option<ember_protocol::PublicKey> {
        self.keys
            .iter()
            .find(|key| key.kid == kid)
            .and_then(SigningKey::public_key)
    }
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct File {
    format: String,
    version: u8,
    bridges: Vec<Approved>,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RemovedFile {
    format: String,
    version: u8,
    removed: Vec<String>,
}

pub struct Store {
    dir: PathBuf,
    bridges: Vec<Approved>,
    /// Bridge IDs the player removed, oldest first.
    removed: Vec<String>,
}

impl Store {
    /// Loads the list. A missing or unreadable file is an empty list; it is
    /// rewritten only on the next explicit approval.
    pub fn open(dir: PathBuf) -> Self {
        // No per-user folder (the known-folder lookup failed): the store is
        // unavailable rather than relative to the working directory.
        if dir.as_os_str().is_empty() {
            return Self {
                dir,
                bridges: Vec::new(),
                removed: Vec::new(),
            };
        }
        let bridges = fs::read(dir.join(FILE))
            .ok()
            .filter(|bytes| bytes.len() <= MAX_FILE)
            .and_then(|bytes| json::parse_as::<File>(&bytes, MAX_FILE).ok())
            .filter(|file| file.format == FORMAT && file.version == 1)
            .map(|file| file.bridges)
            .unwrap_or_default()
            .into_iter()
            .filter(valid)
            .take(MAX_BRIDGES)
            .collect();
        let removed = fs::read(dir.join(REMOVED_FILE))
            .ok()
            .filter(|bytes| bytes.len() <= MAX_FILE)
            .and_then(|bytes| json::parse_as::<RemovedFile>(&bytes, MAX_FILE).ok())
            .filter(|file| file.format == REMOVED_FORMAT && file.version == 1)
            .map(|file| file.removed)
            .unwrap_or_default()
            .into_iter()
            .filter(|id| is_prefixed_id(id, "brg"))
            .fold(Vec::new(), |mut list, id| {
                remember(&mut list, id);
                list
            });
        Self {
            dir,
            bridges,
            removed,
        }
    }

    /// Bridge IDs the player deliberately removed, oldest first.
    pub fn removed(&self) -> Vec<String> {
        self.removed.clone()
    }

    pub fn list(&self) -> Vec<Approved> {
        self.bridges.clone()
    }

    pub fn get(&self, bridge_id: &str) -> Option<Approved> {
        self.bridges
            .iter()
            .find(|bridge| bridge.bridge_id == bridge_id)
            .cloned()
    }

    /// Approves a profile with the keys fetched from its origin, and returns
    /// the bridge IDs whose entries it replaced, so their sessions can be
    /// dropped.
    pub fn approve(
        &mut self,
        profile: &BridgeProfile,
        keys: Vec<SigningKey>,
    ) -> Result<Vec<String>, Failure> {
        let entry = Approved {
            bridge_id: profile.bridge_id.clone(),
            origin: profile.origin.clone(),
            display_name: profile
                .display_name
                .chars()
                .filter(|c| !c.is_control())
                .take(64)
                .collect(),
            approved_at: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_or(0, |elapsed| elapsed.as_secs()),
            keys: usable(keys),
        };
        if !valid(&entry) {
            return Err(Failure::new("invalid_bridge"));
        }
        // One entry per bridge ID and per origin: a bridge that changed its
        // ID at the same origin is a different bridge and replaces it.
        let (displaced, mut kept): (Vec<_>, Vec<_>) =
            self.bridges.iter().cloned().partition(|bridge| {
                bridge.bridge_id == entry.bridge_id || bridge.origin == entry.origin
            });
        if kept.len() >= MAX_BRIDGES {
            return Err(Failure::new("too_many_bridges"));
        }
        let approved_id = entry.bridge_id.clone();
        kept.push(entry);
        self.replace(kept)?;
        // An explicit approval is the only way back after a removal.
        if self.removed.contains(&approved_id) {
            self.removed.retain(|id| *id != approved_id);
            self.save_removed();
        }
        Ok(displaced
            .into_iter()
            .map(|bridge| bridge.bridge_id)
            .collect())
    }

    /// Records an approved bridge's current public keys.
    pub fn set_keys(&mut self, bridge_id: &str, keys: Vec<SigningKey>) -> Result<(), Failure> {
        let mut updated = self.bridges.clone();
        let entry = updated
            .iter_mut()
            .find(|bridge| bridge.bridge_id == bridge_id)
            .ok_or_else(|| Failure::new("bridge_not_approved"))?;
        entry.keys = usable(keys);
        self.replace(updated)
    }

    pub fn forget(&mut self, bridge_id: &str) -> Result<(), Failure> {
        let mut kept = self.bridges.clone();
        kept.retain(|bridge| bridge.bridge_id != bridge_id);
        self.replace(kept)?;
        // The trust is already gone. If the record cannot be saved, memory
        // still holds it for this run and the forget still succeeds.
        remember(&mut self.removed, bridge_id.to_owned());
        self.save_removed();
        Ok(())
    }

    fn save_removed(&self) {
        let Ok(bytes) = serde_json::to_vec_pretty(&RemovedFile {
            format: REMOVED_FORMAT.into(),
            version: 1,
            removed: self.removed.clone(),
        }) else {
            return;
        };
        let _ = write_atomic(&self.dir, REMOVED_FILE, &bytes);
    }

    /// Saves `bridges` and only then makes it the live list, so memory never
    /// says something the file does not.
    fn replace(&mut self, bridges: Vec<Approved>) -> Result<(), Failure> {
        self.save_list(&bridges)?;
        self.bridges = bridges;
        Ok(())
    }

    fn save_list(&self, bridges: &[Approved]) -> Result<(), Failure> {
        let bytes = serde_json::to_vec_pretty(&File {
            format: FORMAT.into(),
            version: 1,
            bridges: bridges.to_vec(),
        })
        .map_err(|_| Failure::new("io"))?;
        write_atomic(&self.dir, FILE, &bytes)
    }
}

/// Notes a removal: one entry per ID, newest last, oldest dropped past the cap.
fn remember(removed: &mut Vec<String>, bridge_id: String) {
    removed.retain(|id| *id != bridge_id);
    removed.push(bridge_id);
    if removed.len() > MAX_REMOVED {
        removed.drain(..removed.len() - MAX_REMOVED);
    }
}

/// Writes `name` in `dir` through a temp file and a rename, so a crash leaves
/// the old file or the new one, never half of one.
fn write_atomic(dir: &Path, name: &str, bytes: &[u8]) -> Result<(), Failure> {
    if dir.as_os_str().is_empty() || !dir.is_absolute() {
        return Err(Failure::new("io"));
    }
    fs::create_dir_all(dir).map_err(|_| Failure::new("io"))?;
    let mut suffix = [0u8; 8];
    getrandom::fill(&mut suffix).map_err(|_| Failure::new("io"))?;
    let temp = dir.join(format!(".tmp-{name}-{}", b64u(&suffix)));
    let result = (|| {
        let mut file = fs::File::create(&temp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        fs::rename(&temp, dir.join(name))
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
        return Err(Failure::new("io"));
    }
    Ok(())
}

/// At most 8 keys, each a usable Ed25519 signature key.
fn usable(keys: Vec<SigningKey>) -> Vec<SigningKey> {
    keys.into_iter()
        .filter(|key| key.public_key().is_some())
        .take(8)
        .collect()
}

fn valid(bridge: &Approved) -> bool {
    is_prefixed_id(&bridge.bridge_id, "brg")
        && check_origin(&bridge.origin, OriginPolicy::AllowLoopbackHttp).is_ok()
        && !bridge.display_name.is_empty()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn profile(id_byte: u8, origin: &str) -> BridgeProfile {
        BridgeProfile {
            bridge_id: ember_protocol::encoding::prefixed_id("brg", [id_byte; 16]),
            origin: origin.into(),
            display_name: "Bridge".into(),
            api_versions: vec!["v1".into()],
            capabilities_url: String::new(),
            signing_keys_url: String::new(),
        }
    }

    #[test]
    fn approvals_persist_and_replace_by_origin() {
        let dir = std::env::temp_dir().join(format!("sf4-bridges-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let mut store = Store::open(dir.clone());
        assert!(store.list().is_empty());
        store
            .approve(&profile(1, "https://bridge.one.example"), Vec::new())
            .unwrap();
        store.approve(&profile(2, "http://127.0.0.1:8787"), Vec::new()).unwrap();
        assert!(store.approve(&profile(3, "http://10.0.0.1:8787"), Vec::new()).is_err());
        assert!(
            store
                .approve(&profile(4, "https://bridge.one.example/path"), Vec::new())
                .is_err()
        );
        let reopened = Store::open(dir.clone());
        assert_eq!(reopened.list().len(), 2);
        let mut store = reopened;
        let displaced = store
            .approve(&profile(5, "https://bridge.one.example"), Vec::new())
            .unwrap();
        assert_eq!(displaced, vec![profile(1, "").bridge_id]);
        assert_eq!(store.list().len(), 2);
        assert!(store.get(&profile(1, "").bridge_id).is_none());
        store.forget(&profile(5, "").bridge_id).unwrap();
        assert_eq!(Store::open(dir.clone()).list().len(), 1);
        let _ = fs::remove_dir_all(&dir);
    }

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("sf4-bridges-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn removal_persists_and_approval_clears_it() {
        let dir = temp_dir("removed");
        let mut store = Store::open(dir.clone());
        let one = profile(1, "https://bridge.one.example");
        store.approve(&one, Vec::new()).unwrap();
        assert!(store.removed().is_empty());
        store.forget(&one.bridge_id).unwrap();
        assert_eq!(store.removed(), vec![one.bridge_id.clone()]);
        let mut reopened = Store::open(dir.clone());
        assert!(reopened.list().is_empty());
        assert_eq!(reopened.removed(), vec![one.bridge_id.clone()]);
        // Approving a different bridge leaves the record alone.
        reopened
            .approve(&profile(2, "https://bridge.two.example"), Vec::new())
            .unwrap();
        assert_eq!(reopened.removed(), vec![one.bridge_id.clone()]);
        reopened.approve(&one, Vec::new()).unwrap();
        assert!(reopened.removed().is_empty());
        assert!(Store::open(dir.clone()).removed().is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn removed_record_dedupes_and_caps() {
        let dir = temp_dir("removed-cap");
        let mut store = Store::open(dir.clone());
        let id = |n: u8| profile(n, "").bridge_id;
        store.forget(&id(1)).unwrap();
        store.forget(&id(2)).unwrap();
        store.forget(&id(1)).unwrap();
        assert_eq!(store.removed(), vec![id(2), id(1)]);
        let last = MAX_REMOVED as u8 + 10;
        for n in 3..=last {
            store.forget(&id(n)).unwrap();
        }
        let removed = Store::open(dir.clone()).removed();
        assert_eq!(removed.len(), MAX_REMOVED);
        assert_eq!(removed.last(), Some(&id(last)));
        assert!(!removed.contains(&id(1)));
        assert!(!removed.contains(&id(2)));
        assert_eq!(removed, store.removed());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn garbage_removed_record_reads_as_empty() {
        let dir = temp_dir("removed-garbage");
        fs::create_dir_all(&dir).unwrap();
        for text in [
            "not json".to_owned(),
            r#"{"format":"other","version":1,"removed":[]}"#.to_owned(),
            r#"{"format":"ember-bridges-removed","version":2,"removed":[]}"#.to_owned(),
            r#"{"format":"ember-bridges-removed","version":1,"removed":[],"extra":1}"#.to_owned(),
            " ".repeat(MAX_FILE + 1),
        ] {
            fs::write(dir.join(REMOVED_FILE), text).unwrap();
            assert!(Store::open(dir.clone()).removed().is_empty());
        }
        // Entries that are not bridge IDs are dropped; valid ones kept once.
        let good = profile(7, "").bridge_id;
        let text = format!(
            r#"{{"format":"ember-bridges-removed","version":1,"removed":["junk","{good}","{good}"]}}"#
        );
        fs::write(dir.join(REMOVED_FILE), text).unwrap();
        assert_eq!(Store::open(dir.clone()).removed(), vec![good]);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn no_folder_means_no_store() {
        let mut store = Store::open(PathBuf::new());
        assert!(
            store
                .approve(&profile(1, "https://bridge.one.example"), Vec::new())
                .is_err()
        );
        assert!(!std::path::Path::new(FILE).exists());
    }
}
