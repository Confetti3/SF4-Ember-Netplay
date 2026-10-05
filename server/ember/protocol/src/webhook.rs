//! Standard Webhooks HMAC-SHA256 signing and verification (spec 20.3).
//!
//! The signed content is `webhook-id "." webhook-timestamp "." raw body`.
//! Verification checks the timestamp and signature before the body is parsed.
use std::fmt;

use hmac::{Hmac, Mac};
use sha2::Sha256;
use zeroize::Zeroizing;

use crate::{
    Error, Result,
    encoding::{b64, decode_b64},
    json::MAX_BODY,
};

pub const TOLERANCE_SECS: u64 = 300;
pub const SECRET_LENGTH: usize = 32;
pub const SECRET_PREFIX: &str = "whsec_";
const MAX_SIGNATURES: usize = 8;
const MAX_ID: usize = 256;

pub const HEADER_ID: &str = "webhook-id";
pub const HEADER_TIMESTAMP: &str = "webhook-timestamp";
pub const HEADER_SIGNATURE: &str = "webhook-signature";

/// One subscription's 32-byte signing secret.
#[derive(Clone)]
pub struct Secret(Zeroizing<[u8; SECRET_LENGTH]>);

impl Secret {
    pub fn from_bytes(bytes: [u8; SECRET_LENGTH]) -> Self {
        Self(Zeroizing::new(bytes))
    }

    /// Accepts standard base64 of 32 bytes, with or without `whsec_`.
    pub fn parse(text: &str) -> Result<Self> {
        let encoded = text.strip_prefix(SECRET_PREFIX).unwrap_or(text);
        let raw = Zeroizing::new(decode_b64(encoded, "webhook secret")?);
        let bytes: [u8; SECRET_LENGTH] = raw
            .as_slice()
            .try_into()
            .map_err(|_| Error::InvalidEncoding("webhook secret"))?;
        Ok(Self::from_bytes(bytes))
    }

    /// `whsec_<base64>`, the form shown once to a subscription owner.
    pub fn reveal(&self) -> Zeroizing<String> {
        Zeroizing::new(format!("{SECRET_PREFIX}{}", b64(self.0.as_slice())))
    }

    fn mac(&self, id: &str, timestamp: &str, body: &[u8]) -> Hmac<Sha256> {
        let mut mac = Hmac::<Sha256>::new_from_slice(self.0.as_slice()).expect("any key length");
        mac.update(id.as_bytes());
        mac.update(b".");
        mac.update(timestamp.as_bytes());
        mac.update(b".");
        mac.update(body);
        mac
    }
}

impl fmt::Debug for Secret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Secret(..)")
    }
}

/// The three delivery headers.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Headers {
    pub id: String,
    pub timestamp: String,
    pub signature: String,
}

/// Signs one delivery attempt. Several secrets produce several `v1,`
/// entries, which is how a rotation overlap is delivered.
pub fn sign(secrets: &[&Secret], id: &str, timestamp: u64, body: &[u8]) -> Result<Headers> {
    check_id(id)?;
    let timestamp = timestamp.to_string();
    let signature = secrets
        .iter()
        .map(|secret| {
            format!(
                "v1,{}",
                b64(&secret.mac(id, &timestamp, body).finalize().into_bytes())
            )
        })
        .collect::<Vec<_>>()
        .join(" ");
    Ok(Headers {
        id: id.to_owned(),
        timestamp,
        signature,
    })
}

/// Verifies a delivery against any of `secrets` (more than one during a
/// rotation overlap). Comparison is constant time.
pub fn verify(secrets: &[&Secret], headers: &Headers, body: &[u8], now: u64) -> Result<()> {
    if body.len() > MAX_BODY {
        return Err(Error::TooLarge);
    }
    check_id(&headers.id)?;
    let ts = &headers.timestamp;
    if ts.is_empty() || ts.len() > 16 || !ts.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Error::InvalidField(HEADER_TIMESTAMP));
    }
    let sent: u64 = ts
        .parse()
        .map_err(|_| Error::InvalidField(HEADER_TIMESTAMP))?;
    if sent.abs_diff(now) > TOLERANCE_SECS {
        return Err(Error::Expired);
    }
    let candidates: Vec<&str> = headers.signature.split_ascii_whitespace().collect();
    if candidates.is_empty() || candidates.len() > MAX_SIGNATURES {
        return Err(Error::InvalidField(HEADER_SIGNATURE));
    }
    let mut valid = false;
    for candidate in candidates {
        let Some(("v1", encoded)) = candidate.split_once(',') else {
            continue;
        };
        let Ok(raw) = decode_b64(encoded, HEADER_SIGNATURE) else {
            continue;
        };
        for secret in secrets {
            valid |= secret.mac(&headers.id, ts, body).verify_slice(&raw).is_ok();
        }
    }
    if valid {
        Ok(())
    } else {
        Err(Error::InvalidSignature)
    }
}

fn check_id(id: &str) -> Result<()> {
    let valid = !id.is_empty() && id.len() <= MAX_ID && id.bytes().all(|b| b.is_ascii_graphic());
    if valid {
        Ok(())
    } else {
        Err(Error::InvalidField(HEADER_ID))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sign_then_verify_with_rotation_overlap() {
        let old = Secret::from_bytes([1; 32]);
        let new = Secret::from_bytes([2; 32]);
        let body = br#"{"a":1}"#;
        let headers = sign(&[&new, &old], "evt_x", 1000, body).unwrap();
        assert!(verify(&[&old], &headers, body, 1000).is_ok());
        assert!(verify(&[&new], &headers, body, 1000).is_ok());
        let only_new = sign(&[&new], "evt_x", 1000, body).unwrap();
        assert_eq!(
            verify(&[&old], &only_new, body, 1000),
            Err(Error::InvalidSignature)
        );
    }

    #[test]
    fn rejects_bad_headers() {
        let secret = Secret::from_bytes([3; 32]);
        let body = b"{}";
        let good = sign(&[&secret], "evt_y", 50_000, body).unwrap();
        let mut changed_id = good.clone();
        changed_id.id = "evt_z".into();
        assert_eq!(
            verify(&[&secret], &changed_id, body, 50_000),
            Err(Error::InvalidSignature)
        );
        let mut signed_ts = good.clone();
        signed_ts.timestamp = "+50000".into();
        assert!(verify(&[&secret], &signed_ts, body, 50_000).is_err());
        let mut many = good.clone();
        many.signature = [good.signature.as_str(); 9].join(" ");
        assert!(verify(&[&secret], &many, body, 50_000).is_err());
        let mut v2 = good.clone();
        v2.signature = good.signature.replace("v1,", "v2,");
        assert_eq!(
            verify(&[&secret], &v2, body, 50_000),
            Err(Error::InvalidSignature)
        );
        assert_eq!(
            verify(&[&secret], &good, body, 50_000 + 301),
            Err(Error::Expired)
        );
        assert_eq!(
            verify(&[&secret], &good, body, 50_000 - 301),
            Err(Error::Expired)
        );
    }

    #[test]
    fn secrets_round_trip_and_hide() {
        let secret = Secret::from_bytes([9; 32]);
        let text = secret.reveal();
        assert!(text.starts_with(SECRET_PREFIX));
        let parsed = Secret::parse(&text).unwrap();
        assert_eq!(parsed.0.as_slice(), secret.0.as_slice());
        assert_eq!(format!("{secret:?}"), "Secret(..)");
        assert!(Secret::parse("whsec_AAAA").is_err());
    }
}
