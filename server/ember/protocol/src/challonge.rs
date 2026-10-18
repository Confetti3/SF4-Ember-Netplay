//! A player's Challonge account, connected to their Ember ID through
//! Challonge's device sign-in: the game shows a short code, the player
//! approves it on Challonge's site, and the bridge reads who signed in.
//! Optional: a bridge offers it when it lists the `challonge` feature.
//! Bracket connectors then find players by the Challonge username on a
//! tournament entry (`partner::Lookup::challonge`).
use serde::{Deserialize, Serialize};

/// The `challonge` capabilities feature: the bridge offers Challonge sign-in.
pub const FEATURE: &str = "challonge";
/// The `challonge.accounts` capabilities feature: a player can read and
/// disconnect their connected account (`ACCOUNT_PATH`), whether or not
/// sign-in is offered now.
pub const ACCOUNTS_FEATURE: &str = "challonge.accounts";
/// The longest a started sign-in waits, whatever Challonge's own code
/// lifetime says.
pub const SIGN_IN_SECS: u64 = 10 * 60;
pub const START_PATH: &str = "/v1/challonge/start";
pub const ACCOUNT_PATH: &str = "/v1/challonge";
pub const MOVE_PATH: &str = "/v1/challonge/move";
/// The longest Challonge username the bridge accepts.
pub const MAX_USERNAME: usize = 64;

/// `POST /v1/challonge/start`: the code to show and where to enter it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignInStarted {
    pub user_code: String,
    pub verification_uri: String,
    /// The same page with the code filled in, when Challonge gives one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub verification_uri_complete: Option<String>,
    pub expires_at: u64,
}

/// `GET /v1/challonge`: the connected account and the sign-in in progress,
/// each `null` when there is none. A pure read: the bridge asks Challonge
/// about a waiting sign-in on its own schedule.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Connection {
    pub account: Option<Account>,
    pub sign_in: Option<SignIn>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Account {
    /// Challonge's user ID.
    pub user_id: String,
    /// The Challonge username when it was connected.
    pub username: String,
    pub connected_at: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignIn {
    pub state: SignInState,
    pub user_code: String,
    pub verification_uri: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub verification_uri_complete: Option<String>,
    pub expires_at: u64,
    /// Set in `moving`: the signed-in account, which another Ember ID holds.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub username: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SignInState {
    /// The code is shown and Challonge has not answered yet.
    Waiting,
    /// The account signed in belongs to another Ember ID: the player
    /// chooses with `POST MOVE_PATH`.
    Moving,
    /// The player refused on Challonge's page.
    Denied,
    /// The code ran out before the player approved it.
    Expired,
    /// Challonge failed or answered something the bridge cannot use.
    Failed,
}

/// `POST /v1/challonge/move`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MoveChoice {
    pub choice: Choice,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Choice {
    /// Move the account here from the other Ember ID.
    Move,
    /// Leave it where it is and end this sign-in.
    Keep,
}

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
        for text in ["", "has space", "kate@example.com", "ünï", &"x".repeat(MAX_USERNAME + 1)] {
            assert!(!is_username(text), "{text}");
        }
        assert_eq!(username_key("Kate_99"), "kate_99");
    }

    #[test]
    fn sign_in_reads_back() {
        let connection = Connection {
            account: None,
            sign_in: Some(SignIn {
                state: SignInState::Moving,
                user_code: "ABCD-1234".into(),
                verification_uri: "https://auth.challonge.com/oauth/device".into(),
                verification_uri_complete: None,
                expires_at: 10,
                username: Some("kate".into()),
            }),
        };
        let text = serde_json::to_string(&connection).unwrap();
        assert!(text.contains("\"state\":\"moving\""), "{text}");
        assert_eq!(serde_json::from_str::<Connection>(&text).unwrap(), connection);
    }
}
