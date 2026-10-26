//! Sign-in sites: services a player connects their own account on, so
//! platforms that run brackets there find the player's Ember ID
//! (docs/guides/INTEGRATIONS.md, "Sign-in sites"). A bridge defines each site in
//! its configuration, as data, under an ID of its choosing; every site runs
//! on the same routes, proof actions, storage and lookup, so a new site needs
//! no code and no protocol change. Discord is not one of them: it keeps its
//! own module (`discord`).
use serde::{Deserialize, Serialize};

use crate::EmberId;

/// The `sites` capabilities feature: the bridge has the site routes below
/// and takes `accounts` in a lookup. Which sites it has is
/// `Capabilities::sign_in_sites`.
pub const FEATURE: &str = "sites";
/// The longest site ID.
pub const MAX_SITE_ID: usize = 32;
const PREFIX: &str = "/v1/sites/";

/// `POST` starts a sign-in on the site, `DELETE` ends the one in flight.
pub fn start_path(site: &str) -> String {
    format!("{PREFIX}{site}/start")
}

/// Where the site sends the player's browser back, and where the bridge's
/// page posts the player's answer.
pub fn callback_path(site: &str) -> String {
    format!("{PREFIX}{site}/callback")
}

/// `GET` reads the player's account on the site, `DELETE` disconnects it.
pub fn account_path(site: &str) -> String {
    format!("{PREFIX}{site}")
}

/// The site a site path names, if `path` is that site's path ending in
/// `suffix` (`/start`, or empty for the account).
pub fn site_of(path: &str, suffix: &str) -> Option<String> {
    path.strip_prefix(PREFIX)?
        .strip_suffix(suffix)
        .filter(|site| is_site_id(site))
        .map(str::to_owned)
}

/// A site ID: 1 to 32 lowercase ASCII letters, digits or `-`. It never holds
/// `:`, so a link's claim `<site>:<user ID>` splits at its first `:`.
pub fn is_site_id(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= MAX_SITE_ID
        && text
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
}

/// A site's user ID, as a lookup may name it and as the bridge stores it: 1
/// to 64 visible ASCII characters. (Discord's are snowflakes,
/// `discord::is_user_id`.)
pub fn is_user_id(text: &str) -> bool {
    !text.is_empty() && text.len() <= 64 && text.bytes().all(|b| b.is_ascii_graphic())
}

/// Whether `host` is a DNS name on the public internet: the rule a bridge
/// holds a sign-in site's hosts to, and Ember a sign-in page's origin. That
/// is two or more labels of 1 to 63 ASCII letters, digits and `-`, none
/// starting or ending with `-`, at most 253 bytes in all; the last label not
/// a number a browser reads as part of an IPv4 address (decimal digits, or
/// `0x` and hex digits), so no IP address in any form; and not a name kept
/// for local or private networks (`localhost`, `.local`, `.internal`,
/// `home.arpa` and the like). Letters may be in either case.
pub fn is_public_host(host: &str) -> bool {
    const PRIVATE: [&str; 11] = [
        "localhost",
        "local",
        "localdomain",
        "internal",
        "intranet",
        "private",
        "corp",
        "lan",
        "home",
        "home.arpa",
        "invalid",
    ];
    let labels: Vec<&str> = host.split('.').collect();
    let named = host.len() <= 253
        && labels.len() >= 2
        && labels.iter().all(|label| {
            (1..=63).contains(&label.len())
                && !label.starts_with('-')
                && !label.ends_with('-')
                && label
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
        });
    let host = host.to_ascii_lowercase();
    named
        && labels.last().is_some_and(|last| !is_number(last))
        && !PRIVATE
            .iter()
            .any(|private| host == *private || host.ends_with(&format!(".{private}")))
}

/// A label a browser reads as part of an IPv4 address.
fn is_number(label: &str) -> bool {
    match label
        .strip_prefix("0x")
        .or_else(|| label.strip_prefix("0X"))
    {
        Some(digits) => digits.bytes().all(|byte| byte.is_ascii_hexdigit()),
        None => label.bytes().all(|byte| byte.is_ascii_digit()),
    }
}

/// What a lookup's `id` is matched against.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LookupBy {
    /// The username the account had when it connected, compared as the
    /// site's definition says (with or without case).
    Username,
    /// The site's user ID, exactly.
    UserId,
}

/// One entry of `Capabilities::sign_in_sites`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SignInSite {
    pub id: String,
    pub display_name: String,
    /// Whether a player can sign in now. A site without it still lets a
    /// player read and disconnect an account connected earlier.
    pub sign_in: bool,
    pub lookup_by: LookupBy,
    /// Where a sign-in sends the player's browser: an Ember opens only a
    /// page on this origin and path that the bridge it trusts names.
    pub authorize_origin: String,
    pub authorize_path: String,
}

/// An account a lookup asks for: on site `site`, the one `id` names
/// (`SignInSite::lookup_by`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SiteAccount {
    pub site: String,
    pub id: String,
}

/// An account a lookup found, linked on the asking connection.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FoundAccount {
    pub site: String,
    /// The ID as the lookup named it.
    pub id: String,
    /// The site's user ID of the account.
    pub user_id: String,
    pub ember_id: EmberId,
    pub participant_id: String,
    /// When the account connected, the last time the bridge saw what it was
    /// called on the site. A site may let an account rename without telling
    /// the bridge, so a connector may refuse an old one.
    pub connected_at: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn site_paths_name_their_site() {
        assert_eq!(start_path("challonge"), "/v1/sites/challonge/start");
        assert_eq!(
            site_of("/v1/sites/startgg/start", "/start").as_deref(),
            Some("startgg")
        );
        assert_eq!(
            site_of(&account_path("startgg"), "").as_deref(),
            Some("startgg")
        );
        for path in [
            "/v1/sites/startgg/callback",
            "/v1/sites//start",
            "/v1/sites/Start.gg/start",
            "/v1/sites/a/b/start",
            "/v1/discord/start",
        ] {
            assert_eq!(site_of(path, "/start"), None, "{path}");
        }
        assert!(is_site_id("challonge-staging") && !is_site_id(&"x".repeat(33)));
    }

    #[test]
    fn only_public_names_are_public_hosts() {
        for host in [
            "api.challonge.com",
            "start.gg",
            "API.Example.org",
            "a-b.c0.io",
            "1.example.com",
            "x.0xyz",
        ] {
            assert!(is_public_host(host), "{host}");
        }
        for host in [
            "",
            "localhost",
            "intranet",
            "10.0.0.1",
            "169.254.169.254",
            "127.1",
            "0x7f.0x1",
            "0X7F.0X1",
            "example.0x",
            "auth.localhost",
            "Auth.LocalHost",
            "metadata.google.internal",
            "printer.local",
            "router.home.arpa",
            "nas.lan",
            "x.invalid",
            "-a.example.com",
            "a-.example.com",
            "a..example.com",
            "example.com.",
            "a_b.example.com",
            "[::1]",
            "user@example.com",
            "example.com:443",
        ] {
            assert!(!is_public_host(host), "{host}");
        }
        assert!(!is_public_host(&format!("{}.com", "a".repeat(64))));
        let long = vec!["a".repeat(63); 4].join(".");
        assert!(long.len() > 253 && !is_public_host(&long));
    }

    #[test]
    fn lookup_by_is_snake_case() {
        assert_eq!(
            serde_json::to_string(&LookupBy::UserId).unwrap(),
            "\"user_id\""
        );
        let by: LookupBy = serde_json::from_str("\"username\"").unwrap();
        assert_eq!(by, LookupBy::Username);
    }
}
