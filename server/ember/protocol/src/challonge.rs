//! A player's Challonge account, connected to their Ember ID through
//! Challonge's own sign-in on the bridge, the same way as Discord's
//! (`discord`): the player's Ember opens the returned Challonge page in the
//! browser, Challonge sends it back to the bridge, and the bridge's page asks
//! before connecting. Optional: a bridge offers it when it lists the
//! `challonge` feature. Bracket connectors then find players by the
//! Challonge username on a tournament entry (`partner::Lookup::challonge`).
pub use crate::sign_in::{Account, Connection, SignInStarted};

/// The `challonge` capabilities feature: the bridge offers Challonge sign-in.
pub const FEATURE: &str = "challonge";
/// The `challonge.accounts` capabilities feature: a player can read and
/// disconnect their connected account (`ACCOUNT_PATH`), whether or not
/// sign-in is offered now. Listed by a bridge configured for Challonge.
pub const ACCOUNTS_FEATURE: &str = "challonge.accounts";
/// How long a started sign-in waits for Challonge's answer.
pub const SIGN_IN_SECS: u64 = 10 * 60;
pub const START_PATH: &str = "/v1/challonge/start";
pub const CALLBACK_PATH: &str = "/v1/challonge/callback";
pub const ACCOUNT_PATH: &str = "/v1/challonge";
/// The longest Challonge username the bridge accepts.
pub const MAX_USERNAME: usize = 64;

/// A Challonge username as a tournament entry or `/me` gives it: 1 to 64
/// ASCII letters, digits, `_`, `-` or `.`.
pub fn is_username(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= MAX_USERNAME
        && text
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

/// The form usernames are compared in: Challonge treats them without case.
pub fn username_key(text: &str) -> String {
    text.to_ascii_lowercase()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn usernames() {
        for text in ["kate", "Kate_99", "a.b-c", &"x".repeat(MAX_USERNAME)] {
            assert!(is_username(text), "{text}");
        }
        for text in [
            "",
            "has space",
            "kate@example.com",
            "\u{fc}n\u{ef}",
            &"x".repeat(MAX_USERNAME + 1),
        ] {
            assert!(!is_username(text), "{text}");
        }
        assert_eq!(username_key("Kate_99"), "kate_99");
    }
}
