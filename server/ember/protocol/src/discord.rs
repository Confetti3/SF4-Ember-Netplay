//! A player's Discord account, connected to their Ember ID through Discord's
//! own sign-in on the bridge. Optional: a bridge offers it when it lists the
//! `discord` feature, and a player connects it only if they choose to.
//! Platforms that find players by their Discord account (BluMint's player
//! lookup) then get the Ember ID, and Discord features can mention the player.
pub use crate::sign_in::{Account, Connection, SIGN_IN_SECS, SignInStarted};

/// The `discord` capabilities feature: the bridge offers Discord sign-in.
pub const FEATURE: &str = "discord";
/// The `discord.accounts` capabilities feature: a player can read and
/// disconnect their connected account (`ACCOUNT_PATH`), whether or not
/// sign-in is offered now.
pub const ACCOUNTS_FEATURE: &str = "discord.accounts";
pub const START_PATH: &str = "/v1/discord/start";
pub const CALLBACK_PATH: &str = "/v1/discord/callback";
pub const ACCOUNT_PATH: &str = "/v1/discord";

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
