//! Discord sign-in (`sign_in`), on bridges that list the `discord` feature:
//! the player approves Ember on Discord's own page, and platforms that find
//! players by their Discord account get their Ember ID.
use ember_protocol::{
    challenge::Action,
    discord::{ACCOUNT_PATH, START_PATH},
};

use super::{browser, sign_in::Routes};

/// Where Discord's sign-in page lives.
const SIGN_IN_PAGE: &str = "https://discord.com/oauth2/authorize?";

pub(super) fn routes() -> Routes {
    Routes {
        start_path: START_PATH.into(),
        account_path: ACCOUNT_PATH.into(),
        connect: Action::DiscordConnect,
        remove: Action::DiscordRemove,
        cancel: Action::DiscordCancel,
        end_unopened: false,
    }
}

/// Discord's own sign-in page, with nothing that could break out of a URL.
pub(super) fn is_sign_in_page(url: &str) -> bool {
    url.starts_with(SIGN_IN_PAGE) && browser::is_plain(url)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_discords_sign_in_page_is_opened() {
        assert!(is_sign_in_page(
            "https://discord.com/oauth2/authorize?response_type=code&client_id=1546980049692135514&scope=identify&state=x"
        ));
        for url in [
            "http://discord.com/oauth2/authorize?x",
            "https://discord.com.example/oauth2/authorize?x",
            "https://discord.com/oauth2/authorize",
            "https://example.com/?https://discord.com/oauth2/authorize?",
            "https://discord.com/oauth2/authorize?a b",
            "file:///C:/Windows/System32/calc.exe",
        ] {
            assert!(!is_sign_in_page(url), "{url}");
        }
    }
}
