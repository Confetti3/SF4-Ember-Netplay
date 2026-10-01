//! The passphrase envelope (spec 8.1): Argon2id derives a key that seals the
//! 32-byte seed with XChaCha20-Poly1305. The same structure serves the local
//! encrypted store (`ember-key-local`) and exported backups
//! (`ember-key-backup`), under different formats and AAD domains.
use argon2::{Algorithm, Argon2, Params, Version};
use chacha20poly1305::{
    XChaCha20Poly1305, XNonce,
    aead::{Aead, KeyInit, Payload},
};
use ember_protocol::{
    EmberId, PublicKey, SigningIdentity,
    encoding::{b64u, decode_b64u},
    json,
    sign::{KEY_BACKUP_DOMAIN, KEY_LOCAL_DOMAIN},
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use super::{Failure, random};

/// Untrusted envelope files are refused above this size before parsing.
pub const MAX_FILE: usize = 16 * 1024;
pub const MAX_PASSPHRASE: usize = 1024;

const ARGON2_VERSION: u32 = 19;
const DEFAULT_MEMORY_KIB: u32 = 64 * 1024;
const DEFAULT_ITERATIONS: u32 = 3;
const DEFAULT_PARALLELISM: u32 = 1;
// Import bounds (spec 8.1), checked before any allocation or derivation. The
// floors are the OWASP Argon2id minimum; Ember never writes weaker files.
const MAX_MEMORY_KIB: u32 = 256 * 1024;
const MIN_MEMORY_KIB: u32 = 19 * 1024;
const MAX_ITERATIONS: u32 = 6;
const MIN_ITERATIONS: u32 = 2;
const MAX_PARALLELISM: u32 = 4;
const SEALED_LEN: usize = 32 + 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Local,
    Backup,
}

impl Kind {
    fn format(self) -> &'static str {
        match self {
            Self::Local => "ember-key-local",
            Self::Backup => "ember-key-backup",
        }
    }

    fn domain(self) -> &'static [u8] {
        match self {
            Self::Local => KEY_LOCAL_DOMAIN,
            Self::Backup => KEY_BACKUP_DOMAIN,
        }
    }
}

/// KDF cost. Tests use a cheaper profile; files written for users always use
/// `Cost::DEFAULT`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cost {
    pub memory_kib: u32,
    pub iterations: u32,
    pub parallelism: u32,
}

impl Cost {
    pub const DEFAULT: Self = Self {
        memory_kib: DEFAULT_MEMORY_KIB,
        iterations: DEFAULT_ITERATIONS,
        parallelism: DEFAULT_PARALLELISM,
    };

    fn within_bounds(self) -> bool {
        (MIN_MEMORY_KIB..=MAX_MEMORY_KIB).contains(&self.memory_kib)
            && (MIN_ITERATIONS..=MAX_ITERATIONS).contains(&self.iterations)
            && (1..=MAX_PARALLELISM).contains(&self.parallelism)
            && self.memory_kib >= 8 * self.parallelism
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct KdfHeader {
    algorithm: String,
    version: u32,
    memory_kib: u32,
    iterations: u32,
    parallelism: u32,
    salt: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Header {
    format: String,
    version: u8,
    algorithm: String,
    ember_id: EmberId,
    public_key: PublicKey,
    kdf: KdfHeader,
    aead: String,
    nonce: String,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Envelope {
    header: Header,
    ciphertext: String,
}

/// What an envelope says about itself before it is opened. Safe to show; it
/// proves nothing until `open` succeeds.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Claimed {
    pub ember_id: EmberId,
}

/// Checks the passphrase rule: exact UTF-8 bytes, never trimmed or
/// normalized, 1 to 1,024 bytes.
pub fn check_passphrase(passphrase: &str) -> Result<(), Failure> {
    if passphrase.is_empty() || passphrase.len() > MAX_PASSPHRASE {
        return Err(Failure::InvalidPassphrase);
    }
    Ok(())
}

pub fn seal(
    kind: Kind,
    identity: &SigningIdentity,
    passphrase: &str,
    cost: Cost,
) -> Result<Vec<u8>, Failure> {
    check_passphrase(passphrase)?;
    let salt: [u8; 16] = random()?;
    let nonce: [u8; 24] = random()?;
    let header = Header {
        format: kind.format().into(),
        version: 1,
        algorithm: "Ed25519".into(),
        ember_id: identity.ember_id().clone(),
        public_key: identity.public_key(),
        kdf: KdfHeader {
            algorithm: "argon2id".into(),
            version: ARGON2_VERSION,
            memory_kib: cost.memory_kib,
            iterations: cost.iterations,
            parallelism: cost.parallelism,
            salt: b64u(&salt),
        },
        aead: "XChaCha20-Poly1305".into(),
        nonce: b64u(&nonce),
    };
    let key = derive(passphrase, &salt, cost)?;
    let aad = aad(kind, &header)?;
    let seed = identity.seed();
    let sealed = XChaCha20Poly1305::new(key.as_slice().into())
        .encrypt(
            XNonce::from_slice(&nonce),
            Payload {
                msg: seed.as_slice(),
                aad: &aad,
            },
        )
        .map_err(|_| Failure::Io)?;
    let envelope = Envelope {
        header,
        ciphertext: b64u(&sealed),
    };
    serde_json::to_vec_pretty(&envelope).map_err(|_| Failure::Io)
}

/// Parses and bounds-checks an envelope without deriving anything.
pub fn inspect(kind: Kind, bytes: &[u8]) -> Result<Claimed, Failure> {
    let (envelope, _) = parse(kind, bytes)?;
    Ok(Claimed {
        ember_id: envelope.header.ember_id,
    })
}

/// Derives, decrypts, and checks that the seed produces the header's key and
/// ID. Any AEAD failure means a wrong passphrase or a modified file; the two
/// are indistinguishable by design.
pub fn open(kind: Kind, bytes: &[u8], passphrase: &str) -> Result<SigningIdentity, Failure> {
    check_passphrase(passphrase)?;
    let (envelope, cost) = parse(kind, bytes)?;
    let header = &envelope.header;
    let salt = decode_b64u::<16>(&header.kdf.salt, "salt").map_err(|_| Failure::Corrupt)?;
    let nonce = decode_b64u::<24>(&header.nonce, "nonce").map_err(|_| Failure::Corrupt)?;
    let sealed = decode_b64u::<SEALED_LEN>(&envelope.ciphertext, "ciphertext")
        .map_err(|_| Failure::Corrupt)?;
    let key = derive(passphrase, &salt, cost)?;
    let aad = aad(kind, header)?;
    let plain = Zeroizing::new(
        XChaCha20Poly1305::new(key.as_slice().into())
            .decrypt(
                XNonce::from_slice(&nonce),
                Payload {
                    msg: &sealed,
                    aad: &aad,
                },
            )
            .map_err(|_| Failure::WrongPassphrase)?,
    );
    let seed: Zeroizing<[u8; 32]> =
        Zeroizing::new(plain.as_slice().try_into().map_err(|_| Failure::Corrupt)?);
    let identity = SigningIdentity::from_seed(&seed).map_err(|_| Failure::Corrupt)?;
    if identity.public_key() != header.public_key || identity.ember_id() != &header.ember_id {
        return Err(Failure::Corrupt);
    }
    Ok(identity)
}

fn parse(kind: Kind, bytes: &[u8]) -> Result<(Envelope, Cost), Failure> {
    if bytes.len() > MAX_FILE {
        return Err(Failure::Corrupt);
    }
    let envelope: Envelope = json::parse_as(bytes, MAX_FILE).map_err(|_| Failure::Corrupt)?;
    let header = &envelope.header;
    if header.format != kind.format() {
        return Err(if header.format.starts_with("ember-key-") {
            Failure::UnsupportedEnvelope
        } else {
            Failure::Corrupt
        });
    }
    if header.version != 1
        || header.algorithm != "Ed25519"
        || header.aead != "XChaCha20-Poly1305"
        || header.kdf.algorithm != "argon2id"
        || header.kdf.version != ARGON2_VERSION
    {
        return Err(Failure::UnsupportedEnvelope);
    }
    if header.public_key.ember_id() != header.ember_id {
        return Err(Failure::Corrupt);
    }
    let cost = Cost {
        memory_kib: header.kdf.memory_kib,
        iterations: header.kdf.iterations,
        parallelism: header.kdf.parallelism,
    };
    if !cost.within_bounds() {
        return Err(Failure::UnsupportedEnvelope);
    }
    Ok((envelope, cost))
}

fn aad(kind: Kind, header: &Header) -> Result<Vec<u8>, Failure> {
    let mut aad = kind.domain().to_vec();
    aad.extend_from_slice(&json::canonical(header).map_err(|_| Failure::Corrupt)?);
    Ok(aad)
}

fn derive(passphrase: &str, salt: &[u8; 16], cost: Cost) -> Result<Zeroizing<[u8; 32]>, Failure> {
    if !cost.within_bounds() {
        return Err(Failure::UnsupportedEnvelope);
    }
    let params = Params::new(cost.memory_kib, cost.iterations, cost.parallelism, Some(32))
        .map_err(|_| Failure::UnsupportedEnvelope)?;
    let mut key = Zeroizing::new([0u8; 32]);
    Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
        .hash_password_into(passphrase.as_bytes(), salt, key.as_mut_slice())
        .map_err(|_| Failure::UnsupportedEnvelope)?;
    Ok(key)
}

#[cfg(test)]
pub(crate) const TEST_COST: Cost = Cost {
    memory_kib: MIN_MEMORY_KIB,
    iterations: MIN_ITERATIONS,
    parallelism: 1,
};

#[cfg(test)]
mod tests {
    use super::*;

    fn identity(byte: u8) -> SigningIdentity {
        SigningIdentity::from_seed(&Zeroizing::new([byte; 32])).unwrap()
    }

    fn rewrite(bytes: &[u8], edit: impl FnOnce(&mut serde_json::Value)) -> Vec<u8> {
        let mut value: serde_json::Value = serde_json::from_slice(bytes).unwrap();
        edit(&mut value);
        serde_json::to_vec(&value).unwrap()
    }

    #[test]
    fn round_trip_and_fresh_salt_and_nonce() {
        let id = identity(1);
        let a = seal(Kind::Backup, &id, "correct horse", TEST_COST).unwrap();
        let b = seal(Kind::Backup, &id, "correct horse", TEST_COST).unwrap();
        assert_ne!(a, b);
        let opened = open(Kind::Backup, &a, "correct horse").unwrap();
        assert_eq!(opened.ember_id(), id.ember_id());
        assert_eq!(inspect(Kind::Backup, &a).unwrap().ember_id, *id.ember_id());
    }

    #[test]
    fn default_cost_is_spec_default() {
        assert_eq!(Cost::DEFAULT.memory_kib, 65_536);
        assert_eq!(Cost::DEFAULT.iterations, 3);
        assert_eq!(Cost::DEFAULT.parallelism, 1);
        assert!(Cost::DEFAULT.within_bounds());
    }

    // ID-12
    #[test]
    fn fails_closed() {
        let id = identity(2);
        let sealed = seal(Kind::Backup, &id, "pass phrase", TEST_COST).unwrap();
        assert_eq!(
            open(Kind::Backup, &sealed, "pass phrasE").err(),
            Some(Failure::WrongPassphrase)
        );
        // Passphrases are exact bytes, never trimmed.
        assert_eq!(
            open(Kind::Backup, &sealed, "pass phrase ").err(),
            Some(Failure::WrongPassphrase)
        );
        assert!(open(Kind::Backup, &sealed[..sealed.len() - 10], "pass phrase").is_err());
        // Header fields are authenticated even when the claim stays coherent.
        let other = identity(3);
        let swapped = rewrite(&sealed, |v| {
            v["header"]["ember_id"] = other.ember_id().as_str().into();
            v["header"]["public_key"] = other.public_key().to_b64u().into();
        });
        assert_eq!(
            open(Kind::Backup, &swapped, "pass phrase").err(),
            Some(Failure::WrongPassphrase)
        );
        let salted = rewrite(&sealed, |v| {
            v["header"]["kdf"]["salt"] = b64u(&[0; 16]).into()
        });
        assert_eq!(
            open(Kind::Backup, &salted, "pass phrase").err(),
            Some(Failure::WrongPassphrase)
        );
        let flipped = rewrite(&sealed, |v| {
            let text = v["ciphertext"].as_str().unwrap();
            let mut raw = decode_b64u::<SEALED_LEN>(text, "c").unwrap();
            raw[0] ^= 1;
            v["ciphertext"] = b64u(&raw).into();
        });
        assert_eq!(
            open(Kind::Backup, &flipped, "pass phrase").err(),
            Some(Failure::WrongPassphrase)
        );
        let incoherent = rewrite(&sealed, |v| {
            v["header"]["ember_id"] = other.ember_id().as_str().into()
        });
        assert_eq!(
            open(Kind::Backup, &incoherent, "pass phrase").err(),
            Some(Failure::Corrupt)
        );
        let unknown = rewrite(&sealed, |v| {
            v["header"]["kdf"]["algorithm"] = "scrypt".into()
        });
        assert_eq!(
            open(Kind::Backup, &unknown, "pass phrase").err(),
            Some(Failure::UnsupportedEnvelope)
        );
        let extra = rewrite(&sealed, |v| v["header"]["note"] = "x".into());
        assert_eq!(
            open(Kind::Backup, &extra, "pass phrase").err(),
            Some(Failure::Corrupt)
        );
        // A local store is not a backup and vice versa.
        assert_eq!(
            open(Kind::Local, &sealed, "pass phrase").err(),
            Some(Failure::UnsupportedEnvelope)
        );
        assert_eq!(
            open(Kind::Backup, b"not json", "pass phrase").err(),
            Some(Failure::Corrupt)
        );
    }

    // ID-13
    #[test]
    fn bounds_cost_before_deriving() {
        let id = identity(4);
        let sealed = seal(Kind::Backup, &id, "pass", TEST_COST).unwrap();
        for (field, value) in [
            ("memory_kib", 4_194_304u64),
            ("memory_kib", 1024),
            ("iterations", 1_000_000),
            ("iterations", 1),
            ("parallelism", 64),
            ("parallelism", 0),
        ] {
            let edited = rewrite(&sealed, |v| v["header"]["kdf"][field] = value.into());
            let started = std::time::Instant::now();
            assert_eq!(
                open(Kind::Backup, &edited, "pass").err(),
                Some(Failure::UnsupportedEnvelope),
                "{field}={value}"
            );
            assert!(started.elapsed() < std::time::Duration::from_secs(1));
        }
        let mut huge = sealed.clone();
        huge.resize(MAX_FILE + 1, b' ');
        assert_eq!(
            open(Kind::Backup, &huge, "pass").err(),
            Some(Failure::Corrupt)
        );
    }

    #[test]
    fn passphrase_rules() {
        assert_eq!(check_passphrase(""), Err(Failure::InvalidPassphrase));
        assert!(check_passphrase(&"a".repeat(1024)).is_ok());
        assert_eq!(
            check_passphrase(&"é".repeat(513)),
            Err(Failure::InvalidPassphrase)
        );
    }
}
