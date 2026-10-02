//! A player's Discord account, connected to their Ember ID through Discord's
//! own sign-in on the bridge. Optional: a bridge offers it when it lists the
//! `discord` feature, and a player connects it only if they choose to.
//! Platforms that find players by their Discord account (BluMint's player
//! lookup) then get the Ember ID, and Discord features can mention the player.
use serde::{Deserialize, Serialize};

/// The `discord` capabilities feature.
pub const FEATURE: &str = "discord";
/// How long a started sign-in waits for Discord's answer.
pub const SIGN_IN_SECS: u64 = 10 * 60;
pub const START_PATH: &str = "/v1/discord/start";
pub const CALLBACK_PATH: &str = "/v1/discord/callback";
pub const ACCOUNT_PATH: &str = "/v1/discord";

/// `POST /v1/discord/start`: where to send the player's browser.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignInStarted {
    pub authorize_url: String,
    pub expires_at: u64,
}

/// `GET /v1/discord`: the connected account, or `account: null`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Connection {
    pub account: Option<Account>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Account {
    /// Discord's user ID (a snowflake, decimal).
    pub user_id: String,
    /// The Discord username when it was connected.
    pub username: String,
    pub connected_at: u64,
}

/// A Discord user ID: 1 to 20 decimal digits, no leading zero.
pub fn is_user_id(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= 20
        && !text.starts_with('0')
        && text.bytes().all(|byte| byte.is_ascii_digit())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn user_ids_are_snowflakes() {
        assert!(is_user_id("274220342558756145"));
        for text in [
            "",
            "0123",
            "27422034255875614a",
            "123456789012345678901",
            "discorduser@example.com",
        ] {
            assert!(!is_user_id(text), "{text}");
        }
    }
}
