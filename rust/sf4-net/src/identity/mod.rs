//! The persistent Ember identity (spec 6 to 8).
//!
//! One Ed25519 seed per Windows user, stored under
//! `%LOCALAPPDATA%\Ember\Identity\v1` and protected with current-user DPAPI,
//! or with a passphrase envelope under Wine/Proton or by choice. A public
//! continuity file records which ID is expected there, so a key that cannot
//! be read is reported, never silently replaced.
//!
//! Every operation here is synchronous and may block on disk, DPAPI or the
//! Argon2 KDF. Callers run it on a blocking worker, never on the helper's
//! network executor or a game thread.
mod envelope;
mod platform;

use std::{
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};

use ember_protocol::{
    EmberId, PublicKey, SigningIdentity,
    encoding::{b64u, decode_b64u_vec},
    json,
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

pub use envelope::{Cost, MAX_FILE as MAX_BACKUP_FILE, check_passphrase};
pub use platform::{default_directory, running_under_wine};

pub const KEY_FILE: &str = "identity.bin";
pub const STATE_FILE: &str = "identity-state.json";
const LOCK_FILE: &str = "identity.lock";
const RETIRED_DIR: &str = "retired";
const TEMP_PREFIX: &str = ".tmp-";
const DPAPI_FORMAT: &str = "ember-key-dpapi";
const STATE_FORMAT: &str = "ember-identity-state";
/// Plaintext inside the DPAPI blob: magic, algorithm (1 = Ed25519), seed,
/// public key. The key is the ID binding checked after every unprotect.
const DPAPI_MAGIC: &[u8] = b"EMBER:KEY-DPAPI-INNER:1\n";
const DPAPI_INNER_LEN: usize = DPAPI_MAGIC.len() + 1 + 32 + 32;
const MAX_STATE_FILE: usize = 4 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum State {
    Disabled,
    Creating,
    Locked,
    Ready,
    Unavailable,
    RecoveryRequired,
}

/// Why an operation failed or a state was entered. Codes are safe to show and
/// log; they never carry key material, passphrases or paths.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Failure {
    PermissionDenied,
    Corrupt,
    UnsupportedEnvelope,
    WrongPassphrase,
    DecryptFailed,
    KeyMissing,
    MetadataMismatch,
    RandomUnavailable,
    InvalidPassphrase,
    PassphraseRequired,
    WrongState,
    ConfirmationMismatch,
    ReplacementNotConfirmed,
    Io,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Backend {
    Dpapi,
    Passphrase,
}

/// The public view of the identity. Safe for IPC, logs and diagnostics.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Status {
    pub state: State,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<Failure>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ember_id: Option<EmberId>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub fingerprint: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub public_key: Option<PublicKey>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub backend: Option<Backend>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub created_at: Option<u64>,
    /// True when enabling needs a passphrase (Wine/Proton).
    pub passphrase_required: bool,
}

/// How a newly created or imported key is protected at rest.
#[derive(Clone, Copy)]
pub enum Protection<'a> {
    /// DPAPI on native Windows. Refused under Wine/Proton.
    Default,
    Passphrase(&'a str),
}

/// The result of opening a backup before committing it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ImportPreview {
    pub ember_id: EmberId,
    pub fingerprint: String,
    /// The backup holds the identity this store already expects.
    pub same_identity: bool,
    /// Committing would retire an existing, different identity.
    pub replaces_existing: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Continuity {
    format: String,
    version: u8,
    ember_id: EmberId,
    public_key: PublicKey,
    backend: Backend,
    created_at: u64,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct DpapiFile {
    format: String,
    version: u8,
    blob: String,
}

enum KeyFile {
    Dpapi(Vec<u8>),
    Local(Vec<u8>),
}

/// Points where a test can inject a failure to model a crash.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Step {
    WriteTemp,
    FlushTemp,
    InstallKey,
    ReadBack,
    WriteContinuity,
}

type Faults = Arc<dyn Fn(Step) -> bool + Send + Sync>;

pub struct Identity {
    dir: PathBuf,
    wine: bool,
    cost: Cost,
    state: State,
    reason: Option<Failure>,
    continuity: Option<Continuity>,
    backend: Option<Backend>,
    key: Option<SigningIdentity>,
    faults: Option<Faults>,
}

impl Identity {
    /// Opens the store at its per-user location. Nothing is created until
    /// the user enables an identity.
    pub fn open_default() -> Self {
        match default_directory() {
            Ok(dir) => Self::open(dir, running_under_wine(), Cost::DEFAULT),
            Err(_) => {
                let mut identity = Self::open(PathBuf::new(), running_under_wine(), Cost::DEFAULT);
                identity.state = State::Unavailable;
                identity.reason = Some(Failure::Io);
                identity
            }
        }
    }

    pub fn open(dir: PathBuf, wine: bool, cost: Cost) -> Self {
        let mut identity = Self {
            dir,
            wine,
            cost,
            state: State::Disabled,
            reason: None,
            continuity: None,
            backend: None,
            key: None,
            faults: None,
        };
        identity.refresh();
        identity
    }

    pub fn status(&self) -> Status {
        let ember_id = self
            .key
            .as_ref()
            .map(|key| key.ember_id().clone())
            .or_else(|| self.continuity.as_ref().map(|c| c.ember_id.clone()));
        Status {
            state: self.state,
            reason: self.reason,
            fingerprint: ember_id.as_ref().map(EmberId::fingerprint),
            public_key: self
                .key
                .as_ref()
                .map(SigningIdentity::public_key)
                .or_else(|| self.continuity.as_ref().map(|c| c.public_key)),
            ember_id,
            backend: self.backend,
            created_at: self.continuity.as_ref().map(|c| c.created_at),
            passphrase_required: self.wine,
        }
    }

    /// The unlocked key, when the state is `Ready`.
    pub fn signer(&self) -> Option<&SigningIdentity> {
        self.key.as_ref().filter(|_| self.state == State::Ready)
    }

    /// Creates the identity (spec 7.4). If another process created one
    /// meanwhile, that identity is loaded instead.
    pub fn enable(&mut self, protection: Protection<'_>) -> Result<Status, Failure> {
        let backend = self.backend_for(protection)?;
        if self.state != State::Disabled {
            return Err(Failure::WrongState);
        }
        platform::create_private_directory(&self.dir).map_err(io_failure)?;
        let _lock = self.lock()?;
        self.evaluate();
        if self.state != State::Disabled {
            return Ok(self.status());
        }
        let seed = random_seed()?;
        let identity = SigningIdentity::from_seed(&seed).map_err(|_| Failure::RandomUnavailable)?;
        drop(seed);
        let bytes = self.key_file_bytes(&identity, protection)?;
        let temp = self.write_temp(&bytes)?;
        if let Err(error) = self.step(Step::InstallKey).and_then(|()| {
            platform::install_new(&temp, &self.dir.join(KEY_FILE)).map_err(io_failure)
        }) {
            let _ = fs::remove_file(&temp);
            // Losing the install race is not an error: load the winner.
            if self.dir.join(KEY_FILE).exists() {
                self.evaluate();
                return Ok(self.status());
            }
            return Err(error);
        }
        self.read_back(&identity, protection)?;
        self.commit(identity, backend, now());
        Ok(self.status())
    }

    /// Unlocks a passphrase-protected store for this helper lifetime.
    pub fn unlock(&mut self, passphrase: &str) -> Result<Status, Failure> {
        check_passphrase(passphrase)?;
        if self.state != State::Locked {
            return Err(Failure::WrongState);
        }
        let _lock = self.lock()?;
        let bytes =
            read_bounded(&self.dir.join(KEY_FILE), MAX_BACKUP_FILE)?.ok_or(Failure::KeyMissing)?;
        let identity = envelope::open(envelope::Kind::Local, &bytes, passphrase)?;
        self.accept(identity, Backend::Passphrase);
        match self.state {
            State::Ready => Ok(self.status()),
            _ => Err(self.reason.unwrap_or(Failure::MetadataMismatch)),
        }
    }

    /// Writes an encrypted backup of the unlocked identity to `path`.
    pub fn export(&self, path: &Path, passphrase: &str) -> Result<(), Failure> {
        let key = self.signer().ok_or(Failure::WrongState)?;
        let bytes = envelope::seal(envelope::Kind::Backup, key, passphrase, self.cost)?;
        write_replacing(path, &bytes)
    }

    /// Opens a backup and reports what committing it would do.
    pub fn preview_import(&self, bytes: &[u8], passphrase: &str) -> Result<ImportPreview, Failure> {
        let imported = envelope::open(envelope::Kind::Backup, bytes, passphrase)?;
        Ok(self.preview(&imported))
    }

    /// Installs the identity from a backup. A different existing identity is
    /// retired, never deleted, and only when `replace` confirms it.
    pub fn import(
        &mut self,
        bytes: &[u8],
        passphrase: &str,
        expected: &EmberId,
        protection: Protection<'_>,
        replace: bool,
    ) -> Result<Status, Failure> {
        let backend = self.backend_for(protection)?;
        let imported = envelope::open(envelope::Kind::Backup, bytes, passphrase)?;
        if imported.ember_id() != expected {
            return Err(Failure::ConfirmationMismatch);
        }
        platform::create_private_directory(&self.dir).map_err(io_failure)?;
        let _lock = self.lock()?;
        self.evaluate();
        let preview = self.preview(&imported);
        if preview.same_identity && matches!(self.state, State::Ready | State::Locked) {
            return Ok(self.status());
        }
        if preview.replaces_existing && !replace {
            return Err(Failure::ReplacementNotConfirmed);
        }
        let key_path = self.dir.join(KEY_FILE);
        let file = self.key_file_bytes(&imported, protection)?;
        let temp = self.write_temp(&file)?;
        // The old store stays intact until the new file is written and readable.
        let retired = if key_path.exists() || self.dir.join(STATE_FILE).exists() {
            Some(self.retire().inspect_err(|_| {
                let _ = fs::remove_file(&temp);
            })?)
        } else {
            None
        };
        if let Err(error) = platform::install_new(&temp, &key_path) {
            let _ = fs::remove_file(&temp);
            if let Some(retired) = &retired {
                self.restore(retired);
            }
            self.evaluate();
            return Err(io_failure(error));
        }
        self.read_back(&imported, protection)?;
        let created = if preview.same_identity {
            self.continuity.as_ref().map_or_else(now, |c| c.created_at)
        } else {
            now()
        };
        self.continuity = None;
        self.commit(imported, backend, created);
        Ok(self.status())
    }

    /// Retires the current store after the user confirmed which ID they are
    /// discarding (`None` when no ID is known). The files move to
    /// `retired\`; nothing is deleted.
    pub fn reset(&mut self, confirm: Option<&EmberId>) -> Result<Status, Failure> {
        if self.state == State::Disabled {
            return Err(Failure::WrongState);
        }
        if self.status().ember_id.as_ref() != confirm {
            return Err(Failure::ConfirmationMismatch);
        }
        let _lock = self.lock()?;
        self.retire()?;
        self.key = None;
        self.continuity = None;
        self.backend = None;
        self.evaluate();
        Ok(self.status())
    }

    #[cfg(test)]
    pub(crate) fn set_faults(&mut self, faults: impl Fn(Step) -> bool + Send + Sync + 'static) {
        self.faults = Some(Arc::new(faults));
    }

    fn backend_for(&self, protection: Protection<'_>) -> Result<Backend, Failure> {
        match protection {
            Protection::Default if self.wine => Err(Failure::PassphraseRequired),
            Protection::Default => Ok(Backend::Dpapi),
            Protection::Passphrase(passphrase) => {
                check_passphrase(passphrase)?;
                Ok(Backend::Passphrase)
            }
        }
    }

    fn preview(&self, imported: &SigningIdentity) -> ImportPreview {
        let current = self.status().ember_id;
        let same_identity = current.as_ref() == Some(imported.ember_id());
        ImportPreview {
            ember_id: imported.ember_id().clone(),
            fingerprint: imported.ember_id().fingerprint(),
            same_identity,
            replaces_existing: !same_identity && self.state != State::Disabled,
        }
    }

    fn refresh(&mut self) {
        if !self.dir.join(KEY_FILE).exists()
            && !self.dir.join(STATE_FILE).exists()
            && !self.dir.is_dir()
        {
            self.set(State::Disabled, None);
            return;
        }
        match self.lock() {
            Ok(_lock) => self.evaluate(),
            Err(failure) => self.set(State::Unavailable, Some(failure)),
        }
    }

    /// Recomputes the state from disk. Must hold the store lock.
    fn evaluate(&mut self) {
        self.remove_temps();
        self.key = None;
        self.backend = None;
        let continuity = match read_bounded(&self.dir.join(STATE_FILE), MAX_STATE_FILE) {
            Ok(Some(bytes)) => Some(parse_continuity(&bytes)),
            Ok(None) => None,
            Err(failure) => return self.set(State::Unavailable, Some(failure)),
        };
        self.continuity = continuity.clone().and_then(Result::ok);
        let key = match read_bounded(&self.dir.join(KEY_FILE), MAX_BACKUP_FILE) {
            Ok(key) => key,
            Err(failure) => return self.set(State::Unavailable, Some(failure)),
        };
        let Some(bytes) = key else {
            return match continuity {
                None => self.set(State::Disabled, None),
                // A guard exists but its key does not: never start over.
                Some(_) => self.set(State::RecoveryRequired, Some(Failure::KeyMissing)),
            };
        };
        match parse_key_file(&bytes) {
            Err(failure) => self.set(State::Unavailable, Some(failure)),
            Ok(KeyFile::Dpapi(blob)) => match unseal_dpapi(&blob) {
                Ok(identity) => self.accept(identity, Backend::Dpapi),
                Err(failure) => {
                    self.backend = Some(Backend::Dpapi);
                    self.set(State::Unavailable, Some(failure));
                }
            },
            Ok(KeyFile::Local(bytes)) => {
                self.backend = Some(Backend::Passphrase);
                match envelope::inspect(envelope::Kind::Local, &bytes) {
                    Err(failure) => self.set(State::Unavailable, Some(failure)),
                    Ok(claimed) => match &self.continuity {
                        Some(guard) if guard.ember_id != claimed.ember_id => {
                            self.set(State::RecoveryRequired, Some(Failure::MetadataMismatch));
                        }
                        _ => self.set(State::Locked, None),
                    },
                }
            }
        }
    }

    /// Adopts a key read from the store, repairing missing continuity data.
    fn accept(&mut self, identity: SigningIdentity, backend: Backend) {
        self.backend = Some(backend);
        match &self.continuity {
            Some(guard) if &guard.ember_id == identity.ember_id() => {
                self.key = Some(identity);
                self.set(State::Ready, None);
            }
            Some(_) => self.set(State::RecoveryRequired, Some(Failure::MetadataMismatch)),
            None => self.commit(identity, backend, now()),
        }
    }

    /// Records continuity data for a key that is installed and verified, then
    /// reports ready. A failed continuity write leaves the key usable; the
    /// next start repairs it.
    fn commit(&mut self, identity: SigningIdentity, backend: Backend, created_at: u64) {
        let continuity = Continuity {
            format: STATE_FORMAT.into(),
            version: 1,
            ember_id: identity.ember_id().clone(),
            public_key: identity.public_key(),
            backend,
            created_at,
        };
        let written = self
            .step(Step::WriteContinuity)
            .and_then(|()| serde_json::to_vec_pretty(&continuity).map_err(|_| Failure::Io))
            .and_then(|bytes| write_replacing(&self.dir.join(STATE_FILE), &bytes));
        self.continuity = Some(continuity);
        self.backend = Some(backend);
        self.key = Some(identity);
        self.set(State::Ready, written.err());
    }

    fn key_file_bytes(
        &self,
        identity: &SigningIdentity,
        protection: Protection<'_>,
    ) -> Result<Vec<u8>, Failure> {
        match protection {
            Protection::Default => seal_dpapi(identity),
            Protection::Passphrase(passphrase) => {
                envelope::seal(envelope::Kind::Local, identity, passphrase, self.cost)
            }
        }
    }

    /// Reads the installed key file back and checks it yields `expected`.
    fn read_back(
        &mut self,
        expected: &SigningIdentity,
        protection: Protection<'_>,
    ) -> Result<(), Failure> {
        let result = self.step(Step::ReadBack).and_then(|()| {
            let bytes = read_bounded(&self.dir.join(KEY_FILE), MAX_BACKUP_FILE)?
                .ok_or(Failure::KeyMissing)?;
            let identity = match (parse_key_file(&bytes)?, protection) {
                (KeyFile::Dpapi(blob), Protection::Default) => unseal_dpapi(&blob)?,
                (KeyFile::Local(bytes), Protection::Passphrase(passphrase)) => {
                    envelope::open(envelope::Kind::Local, &bytes, passphrase)?
                }
                _ => return Err(Failure::Corrupt),
            };
            if identity.ember_id() == expected.ember_id() {
                Ok(())
            } else {
                Err(Failure::Corrupt)
            }
        });
        if let Err(failure) = result {
            // The installed file is the established identity now, readable or
            // not. Report it; never remove it.
            self.evaluate();
            if self.state == State::Ready {
                return Ok(());
            }
            return Err(failure);
        }
        Ok(())
    }

    fn write_temp(&self, bytes: &[u8]) -> Result<PathBuf, Failure> {
        let suffix: [u8; 8] = random()?;
        let path = self.dir.join(format!("{TEMP_PREFIX}{}", b64u(&suffix)));
        let result = (|| {
            self.step(Step::WriteTemp)?;
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&path)
                .map_err(io_failure)?;
            file.write_all(bytes).map_err(io_failure)?;
            self.step(Step::FlushTemp)?;
            file.sync_all().map_err(io_failure)
        })();
        if let Err(failure) = result {
            let _ = fs::remove_file(&path);
            return Err(failure);
        }
        Ok(path)
    }

    /// Moves the key and continuity files into a fresh `retired\` folder.
    fn retire(&self) -> Result<PathBuf, Failure> {
        let suffix: [u8; 4] = random()?;
        let folder = self
            .dir
            .join(RETIRED_DIR)
            .join(format!("{}-{}", now(), b64u(&suffix)));
        fs::create_dir_all(&folder).map_err(io_failure)?;
        for name in [KEY_FILE, STATE_FILE] {
            let from = self.dir.join(name);
            if from.exists() {
                platform::install_new(&from, &folder.join(name)).map_err(io_failure)?;
            }
        }
        Ok(folder)
    }

    fn restore(&self, folder: &Path) {
        for name in [KEY_FILE, STATE_FILE] {
            let from = folder.join(name);
            if from.exists() {
                let _ = platform::install_new(&from, &self.dir.join(name));
            }
        }
    }

    fn remove_temps(&self) {
        let Ok(entries) = fs::read_dir(&self.dir) else {
            return;
        };
        for entry in entries.flatten() {
            if entry.file_name().to_string_lossy().starts_with(TEMP_PREFIX) {
                let _ = fs::remove_file(entry.path());
            }
        }
    }

    fn lock(&self) -> Result<File, Failure> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(self.dir.join(LOCK_FILE))
            .map_err(io_failure)?;
        file.lock().map_err(io_failure)?;
        Ok(file)
    }

    fn step(&self, step: Step) -> Result<(), Failure> {
        match &self.faults {
            Some(fails) if fails(step) => Err(Failure::Io),
            _ => Ok(()),
        }
    }

    fn set(&mut self, state: State, reason: Option<Failure>) {
        self.state = state;
        self.reason = reason;
        if state != State::Ready {
            self.key = None;
        }
    }
}

impl std::fmt::Debug for Identity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Identity")
            .field("status", &self.status())
            .finish_non_exhaustive()
    }
}

pub(crate) fn random<const N: usize>() -> Result<[u8; N], Failure> {
    let mut bytes = [0u8; N];
    getrandom::fill(&mut bytes).map_err(|_| Failure::RandomUnavailable)?;
    Ok(bytes)
}

/// A fresh seed from the operating system RNG. There is no fallback.
fn random_seed() -> Result<Zeroizing<[u8; 32]>, Failure> {
    let mut seed = Zeroizing::new([0u8; 32]);
    getrandom::fill(seed.as_mut_slice()).map_err(|_| Failure::RandomUnavailable)?;
    Ok(seed)
}

fn seal_dpapi(identity: &SigningIdentity) -> Result<Vec<u8>, Failure> {
    let mut inner = Zeroizing::new(Vec::with_capacity(DPAPI_INNER_LEN));
    inner.extend_from_slice(DPAPI_MAGIC);
    inner.push(1);
    inner.extend_from_slice(identity.seed().as_slice());
    inner.extend_from_slice(&identity.public_key().to_bytes());
    let blob = platform::protect(&inner).map_err(|_| Failure::DecryptFailed)?;
    serde_json::to_vec_pretty(&DpapiFile {
        format: DPAPI_FORMAT.into(),
        version: 1,
        blob: b64u(&blob),
    })
    .map_err(|_| Failure::Io)
}

fn unseal_dpapi(blob: &[u8]) -> Result<SigningIdentity, Failure> {
    let inner = platform::unprotect(blob).map_err(|_| Failure::DecryptFailed)?;
    if inner.len() != DPAPI_INNER_LEN
        || !inner.starts_with(DPAPI_MAGIC)
        || inner[DPAPI_MAGIC.len()] != 1
    {
        return Err(Failure::Corrupt);
    }
    let start = DPAPI_MAGIC.len() + 1;
    let seed: Zeroizing<[u8; 32]> = Zeroizing::new(
        inner[start..start + 32]
            .try_into()
            .map_err(|_| Failure::Corrupt)?,
    );
    let identity = SigningIdentity::from_seed(&seed).map_err(|_| Failure::Corrupt)?;
    if identity.public_key().to_bytes() != inner[start + 32..] {
        return Err(Failure::Corrupt);
    }
    Ok(identity)
}

fn parse_key_file(bytes: &[u8]) -> Result<KeyFile, Failure> {
    let value = json::parse(bytes, MAX_BACKUP_FILE).map_err(|_| Failure::Corrupt)?;
    if value.get("format").and_then(json::Value::as_str) == Some(DPAPI_FORMAT) {
        let file: DpapiFile = json::from_value(&value).map_err(|_| Failure::Corrupt)?;
        if file.version != 1 {
            return Err(Failure::UnsupportedEnvelope);
        }
        let blob =
            decode_b64u_vec(&file.blob, MAX_BACKUP_FILE, "blob").map_err(|_| Failure::Corrupt)?;
        return Ok(KeyFile::Dpapi(blob));
    }
    match value
        .get("header")
        .and_then(|header| header.get("format"))
        .and_then(json::Value::as_str)
    {
        Some("ember-key-local") => Ok(KeyFile::Local(bytes.to_vec())),
        Some(format) if format.starts_with("ember-key-") => Err(Failure::UnsupportedEnvelope),
        _ => match value.get("format").and_then(json::Value::as_str) {
            Some(format) if format.starts_with("ember-key-") => Err(Failure::UnsupportedEnvelope),
            _ => Err(Failure::Corrupt),
        },
    }
}

fn parse_continuity(bytes: &[u8]) -> Result<Continuity, Failure> {
    let continuity: Continuity =
        json::parse_as(bytes, MAX_STATE_FILE).map_err(|_| Failure::Corrupt)?;
    if continuity.format != STATE_FORMAT
        || continuity.version != 1
        || continuity.public_key.ember_id() != continuity.ember_id
    {
        return Err(Failure::Corrupt);
    }
    Ok(continuity)
}

/// Reads at most `max` bytes. Missing files are `None`.
fn read_bounded(path: &Path, max: usize) -> Result<Option<Vec<u8>>, Failure> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(io_failure(error)),
    };
    let length = file.metadata().map_err(io_failure)?.len();
    if length > max as u64 {
        return Err(Failure::Corrupt);
    }
    let mut bytes = Vec::with_capacity(length as usize);
    file.take(max as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(io_failure)?;
    if bytes.len() > max {
        return Err(Failure::Corrupt);
    }
    Ok(Some(bytes))
}

/// Writes through a sibling temporary file and moves it into place.
fn write_replacing(path: &Path, bytes: &[u8]) -> Result<(), Failure> {
    let suffix: [u8; 8] = random()?;
    let name = path.file_name().ok_or(Failure::Io)?.to_string_lossy();
    let temp = path.with_file_name(format!("{TEMP_PREFIX}{name}-{}", b64u(&suffix)));
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        platform::replace(&temp, path)
    })();
    if let Err(error) = result {
        let _ = fs::remove_file(&temp);
        return Err(io_failure(error));
    }
    Ok(())
}

fn io_failure(error: io::Error) -> Failure {
    match error.kind() {
        io::ErrorKind::PermissionDenied => Failure::PermissionDenied,
        _ => Failure::Io,
    }
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_secs())
}

#[cfg(test)]
mod tests;
