//! The shapes every sign-in on the bridge shares, Discord's (`discord`) and
//! each configured site's (`sites`): the player's Ember asks to start one
//! and opens the returned page in the browser, the site sends the browser
//! back to the bridge, and the bridge keeps the account's user ID and
//! username.
use serde::{Deserialize, Serialize};

/// `POST <site start path>`: where to send the player's browser.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignInStarted {
    pub authorize_url: String,
    pub expires_at: u64,
}

/// `GET <site account path>`: the connected account, or `account: null`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Connection {
    pub account: Option<Account>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Account {
    /// The site's user ID (for Discord a snowflake, decimal).
    pub user_id: String,
    /// The username when it was connected.
    pub username: String,
    pub connected_at: u64,
}
