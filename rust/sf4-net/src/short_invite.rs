//! Short invitation links: `https://embernetplay.link/j#XXXX-XXXX-XXXX`.
//!
//! The 12-symbol code is derived from the room and its capability, so every
//! member of a room produces the same code. Argon2id turns the code into a
//! locator, a sealing key and a write token on this machine. The link
//! service stores the sealed full invitation under the locator and never
//! receives the code, the key or the invitation. A joiner derives the same
//! values from the code, fetches the record, opens it and checks that the
//! invitation inside derives the code it was given. See
//! `docs/design/SHORT_INVITATIONS.md`.
use std::{
    io,
    sync::{Arc, OnceLock},
    time::Duration,
};

use aes_gcm::{
    Aes256Gcm, Nonce,
    aead::{Aead, KeyInit, Payload},
};
use argon2::{Algorithm, Argon2, Params, Version};

/// Where a short code is shown and copied. The fragment never reaches a
/// web server, so the code stays out of access logs and link previews.
pub const LINK_PREFIX: &str = "https://embernetplay.link/j#";
pub const DEFAULT_SERVICE: &str = "https://embernetplay.link/s/v1/";
/// Tests and local runs may point the helper at a loopback service.
pub const SERVICE_OVERRIDE: &str = "SF4E_SHORT_INVITE_SERVICE";

pub const CODE_SYMBOLS: usize = 12;
const ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const DOMAIN: &[u8] = b"SF4 Ember short invitation v1";
const CODE_CONTEXT: &str = "SF4 Ember short invitation code v1";

// Wire constants, matching rust/ember-short.
const WIRE_VERSION: u8 = 1;
const NONCE_BYTES: usize = 12;
const TAG_BYTES: usize = 16;
pub const MAX_SEALED_BYTES: usize = 1024;
pub const MAX_PLAINTEXT_BYTES: usize = MAX_SEALED_BYTES - NONCE_BYTES - TAG_BYTES;
pub const MIN_TTL_SECS: u64 = 60;
pub const MAX_TTL_SECS: u64 = 2 * 60 * 60;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(8);

/// Why a short link could not be made or opened. Only this word leaves the
/// helper, never a code or a key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShortError {
    /// The service could not be reached or refused the request.
    Unavailable,
    /// No record under this code: mistyped, or the room's link expired.
    Unknown,
    /// The record does not open with this code or holds no invitation.
    Malformed,
}

impl ShortError {
    pub fn reason(self) -> &'static str {
        match self {
            Self::Unavailable => "short_unavailable",
            Self::Unknown => "short_unknown",
            Self::Malformed => "malformed",
        }
    }
}

/// Values derived from one code. No Debug: they open the record.
#[derive(Clone)]
pub struct Keys {
    locator: [u8; 16],
    key: [u8; 32],
    write: [u8; 16],
}

impl Keys {
    fn locator_text(&self) -> String {
        use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
        URL_SAFE_NO_PAD.encode(self.locator)
    }

    fn aad(&self) -> Vec<u8> {
        [DOMAIN, &self.locator[..]].concat()
    }
}

/// The canonical 12-symbol code of a room: Crockford base32 of 60 bits
/// derived from the room ID and capability.
pub fn code_from_secrets(room: &[u8; 16], capability: &[u8; 32]) -> String {
    let mut input = [0u8; 48];
    input[..16].copy_from_slice(room);
    input[16..].copy_from_slice(capability);
    let digest = blake3::derive_key(CODE_CONTEXT, &input);
    let bits = u64::from_be_bytes(digest[..8].try_into().unwrap_or_default()) >> 4;
    (0..CODE_SYMBOLS)
        .map(|index| ALPHABET[((bits >> (55 - 5 * index)) & 31) as usize] as char)
        .collect()
}

/// `XXXX-XXXX-XXXX`.
pub fn display_code(code: &str) -> String {
    let mut text = String::with_capacity(CODE_SYMBOLS + 2);
    for (index, symbol) in code.chars().enumerate() {
        if index > 0 && index % 4 == 0 {
            text.push('-');
        }
        text.push(symbol);
    }
    text
}

pub fn link(code: &str) -> String {
    format!("{LINK_PREFIX}{}", display_code(code))
}

/// The canonical code in a pasted short link or code, or None when the text
/// is not one. Accepts the link with `#` or `/`, with or without the scheme
/// or `www.`, and the bare code with or without dashes or spaces, in any
/// case. Crockford look-alikes (O for 0, I and L for 1) are read as digits.
pub fn parse(text: &str) -> Option<String> {
    let text = text.trim_matches(|character: char| character.is_ascii_whitespace());
    if text.len() > 96 {
        return None;
    }
    let lower = text.to_ascii_lowercase();
    let mut rest = lower.as_str();
    for scheme in ["https://", "http://"] {
        if let Some(stripped) = rest.strip_prefix(scheme) {
            rest = stripped;
            break;
        }
    }
    rest = rest.strip_prefix("www.").unwrap_or(rest);
    if let Some(path) = rest.strip_prefix("embernetplay.link") {
        rest = path
            .strip_prefix("/j#")
            .or_else(|| path.strip_prefix("/j/#"))
            .or_else(|| path.strip_prefix("/j/"))?;
        rest = rest.trim_end_matches('/');
    } else if text.contains(':') || text.contains('/') {
        return None;
    }
    let mut code = String::with_capacity(CODE_SYMBOLS);
    for character in rest.chars() {
        let symbol = match character.to_ascii_uppercase() {
            '-' | ' ' => continue,
            'O' => '0',
            'I' | 'L' => '1',
            other if ALPHABET.contains(&(other as u8)) && other.is_ascii() => other,
            _ => return None,
        };
        if code.len() == CODE_SYMBOLS {
            return None;
        }
        code.push(symbol);
    }
    (code.len() == CODE_SYMBOLS).then_some(code)
}

/// Argon2id with OWASP's 19 MiB, two-pass profile. A few tens of
/// milliseconds here; the same cost for every guess anyone else makes.
pub fn derive(code: &str) -> io::Result<Keys> {
    let params = Params::new(19 * 1024, 2, 1, Some(64)).map_err(|_| io::ErrorKind::InvalidInput)?;
    let mut output = [0u8; 64];
    Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
        .hash_password_into(code.as_bytes(), DOMAIN, &mut output)
        .map_err(|_| io::ErrorKind::InvalidInput)?;
    let mut keys = Keys {
        locator: [0; 16],
        key: [0; 32],
        write: [0; 16],
    };
    keys.locator.copy_from_slice(&output[..16]);
    keys.key.copy_from_slice(&output[16..48]);
    keys.write.copy_from_slice(&output[48..]);
    Ok(keys)
}

/// `derive` off the async workers.
pub async fn derive_async(code: String) -> Result<Keys, ShortError> {
    tokio::task::spawn_blocking(move || derive(&code))
        .await
        .map_err(|_| ShortError::Malformed)?
        .map_err(|_| ShortError::Malformed)
}

pub fn seal(keys: &Keys, plaintext: &[u8]) -> Option<Vec<u8>> {
    if plaintext.is_empty() || plaintext.len() > MAX_PLAINTEXT_BYTES {
        return None;
    }
    let cipher = Aes256Gcm::new_from_slice(&keys.key).ok()?;
    let nonce: [u8; NONCE_BYTES] = rand::random();
    let aad = keys.aad();
    let ciphertext = cipher
        .encrypt(
            Nonce::from_slice(&nonce),
            Payload {
                msg: plaintext,
                aad: &aad,
            },
        )
        .ok()?;
    Some([&nonce[..], &ciphertext].concat())
}

pub fn open(keys: &Keys, sealed: &[u8]) -> Option<Vec<u8>> {
    if sealed.len() <= NONCE_BYTES + TAG_BYTES || sealed.len() > MAX_SEALED_BYTES {
        return None;
    }
    let cipher = Aes256Gcm::new_from_slice(&keys.key).ok()?;
    let aad = keys.aad();
    cipher
        .decrypt(
            Nonce::from_slice(&sealed[..NONCE_BYTES]),
            Payload {
                msg: &sealed[NONCE_BYTES..],
                aad: &aad,
            },
        )
        .ok()
}

/// The service base URL: the pinned one, or a loopback override for tests.
pub fn service_base() -> String {
    std::env::var(SERVICE_OVERRIDE)
        .ok()
        .filter(|base| loopback_service(base))
        .unwrap_or_else(|| DEFAULT_SERVICE.to_owned())
}

pub fn loopback_service(base: &str) -> bool {
    ["http://127.0.0.1:", "http://localhost:", "http://[::1]:"]
        .iter()
        .any(|prefix| base.starts_with(prefix))
        && base.ends_with('/')
        && base.len() < 128
}

fn client() -> Option<&'static reqwest::Client> {
    static CLIENT: OnceLock<Option<reqwest::Client>> = OnceLock::new();
    CLIENT
        .get_or_init(|| {
            // Bundled Mozilla roots rather than the platform store: the
            // helper also runs under Wine, whose certificate store varies.
            let roots = rustls::RootCertStore {
                roots: webpki_roots::TLS_SERVER_ROOTS.to_vec(),
            };
            let mut tls = rustls::ClientConfig::builder_with_provider(Arc::new(
                rustls::crypto::ring::default_provider(),
            ))
            .with_safe_default_protocol_versions()
            .ok()?
            .with_root_certificates(roots)
            .with_no_client_auth();
            tls.alpn_protocols = vec![b"http/1.1".to_vec()];
            reqwest::Client::builder()
                .tls_backend_preconfigured(tls)
                .http1_only()
                .redirect(reqwest::redirect::Policy::none())
                .connect_timeout(CONNECT_TIMEOUT)
                .timeout(REQUEST_TIMEOUT)
                .user_agent(concat!("sf4-net/", env!("CARGO_PKG_VERSION")))
                .build()
                .ok()
        })
        .as_ref()
}

/// Stores `invitation`, sealed, for `ttl` seconds (clamped to the service's
/// range). Any member of the room may replace the record.
pub async fn publish(
    service: &str,
    keys: &Keys,
    invitation: &str,
    ttl: u64,
) -> Result<(), ShortError> {
    let sealed = seal(keys, invitation.as_bytes()).ok_or(ShortError::Malformed)?;
    let ttl = ttl.clamp(MIN_TTL_SECS, MAX_TTL_SECS) as u32;
    let mut body = Vec::with_capacity(5 + keys.write.len() + sealed.len());
    body.push(WIRE_VERSION);
    body.extend_from_slice(&ttl.to_le_bytes());
    body.extend_from_slice(&keys.write);
    body.extend_from_slice(&sealed);
    let response = client()
        .ok_or(ShortError::Unavailable)?
        .put(format!("{service}{}", keys.locator_text()))
        .header(reqwest::header::CONTENT_TYPE, "application/octet-stream")
        .body(body)
        .send()
        .await
        .map_err(|_| ShortError::Unavailable)?;
    if response.status().is_success() {
        Ok(())
    } else {
        Err(ShortError::Unavailable)
    }
}

/// Fetches and opens the record for `keys`.
pub async fn fetch(service: &str, keys: &Keys) -> Result<String, ShortError> {
    let response = client()
        .ok_or(ShortError::Unavailable)?
        .get(format!("{service}{}", keys.locator_text()))
        .send()
        .await
        .map_err(|_| ShortError::Unavailable)?;
    match response.status() {
        status if status.is_success() => {}
        reqwest::StatusCode::NOT_FOUND => return Err(ShortError::Unknown),
        _ => return Err(ShortError::Unavailable),
    }
    if response
        .content_length()
        .is_some_and(|length| length > MAX_SEALED_BYTES as u64)
    {
        return Err(ShortError::Malformed);
    }
    let sealed = response
        .bytes()
        .await
        .map_err(|_| ShortError::Unavailable)?;
    let plaintext = open(keys, &sealed).ok_or(ShortError::Malformed)?;
    String::from_utf8(plaintext).map_err(|_| ShortError::Malformed)
}

/// Derives the keys for `code` and fetches the invitation it stands for.
/// The caller still checks that the invitation derives `code`.
pub async fn resolve(service: &str, code: String) -> Result<String, ShortError> {
    let keys = derive_async(code).await?;
    fetch(service, &keys).await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_room_has_one_stable_code_in_the_crockford_alphabet() {
        let room = [3; 16];
        let capability = [9; 32];
        let code = code_from_secrets(&room, &capability);
        assert_eq!(code.len(), CODE_SYMBOLS);
        assert!(code.bytes().all(|symbol| ALPHABET.contains(&symbol)));
        assert_eq!(code, code_from_secrets(&room, &capability));
        let mut other = capability;
        other[31] ^= 1;
        assert_ne!(code, code_from_secrets(&room, &other));
        let mut other_room = room;
        other_room[0] ^= 1;
        assert_ne!(code, code_from_secrets(&other_room, &capability));
        let shown = link(&code);
        assert_eq!(shown.len(), LINK_PREFIX.len() + 14);
        assert_eq!(parse(&shown).as_deref(), Some(code.as_str()));
    }

    #[test]
    fn pasted_links_and_codes_normalize_to_the_canonical_code() {
        let code = "7K3M0X1RT9PZ";
        for text in [
            "https://embernetplay.link/j#7K3M-0X1R-T9PZ",
            "https://embernetplay.link/j/7K3M-0X1R-T9PZ",
            "https://embernetplay.link/j/#7k3m0x1rt9pz",
            "HTTPS://WWW.EMBERNETPLAY.LINK/j#7K3M-0X1R-T9PZ\r\n",
            "embernetplay.link/j#7K3M-0X1R-T9PZ",
            "http://embernetplay.link/j/7K3M0X1RT9PZ/",
            "7K3M-0X1R-T9PZ",
            " 7k3m 0x1r t9pz ",
            "7k3mox1rt9pz",
            "7K3M-OXIR-T9PZ",
            "7K3M-0XLR-T9PZ",
        ] {
            assert_eq!(parse(text).as_deref(), Some(code), "{text:?}");
        }
        for text in [
            "",
            "7K3M-0X1R-T9P",
            "7K3M-0X1R-T9PZZ",
            "7K3M-0X1R-T9PU",
            "https://example.com/j#7K3M-0X1R-T9PZ",
            "https://embernetplay.link.evil/j#7K3M-0X1R-T9PZ",
            "https://embernetplay.link/x#7K3M-0X1R-T9PZ",
            "sf4e3:AAAAAAAAAAAA",
            "emd2:7K3M0X1RT9PZ",
            "7K3M_0X1R_T9PZ",
            "7K3M-0X1R-T9PZ\u{e9}",
        ] {
            assert_eq!(parse(text), None, "{text:?}");
        }
        assert_eq!(parse(&"7".repeat(97)), None);
    }

    #[test]
    fn derivation_is_deterministic_and_separates_codes() {
        let first = derive("7K3M0X1RT9PZ").unwrap();
        let again = derive("7K3M0X1RT9PZ").unwrap();
        let other = derive("7K3M0X1RT9PY").unwrap();
        assert_eq!(first.locator, again.locator);
        assert_eq!(first.key, again.key);
        assert_eq!(first.write, again.write);
        assert_ne!(first.locator, other.locator);
        assert_ne!(first.key, other.key);
        assert_ne!(first.write, other.write);
        assert_ne!(&first.key[..16], &first.locator[..]);
        assert_eq!(first.locator_text().len(), 22);
    }

    #[test]
    fn sealed_records_open_only_with_the_right_code_and_locator() {
        let keys = derive("7K3M0X1RT9PZ").unwrap();
        let invitation = format!("sf4e3:{}", "A".repeat(271));
        let sealed = seal(&keys, invitation.as_bytes()).unwrap();
        assert_eq!(open(&keys, &sealed).unwrap(), invitation.as_bytes());
        // A fresh nonce every time.
        assert_ne!(seal(&keys, invitation.as_bytes()).unwrap(), sealed);
        // Wrong code.
        let wrong = derive("7K3M0X1RT9PY").unwrap();
        assert!(open(&wrong, &sealed).is_none());
        // Right key, record moved to another locator.
        let mut moved = keys.clone();
        moved.locator[0] ^= 1;
        assert!(open(&moved, &sealed).is_none());
        // Any flipped byte, or a cut, is refused.
        for index in [0, NONCE_BYTES, sealed.len() - 1] {
            let mut damaged = sealed.clone();
            damaged[index] ^= 0x40;
            assert!(open(&keys, &damaged).is_none());
        }
        assert!(open(&keys, &sealed[..sealed.len() - 1]).is_none());
        assert!(open(&keys, &sealed[..NONCE_BYTES + TAG_BYTES]).is_none());
        assert!(open(&keys, &[]).is_none());
        // Size bounds match the service.
        assert!(seal(&keys, &[]).is_none());
        assert!(seal(&keys, &vec![b'x'; MAX_PLAINTEXT_BYTES + 1]).is_none());
        let largest = seal(&keys, &vec![b'x'; MAX_PLAINTEXT_BYTES]).unwrap();
        assert_eq!(largest.len(), MAX_SEALED_BYTES);
        assert!(open(&keys, &[largest.clone(), vec![0]].concat()).is_none());
    }

    /// Against a running service, such as the deployed one through
    /// `ssh -L 47819:127.0.0.1:47819 vps` and
    /// `SF4E_SHORT_INVITE_SERVICE=http://127.0.0.1:47819/s/v1/`.
    #[tokio::test]
    #[ignore = "needs a running link service; run explicitly"]
    async fn a_running_service_stores_and_returns_a_record() {
        let service = service_base();
        assert_ne!(service, DEFAULT_SERVICE, "set {SERVICE_OVERRIDE}");
        let code = code_from_secrets(&rand::random(), &rand::random());
        let keys = derive(&code).unwrap();
        assert_eq!(fetch(&service, &keys).await, Err(ShortError::Unknown));
        publish(&service, &keys, "sf4e3:first", 120).await.unwrap();
        assert_eq!(
            resolve(&service, code.clone()).await.unwrap(),
            "sf4e3:first"
        );
        publish(&service, &keys, "sf4e3:second", 120).await.unwrap();
        assert_eq!(fetch(&service, &keys).await.unwrap(), "sf4e3:second");
        // Another code's write token cannot replace it.
        let mut other = derive(&code_from_secrets(&rand::random(), &rand::random())).unwrap();
        other.locator = keys.locator;
        assert_eq!(
            publish(&service, &other, "sf4e3:third", 120).await,
            Err(ShortError::Unavailable)
        );
        assert_eq!(fetch(&service, &keys).await.unwrap(), "sf4e3:second");
    }

    /// The pinned HTTPS origin answers with the bundled roots. Before the
    /// service is installed nginx answers 404, which reads as Unknown too.
    #[tokio::test]
    #[ignore = "needs internet access; run explicitly"]
    async fn the_pinned_origin_answers_over_tls() {
        let keys = derive(&code_from_secrets(&rand::random(), &rand::random())).unwrap();
        assert_eq!(
            fetch(DEFAULT_SERVICE, &keys).await,
            Err(ShortError::Unknown)
        );
    }

    #[test]
    fn only_a_loopback_service_may_replace_the_pinned_one() {
        assert!(loopback_service("http://127.0.0.1:47810/s/v1/"));
        assert!(loopback_service("http://[::1]:9/s/v1/"));
        assert!(!loopback_service("http://127.0.0.1:47810/s/v1"));
        assert!(!loopback_service("https://example.com/s/v1/"));
        assert!(!loopback_service("http://127.0.0.1.example.com/s/v1/"));
        assert!(!loopback_service("http://192.168.1.2:80/"));
    }
}
