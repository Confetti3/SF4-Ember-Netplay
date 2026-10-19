//! What differs between sign-ins: Discord's, built into the bridge, and each
//! configured site's, read from its definition (`config::sites`). `Site`'s
//! methods here are what the routes and pages need (the name, paths, proof
//! targets and key domains); `OAuth` is the sign-in itself (where the
//! browser goes, the code exchange, how to read who signed in, and the
//! request limits).
//! Nothing here logs or formats an error: the requests carry the client
//! secret, the code and the access token, and a reqwest error's text
//! includes the URL.
use ember_protocol::{
    challenge::{Action, Method as ProofMethod},
    discord,
    sites::{self, LookupBy},
};
use reqwest::header;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use zeroize::Zeroizing;

pub use crate::config::sites::Limits;
use crate::{
    AppState,
    config::{
        Config,
        sites::{CreateRequest, Encoding, Method, TournamentsRead, UsernameRule},
    },
    linking::Site,
    util::{outbound_client, read_bounded},
};

/// What a proof authorizes on a sign-in's routes.
#[derive(Clone, Copy)]
pub enum Step {
    /// `POST <start>`.
    Connect,
    /// `DELETE <start>`.
    Cancel,
    /// `DELETE <account>`.
    Remove,
    /// `POST <organize>`: an organizer sign-in.
    Organize,
    /// `DELETE <organize>`: drops the organizer grant, keeps the account.
    Withdraw,
}

impl Site {
    pub fn start_path(&self) -> String {
        match self {
            Self::Discord => discord::START_PATH.into(),
            Self::Configured(id) => sites::start_path(id),
        }
    }

    pub fn callback_path(&self) -> String {
        match self {
            Self::Discord => discord::CALLBACK_PATH.into(),
            Self::Configured(id) => sites::callback_path(id),
        }
    }

    pub fn account_path(&self) -> String {
        match self {
            Self::Discord => discord::ACCOUNT_PATH.into(),
            Self::Configured(id) => sites::account_path(id),
        }
    }

    /// Where an organizer sign-in starts; Discord offers none.
    pub fn organize_path(&self) -> String {
        sites::organize_path(self.id())
    }

    /// The action, method and path a proof for `step` names.
    pub fn target(&self, step: Step) -> (Action, ProofMethod, String) {
        // Discord's action, a configured site's, the method and the path.
        let (discord, site, method, path) = match step {
            Step::Connect => (
                Action::DiscordConnect,
                Action::SiteConnect,
                ProofMethod::Post,
                self.start_path(),
            ),
            Step::Cancel => (
                Action::DiscordCancel,
                Action::SiteCancel,
                ProofMethod::Delete,
                self.start_path(),
            ),
            Step::Remove => (
                Action::DiscordRemove,
                Action::SiteRemove,
                ProofMethod::Delete,
                self.account_path(),
            ),
            // The connect and remove actions, told apart by their path.
            Step::Organize => (
                Action::DiscordConnect,
                Action::SiteConnect,
                ProofMethod::Post,
                self.organize_path(),
            ),
            Step::Withdraw => (
                Action::DiscordRemove,
                Action::SiteRemove,
                ProofMethod::Delete,
                self.organize_path(),
            ),
        };
        let action = if *self == Self::Discord {
            discord
        } else {
            site
        };
        (action, method, path)
    }

    /// The name the browser pages show: the site's display name, or its ID
    /// once it is out of the configuration.
    pub fn label(&self, config: &Config) -> String {
        match self {
            Self::Discord => "Discord".into(),
            Self::Configured(id) => config
                .sign_in_site(id)
                .map_or_else(|| id.clone(), |site| site.display_name.clone()),
        }
    }

    /// The name of this sign-in's keys for `purpose` (hash domains, rate
    /// limits): Discord's as they always were, a configured site's apart
    /// from every other site's.
    pub fn domain(&self, purpose: &str) -> String {
        match self {
            Self::Discord => format!("discord-{purpose}"),
            Self::Configured(id) => format!("site-{purpose}:{id}"),
        }
    }
}

/// The most of a site's token or user answer the bridge reads. A site is a
/// third party even when its definition is trusted, and both answers are a
/// few hundred bytes.
const MAX_ANSWER: usize = 64 * 1024;

/// An organizer's events, a few hundred bytes each.
const MAX_LISTING: usize = 512 * 1024;

/// The most events a listing gives.
pub const MAX_TOURNAMENTS: usize = 100;

/// A sign-in's OAuth application as this bridge is configured for it.
pub struct OAuth {
    authorize_url: String,
    /// Parameters the authorize page takes beyond the standard ones.
    authorize_params: Vec<(String, String)>,
    token_url: String,
    /// Where a refresh token is exchanged.
    refresh_url: String,
    token_encoding: Encoding,
    /// Fields the token request carries beyond the standard ones.
    token_params: Vec<(String, String)>,
    client_id: String,
    client_secret: Zeroizing<String>,
    scope: String,
    user: UserRequest,
    read: Read,
    /// How an organizer sign-in's site lists the organizer's events.
    tournaments: Option<(UserRequest, TournamentsRead)>,
    /// How an organizer sign-in's site creates an event for the organizer.
    create: Option<CreateRequest>,
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
    /// A configured site's JSON pointers. `username_rule` is the site's
    /// when its lookups match usernames, and the username must keep to it;
    /// otherwise there is none, and the username is only shown, kept to 64
    /// visible characters.
    Pointers {
        id: String,
        username: Option<String>,
        username_rule: Option<UsernameRule>,
    },
}

/// An event an organizer runs, as the site lists it: the ID the bracket
/// connector takes, and the name shown.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Tournament {
    pub id: String,
    pub name: String,
}

/// Who signed in.
pub struct User {
    pub id: String,
    pub username: String,
}

/// The tokens a code or refresh token was exchanged for.
pub struct Tokens {
    pub access: Zeroizing<String>,
    pub refresh: Option<Zeroizing<String>>,
    /// How long the access token lasts, when the site says.
    pub expires_in: Option<u64>,
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
                    refresh_url: format!("{}/oauth2/token", discord.api_base),
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
                    tournaments: None,
                    create: None,
                    limits: None,
                })
            }
            Site::Configured(id) => {
                let site = config.sign_in_site(id)?;
                Some(Self {
                    authorize_url: site.authorize_url.clone(),
                    authorize_params: pairs(&site.authorize_params),
                    token_url: site.token_url.clone(),
                    refresh_url: site.token_url.clone(),
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
                        username_rule: (site.lookup_by == LookupBy::Username)
                            .then(|| site.username_rule.clone()),
                    },
                    tournaments: None,
                    create: None,
                    limits: site.limits,
                })
            }
        }
    }

    /// The scopes the sign-in asks for.
    pub fn scope(&self) -> &str {
        &self.scope
    }

    /// The site's organizer sign-in, when its definition offers one: its
    /// organizer scope in place of `scope`, in the token request too.
    pub fn organizer(state: &AppState, site: &Site) -> Option<Self> {
        let Site::Configured(id) = site else {
            return None;
        };
        let organizer = state.config.sign_in_site(id)?.organizer.as_ref()?;
        let mut oauth = Self::of(state, site)?;
        oauth.scope = organizer.scope.clone();
        for (name, value) in &mut oauth.token_params {
            if name == "scope" {
                *value = organizer.scope.clone();
            }
        }
        if let Some(url) = &organizer.refresh_url {
            oauth.refresh_url = url.clone();
        }
        oauth.tournaments = organizer.tournaments.as_ref().map(|read| {
            let request = UserRequest {
                method: read.method,
                url: read.url.clone(),
                headers: pairs(&read.headers),
                body: read.body.clone(),
            };
            (request, read.clone())
        });
        oauth.create = organizer.create.clone();
        Some(oauth)
    }

    /// The form lookups by username match `username` in, for a configured
    /// site whose lookups name usernames. Other sites' accounts have none:
    /// their usernames are only shown, and may be shared by many accounts.
    pub fn username_key(&self, username: &str) -> Option<String> {
        match &self.read {
            Read::Pointers {
                username_rule: Some(rule),
                ..
            } => Some(rule.key(username)),
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
    /// The tokens come back too; a player sign-in drops them.
    pub async fn user(&self, redirect_uri: &str, code: &str) -> Option<(SignedIn, Tokens)> {
        let client = outbound_client().build().ok()?;
        let tokens = self
            .tokens(
                &client,
                &self.token_url,
                [
                    ("grant_type", "authorization_code"),
                    ("code", code),
                    ("redirect_uri", redirect_uri),
                ],
            )
            .await?;
        let body = send(&client, &self.user, &tokens.access, MAX_ANSWER).await?;
        let user = match &self.read {
            Read::Discord => discord_user(&body),
            Read::Pointers {
                id,
                username,
                username_rule,
            } => pointed_user(&body, id, username.as_deref(), username_rule.as_ref()),
        }?;
        Some((user, tokens))
    }

    /// The events the organizer whose access token is `token` runs, at
    /// most `MAX_TOURNAMENTS`, or None on any failure or when the site
    /// lists none.
    pub async fn tournaments(&self, token: &str) -> Option<Vec<Tournament>> {
        let (request, read) = self.tournaments.as_ref()?;
        let client = outbound_client().build().ok()?;
        let body = send(&client, request, token, MAX_LISTING).await?;
        Some(listed(&serde_json::from_slice(&body).ok()?, read))
    }

    /// The request that creates an event titled `title` with the
    /// organizer's `fields`: None when the site creates none, a refusal
    /// when a field is not one the site takes, or not one of its values.
    pub fn creation(
        &self,
        title: &str,
        fields: &serde_json::Map<String, Value>,
    ) -> Option<Result<Creation, String>> {
        let create = self.create.as_ref()?;
        let mut body = create.body.clone();
        set(&mut body, &create.name_pointer, Value::from(title));
        for (name, value) in fields {
            match create.fields.get(name) {
                Some(field) if field.values.contains(value) => {
                    set(&mut body, &field.pointer, value.clone());
                }
                _ => {
                    let known: Vec<&str> = create.fields.keys().map(String::as_str).collect();
                    return Some(Err(format!(
                        "{name} is not a setting this site takes with that value; it takes {}.",
                        known.join(", ")
                    )));
                }
            }
        }
        Some(Ok(Creation {
            request: UserRequest {
                method: Method::Post,
                url: create.url.clone(),
                headers: pairs(&create.headers),
                body: Some(body),
            },
            read: create.clone(),
        }))
    }

    /// Exchanges an organizer's refresh token for fresh tokens, or None on
    /// any failure.
    pub async fn refresh(&self, redirect_uri: &str, refresh_token: &str) -> Option<Tokens> {
        let client = outbound_client().build().ok()?;
        self.tokens(
            &client,
            &self.refresh_url,
            [
                ("grant_type", "refresh_token"),
                ("refresh_token", refresh_token),
                ("redirect_uri", redirect_uri),
            ],
        )
        .await
    }

    /// Posts a token request to `url`: `grant` with the client's ID and
    /// secret and the definition's `token_params`.
    async fn tokens(
        &self,
        client: &reqwest::Client,
        url: &str,
        grant: [(&str, &str); 3],
    ) -> Option<Tokens> {
        #[derive(Deserialize)]
        struct Token {
            access_token: String,
            #[serde(default)]
            refresh_token: Option<String>,
            #[serde(default)]
            expires_in: Option<u64>,
        }
        let fields = grant.into_iter().chain([
            ("client_id", self.client_id.as_str()),
            ("client_secret", self.client_secret.as_str()),
        ]);
        let fields = fields.chain(
            self.token_params
                .iter()
                .map(|(name, value)| (name.as_str(), value.as_str())),
        );
        // The body carries the client secret and the code. It goes to the
        // request by value, without a copy left behind here.
        let (content_type, body) = match self.token_encoding {
            Encoding::Form => (
                "application/x-www-form-urlencoded",
                url::form_urlencoded::Serializer::new(String::new())
                    .extend_pairs(fields)
                    .finish(),
            ),
            Encoding::Json => (
                "application/json",
                serde_json::to_string(
                    &fields
                        .map(|(name, value)| (name.to_owned(), Value::from(value)))
                        .collect::<serde_json::Map<String, Value>>(),
                )
                .ok()?,
            ),
        };
        let response = client
            .post(url)
            .header(header::CONTENT_TYPE, content_type)
            .body(body)
            .send()
            .await
            .ok()?;
        if !response.status().is_success() {
            return None;
        }
        let token: Token =
            serde_json::from_slice(&read_bounded(response, MAX_ANSWER).await?).ok()?;
        Some(Tokens {
            access: Zeroizing::new(token.access_token),
            refresh: token.refresh_token.map(Zeroizing::new),
            expires_in: token.expires_in,
        })
    }
}

/// An event to create on a site (`OAuth::creation`).
pub struct Creation {
    request: UserRequest,
    read: CreateRequest,
}

/// An event a site created: the ID the bracket connector takes, and its
/// page when the site says it.
pub struct Created {
    pub id: String,
    pub link: Option<String>,
}

impl Creation {
    /// Creates the event as the organizer whose access token is `token`;
    /// None on any failure.
    pub async fn send(&self, token: &str) -> Option<Created> {
        let client = outbound_client().build().ok()?;
        let answer: Value =
            serde_json::from_slice(&send(&client, &self.request, token, MAX_ANSWER).await?).ok()?;
        let id = text_at(&answer, &self.read.id_pointer)
            .filter(|id| id.len() <= 128 && id.bytes().all(|b| b.is_ascii_graphic()))?;
        let link = self
            .read
            .link_pointer
            .as_deref()
            .and_then(|pointer| text_at(&answer, pointer))
            .filter(|path| path.bytes().all(|b| b.is_ascii_graphic()))
            .map(|path| format!("{}{path}", self.read.link_prefix.as_deref().unwrap_or("")));
        Some(Created { id, link })
    }
}

/// Puts `value` at `pointer` in `body`, whose parent object the definition
/// was checked to have (`config::sites`).
fn set(body: &mut Value, pointer: &str, value: Value) {
    if let Some((parent, name)) = pointer.rsplit_once('/')
        && let Some(object) = body.pointer_mut(parent).and_then(Value::as_object_mut)
    {
        object.insert(name.to_owned(), value);
    }
}

/// A definition's parameters or headers, in order.
fn pairs(params: &std::collections::BTreeMap<String, String>) -> Vec<(String, String)> {
    params
        .iter()
        .map(|(name, value)| (name.clone(), value.clone()))
        .collect()
}

/// Sends `request` with `token` as a bearer; the answer's body, at most
/// `limit` bytes, if the site answers with success.
async fn send(
    client: &reqwest::Client,
    request: &UserRequest,
    token: &str,
    limit: usize,
) -> Option<Vec<u8>> {
    let builder = match request.method {
        Method::Get => client.get(&request.url),
        Method::Post => client.post(&request.url),
    };
    // A body is JSON unless the definition's own `Content-Type` says
    // otherwise: its headers replace the default, so none goes twice.
    let mut headers = header::HeaderMap::new();
    if request.body.is_some() {
        headers.insert(
            header::CONTENT_TYPE,
            header::HeaderValue::from_static("application/json"),
        );
    }
    for (name, value) in &request.headers {
        headers.insert(
            header::HeaderName::from_bytes(name.as_bytes()).ok()?,
            header::HeaderValue::from_str(value).ok()?,
        );
    }
    let builder = match &request.body {
        Some(body) => builder.body(body.to_string()),
        None => builder,
    };
    let response = builder
        .headers(headers)
        .bearer_auth(token)
        .send()
        .await
        .ok()?;
    if !response.status().is_success() {
        return None;
    }
    read_bounded(response, limit).await
}

/// Text at `pointer` in `value`: a string, or a whole number of zero or
/// more as its decimal digits.
fn text_at(value: &Value, pointer: &str) -> Option<String> {
    match value.pointer(pointer)? {
        Value::String(text) => Some(text.clone()),
        Value::Number(number) if number.is_u64() => Some(number.to_string()),
        _ => None,
    }
}

/// The events a listing's answer holds, read through `read`'s pointers.
/// One without an ID of 1 to 128 visible characters, or a name, is left
/// out; names are kept as usernames are shown.
fn listed(answer: &Value, read: &TournamentsRead) -> Vec<Tournament> {
    let tournaments = answer
        .pointer(&read.list_pointer)
        .and_then(Value::as_array)
        .map(Vec::as_slice)
        .unwrap_or_default();
    let mut found = Vec::new();
    for tournament in tournaments {
        let events = match &read.events_pointer {
            Some(pointer) => tournament
                .pointer(pointer)
                .and_then(Value::as_array)
                .map(Vec::as_slice)
                .unwrap_or_default(),
            None => std::slice::from_ref(tournament),
        };
        let prefix = read
            .tournament_name_pointer
            .as_deref()
            .and_then(|pointer| text_at(tournament, pointer));
        for event in events {
            let id = text_at(event, &read.id_pointer)
                .filter(|id| id.len() <= 128 && id.bytes().all(|b| b.is_ascii_graphic()));
            let name = text_at(event, &read.name_pointer).map(|name| shown(&name));
            if let (Some(id), Some(name)) = (id, name.filter(|name| !name.is_empty())) {
                let name = match &prefix {
                    Some(prefix) => shown(&format!("{}: {name}", shown(prefix))),
                    None => name,
                };
                found.push(Tournament { id, name });
            }
        }
    }
    found.truncate(MAX_TOURNAMENTS);
    found
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

/// A configured site's answer, read through its pointers. A string, or a
/// whole number of zero or more read as its decimal digits, is text. With a
/// `rule` (lookups match usernames) the account needs a username that keeps
/// to it: one that does not is unusable, and none (or nothing shown of it)
/// is no answer. Without one, the username is only shown, and the user ID
/// is shown when there is none.
fn pointed_user(
    body: &[u8],
    id: &str,
    username: Option<&str>,
    rule: Option<&UsernameRule>,
) -> Option<SignedIn> {
    let answer: Value = serde_json::from_slice(body).ok()?;
    let text = |pointer: &str| text_at(&answer, pointer);
    let id = text(id).filter(|id| sites::is_user_id(id))?;
    let username = username
        .and_then(text)
        .filter(|name| !shown(name).is_empty());
    Some(match (rule, username) {
        (Some(_), None) => return None,
        (Some(rule), Some(username)) if !rule.allows(&username) => {
            SignedIn::Unusable(shown(&username))
        }
        (Some(_), Some(username)) => SignedIn::User(User { id, username }),
        (None, username) => SignedIn::User(User {
            username: username.map_or_else(|| id.clone(), |name| shown(&name)),
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
    fn a_listing_reads_events_inside_tournaments_and_skips_what_it_cannot_read() {
        let read: TournamentsRead = serde_json::from_value(serde_json::json!({
            "method": "POST",
            "url": "https://api.start.gg/gql/alpha",
            "list_pointer": "/data/currentUser/tournaments/nodes",
            "events_pointer": "/events",
            "id_pointer": "/slug",
            "name_pointer": "/name",
            "tournament_name_pointer": "/name",
        }))
        .unwrap();
        let answer = serde_json::json!({ "data": { "currentUser": { "tournaments": { "nodes": [
            { "name": "Ember Weekly 12", "events": [
                { "name": "USF4 Singles", "slug": "tournament/ember-weekly-12/event/usf4-singles" },
                { "name": "No slug" },
                { "name": "", "slug": "tournament/ember-weekly-12/event/blank" },
            ] },
            { "name": "No events" },
        ] } } } });
        assert_eq!(
            listed(&answer, &read),
            [Tournament {
                id: "tournament/ember-weekly-12/event/usf4-singles".into(),
                name: "Ember Weekly 12: USF4 Singles".into(),
            }]
        );
        assert!(listed(&serde_json::json!({ "errors": [] }), &read).is_empty());
    }

    #[test]
    fn discord_says_who_signed_in_as_it_always_did() {
        assert_eq!(
            read(discord_user(
                br#"{"id":"274220342558756145","username":"player1\u0007"}"#
            )),
            Some(Ok(("274220342558756145".into(), "player1".into())))
        );
        assert_eq!(
            read(discord_user(br#"{"id":"0123","username":"player1"}"#)),
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
        let user = |body: &[u8], by_username: bool| {
            read(pointed_user(
                body,
                "/data/id",
                Some("/data/name"),
                by_username.then_some(&rule),
            ))
        };
        // A string ID, or a number read as its digits.
        assert_eq!(
            user(br#"{"data":{"id":"1001","name":"Player_1"}}"#, true),
            Some(Ok(("1001".into(), "Player_1".into())))
        );
        assert_eq!(
            user(br#"{"data":{"id":1002,"name":"Player_1"}}"#, true),
            Some(Ok(("1002".into(), "Player_1".into())))
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
        // No username, or one with nothing to show: the ID is shown where
        // lookups match IDs, and it is no answer where they match usernames.
        // No ID: no answer.
        for body in [
            &br#"{"data":{"id":7}}"#[..],
            br#"{"data":{"id":7,"name":null}}"#,
            br#"{"data":{"id":7,"name":""}}"#,
            br#"{"data":{"id":7,"name":"\u0007"}}"#,
        ] {
            assert_eq!(user(body, false), Some(Ok(("7".into(), "7".into()))));
            assert!(user(body, true).is_none());
        }
        for body in [
            &br#"{"data":{"name":"player1"}}"#[..],
            br#"{"data":{"id":1.5,"name":"player1"}}"#,
            br#"{"data":{"id":-5,"name":"player1"}}"#,
            br#"{"data":{"id":"has space","name":"player1"}}"#,
            br#"not json"#,
        ] {
            assert!(user(body, true).is_none());
        }
    }
}
