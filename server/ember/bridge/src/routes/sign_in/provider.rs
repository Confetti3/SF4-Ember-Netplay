//! What differs between the sites a player signs in with: their names,
//! routes, proof actions and capabilities features (`Profile`), where the
//! browser goes, the endpoints the code is exchanged and the user read at,
//! how the site says who signed in, and the limits on its requests
//! (`OAuth`). The flow itself is `sign_in`'s. Nothing here logs or formats an error: the requests
//! carry the client secret, the code and the access token, and a reqwest
//! error's text includes the URL.
use ember_protocol::{challenge::Action, challonge, discord};
use reqwest::header;
use serde::Deserialize;
use zeroize::Zeroizing;

use crate::{AppState, linking::Site, util::outbound_client};

/// What a site's sign-in is called, where its routes are, the proof actions
/// that authorize them, how long a sign-in waits, and the capabilities
/// features it is listed under.
pub struct Profile {
    /// The site's name as the browser pages show it.
    pub label: &'static str,
    pub start: &'static str,
    pub callback: &'static str,
    pub account: &'static str,
    pub connect: Action,
    pub cancel: Action,
    pub remove: Action,
    pub sign_in_secs: u64,
    /// Listed when sign-in is offered.
    pub feature: &'static str,
    /// Listed always: a connected account can be read and disconnected.
    pub accounts_feature: &'static str,
}

impl Profile {
    pub fn of(site: Site) -> Self {
        match site {
            Site::Discord => Self {
                label: "Discord",
                start: discord::START_PATH,
                callback: discord::CALLBACK_PATH,
                account: discord::ACCOUNT_PATH,
                connect: Action::DiscordConnect,
                cancel: Action::DiscordCancel,
                remove: Action::DiscordRemove,
                sign_in_secs: discord::SIGN_IN_SECS,
                feature: discord::FEATURE,
                accounts_feature: discord::ACCOUNTS_FEATURE,
            },
            Site::Challonge => Self {
                label: "Challonge",
                start: challonge::START_PATH,
                callback: challonge::CALLBACK_PATH,
                account: challonge::ACCOUNT_PATH,
                connect: Action::ChallongeConnect,
                cancel: Action::ChallongeCancel,
                remove: Action::ChallongeRemove,
                sign_in_secs: challonge::SIGN_IN_SECS,
                feature: challonge::FEATURE,
                accounts_feature: challonge::ACCOUNTS_FEATURE,
            },
        }
    }
}

/// What a site that limits its requests lets the bridge spend: requests per
/// calendar month and per day (UTC), and sign-ins started per hour across
/// the bridge, so throwaway Ember IDs cannot spend a month in one go.
#[derive(Clone, Copy)]
pub struct Limits {
    pub monthly: u64,
    pub daily: u64,
    pub starts_per_hour: usize,
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
    read_user: fn(&[u8]) -> Option<SignedIn>,
    /// For a site that limits its requests; a sign-in's exchange spends two.
    pub limits: Option<Limits>,
}

/// Who signed in.
pub struct User {
    pub id: String,
    pub username: String,
}

/// What the user endpoint said.
pub enum SignedIn {
    User(User),
    /// An account whose username lookups cannot match, as the site gave it
    /// (control characters dropped, at most 64 characters).
    Unusable(String),
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
                    limits: None,
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
                    limits: Some(Limits {
                        monthly: challonge.monthly_request_budget,
                        daily: challonge.daily_request_budget,
                        starts_per_hour: challonge.starts_per_hour,
                    }),
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
    pub async fn user(&self, redirect_uri: &str, code: &str) -> Option<SignedIn> {
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

/// Kept for the page: control characters dropped, at most 64 characters.
fn shown(username: &str) -> String {
    username
        .chars()
        .filter(|c| !c.is_control())
        .take(64)
        .collect()
}

/// Discord's `/users/@me`: a snowflake and a username, kept to 64 visible
/// characters.
fn discord_user(body: &[u8]) -> Option<SignedIn> {
    #[derive(Deserialize)]
    struct Me {
        id: String,
        username: String,
    }
    let me: Me = serde_json::from_slice(body).ok()?;
    let username = shown(&me.username);
    (discord::is_user_id(&me.id) && !username.is_empty()).then_some(SignedIn::User(User {
        id: me.id,
        username,
    }))
}

/// Challonge's `/me`: `data.id`, a string (a number is read too), and
/// `data.attributes.username`, which must be a username lookups can match.
fn challonge_user(body: &[u8]) -> Option<SignedIn> {
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
    if id.is_empty() || id.len() > 32 || !id.bytes().all(|b| b.is_ascii_alphanumeric()) {
        return None;
    }
    Some(if challonge::is_username(&username) {
        SignedIn::User(User { id, username })
    } else {
        SignedIn::Unusable(shown(&username))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Who signed in, as (ID, username), or the unusable username.
    fn read(found: Option<SignedIn>) -> Option<Result<(String, String), String>> {
        found.map(|signed_in| match signed_in {
            SignedIn::User(user) => Ok((user.id, user.username)),
            SignedIn::Unusable(username) => Err(username),
        })
    }

    #[test]
    fn each_site_says_who_signed_in_its_own_way() {
        let user = |id: &str, name: &str| Some(Ok((id.to_owned(), name.to_owned())));
        assert_eq!(
            read(discord_user(
                br#"{"id":"274220342558756145","username":"kate\u0007"}"#
            )),
            user("274220342558756145", "kate")
        );
        assert_eq!(
            read(discord_user(br#"{"id":"0123","username":"kate"}"#)),
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
            assert_eq!(read(challonge_user(body)), user(id, "Kate_99"));
        }
        // A username lookups cannot match is told apart from no answer.
        assert_eq!(
            read(challonge_user(
                br#"{"data":{"id":"1001","attributes":{"username":"has space"}}}"#
            )),
            Some(Err("has space".to_owned()))
        );
        for body in [
            &br#"{"data":{"id":"10-01","attributes":{"username":"kate"}}}"#[..],
            br#"{"id":"1001","username":"kate"}"#,
        ] {
            assert!(challonge_user(body).is_none());
        }
    }
}
