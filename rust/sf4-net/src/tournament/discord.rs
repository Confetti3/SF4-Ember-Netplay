//! Discord sign-in (`sign_in`), on bridges that list the `discord` feature:
//! the player approves Ember on Discord's own page, and platforms that find
//! players by their Discord account get their Ember ID.
use ember_protocol::{
    challenge::Action,
    discord::{ACCOUNT_PATH, START_PATH},
};

use super::{browser, sign_in::Site};

/// Where Discord's sign-in page lives.
const SIGN_IN_PAGE: &str = "https://discord.com/oauth2/authorize?";

pub struct Discord;

impl Site for Discord {
    const START_PATH: &'static str = START_PATH;
    const ACCOUNT_PATH: &'static str = ACCOUNT_PATH;
    const CONNECT: Action = Action::DiscordConnect;
    const REMOVE: Action = Action::DiscordRemove;
    const CANCEL: Action = Action::DiscordCancel;

    fn is_sign_in_page(url: &str) -> bool {
        url.starts_with(SIGN_IN_PAGE) && browser::is_plain(url)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_discords_sign_in_page_is_opened() {
        assert!(Discord::is_sign_in_page(
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
            assert!(!Discord::is_sign_in_page(url), "{url}");
        }
    }
}
