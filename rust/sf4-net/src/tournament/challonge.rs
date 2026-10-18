//! Challonge sign-in (`sign_in`), on bridges that list the `challonge`
//! feature: the player approves Ember on Challonge's own page, and bracket
//! connectors then find them by their Challonge username.
use ember_protocol::{
    challenge::Action,
    challonge::{ACCOUNT_PATH, START_PATH},
};

use super::{browser, sign_in::Site};

/// Challonge's site: its pages are on this domain or a subdomain of it,
/// api.challonge.com among them.
const DOMAIN: &str = "challonge.com";

pub struct Challonge;

impl Site for Challonge {
    const START_PATH: &'static str = START_PATH;
    const ACCOUNT_PATH: &'static str = ACCOUNT_PATH;
    const CONNECT: Action = Action::ChallongeConnect;
    const REMOVE: Action = Action::ChallongeRemove;
    const CANCEL: Action = Action::ChallongeCancel;

    /// An https page on challonge.com or a subdomain, with no user, port or
    /// anything else in its host.
    fn is_sign_in_page(url: &str) -> bool {
        let Some(rest) = url.strip_prefix("https://") else {
            return false;
        };
        let host = rest.split(['/', '?', '#']).next().unwrap_or_default();
        let labels = host.split('.').all(|label| {
            !label.is_empty()
                && label
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
        });
        let host = host.to_ascii_lowercase();
        browser::is_plain(url)
            && labels
            && (host == DOMAIN
                || host
                    .strip_suffix(DOMAIN)
                    .is_some_and(|subdomain| subdomain.ends_with('.')))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_challonges_own_pages_are_opened() {
        for url in [
            "https://api.challonge.com/oauth/authorize?response_type=code&client_id=x&scope=me&state=y",
            "https://challonge.com/oauth/authorize?state=y",
            "https://API.Challonge.com/oauth/authorize#top",
            "https://challonge.com",
        ] {
            assert!(Challonge::is_sign_in_page(url), "{url}");
        }
        for url in [
            "http://api.challonge.com/oauth/authorize",
            "https://challonge.com.example/oauth/authorize",
            "https://evilchallonge.com/oauth/authorize",
            "https://x@api.challonge.com/oauth/authorize",
            "https://api.challonge.com:8443/oauth/authorize",
            "https://challonge.com\\@example.com/",
            "https://.challonge.com/oauth/authorize",
            "https://a..challonge.com/oauth/authorize",
            "https://api.challonge.com/oauth/authorize?a b",
            "https://api.challonge.com/\"authorize",
            "https://example.com/?https://api.challonge.com/oauth/authorize",
            "file:///C:/Windows/System32/calc.exe",
            "",
        ] {
            assert!(!Challonge::is_sign_in_page(url), "{url}");
        }
        let long = format!("https://api.challonge.com/{}", "a".repeat(2048));
        assert!(!Challonge::is_sign_in_page(&long));
    }
}
