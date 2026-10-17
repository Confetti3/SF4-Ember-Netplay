//! Bridges the user approved (spec 9.1): `bridges.json` holds public
//! profile data and the bridge's public signing keys only, never a token.
//! Discovery documents and deep links cannot add an entry; only an explicit
//! approval from the native UI does. `removed.json` remembers the bridges the
//! player deliberately forgot, so nothing re-trusts one without a fresh
//! approval. The bridges this build pins (builtin.rs) are trusted beside the
//! approved ones and never written to `bridges.json`.
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
    /// Pinned by this build: never saved, and its keys never replaced. Never
    /// read from `bridges.json` either; a file that claims it is unreadable.
    #[serde(
        default,
        skip_deserializing,
        skip_serializing_if = "std::ops::Not::not"
    )]
    pub builtin: bool,
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
    /// The player's approvals, then the built-ins they did not remove.
    bridges: Vec<Approved>,
    /// Every usable built-in, removed or not, so an approval can restore one.
    pinned: Vec<Approved>,
    /// Bridge IDs the player removed, oldest first.
    removed: Vec<String>,
}

impl Store {
    /// Loads the list and adds the `pinned` bridges the player did not
    /// remove. A missing or unreadable file is an empty list; it is
    /// rewritten only on the next explicit approval.
    pub fn open(dir: PathBuf, pinned: Vec<Approved>) -> Self {
        // No per-user folder (the known-folder lookup failed): the store is
        // unavailable rather than relative to the working directory. It
        // trusts no built-in either, since it could not record a removal.
        if dir.as_os_str().is_empty() {
            return Self {
                dir,
                bridges: Vec::new(),
                pinned: Vec::new(),
                removed: Vec::new(),
            };
        }
        let approved = fs::read(dir.join(FILE))
            .ok()
            .filter(|bytes| bytes.len() <= MAX_FILE)
            .and_then(|bytes| json::parse_as::<File>(&bytes, MAX_FILE).ok())
            .filter(|file| file.format == FORMAT && file.version == 1)
            .map(|file| file.bridges)
            .unwrap_or_default()
            .into_iter()
            .filter(valid)
            .take(MAX_BRIDGES);
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
        let pinned: Vec<Approved> = pinned
            .into_iter()
            .map(|bridge| Approved {
                builtin: true,
                ..bridge
            })
            .filter(usable_pin)
            .collect();
        let live: Vec<Approved> = pinned
            .iter()
            .filter(|bridge| !removed.contains(&bridge.bridge_id))
            .cloned()
            .collect();
        // A built-in holds its ID and origin: an approval of either, made by
        // a build without it, gives way and goes with the next save.
        let mut bridges: Vec<Approved> = approved
            .filter(|bridge| !live.iter().any(|pin| same_bridge(pin, bridge)))
            .collect();
        bridges.extend(live);
        Self {
            dir,
            bridges,
            pinned,
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
    ///
    /// A built-in's ID is only ever the built-in: approving it again after
    /// the player removed it restores the pinned entry, pinned keys and all,
    /// and only at its own origin. A built-in the player kept holds its
    /// origin; another bridge there can be approved once it is removed.
    pub fn approve(
        &mut self,
        profile: &BridgeProfile,
        keys: Vec<SigningKey>,
    ) -> Result<Vec<String>, Failure> {
        let pin = self
            .pinned
            .iter()
            .find(|pin| pin.bridge_id == profile.bridge_id);
        let entry = match pin {
            Some(pin) if pin.origin == profile.origin => pin.clone(),
            Some(_) => return Err(Failure::new("bridge_changed")),
            None => Approved {
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
                builtin: false,
            },
        };
        if !valid(&entry) {
            return Err(Failure::new("invalid_bridge"));
        }
        let held = |bridge: &Approved| bridge.builtin && bridge.origin == entry.origin;
        if !entry.builtin && self.bridges.iter().any(held) {
            return Err(Failure::new("bridge_changed"));
        }
        // One entry per bridge ID and per origin: a bridge that changed its
        // ID at the same origin is a different bridge and replaces it.
        let (displaced, mut kept): (Vec<_>, Vec<_>) = self
            .bridges
            .iter()
            .cloned()
            .partition(|bridge| same_bridge(bridge, &entry));
        if kept.len() >= MAX_BRIDGES {
            return Err(Failure::new("too_many_bridges"));
        }
        let approved_id = entry.bridge_id.clone();
        kept.push(entry);
        // The player's own stay ahead of the built-ins.
        kept.sort_by_key(|bridge| bridge.builtin);
        self.replace(kept)?;
        // An explicit approval is the only way back after a removal.
        // A record that cannot be cleared on disk only makes onboarding ask
        // again after a restart.
        if self.removed.contains(&approved_id) {
            self.removed.retain(|id| *id != approved_id);
            let _ = self.save_removed(&self.removed);
        }
        Ok(displaced
            .into_iter()
            .map(|bridge| bridge.bridge_id)
            .collect())
    }

    /// Records an approved bridge's current public keys. A built-in's keys
    /// are pinned and never replaced.
    pub fn set_keys(&mut self, bridge_id: &str, keys: Vec<SigningKey>) -> Result<(), Failure> {
        let mut updated = self.bridges.clone();
        let entry = updated
            .iter_mut()
            .find(|bridge| bridge.bridge_id == bridge_id)
            .ok_or_else(|| Failure::new("bridge_not_approved"))?;
        if entry.builtin {
            return Err(Failure::new("bridge_pinned"));
        }
        entry.keys = usable(keys);
        self.replace(updated)
    }

    pub fn forget(&mut self, bridge_id: &str) -> Result<(), Failure> {
        // The removal is recorded first: should the trust then fail to go,
        // the record only keeps onboarding from trusting it by itself.
        let mut removed = self.removed.clone();
        remember(&mut removed, bridge_id.to_owned());
        self.save_removed(&removed)?;
        self.removed = removed;
        let mut kept = self.bridges.clone();
        kept.retain(|bridge| bridge.bridge_id != bridge_id);
        self.replace(kept)
    }

    fn save_removed(&self, removed: &[String]) -> Result<(), Failure> {
        let bytes = serde_json::to_vec_pretty(&RemovedFile {
            format: REMOVED_FORMAT.into(),
            version: 1,
            removed: removed.to_vec(),
        })
        .map_err(|_| Failure::new("io"))?;
        write_atomic(&self.dir, REMOVED_FILE, &bytes)
    }

    /// Saves `bridges` and only then makes it the live list, so memory never
    /// says something the file does not.
    fn replace(&mut self, bridges: Vec<Approved>) -> Result<(), Failure> {
        self.save_list(&bridges)?;
        self.bridges = bridges;
        Ok(())
    }

    /// Saves the player's approvals; the built-ins are the build's.
    fn save_list(&self, bridges: &[Approved]) -> Result<(), Failure> {
        let bytes = serde_json::to_vec_pretty(&File {
            format: FORMAT.into(),
            version: 1,
            bridges: bridges
                .iter()
                .filter(|bridge| !bridge.builtin)
                .cloned()
                .collect(),
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

/// A built-in is trusted only whole: valid, with one to 8 pinned keys, each
/// usable. One that could verify nothing it signs is not trusted at all.
fn usable_pin(bridge: &Approved) -> bool {
    valid(bridge)
        && (1..=8).contains(&bridge.keys.len())
        && bridge.keys.iter().all(|key| key.public_key().is_some())
}

/// One entry may hold each bridge ID and each origin.
fn same_bridge(one: &Approved, other: &Approved) -> bool {
    one.bridge_id == other.bridge_id || one.origin == other.origin
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
        let mut store = Store::open(dir.clone(), Vec::new());
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
        let reopened = Store::open(dir.clone(), Vec::new());
        assert_eq!(reopened.list().len(), 2);
        let mut store = reopened;
        let displaced = store
            .approve(&profile(5, "https://bridge.one.example"), Vec::new())
            .unwrap();
        assert_eq!(displaced, vec![profile(1, "").bridge_id]);
        assert_eq!(store.list().len(), 2);
        assert!(store.get(&profile(1, "").bridge_id).is_none());
        store.forget(&profile(5, "").bridge_id).unwrap();
        assert_eq!(Store::open(dir.clone(), Vec::new()).list().len(), 1);
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
        let mut store = Store::open(dir.clone(), Vec::new());
        let one = profile(1, "https://bridge.one.example");
        store.approve(&one, Vec::new()).unwrap();
        assert!(store.removed().is_empty());
        store.forget(&one.bridge_id).unwrap();
        assert_eq!(store.removed(), vec![one.bridge_id.clone()]);
        let mut reopened = Store::open(dir.clone(), Vec::new());
        assert!(reopened.list().is_empty());
        assert_eq!(reopened.removed(), vec![one.bridge_id.clone()]);
        // Approving a different bridge leaves the record alone.
        reopened
            .approve(&profile(2, "https://bridge.two.example"), Vec::new())
            .unwrap();
        assert_eq!(reopened.removed(), vec![one.bridge_id.clone()]);
        reopened.approve(&one, Vec::new()).unwrap();
        assert!(reopened.removed().is_empty());
        assert!(Store::open(dir.clone(), Vec::new()).removed().is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn removed_record_dedupes_and_caps() {
        let dir = temp_dir("removed-cap");
        let mut store = Store::open(dir.clone(), Vec::new());
        let id = |n: u8| profile(n, "").bridge_id;
        store.forget(&id(1)).unwrap();
        store.forget(&id(2)).unwrap();
        store.forget(&id(1)).unwrap();
        assert_eq!(store.removed(), vec![id(2), id(1)]);
        let last = MAX_REMOVED as u8 + 10;
        for n in 3..=last {
            store.forget(&id(n)).unwrap();
        }
        let removed = Store::open(dir.clone(), Vec::new()).removed();
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
            assert!(Store::open(dir.clone(), Vec::new()).removed().is_empty());
        }
        // Entries that are not bridge IDs are dropped; valid ones kept once.
        let good = profile(7, "").bridge_id;
        let text = format!(
            r#"{{"format":"ember-bridges-removed","version":1,"removed":["junk","{good}","{good}"]}}"#
        );
        fs::write(dir.join(REMOVED_FILE), text).unwrap();
        assert_eq!(Store::open(dir.clone(), Vec::new()).removed(), vec![good]);
        let _ = fs::remove_dir_all(&dir);
    }

    const PINNED_ORIGIN: &str = "https://bridge-staging.example";

    fn key(seed: u8) -> String {
        ember_protocol::SigningIdentity::from_seed(&zeroize::Zeroizing::new([seed; 32]))
            .unwrap()
            .public_key()
            .to_b64u()
    }

    /// A built-in at `PINNED_ORIGIN` with ID byte 9 and the key of seed 9.
    fn pinned() -> Vec<Approved> {
        let (id, x) = (profile(9, "").bridge_id, key(9));
        super::super::builtin::entries(&[super::super::builtin::Builtin {
            bridge_id: &id,
            origin: PINNED_ORIGIN,
            display_name: "Ember bridge (staging)",
            keys: &[("k9", &x)],
        }])
    }

    fn signing_key(kid: &str, x: String) -> SigningKey {
        SigningKey {
            kty: "OKP".into(),
            crv: "Ed25519".into(),
            alg: "EdDSA".into(),
            usage: "sig".into(),
            kid: kid.into(),
            x,
        }
    }

    #[test]
    fn a_builtin_is_trusted_without_being_saved() {
        let dir = temp_dir("builtin");
        let pin = profile(9, PINNED_ORIGIN);
        let mut store = Store::open(dir.clone(), pinned());
        let trusted = store.get(&pin.bridge_id).expect("the built-in");
        assert!(trusted.builtin);
        assert_eq!(trusted.origin, PINNED_ORIGIN);
        assert!(trusted.key("k9").is_some());
        // Listed after the player's own, and marked for the native side.
        store
            .approve(&profile(1, "https://bridge.one.example"), Vec::new())
            .unwrap();
        let listed = serde_json::to_value(store.list()).unwrap();
        assert_eq!(listed[0]["bridge_id"], profile(1, "").bridge_id);
        assert!(listed[0].get("builtin").is_none());
        assert_eq!(listed[1]["bridge_id"], pin.bridge_id);
        assert_eq!(listed[1]["builtin"], true);
        // The file holds only the player's approval.
        let file = fs::read_to_string(dir.join(FILE)).unwrap();
        assert!(!file.contains(&pin.bridge_id) && !file.contains("builtin"));
        assert_eq!(Store::open(dir.clone(), Vec::new()).list().len(), 1);
        let reopened = Store::open(dir.clone(), pinned());
        assert_eq!(reopened.list(), store.list());
        // A file that claims a built-in is not read.
        fs::write(
            dir.join(FILE),
            file.replace("\"keys\"", "\"builtin\": true, \"keys\""),
        )
        .unwrap();
        assert!(Store::open(dir.clone(), Vec::new()).list().is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_removed_builtin_stays_removed_until_approved_again() {
        let dir = temp_dir("builtin-removed");
        let pin = profile(9, PINNED_ORIGIN);
        let mut store = Store::open(dir.clone(), pinned());
        store.forget(&pin.bridge_id).unwrap();
        assert!(store.get(&pin.bridge_id).is_none());
        assert_eq!(store.removed(), vec![pin.bridge_id.clone()]);
        let mut store = Store::open(dir.clone(), pinned());
        assert!(store.list().is_empty());
        // Approved again by its address: the pinned entry comes back with
        // its pinned keys, whatever the origin served, and is still not saved.
        store
            .approve(&pin, vec![signing_key("other", key(5))])
            .unwrap();
        let restored = store.get(&pin.bridge_id).unwrap();
        assert!(restored.builtin);
        assert!(restored.key("k9").is_some() && restored.key("other").is_none());
        assert!(store.removed().is_empty());
        assert!(Store::open(dir.clone(), Vec::new()).list().is_empty());
        assert!(
            Store::open(dir.clone(), pinned())
                .get(&pin.bridge_id)
                .is_some()
        );
        // Its ID is never approved at another origin.
        let elsewhere = profile(9, "https://bridge.one.example");
        let refused = store.approve(&elsewhere, Vec::new()).unwrap_err();
        assert_eq!(refused.0, "bridge_changed");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_approval_cannot_evict_a_builtin() {
        let dir = temp_dir("builtin-evict");
        let pin = profile(9, PINNED_ORIGIN);
        let mut store = Store::open(dir.clone(), pinned());
        let other = profile(2, PINNED_ORIGIN);
        let refused = store.approve(&other, Vec::new()).unwrap_err();
        assert_eq!(refused.0, "bridge_changed");
        assert_eq!(store.list(), Store::open(dir.clone(), pinned()).list());
        // Removed, it gives the origin up to the bridge the player approves.
        store.forget(&pin.bridge_id).unwrap();
        assert!(store.approve(&other, Vec::new()).unwrap().is_empty());
        assert!(!store.get(&other.bridge_id).unwrap().builtin);
        // Approving the built-in again replaces that bridge.
        let displaced = store.approve(&pin, Vec::new()).unwrap();
        assert_eq!(displaced, vec![other.bridge_id.clone()]);
        assert_eq!(store.list().len(), 1);
        assert!(Store::open(dir.clone(), Vec::new()).list().is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_older_approval_of_a_builtin_gives_way_to_it() {
        let dir = temp_dir("builtin-older");
        // A build without the built-in approved it, and another bridge at its origin.
        let mut older = Store::open(dir.clone(), Vec::new());
        older
            .approve(&profile(9, PINNED_ORIGIN), Vec::new())
            .unwrap();
        older
            .approve(&profile(1, "https://bridge.one.example"), Vec::new())
            .unwrap();
        let store = Store::open(dir.clone(), pinned());
        let ids: Vec<_> = store
            .list()
            .into_iter()
            .map(|b| (b.bridge_id, b.builtin))
            .collect();
        assert_eq!(
            ids,
            vec![
                (profile(1, "").bridge_id, false),
                (profile(9, "").bridge_id, true)
            ]
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_builtins_keys_are_never_replaced() {
        let dir = temp_dir("builtin-keys");
        let pin = profile(9, PINNED_ORIGIN);
        let mut store = Store::open(dir.clone(), pinned());
        let refused = store
            .set_keys(&pin.bridge_id, vec![signing_key("k5", key(5))])
            .unwrap_err();
        assert_eq!(refused.0, "bridge_pinned");
        let kept = store.get(&pin.bridge_id).unwrap();
        assert!(kept.key("k9").is_some() && kept.key("k5").is_none());
        // An approved bridge's keys still follow its origin.
        let one = profile(1, "https://bridge.one.example");
        store.approve(&one, Vec::new()).unwrap();
        store
            .set_keys(&one.bridge_id, vec![signing_key("k5", key(5))])
            .unwrap();
        assert!(store.get(&one.bridge_id).unwrap().key("k5").is_some());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_unusable_pin_is_not_trusted() {
        let dir = temp_dir("builtin-unusable");
        let good = pinned().remove(0);
        let unusable = [
            Approved {
                bridge_id: "brg_FILL_IN".into(),
                ..good.clone()
            },
            Approved {
                origin: "http://bridge.example".into(),
                ..good.clone()
            },
            Approved {
                keys: Vec::new(),
                ..good.clone()
            },
            Approved {
                keys: vec![good.keys[0].clone(), signing_key("bad", "FILL_IN".into())],
                ..good.clone()
            },
        ];
        for pin in unusable {
            assert!(Store::open(dir.clone(), vec![pin]).list().is_empty());
        }
        assert!(!dir.exists());
    }

    #[test]
    fn no_folder_means_no_store() {
        let mut store = Store::open(PathBuf::new(), pinned());
        assert!(store.list().is_empty());
        assert!(
            store
                .approve(&profile(1, "https://bridge.one.example"), Vec::new())
                .is_err()
        );
        assert!(!std::path::Path::new(FILE).exists());
    }
}
