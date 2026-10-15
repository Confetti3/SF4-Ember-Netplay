//! The persistent Ember identity: an Ed25519 key and the ID derived from it.
use std::fmt;

use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use sha2::{Digest, Sha256};
use zeroize::Zeroizing;

use crate::{
    Error, Result,
    encoding::{b64u, base32_lower, decode_b64u},
    json::Value,
    sign::Domain,
};

pub const ID_PREFIX: &str = "emb1_";
pub const ID_LENGTH: usize = 57;
/// `ASCII("ember-id-v1") || 0x00`, frozen for v1 (spec 6.2).
pub const ID_DOMAIN: &[u8] = b"ember-id-v1\0";

/// A full canonical Ember ID. Construction checks the textual form only;
/// whether it belongs to a key is checked with `PublicKey::ember_id`.
#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct EmberId(String);

impl EmberId {
    pub fn parse(text: &str) -> Result<Self> {
        let valid = text.len() == ID_LENGTH
            && text.starts_with(ID_PREFIX)
            && text[ID_PREFIX.len()..]
                .bytes()
                .all(|byte| byte.is_ascii_lowercase() || (b'2'..=b'7').contains(&byte))
            // 256 bits in 52 symbols leaves four zero bits in the last one.
            && matches!(text.as_bytes()[ID_LENGTH - 1], b'a' | b'q');
        if valid {
            Ok(Self(text.to_owned()))
        } else {
            Err(Error::InvalidField("ember_id"))
        }
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// First and last eight symbols of the encoded part, for a person to
    /// compare two screens. Never used for lookup or authorization.
    pub fn fingerprint(&self) -> String {
        let encoded = &self.0[ID_PREFIX.len()..];
        format!("{}-{}", &encoded[..8], &encoded[encoded.len() - 8..])
    }
}

impl fmt::Debug for EmberId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl fmt::Display for EmberId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl Serialize for EmberId {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        serializer.serialize_str(&self.0)
    }
}

impl<'de> Deserialize<'de> for EmberId {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> std::result::Result<Self, D::Error> {
        let text = <std::borrow::Cow<'de, str>>::deserialize(deserializer)?;
        Self::parse(&text).map_err(serde::de::Error::custom)
    }
}

/// A validated Ed25519 public key.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct PublicKey(VerifyingKey);

impl PublicKey {
    pub fn from_bytes(bytes: &[u8; 32]) -> Result<Self> {
        let key = VerifyingKey::from_bytes(bytes).map_err(|_| Error::InvalidKey)?;
        if key.is_weak() {
            return Err(Error::InvalidKey);
        }
        Ok(Self(key))
    }

    /// Decodes the 43-character wire form.
    pub fn from_b64u(text: &str) -> Result<Self> {
        Self::from_bytes(&decode_b64u::<32>(text, "public_key")?)
    }

    pub fn to_bytes(&self) -> [u8; 32] {
        self.0.to_bytes()
    }

    pub fn to_b64u(&self) -> String {
        b64u(self.0.as_bytes())
    }

    /// `"emb1_" || base32(SHA-256(ID_DOMAIN || key))`.
    pub fn ember_id(&self) -> EmberId {
        let digest = Sha256::new()
            .chain_update(ID_DOMAIN)
            .chain_update(self.0.as_bytes())
            .finalize();
        EmberId(format!("{ID_PREFIX}{}", base32_lower(&digest)))
    }

    /// Verifies a signature over `domain || JCS(value)`, using the strict
    /// rules that reject malleable and small-order encodings.
    pub fn verify(&self, domain: Domain, value: &Value, signature: &str) -> Result<()> {
        let signature = Signature::from_bytes(&decode_b64u::<64>(signature, "signature")?);
        self.0
            .verify_strict(&domain.signing_bytes(value), &signature)
            .map_err(|_| Error::InvalidSignature)
    }
}

impl fmt::Debug for PublicKey {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_b64u())
    }
}

impl Serialize for PublicKey {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        serializer.serialize_str(&self.to_b64u())
    }
}

impl<'de> Deserialize<'de> for PublicKey {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> std::result::Result<Self, D::Error> {
        let text = <std::borrow::Cow<'de, str>>::deserialize(deserializer)?;
        Self::from_b64u(&text).map_err(serde::de::Error::custom)
    }
}

/// A live identity key. It has no `Debug` that shows key material, no serde,
/// and signs only through typed operations in this crate; `SigningKey`
/// zeroizes itself on drop.
pub struct SigningIdentity {
    key: SigningKey,
    public: PublicKey,
    id: EmberId,
}

impl SigningIdentity {
    /// Builds the identity from its 32-byte seed. The caller owns where the
    /// seed came from (OS RNG, DPAPI blob, encrypted envelope).
    pub fn from_seed(seed: &Zeroizing<[u8; 32]>) -> Result<Self> {
        let key = SigningKey::from_bytes(seed);
        let public = PublicKey::from_bytes(&key.verifying_key().to_bytes())?;
        let id = public.ember_id();
        Ok(Self { key, public, id })
    }

    pub fn public_key(&self) -> PublicKey {
        self.public
    }

    pub fn ember_id(&self) -> &EmberId {
        &self.id
    }

    /// The seed, for re-protecting it in storage or a backup envelope.
    pub fn seed(&self) -> Zeroizing<[u8; 32]> {
        Zeroizing::new(self.key.to_bytes())
    }

    pub(crate) fn sign(&self, domain: Domain, value: &Value) -> String {
        b64u(&self.key.sign(&domain.signing_bytes(value)).to_bytes())
    }
}

impl fmt::Debug for SigningIdentity {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SigningIdentity")
            .field("ember_id", &self.id)
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_malformed_ids() {
        let good = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
        assert!(EmberId::parse(good).is_ok());
        for bad in [
            &good[..56],
            &good.to_uppercase(),
            &good.replace("emb1_", "emb2_"),
            &good.replace('j', "1"),
            // A last symbol with nonzero padding bits.
            &format!("{}b", &good[..56]),
        ] {
            assert!(EmberId::parse(bad).is_err(), "{bad}");
        }
        assert_eq!(
            EmberId::parse(good).unwrap().fingerprint(),
            "j25zrhe6-pmdhvlja"
        );
    }

    #[test]
    fn rejects_small_order_keys() {
        assert_eq!(PublicKey::from_bytes(&[0; 32]), Err(Error::InvalidKey));
        let mut identity = [0u8; 32];
        identity[0] = 1;
        assert_eq!(PublicKey::from_bytes(&identity), Err(Error::InvalidKey));
    }

    #[test]
    fn debug_hides_the_seed() {
        let identity = SigningIdentity::from_seed(&Zeroizing::new([7; 32])).unwrap();
        let text = format!("{identity:?}");
        assert!(text.contains("emb1_"));
        assert!(!text.contains("0707"));
        assert!(!text.contains("[7"));
    }
}
