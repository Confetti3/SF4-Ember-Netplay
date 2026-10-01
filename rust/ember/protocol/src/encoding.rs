//! Canonical text encodings used on the wire.
use std::fmt;

use base64::{
    Engine,
    engine::general_purpose::{STANDARD, URL_SAFE_NO_PAD},
};
use serde::{Deserialize, Deserializer, Serialize, Serializer};

use crate::{Error, Result};

const BASE32: &[u8; 32] = b"abcdefghijklmnopqrstuvwxyz234567";

/// Unpadded base64url.
pub fn b64u(data: &[u8]) -> String {
    URL_SAFE_NO_PAD.encode(data)
}

/// Decodes unpadded base64url of exactly `N` bytes. The text must re-encode
/// to itself, so trailing bits and padding variants are rejected.
pub fn decode_b64u<const N: usize>(text: &str, field: &'static str) -> Result<[u8; N]> {
    if text.len() != (4 * N).div_ceil(3) {
        return Err(Error::InvalidEncoding(field));
    }
    let raw = URL_SAFE_NO_PAD
        .decode(text)
        .map_err(|_| Error::InvalidEncoding(field))?;
    let bytes: [u8; N] = raw.try_into().map_err(|_| Error::InvalidEncoding(field))?;
    if b64u(&bytes) != text {
        return Err(Error::InvalidEncoding(field));
    }
    Ok(bytes)
}

/// Standard padded base64, as the Standard Webhooks signature header uses.
pub fn b64(data: &[u8]) -> String {
    STANDARD.encode(data)
}

/// Decodes standard padded base64, rejecting any other form.
pub fn decode_b64(text: &str, field: &'static str) -> Result<Vec<u8>> {
    let raw = STANDARD
        .decode(text)
        .map_err(|_| Error::InvalidEncoding(field))?;
    if STANDARD.encode(&raw) != text {
        return Err(Error::InvalidEncoding(field));
    }
    Ok(raw)
}

/// RFC 4648 base32, lowercase, without padding.
pub fn base32_lower(data: &[u8]) -> String {
    let mut out = String::with_capacity((data.len() * 8).div_ceil(5));
    let mut buffer: u16 = 0;
    let mut bits = 0;
    for &byte in data {
        buffer = (buffer << 8) | u16::from(byte);
        bits += 8;
        while bits >= 5 {
            bits -= 5;
            out.push(BASE32[usize::from((buffer >> bits) & 31)] as char);
        }
        buffer &= (1 << bits) - 1;
    }
    if bits > 0 {
        out.push(BASE32[usize::from((buffer << (5 - bits)) & 31)] as char);
    }
    out
}

/// A native 64-bit counter or revision. It travels as a canonical decimal
/// string: digits only, no sign, no leading zero except `"0"`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Counter(pub u64);

impl Counter {
    pub fn parse(text: &str) -> Result<Self> {
        let canonical = !text.is_empty()
            && text.len() <= 20
            && text.bytes().all(|byte| byte.is_ascii_digit())
            && (text == "0" || !text.starts_with('0'));
        if !canonical {
            return Err(Error::InvalidEncoding("counter"));
        }
        text.parse()
            .map(Self)
            .map_err(|_| Error::InvalidEncoding("counter"))
    }

    pub fn is_positive(self) -> bool {
        self.0 > 0
    }
}

impl fmt::Display for Counter {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl Serialize for Counter {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        serializer.collect_str(&self.0)
    }
}

impl<'de> Deserialize<'de> for Counter {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> std::result::Result<Self, D::Error> {
        let text = <std::borrow::Cow<'de, str>>::deserialize(deserializer)?;
        Self::parse(&text).map_err(serde::de::Error::custom)
    }
}

/// True for `<prefix>_<lowercase RFC 4122 version 4 UUID>`, the bridge's
/// identifier format (`brg_`, `emt_`, `chl_` and so on).
pub fn is_prefixed_id(text: &str, prefix: &str) -> bool {
    let Some(uuid) = text
        .strip_prefix(prefix)
        .and_then(|rest| rest.strip_prefix('_'))
    else {
        return false;
    };
    let bytes = uuid.as_bytes();
    bytes.len() == 36
        && bytes.iter().enumerate().all(|(index, &byte)| match index {
            8 | 13 | 18 | 23 => byte == b'-',
            14 => byte == b'4',
            19 => matches!(byte, b'8' | b'9' | b'a' | b'b'),
            _ => byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte),
        })
}

/// Formats 16 random bytes as a prefixed version 4 UUID. The caller supplies
/// the randomness so this crate stays free of an RNG.
pub fn prefixed_id(prefix: &str, mut random: [u8; 16]) -> String {
    random[6] = (random[6] & 0x0f) | 0x40;
    random[8] = (random[8] & 0x3f) | 0x80;
    let hex: String = random.iter().map(|byte| format!("{byte:02x}")).collect();
    format!(
        "{prefix}_{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

/// Which origins a verifier accepts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OriginPolicy {
    /// `https://host[:port]` only.
    HttpsOnly,
    /// Also `http://` on 127.0.0.1, `[::1]` or localhost, for local test
    /// bridges. Never used by a production profile.
    AllowLoopbackHttp,
}

/// Validates a bare origin: scheme and authority, no path, query, fragment or
/// credentials.
pub fn check_origin(origin: &str, policy: OriginPolicy) -> Result<()> {
    let authority = if let Some(rest) = origin.strip_prefix("https://") {
        rest
    } else if let Some(rest) = origin.strip_prefix("http://") {
        let host = rest
            .rsplit_once(':')
            .filter(|(_, port)| port.bytes().all(|b| b.is_ascii_digit()) && !port.is_empty())
            .map_or(rest, |(host, _)| host);
        if policy != OriginPolicy::AllowLoopbackHttp
            || !matches!(host, "127.0.0.1" | "[::1]" | "localhost")
        {
            return Err(Error::InvalidField("origin"));
        }
        rest
    } else {
        return Err(Error::InvalidField("origin"));
    };
    let valid = !authority.is_empty()
        && authority.len() <= 253 + 6
        && authority
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b':' | b'[' | b']'));
    if valid {
        Ok(())
    } else {
        Err(Error::InvalidField("origin"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base32_matches_rfc4648_vectors() {
        for (input, expected) in [
            ("", ""),
            ("f", "my"),
            ("fo", "mzxq"),
            ("foo", "mzxw6"),
            ("foob", "mzxw6yq"),
            ("fooba", "mzxw6ytb"),
            ("foobar", "mzxw6ytboi"),
        ] {
            assert_eq!(base32_lower(input.as_bytes()), expected);
        }
    }

    #[test]
    fn base64url_requires_exact_canonical_text() {
        let key = [0xffu8; 32];
        let text = b64u(&key);
        assert_eq!(decode_b64u::<32>(&text, "k").unwrap(), key);
        assert!(decode_b64u::<32>(&format!("{text}="), "k").is_err());
        assert!(decode_b64u::<32>(&text[..42], "k").is_err());
        assert!(decode_b64u::<31>(&text, "k").is_err());
        // Same length, nonzero trailing bits.
        let mut tampered = text.clone();
        tampered.replace_range(42.., "_");
        assert!(decode_b64u::<32>(&tampered, "k").is_err());
        assert!(decode_b64u::<32>(&text.replace('_', "/"), "k").is_err());
    }

    #[test]
    fn counters_are_canonical_decimal() {
        assert_eq!(Counter::parse("0").unwrap(), Counter(0));
        assert_eq!(
            Counter::parse("18446744073709551615").unwrap(),
            Counter(u64::MAX)
        );
        for bad in [
            "",
            "03",
            "-1",
            "+1",
            "1.0",
            " 1",
            "18446744073709551616",
            "1e3",
        ] {
            assert!(Counter::parse(bad).is_err(), "{bad}");
        }
        assert_eq!(serde_json::to_string(&Counter(12)).unwrap(), "\"12\"");
        assert!(serde_json::from_str::<Counter>("12").is_err());
    }

    #[test]
    fn prefixed_ids_round_trip() {
        let id = prefixed_id("emt", [0xab; 16]);
        assert!(is_prefixed_id(&id, "emt"));
        assert!(!is_prefixed_id(&id, "brg"));
        assert!(is_prefixed_id(
            "brg_11111111-1111-4111-8111-111111111111",
            "brg"
        ));
        assert!(!is_prefixed_id(
            "brg_11111111-1111-1111-8111-111111111111",
            "brg"
        ));
        assert!(!is_prefixed_id(
            "brg_11111111-1111-4111-8111-11111111111A",
            "brg"
        ));
    }

    #[test]
    fn origins_are_bare() {
        use OriginPolicy::*;
        assert!(check_origin("https://bridge.ember.example", HttpsOnly).is_ok());
        assert!(check_origin("https://bridge.ember.example:8443", HttpsOnly).is_ok());
        for bad in [
            "https://bridge.ember.example/",
            "https://user@bridge.ember.example",
            "https://bridge.ember.example?x",
            "http://bridge.ember.example",
            "http://127.0.0.1:8080",
            "ftp://bridge",
            "https://",
        ] {
            assert!(check_origin(bad, HttpsOnly).is_err(), "{bad}");
        }
        assert!(check_origin("http://127.0.0.1:8080", AllowLoopbackHttp).is_ok());
        assert!(check_origin("http://localhost:8080", AllowLoopbackHttp).is_ok());
        assert!(check_origin("http://10.0.0.1:8080", AllowLoopbackHttp).is_err());
        assert!(check_origin("http://127.0.0.1.evil.example", AllowLoopbackHttp).is_err());
    }
}
