//! Bridges the user approved (spec 9.1): `bridges.json` holds public
//! profile data and the bridge's public signing keys only, never a token.
//! Discovery documents and deep links cannot add an entry; only an explicit
//! approval from the native UI does.
use std::{
    fs,
    io::Write,
    path::PathBuf,
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

pub struct Store {
    dir: PathBuf,
    bridges: Vec<Approved>,
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
        Self { dir, bridges }
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

    /// Approves a profile and returns the bridge IDs whose entries it
    /// replaced, so their sessions can be dropped.
    pub fn approve(&mut self, profile: &BridgeProfile) -> Result<Vec<String>, Failure> {
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
            keys: Vec::new(),
        };
        if !valid(&entry) {
            return Err(Failure::new("invalid_bridge"));
        }
        // One entry per bridge ID and per origin: a bridge that changed its
        // ID at the same origin is a different bridge and replaces it.
        let (displaced, kept): (Vec<_>, Vec<_>) = self.bridges.drain(..).partition(|bridge| {
            bridge.bridge_id == entry.bridge_id || bridge.origin == entry.origin
        });
        self.bridges = kept;
        if self.bridges.len() >= MAX_BRIDGES {
            self.bridges.extend(displaced);
            return Err(Failure::new("too_many_bridges"));
        }
        self.bridges.push(entry);
        if let Err(failure) = self.save() {
            self.bridges.pop();
            self.bridges.extend(displaced);
            return Err(failure);
        }
        Ok(displaced
            .into_iter()
            .map(|bridge| bridge.bridge_id)
            .collect())
    }

    /// Records an approved bridge's current public keys (at most 8 usable ones).
    pub fn set_keys(&mut self, bridge_id: &str, keys: Vec<SigningKey>) -> Result<(), Failure> {
        let keys: Vec<SigningKey> = keys
            .into_iter()
            .filter(|key| key.public_key().is_some())
            .take(8)
            .collect();
        let Some(entry) = self.bridges.iter_mut().find(|bridge| bridge.bridge_id == bridge_id) else {
            return Err(Failure::new("bridge_not_approved"));
        };
        let previous = std::mem::replace(&mut entry.keys, keys);
        if let Err(failure) = self.save() {
            if let Some(entry) = self.bridges.iter_mut().find(|bridge| bridge.bridge_id == bridge_id) {
                entry.keys = previous;
            }
            return Err(failure);
        }
        Ok(())
    }

    pub fn forget(&mut self, bridge_id: &str) -> Result<(), Failure> {
        self.bridges.retain(|bridge| bridge.bridge_id != bridge_id);
        self.save()
    }

    fn save(&self) -> Result<(), Failure> {
        if self.dir.as_os_str().is_empty() || !self.dir.is_absolute() {
            return Err(Failure::new("io"));
        }
        fs::create_dir_all(&self.dir).map_err(|_| Failure::new("io"))?;
        let bytes = serde_json::to_vec_pretty(&File {
            format: FORMAT.into(),
            version: 1,
            bridges: self.bridges.clone(),
        })
        .map_err(|_| Failure::new("io"))?;
        let mut suffix = [0u8; 8];
        getrandom::fill(&mut suffix).map_err(|_| Failure::new("io"))?;
        let temp = self.dir.join(format!(".tmp-{FILE}-{}", b64u(&suffix)));
        let result = (|| {
            let mut file = fs::File::create(&temp)?;
            file.write_all(&bytes)?;
            file.sync_all()?;
            drop(file);
            fs::rename(&temp, self.dir.join(FILE))
        })();
        if result.is_err() {
            let _ = fs::remove_file(&temp);
            return Err(Failure::new("io"));
        }
        Ok(())
    }
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
            .approve(&profile(1, "https://bridge.one.example"))
            .unwrap();
        store.approve(&profile(2, "http://127.0.0.1:8787")).unwrap();
        assert!(store.approve(&profile(3, "http://10.0.0.1:8787")).is_err());
        assert!(
            store
                .approve(&profile(4, "https://bridge.one.example/path"))
                .is_err()
        );
        let reopened = Store::open(dir.clone());
        assert_eq!(reopened.list().len(), 2);
        let mut store = reopened;
        let displaced = store
            .approve(&profile(5, "https://bridge.one.example"))
            .unwrap();
        assert_eq!(displaced, vec![profile(1, "").bridge_id]);
        assert_eq!(store.list().len(), 2);
        assert!(store.get(&profile(1, "").bridge_id).is_none());
        store.forget(&profile(5, "").bridge_id).unwrap();
        assert_eq!(Store::open(dir.clone()).list().len(), 1);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn no_folder_means_no_store() {
        let mut store = Store::open(PathBuf::new());
        assert!(
            store
                .approve(&profile(1, "https://bridge.one.example"))
                .is_err()
        );
        assert!(!std::path::Path::new(FILE).exists());
    }
}
