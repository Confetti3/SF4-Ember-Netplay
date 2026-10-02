//! The bridge's three independent server secrets (spec 9.4, SEC-05):
//! an HMAC key for every stored token, code and credential hash; an
//! encryption key for webhook secrets at rest; and the Ed25519 seed whose
//! public half `/v1/signing-keys` publishes. None doubles as another.
use std::{fs, io::Write, path::Path};

use chacha20poly1305::{
    XChaCha20Poly1305, XNonce,
    aead::{Aead, KeyInit},
};
use ember_protocol::{
    PublicKey, SigningIdentity,
    encoding::{b64u, decode_b64u},
    json,
};
use hmac::{Hmac, Mac};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use zeroize::Zeroizing;

use crate::util::random;

const FORMAT: &str = "ember-bridge-secrets";

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct File {
    format: String,
    version: u8,
    hmac_key: String,
    seal_key: String,
    signing_seed: String,
}

pub struct Keys {
    hmac: Zeroizing<[u8; 32]>,
    seal: Zeroizing<[u8; 32]>,
    signing: SigningIdentity,
}

impl Keys {
    /// Fresh random keys. Used by `init` and by tests.
    pub fn generate() -> Self {
        let signing = SigningIdentity::from_seed(&Zeroizing::new(random())).expect("valid seed");
        Self {
            hmac: Zeroizing::new(random()),
            seal: Zeroizing::new(random()),
            signing,
        }
    }

    pub fn load(path: &Path) -> Result<Self, String> {
        let bytes = Zeroizing::new(
            fs::read(path).map_err(|error| format!("cannot read secrets: {error}"))?,
        );
        let file: File =
            json::parse_as(&bytes, 4096).map_err(|_| "invalid secrets file".to_owned())?;
        if file.format != FORMAT || file.version != 1 {
            return Err("unsupported secrets file".into());
        }
        let decode = |text: &str| {
            decode_b64u::<32>(text, "secret").map_err(|_| "invalid secrets file".to_owned())
        };
        let hmac = Zeroizing::new(decode(&file.hmac_key)?);
        let seal = Zeroizing::new(decode(&file.seal_key)?);
        let seed = Zeroizing::new(decode(&file.signing_seed)?);
        if *hmac == *seal || *hmac == *seed || *seal == *seed {
            return Err("bridge secrets must be distinct".into());
        }
        let signing =
            SigningIdentity::from_seed(&seed).map_err(|_| "invalid signing seed".to_owned())?;
        Ok(Self {
            hmac,
            seal,
            signing,
        })
    }

    /// Writes a new secrets file; refuses to overwrite an existing one.
    pub fn save_new(&self, path: &Path) -> Result<(), String> {
        let file = File {
            format: FORMAT.into(),
            version: 1,
            hmac_key: b64u(self.hmac.as_slice()),
            seal_key: b64u(self.seal.as_slice()),
            signing_seed: b64u(self.signing.seed().as_slice()),
        };
        let text =
            Zeroizing::new(serde_json::to_vec_pretty(&file).map_err(|error| error.to_string())?);
        let mut options = fs::OpenOptions::new();
        options.write(true).create_new(true);
        // Owner only, whatever the umask. On Windows the file inherits the
        // folder's ACL.
        #[cfg(unix)]
        std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
        let mut out = options
            .open(path)
            .map_err(|error| format!("cannot create {}: {error}", path.display()))?;
        out.write_all(&text).map_err(|error| error.to_string())?;
        out.sync_all().map_err(|error| error.to_string())
    }

    /// HMAC-SHA256 under the bridge key, separated by `purpose`.
    pub fn keyed_hash(&self, purpose: &str, data: &[u8]) -> [u8; 32] {
        let mut mac =
            <Hmac<Sha256> as Mac>::new_from_slice(self.hmac.as_slice()).expect("any key length");
        mac.update(purpose.as_bytes());
        mac.update(&[0]);
        mac.update(data);
        mac.finalize().into_bytes().into()
    }

    /// `nonce || ciphertext` under the encryption key.
    pub fn seal(&self, plain: &[u8]) -> Vec<u8> {
        let nonce: [u8; 24] = random();
        let mut out = nonce.to_vec();
        out.extend(
            XChaCha20Poly1305::new(self.seal.as_slice().into())
                .encrypt(XNonce::from_slice(&nonce), plain)
                .expect("encryption of a bounded buffer"),
        );
        out
    }

    pub fn open(&self, sealed: &[u8]) -> Option<Zeroizing<Vec<u8>>> {
        if sealed.len() < 24 {
            return None;
        }
        let (nonce, body) = sealed.split_at(24);
        XChaCha20Poly1305::new(self.seal.as_slice().into())
            .decrypt(XNonce::from_slice(nonce), body)
            .ok()
            .map(Zeroizing::new)
    }

    pub fn signing_key(&self) -> PublicKey {
        self.signing.public_key()
    }

    /// Key ID: the first 16 characters of the key's SHA-256 thumbprint.
    pub fn signing_kid(&self) -> String {
        b64u(&Sha256::digest(self.signing.public_key().to_bytes()))[..16].to_owned()
    }
}

impl std::fmt::Debug for Keys {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Keys(..)")
    }
}
