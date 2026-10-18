//! What differs between the sites a player signs in with: their routes and
//! proof actions, where the browser goes, the endpoints the code is exchanged
//! and the user read at, and how the site says who signed in. The flow
//! itself is `sign_in`'s. Nothing here logs or formats an error: the requests
//! carry the client secret, the code and the access token, and a reqwest
//! error's text includes the URL.
use ember_protocol::{challenge::Action, challonge, discord};
use reqwest::header;
use serde::Deserialize;
use zeroize::Zeroizing;

use crate::{AppState, linking::Site, util::outbound_client};

/// A site's sign-in routes and the proof actions that authorize them.
pub struct Routes {
    pub start: &'static str,
    pub callback: &'static str,
    pub account: &'static str,
    pub connect: Action,
    pub cancel: Action,
    pub remove: Action,
}

impl Routes {
    pub fn of(site: Site) -> Self {
        match site {
            Site::Discord => Self {
                start: discord::START_PATH,
                callback: discord::CALLBACK_PATH,
                account: discord::ACCOUNT_PATH,
                connect: Action::DiscordConnect,
                cancel: Action::DiscordCancel,
                remove: Action::DiscordRemove,
            },
            Site::Challonge => Self {
                start: challonge::START_PATH,
                callback: challonge::CALLBACK_PATH,
                account: challonge::ACCOUNT_PATH,
                connect: Action::ChallongeConnect,
                cancel: Action::ChallongeCancel,
                remove: Action::ChallongeRemove,
            },
        }
    }
}

/// The site's name as the browser pages show it.
pub fn label(site: Site) -> &'static str {
    match site {
        Site::Discord => "Discord",
        Site::Challonge => "Challonge",
    }
}

/// A site's OAuth application as this bridge is configured for it.
pub struct OAuth {
    authorize_url: String,
    /// Parameters the site's authorize page takes beyond the standard ones.
    authorize_extra: &'static [(&'static str, &'static str)],
    token_url: String,
    me_url: String,
    client_id: String,
    client_secret: Zeroizing<String>,
    scope: String,
    /// Headers the site's user endpoint needs besides the bearer token.
    me_headers: &'static [(&'static str, &'static str)],
    /// Reads who signed in from the user endpoint's answer.
    read_user: fn(&[u8]) -> Option<User>,
    /// Requests to the site per month, for a site that limits them; a
    /// sign-in's exchange spends two.
    pub budget: Option<u64>,
}

/// Who signed in.
pub struct User {
    pub id: String,
    pub username: String,
}

impl OAuth {
    /// The site's sign-in, when the configuration has it and the integration
    /// secrets hold its client secret.
    pub fn of(state: &AppState, site: Site) -> Option<Self> {
        let config = &state.config;
        let secrets = &state.integrations;
        match site {
            Site::Discord => {
                let discord = config.discord.as_ref()?;
                Some(Self {
                    authorize_url: format!(
                        "{}/oauth2/authorize",
                        discord.api_base.trim_end_matches("/api")
                    ),
                    // The consent screen even for an account that approved
                    // Ember before, so the player sees which account it is.
                    authorize_extra: &[("prompt", "consent")],
                    token_url: format!("{}/oauth2/token", discord.api_base),
                    me_url: format!("{}/users/@me", discord.api_base),
                    client_id: discord.client_id.clone(),
                    client_secret: secrets.discord_client_secret.clone()?,
                    scope: "identify".into(),
                    me_headers: &[],
                    read_user: discord_user,
                    budget: None,
                })
            }
            Site::Challonge => {
                let challonge = config.challonge.as_ref()?;
                Some(Self {
                    authorize_url: format!("{}/oauth/authorize", challonge.api_base),
                    authorize_extra: &[],
                    token_url: format!("{}{}", challonge.api_base, challonge.token_path),
                    me_url: format!("{}{}", challonge.api_base, challonge.me_path),
                    client_id: challonge.client_id.clone(),
                    client_secret: secrets.challonge_client_secret.clone()?,
                    scope: challonge.scope.clone(),
                    // Challonge's API takes OAuth tokens only with these.
                    me_headers: &[
                        ("accept", "application/json"),
                        ("content-type", "application/vnd.api+json"),
                        ("authorization-type", "v2"),
                    ],
                    read_user: challonge_user,
                    budget: Some(challonge.monthly_request_budget),
                })
            }
        }
    }

    /// The site's page that asks the player to sign in, carrying `state`.
    pub fn authorize_url(&self, redirect_uri: &str, state: &str) -> Option<String> {
        let params = [
            ("response_type", "code"),
            ("client_id", self.client_id.as_str()),
            ("scope", self.scope.as_str()),
            ("redirect_uri", redirect_uri),
            ("state", state),
        ];
        let params = params
            .into_iter()
            .chain(self.authorize_extra.iter().copied());
        url::Url::parse_with_params(&self.authorize_url, params)
            .ok()
            .map(String::from)
    }

    /// Exchanges the code and reads who signed in, or None on any failure.
    /// The token is dropped here.
    pub async fn user(&self, redirect_uri: &str, code: &str) -> Option<User> {
        #[derive(Deserialize)]
        struct Token {
            access_token: String,
        }
        let client = outbound_client().build().ok()?;
        let form = url::form_urlencoded::Serializer::new(String::new())
            .append_pair("grant_type", "authorization_code")
            .append_pair("code", code)
            .append_pair("redirect_uri", redirect_uri)
            .append_pair("client_id", &self.client_id)
            .append_pair("client_secret", &self.client_secret)
            .finish();
        let response = client
            .post(&self.token_url)
            .header(header::CONTENT_TYPE, "application/x-www-form-urlencoded")
            .body(form)
            .send()
            .await
            .ok()?;
        if !response.status().is_success() {
            return None;
        }
        let token: Token = serde_json::from_slice(&response.bytes().await.ok()?).ok()?;
        let token = Zeroizing::new(token.access_token);
        let request = self
            .me_headers
            .iter()
            .fold(client.get(&self.me_url), |request, (name, value)| {
                request.header(*name, *value)
            });
        let response = request.bearer_auth(token.as_str()).send().await.ok()?;
        if !response.status().is_success() {
            return None;
        }
        (self.read_user)(&response.bytes().await.ok()?)
    }
}

/// Discord's `/users/@me`: a snowflake and a username, kept to 64 visible
/// characters.
fn discord_user(body: &[u8]) -> Option<User> {
    #[derive(Deserialize)]
    struct Me {
        id: String,
        username: String,
    }
    let me: Me = serde_json::from_slice(body).ok()?;
    let username: String = me
        .username
        .chars()
        .filter(|c| !c.is_control())
        .take(64)
        .collect();
    (discord::is_user_id(&me.id) && !username.is_empty()).then_some(User {
        id: me.id,
        username,
    })
}

/// Challonge's `/me`: `data.id`, a string (a number is read too), and
/// `data.attributes.username`, which must be a username lookups can match.
fn challonge_user(body: &[u8]) -> Option<User> {
    #[derive(Deserialize)]
    struct Me {
        data: Data,
    }
    #[derive(Deserialize)]
    struct Data {
        id: Id,
        attributes: Attributes,
    }
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Id {
        Text(String),
        Number(u64),
    }
    #[derive(Deserialize)]
    struct Attributes {
        username: String,
    }
    let me: Me = serde_json::from_slice(body).ok()?;
    let id = match me.data.id {
        Id::Text(id) => id,
        Id::Number(id) => id.to_string(),
    };
    let username = me.data.attributes.username;
    let usable_id =
        !id.is_empty() && id.len() <= 32 && id.bytes().all(|b| b.is_ascii_alphanumeric());
    (usable_id && challonge::is_username(&username)).then_some(User { id, username })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn each_site_says_who_signed_in_its_own_way() {
        let user = |found: Option<User>| found.map(|user| (user.id, user.username));
        assert_eq!(
            user(discord_user(
                br#"{"id":"274220342558756145","username":"kate\u0007"}"#
            )),
            Some(("274220342558756145".into(), "kate".into()))
        );
        assert_eq!(
            user(discord_user(br#"{"id":"0123","username":"kate"}"#)),
            None
        );
        for (body, id) in [
            (
                &br#"{"data":{"id":"1001","attributes":{"username":"Kate_99"}}}"#[..],
                "1001",
            ),
            (
                br#"{"data":{"id":1002,"attributes":{"username":"Kate_99","email":null}}}"#,
                "1002",
            ),
        ] {
            assert_eq!(
                user(challonge_user(body)),
                Some((id.into(), "Kate_99".into()))
            );
        }
        for body in [
            &br#"{"data":{"id":"1001","attributes":{"username":"has space"}}}"#[..],
            br#"{"data":{"id":"10-01","attributes":{"username":"kate"}}}"#,
            br#"{"id":"1001","username":"kate"}"#,
        ] {
            assert!(challonge_user(body).is_none());
        }
    }
}
