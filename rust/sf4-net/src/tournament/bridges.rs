//! Bridges the user approved (spec 9.1): `bridges.json` holds public
//! profile data and the bridge's public signing keys only, never a token.
//! Discovery documents and deep links cannot add an entry; only an explicit
//! approval from the native UI does. `removed.json` remembers the bridges the
//! player deliberately forgot, so nothing re-trusts one without a fresh
//! approval.
//!
//! The bridges this build pins (builtin.rs) are trusted beside the approved
//! ones and never written to `bridges.json`. A built-in is dormant until the
//! player uses it; `activated.json` remembers the ones they did. Each file
//! has its own list here and is written only when that list changes.
use std::{
    fs,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use ember_protocol::{
    api::{BridgeProfile, SigningKey},
    encoding::{OriginPolicy, check_origin, is_prefixed_id},
    json,
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};

use crate::identity::durable::{self, Existing};

use super::{Code, Failure, builtin::Builtin};

const FILE: &str = "bridges.json";
const FORMAT: &str = "ember-bridges";
const MAX_BRIDGES: usize = 16;
const MAX_FILE: usize = 64 * 1024;
// Separate files, so `bridges.json` keeps the format older builds parse.
const REMOVED_FILE: &str = "removed.json";
const REMOVED_FORMAT: &str = "ember-bridges-removed";
const ACTIVATED_FILE: &str = "activated.json";
const ACTIVATED_FORMAT: &str = "ember-bridges-activated";
/// The most bridge IDs `removed.json` or `activated.json` keeps.
const MAX_RECORDED: usize = 64;

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

/// A trusted bridge as `bridge_list` reports it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Listed {
    #[serde(flatten)]
    pub bridge: Approved,
    /// Built into this build: trusted without an approval, its keys pinned.
    #[serde(skip_serializing_if = "std::ops::Not::not")]
    pub builtin: bool,
    /// A built-in the player has used. Until then it is dormant: trusted, so
    /// links naming it work, but not one of the player's services on the
    /// native side. Absent means false.
    #[serde(skip_serializing_if = "std::ops::Not::not")]
    pub active: bool,
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

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ActivatedFile {
    format: String,
    version: u8,
    activated: Vec<String>,
}

pub struct Store {
    dir: PathBuf,
    /// The player's approvals, as `bridges.json` holds them. One that a live
    /// built-in covers stays here, unlisted, for a build without it.
    approved: Vec<Approved>,
    /// The usable bridges this build pins, removed or not.
    pinned: Vec<Approved>,
    /// Bridge IDs the player removed, oldest first.
    removed: Vec<String>,
    /// Built-in IDs the player used, oldest first.
    activated: Vec<String>,
}

impl Store {
    /// Loads the records and pins `builtins`. A missing or unreadable file
    /// is an empty list; it is rewritten only when what it holds changes.
    pub fn open(dir: PathBuf, builtins: &[Builtin]) -> Self {
        // No per-user folder (the known-folder lookup failed): the store is
        // unavailable rather than relative to the working directory. It
        // trusts no built-in either, since it could not record a removal.
        if dir.as_os_str().is_empty() {
            return Self {
                dir,
                approved: Vec::new(),
                pinned: Vec::new(),
                removed: Vec::new(),
                activated: Vec::new(),
            };
        }
        let approved = read::<File>(&dir, FILE)
            .filter(|file| file.format == FORMAT && file.version == 1)
            .map(|file| file.bridges)
            .unwrap_or_default()
            .into_iter()
            .filter(valid)
            .take(MAX_BRIDGES)
            .collect();
        let removed = bridge_ids(
            read::<RemovedFile>(&dir, REMOVED_FILE)
                .filter(|file| file.format == REMOVED_FORMAT && file.version == 1)
                .map(|file| file.removed),
        );
        let activated = bridge_ids(
            read::<ActivatedFile>(&dir, ACTIVATED_FILE)
                .filter(|file| file.format == ACTIVATED_FORMAT && file.version == 1)
                .map(|file| file.activated),
        );
        let pinned = builtins
            .iter()
            .map(Builtin::entry)
            .filter(usable_pin)
            .collect();
        Self {
            dir,
            approved,
            pinned,
            removed,
            activated,
        }
    }

    /// Bridge IDs the player deliberately removed, oldest first.
    pub fn removed(&self) -> Vec<String> {
        self.removed.clone()
    }

    /// The trusted bridges: the player's approvals, then the built-ins they
    /// did not remove. A built-in holds its ID and origin, so an approval of
    /// either is not listed while it is.
    pub fn list(&self) -> Vec<Listed> {
        let live: Vec<&Approved> = self.live().collect();
        let own = self
            .approved
            .iter()
            .filter(|bridge| !live.iter().any(|pin| same_bridge(pin, bridge)))
            .map(|bridge| Listed {
                bridge: bridge.clone(),
                builtin: false,
                active: false,
            });
        let pinned = live.iter().map(|pin| Listed {
            bridge: (*pin).clone(),
            builtin: true,
            active: self.used(pin),
        });
        own.chain(pinned).collect()
    }

    /// The trusted bridge `bridge_id`, found in the order `list` gives and
    /// cloned alone.
    pub fn get(&self, bridge_id: &str) -> Option<Approved> {
        self.approved
            .iter()
            .filter(|bridge| !self.live().any(|pin| same_bridge(pin, bridge)))
            .chain(self.live())
            .find(|bridge| bridge.bridge_id == bridge_id)
            .cloned()
    }

    /// Whether this build pins `bridge_id`: its keys are never replaced, and
    /// its origin is checked before each new session.
    pub fn pins(&self, bridge_id: &str) -> bool {
        self.pinned.iter().any(|pin| pin.bridge_id == bridge_id)
    }

    fn live(&self) -> impl Iterator<Item = &Approved> {
        self.pinned
            .iter()
            .filter(|pin| !self.removed.contains(&pin.bridge_id))
    }

    /// A built-in the player used: a session to it opened, they approved it
    /// by its address, or a build without it saved their approval of it.
    fn used(&self, pin: &Approved) -> bool {
        self.activated.contains(&pin.bridge_id)
            || self
                .approved
                .iter()
                .any(|bridge| bridge.bridge_id == pin.bridge_id && bridge.origin == pin.origin)
    }

    /// Approves a profile with the keys fetched from its origin, and returns
    /// the bridge IDs whose entries it replaced, so their sessions can be
    /// dropped.
    ///
    /// A built-in's ID is only ever the built-in: approving it, at its own
    /// origin only, trusts it again with its pinned keys and marks it used.
    /// A built-in the player kept holds its origin; another bridge there can
    /// be approved once it is removed.
    pub fn approve(
        &mut self,
        profile: &BridgeProfile,
        keys: Vec<SigningKey>,
    ) -> Result<Vec<String>, Failure> {
        let pin = self
            .pinned
            .iter()
            .find(|pin| pin.bridge_id == profile.bridge_id)
            .cloned();
        if let Some(pin) = pin {
            if pin.origin != profile.origin {
                return Err(Failure::new(Code::BridgeChanged));
            }
            return self.restore(&pin);
        }
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
            return Err(Failure::new(Code::InvalidBridge));
        }
        if self.live().any(|pin| pin.origin == entry.origin) {
            return Err(Failure::new(Code::BridgeChanged));
        }
        // One entry per bridge ID and per origin: a bridge that changed its
        // ID at the same origin is a different bridge and replaces it.
        let (displaced, mut kept): (Vec<_>, Vec<_>) = self
            .approved
            .iter()
            .cloned()
            .partition(|bridge| same_bridge(bridge, &entry));
        if kept.len() >= MAX_BRIDGES {
            return Err(Failure::new(Code::TooManyBridges));
        }
        let approved_id = entry.bridge_id.clone();
        kept.push(entry);
        self.save_approved(kept)?;
        self.unremove(&approved_id);
        Ok(ids(displaced))
    }

    /// Trusts a built-in again, as used. Another approval at its origin gives
    /// way to it; the player's own approval of the built-in itself stays, for
    /// a build without it.
    fn restore(&mut self, pin: &Approved) -> Result<Vec<String>, Failure> {
        let itself =
            |bridge: &Approved| bridge.bridge_id == pin.bridge_id && bridge.origin == pin.origin;
        let (displaced, kept): (Vec<_>, Vec<_>) = self
            .approved
            .iter()
            .cloned()
            .partition(|bridge| same_bridge(bridge, pin) && !itself(bridge));
        if !displaced.is_empty() {
            self.save_approved(kept)?;
        }
        if !self.activated.contains(&pin.bridge_id) {
            let mut activated = self.activated.clone();
            remember(&mut activated, pin.bridge_id.clone());
            self.save_activated(activated)?;
        }
        self.unremove(&pin.bridge_id);
        Ok(ids(displaced))
    }

    /// An explicit approval is the only way back after a removal. A record
    /// that cannot be cleared on disk only makes onboarding ask again after a
    /// restart.
    fn unremove(&mut self, bridge_id: &str) {
        if self.removed.iter().any(|id| id == bridge_id) {
            self.removed.retain(|id| id != bridge_id);
            let _ = self.save_removed(&self.removed);
        }
    }

    /// Records an approved bridge's current public keys. A built-in's keys
    /// are pinned and never replaced.
    pub fn set_keys(&mut self, bridge_id: &str, keys: Vec<SigningKey>) -> Result<(), Failure> {
        if self.pins(bridge_id) {
            return Err(Failure::new(Code::BridgePinned));
        }
        let mut updated = self.approved.clone();
        let entry = updated
            .iter_mut()
            .find(|bridge| bridge.bridge_id == bridge_id)
            .ok_or_else(|| Failure::new(Code::BridgeNotApproved))?;
        entry.keys = usable(keys);
        self.save_approved(updated)
    }

    /// Marks a built-in used once a session to it opened. It counts as used
    /// for the rest of the run even when that cannot be recorded; after a
    /// restart it is dormant again until its next session.
    pub fn activate(&mut self, bridge_id: &str) -> Result<(), Failure> {
        if !self.pins(bridge_id) || self.activated.iter().any(|id| id == bridge_id) {
            return Ok(());
        }
        remember(&mut self.activated, bridge_id.to_owned());
        self.write_activated(&self.activated)
    }

    /// Stops trusting a bridge. Every saved approval of it goes, and for a
    /// built-in any at its origin too, so none takes its place on the next
    /// start; so does a built-in's record of use.
    pub fn forget(&mut self, bridge_id: &str) -> Result<(), Failure> {
        // The removal is recorded first: should the trust then fail to go,
        // the record only keeps onboarding from trusting it by itself.
        let mut removed = self.removed.clone();
        remember(&mut removed, bridge_id.to_owned());
        self.save_removed(&removed)?;
        self.removed = removed;
        let pinned_origin = self
            .pinned
            .iter()
            .find(|pin| pin.bridge_id == bridge_id)
            .map(|pin| pin.origin.clone());
        let kept: Vec<Approved> = self
            .approved
            .iter()
            .filter(|bridge| {
                bridge.bridge_id != bridge_id && pinned_origin.as_ref() != Some(&bridge.origin)
            })
            .cloned()
            .collect();
        if kept.len() != self.approved.len() {
            self.save_approved(kept)?;
        }
        if self.activated.iter().any(|id| id == bridge_id) {
            let mut activated = self.activated.clone();
            activated.retain(|id| id != bridge_id);
            self.save_activated(activated)?;
        }
        Ok(())
    }

    fn save_removed(&self, removed: &[String]) -> Result<(), Failure> {
        self.save(
            REMOVED_FILE,
            &RemovedFile {
                format: REMOVED_FORMAT.into(),
                version: 1,
                removed: removed.to_vec(),
            },
        )
    }

    /// Saves the player's approvals and only then makes them the live list,
    /// so memory never says something the file does not.
    fn save_approved(&mut self, approved: Vec<Approved>) -> Result<(), Failure> {
        self.save(
            FILE,
            &File {
                format: FORMAT.into(),
                version: 1,
                bridges: approved.clone(),
            },
        )?;
        self.approved = approved;
        Ok(())
    }

    fn save_activated(&mut self, activated: Vec<String>) -> Result<(), Failure> {
        self.write_activated(&activated)?;
        self.activated = activated;
        Ok(())
    }

    fn write_activated(&self, activated: &[String]) -> Result<(), Failure> {
        self.save(
            ACTIVATED_FILE,
            &ActivatedFile {
                format: ACTIVATED_FORMAT.into(),
                version: 1,
                activated: activated.to_vec(),
            },
        )
    }

    fn save<T: Serialize>(&self, name: &str, file: &T) -> Result<(), Failure> {
        let bytes = serde_json::to_vec_pretty(file).map_err(|_| Failure::new(Code::Io))?;
        write_atomic(&self.dir, name, &bytes)
    }
}

/// `name` in `dir`, when it is there, small enough and parses as a `T`.
fn read<T: DeserializeOwned>(dir: &Path, name: &str) -> Option<T> {
    fs::read(dir.join(name))
        .ok()
        .filter(|bytes| bytes.len() <= MAX_FILE)
        .and_then(|bytes| json::parse_as::<T>(&bytes, MAX_FILE).ok())
}

/// A record's bridge IDs, each once, without any that are not one.
fn bridge_ids(record: Option<Vec<String>>) -> Vec<String> {
    record
        .unwrap_or_default()
        .into_iter()
        .filter(|id| is_prefixed_id(id, "brg"))
        .fold(Vec::new(), |mut list, id| {
            remember(&mut list, id);
            list
        })
}

fn ids(bridges: Vec<Approved>) -> Vec<String> {
    bridges.into_iter().map(|bridge| bridge.bridge_id).collect()
}

/// Notes a bridge ID: one entry per ID, newest last, oldest dropped past the cap.
fn remember(list: &mut Vec<String>, bridge_id: String) {
    list.retain(|id| *id != bridge_id);
    list.push(bridge_id);
    if list.len() > MAX_RECORDED {
        list.drain(..list.len() - MAX_RECORDED);
    }
}

/// Writes `name` in `dir` through a temp file and a move, so a crash leaves
/// the old file or the new one, never half of one.
fn write_atomic(dir: &Path, name: &str, bytes: &[u8]) -> Result<(), Failure> {
    if dir.as_os_str().is_empty() || !dir.is_absolute() {
        return Err(Failure::new(Code::Io));
    }
    fs::create_dir_all(dir).map_err(|_| Failure::new(Code::Io))?;
    durable::replace(&dir.join(name), bytes, Existing::Replace).map_err(|_| Failure::new(Code::Io))
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
mod tests;
