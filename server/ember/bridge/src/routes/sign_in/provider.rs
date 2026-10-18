//! What differs between sign-ins: Discord's, built into the bridge, and each
//! configured site's, read from its definition (`config::sites`). `Profile`
//! is what the routes and pages need (the name, paths, proof actions and
//! key domains); `OAuth` is the sign-in itself (where the browser goes, the
//! code exchange, how to read who signed in, and the request limits).
//! Nothing here logs or formats an error: the requests carry the client
//! secret, the code and the access token, and a reqwest error's text
//! includes the URL.
use ember_protocol::{
    challenge::Action,
    discord,
    sites::{self, LookupBy},
};
use reqwest::header;
use serde::Deserialize;
use serde_json::Value;
use zeroize::Zeroizing;

pub use crate::config::sites::Limits;
use crate::{
    AppState,
    config::{
        Config,
        sites::{Encoding, Method, UsernameRule},
    },
    linking::Site,
    util::outbound_client,
};

/// What a sign-in is called, where its routes are, the proof actions that
/// authorize them, how long a sign-in waits, and the names its keys live
/// under.
pub struct Profile {
    site: Site,
    /// The name the browser pages show.
    pub label: String,
    pub start: String,
    pub callback: String,
    pub account: String,
    pub connect: Action,
    pub cancel: Action,
    pub remove: Action,
    pub sign_in_secs: u64,
}

impl Profile {
    pub fn of(config: &Config, site: &Site) -> Self {
        match site {
            Site::Discord => Self {
                site: Site::Discord,
                label: "Discord".into(),
                start: discord::START_PATH.into(),
                callback: discord::CALLBACK_PATH.into(),
                account: discord::ACCOUNT_PATH.into(),
                connect: Action::DiscordConnect,
                cancel: Action::DiscordCancel,
                remove: Action::DiscordRemove,
                sign_in_secs: discord::SIGN_IN_SECS,
            },
            Site::Configured(id) => Self {
                site: site.clone(),
                label: config
                    .sign_in_site(id)
                    .map_or_else(|| id.clone(), |site| site.display_name.clone()),
                start: sites::start_path(id),
                callback: sites::callback_path(id),
                account: sites::account_path(id),
                connect: Action::SiteConnect,
                cancel: Action::SiteCancel,
                remove: Action::SiteRemove,
                sign_in_secs: sites::SIGN_IN_SECS,
            },
        }
    }

    /// The name of this sign-in's keys for `purpose` (hash domains, rate
    /// limits): Discord's as they always were, a configured site's apart
    /// from every other site's.
    pub fn domain(&self, purpose: &str) -> String {
        match &self.site {
            Site::Discord => format!("discord-{purpose}"),
            Site::Configured(id) => format!("site-{purpose}:{id}"),
        }
    }
}

/// A sign-in's OAuth application as this bridge is configured for it.
pub struct OAuth {
    authorize_url: String,
    /// Parameters the authorize page takes beyond the standard ones.
    authorize_params: Vec<(String, String)>,
    token_url: String,
    token_encoding: Encoding,
    /// Fields the token request carries beyond the standard ones.
    token_params: Vec<(String, String)>,
    client_id: String,
    client_secret: Zeroizing<String>,
    scope: String,
    user: UserRequest,
    read: Read,
    /// For a site that limits its requests; a sign-in's exchange spends two.
    pub limits: Option<Limits>,
}

/// The request that reads who signed in, with the access token as a bearer.
struct UserRequest {
    method: Method,
    url: String,
    headers: Vec<(String, String)>,
    body: Option<Value>,
}

/// How the user endpoint's answer says who signed in.
enum Read {
    Discord,
    /// A configured site's JSON pointers. `username_rule` is enforced when
    /// lookups match usernames (`enforced`); otherwise the username is only
    /// shown, and kept to 64 visible characters.
    Pointers {
        id: String,
        username: Option<String>,
        username_rule: UsernameRule,
        enforced: bool,
    },
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
    /// The sign-in, when the configuration has it and the integration
    /// secrets hold its client secret.
    pub fn of(state: &AppState, site: &Site) -> Option<Self> {
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
                    authorize_params: vec![("prompt".into(), "consent".into())],
                    token_url: format!("{}/oauth2/token", discord.api_base),
                    token_encoding: Encoding::Form,
                    token_params: Vec::new(),
                    client_id: discord.client_id.clone(),
                    client_secret: secrets.discord_client_secret.clone()?,
                    scope: "identify".into(),
                    user: UserRequest {
                        method: Method::Get,
                        url: format!("{}/users/@me", discord.api_base),
                        headers: Vec::new(),
                        body: None,
                    },
                    read: Read::Discord,
                    limits: None,
                })
            }
            Site::Configured(id) => {
                let site = config.sign_in_site(id)?;
                let pairs = |params: &std::collections::BTreeMap<String, String>| {
                    params
                        .iter()
                        .map(|(name, value)| (name.clone(), value.clone()))
                        .collect()
                };
                Some(Self {
                    authorize_url: site.authorize_url.clone(),
                    authorize_params: pairs(&site.authorize_params),
                    token_url: site.token_url.clone(),
                    token_encoding: site.token_encoding,
                    token_params: pairs(&site.token_params),
                    client_id: site.client_id.clone(),
                    client_secret: secrets.site_client_secrets.get(id)?.clone(),
                    scope: site.scope.clone(),
                    user: UserRequest {
                        method: site.user.method,
                        url: site.user.url.clone(),
                        headers: pairs(&site.user.headers),
                        body: site.user.body.clone(),
                    },
                    read: Read::Pointers {
                        id: site.user.id_pointer.clone(),
                        username: site.user.username_pointer.clone(),
                        username_rule: site.username_rule.clone(),
                        enforced: site.lookup_by == LookupBy::Username,
                    },
                    limits: site.limits,
                })
            }
        }
    }

    /// The form lookups by username match `username` in, for a configured
    /// site whose lookups name usernames. Other sites' accounts have none:
    /// their usernames are only shown, and may be shared by many accounts.
    pub fn username_key(&self, username: &str) -> Option<String> {
        match &self.read {
            Read::Pointers {
                username_rule,
                enforced: true,
                ..
            } => Some(username_rule.key(username)),
            _ => None,
        }
    }

    /// The site's page that asks the player to sign in, carrying `state`.
    pub fn authorize_url(&self, redirect_uri: &str, state: &str) -> Option<String> {
        let standard = [
            ("response_type", "code"),
            ("client_id", self.client_id.as_str()),
            ("scope", self.scope.as_str()),
            ("redirect_uri", redirect_uri),
            ("state", state),
        ];
        let params = standard
            .into_iter()
            .filter(|(name, value)| *name != "scope" || !value.is_empty())
            .chain(
                self.authorize_params
                    .iter()
                    .map(|(name, value)| (name.as_str(), value.as_str())),
            );
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
        let fields = [
            ("grant_type", "authorization_code"),
            ("code", code),
            ("redirect_uri", redirect_uri),
            ("client_id", self.client_id.as_str()),
            ("client_secret", self.client_secret.as_str()),
        ];
        let fields = fields.into_iter().chain(
            self.token_params
                .iter()
                .map(|(name, value)| (name.as_str(), value.as_str())),
        );
        let (content_type, body) = match self.token_encoding {
            Encoding::Form => (
                "application/x-www-form-urlencoded",
                Zeroizing::new(
                    url::form_urlencoded::Serializer::new(String::new())
                        .extend_pairs(fields)
                        .finish(),
                ),
            ),
            Encoding::Json => (
                "application/json",
                Zeroizing::new(
                    serde_json::to_string(
                        &fields
                            .map(|(name, value)| (name.to_owned(), Value::from(value)))
                            .collect::<serde_json::Map<String, Value>>(),
                    )
                    .ok()?,
                ),
            ),
        };
        let response = client
            .post(&self.token_url)
            .header(header::CONTENT_TYPE, content_type)
            .body(body.as_str().to_owned())
            .send()
            .await
            .ok()?;
        if !response.status().is_success() {
            return None;
        }
        let token: Token = serde_json::from_slice(&response.bytes().await.ok()?).ok()?;
        let token = Zeroizing::new(token.access_token);
        let request = match self.user.method {
            Method::Get => client.get(&self.user.url),
            Method::Post => client.post(&self.user.url),
        };
        // A body is JSON unless the definition's own `Content-Type` says
        // otherwise: its headers replace the default, so none goes twice.
        let mut headers = header::HeaderMap::new();
        if self.user.body.is_some() {
            headers.insert(
                header::CONTENT_TYPE,
                header::HeaderValue::from_static("application/json"),
            );
        }
        for (name, value) in &self.user.headers {
            headers.insert(
                header::HeaderName::from_bytes(name.as_bytes()).ok()?,
                header::HeaderValue::from_str(value).ok()?,
            );
        }
        let request = match &self.user.body {
            Some(body) => request.body(body.to_string()),
            None => request,
        };
        let response = request
            .headers(headers)
            .bearer_auth(token.as_str())
            .send()
            .await
            .ok()?;
        if !response.status().is_success() {
            return None;
        }
        let body = response.bytes().await.ok()?;
        match &self.read {
            Read::Discord => discord_user(&body),
            Read::Pointers {
                id,
                username,
                username_rule,
                enforced,
            } => pointed_user(&body, id, username.as_deref(), username_rule, *enforced),
        }
    }
}

/// A site's user ID, as a lookup may name it: 1 to 64 visible ASCII
/// characters.
pub fn is_user_id(text: &str) -> bool {
    !text.is_empty() && text.len() <= 64 && text.bytes().all(|b| b.is_ascii_graphic())
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

/// A configured site's answer, read through its pointers. A string or a
/// whole number is read as text; without a username the user ID is shown.
fn pointed_user(
    body: &[u8],
    id: &str,
    username: Option<&str>,
    rule: &UsernameRule,
    enforced: bool,
) -> Option<SignedIn> {
    let answer: Value = serde_json::from_slice(body).ok()?;
    let text = |pointer: &str| match answer.pointer(pointer)? {
        Value::String(text) => Some(text.clone()),
        Value::Number(number) if number.is_u64() || number.is_i64() => Some(number.to_string()),
        _ => None,
    };
    let id = text(id).filter(|id| is_user_id(id))?;
    let username = username.and_then(text).unwrap_or_else(|| id.clone());
    Some(match enforced {
        true if !rule.allows(&username) => SignedIn::Unusable(shown(&username)),
        true => SignedIn::User(User { id, username }),
        false => SignedIn::User(User {
            username: shown(&username),
            id,
        }),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::sites::Characters;

    /// Who signed in, as (ID, username), or the unusable username.
    fn read(found: Option<SignedIn>) -> Option<Result<(String, String), String>> {
        found.map(|signed_in| match signed_in {
            SignedIn::User(user) => Ok((user.id, user.username)),
            SignedIn::Unusable(username) => Err(username),
        })
    }

    #[test]
    fn discord_says_who_signed_in_as_it_always_did() {
        assert_eq!(
            read(discord_user(
                br#"{"id":"274220342558756145","username":"kate\u0007"}"#
            )),
            Some(Ok(("274220342558756145".into(), "kate".into())))
        );
        assert_eq!(
            read(discord_user(br#"{"id":"0123","username":"kate"}"#)),
            None
        );
    }

    #[test]
    fn a_site_says_who_signed_in_where_its_pointers_point() {
        let rule = UsernameRule {
            characters: Characters::LettersDigits,
            also: "_".into(),
            max_length: 64,
            case_insensitive: true,
        };
        let user = |body: &[u8], enforced| {
            read(pointed_user(
                body,
                "/data/id",
                Some("/data/name"),
                &rule,
                enforced,
            ))
        };
        // A string ID, or a number read as its digits.
        assert_eq!(
            user(br#"{"data":{"id":"1001","name":"Kate_99"}}"#, true),
            Some(Ok(("1001".into(), "Kate_99".into())))
        );
        assert_eq!(
            user(br#"{"data":{"id":1002,"name":"Kate_99"}}"#, true),
            Some(Ok(("1002".into(), "Kate_99".into())))
        );
        // A username lookups cannot match is told apart from no answer,
        // where lookups match usernames; elsewhere it is only shown.
        assert_eq!(
            user(br#"{"data":{"id":"1","name":"has space"}}"#, true),
            Some(Err("has space".into()))
        );
        assert_eq!(
            user(br#"{"data":{"id":"1","name":"has space"}}"#, false),
            Some(Ok(("1".into(), "has space".into())))
        );
        // No username: the ID is shown. No ID: no answer.
        assert_eq!(
            user(br#"{"data":{"id":7,"name":null}}"#, false),
            Some(Ok(("7".into(), "7".into())))
        );
        for body in [
            &br#"{"data":{"name":"kate"}}"#[..],
            br#"{"data":{"id":1.5,"name":"kate"}}"#,
            br#"{"data":{"id":"has space","name":"kate"}}"#,
            br#"not json"#,
        ] {
            assert!(user(body, true).is_none());
        }
    }
}
