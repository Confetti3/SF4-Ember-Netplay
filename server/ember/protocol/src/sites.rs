//! Sign-in sites: services a player connects their own account on, so
//! platforms that run brackets there find the player's Ember ID
//! (the sign-in sites design, GENERIC_SITES.md). A bridge defines each site in
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
/// How long a started sign-in waits for the site's answer.
pub const SIGN_IN_SECS: u64 = 10 * 60;
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
#[serde(deny_unknown_fields)]
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
    fn lookup_by_is_snake_case() {
        assert_eq!(
            serde_json::to_string(&LookupBy::UserId).unwrap(),
            "\"user_id\""
        );
        let by: LookupBy = serde_json::from_str("\"username\"").unwrap();
        assert_eq!(by, LookupBy::Username);
    }
}
