//! Sign-in sites as data (docs/guides/INTEGRATIONS.md, "Sign-in sites"). A site
//! is an OAuth authorization-code sign-in the operator defines: where the
//! browser goes, where the code is exchanged, how to read who signed in, what
//! a username may be, and what lookups match. The bridge's code knows no site
//! by name; the example definitions in `deploy/sites/` are only data.
use std::collections::BTreeMap;

use ember_protocol::{
    encoding::OriginPolicy,
    sites::{LookupBy, is_public_host, is_site_id},
};
use serde::{Deserialize, Serialize};

/// One entry of `Config::sign_in_sites`.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignInSite {
    /// The site ID its routes, storage and lookups use (`sites::is_site_id`).
    pub id: String,
    /// The name the pages and Ember show.
    pub display_name: String,
    /// The OAuth application's client ID. Its client secret is
    /// `site_client_secrets.<id>` in the integration secrets, and its
    /// redirect list must hold `<origin>/v1/sites/<id>/callback`.
    pub client_id: String,
    #[serde(default)]
    pub scope: String,
    /// Where the browser goes, with the standard OAuth parameters and
    /// `authorize_params`.
    pub authorize_url: String,
    #[serde(default)]
    pub authorize_params: BTreeMap<String, String>,
    pub token_url: String,
    pub token_encoding: Encoding,
    /// Fields the token request's body carries besides the standard ones.
    #[serde(default)]
    pub token_params: BTreeMap<String, String>,
    pub user: UserRead,
    pub username_rule: UsernameRule,
    pub lookup_by: LookupBy,
    /// The hosts every URL above must be on, over https.
    pub hosts: Vec<String>,
    #[serde(default)]
    pub limits: Option<Limits>,
    /// Offers organizers a sign-in that keeps the site's tokens, to run
    /// their events (docs/design/TOURNAMENT_DESK.md). None offers none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub organizer: Option<Organizer>,
}

/// What an organizer sign-in asks the site for.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Organizer {
    /// The scopes it asks for, in place of `scope`; also in place of a
    /// `scope` in `token_params`.
    pub scope: String,
    /// Where a refresh token is exchanged, when not at `token_url`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub refresh_url: Option<String>,
    /// How to list the events the organizer runs on the site, with their
    /// access token as a bearer. None lists none, so none can be run.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tournaments: Option<TournamentsRead>,
    /// How to create an event on the site for the organizer, with their
    /// access token as a bearer. None creates none: the organizer makes it
    /// on the site and picks it from `tournaments`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub create: Option<CreateRequest>,
    /// How players who sign up through Ember are added to an event as its
    /// participants, and taken out again, with the organizer's access
    /// token as a bearer. None takes no sign-ups through Ember.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub participants: Option<ParticipantsRequest>,
}

/// The requests that add a player to an organizer's event and take them
/// out: `body` POSTed to `url`, with the player's name for the event at
/// `name_pointer`, their site username at `username_pointer` and Ember's
/// marker at `marker_pointer`; `remove_url` DELETEd. `{tournament}` in
/// either URL is the event's ID, `{participant}` in `remove_url` the ID
/// the answer has at `id_pointer`.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ParticipantsRequest {
    pub url: String,
    pub remove_url: String,
    #[serde(default)]
    pub headers: BTreeMap<String, String>,
    pub body: serde_json::Value,
    pub name_pointer: String,
    pub username_pointer: String,
    pub marker_pointer: String,
    pub id_pointer: String,
}

/// The request that creates an organizer's event: `body` POSTed to `url`
/// with the event's title at `name_pointer` and each field the organizer
/// sets at its pointer, one of its `values`. Every pointer's parent is in
/// `body`. In the answer, `id_pointer` is the ID the bracket connector's
/// `tournament` takes (as `tournaments` lists it), and the text at
/// `link_pointer` after `link_prefix` is the event's page. `delete_url`,
/// with `{tournament}` the event's ID, is DELETEd to delete an event it
/// created; None deletes none. `start_body` PUT to `start_url` (with
/// `{tournament}`) starts an event's bracket; None leaves that to the site.
/// `reset_body` PUT there takes a started bracket back to before its start,
/// for a site that deletes only those (Challonge).
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateRequest {
    pub url: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub delete_url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub start_url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub start_body: Option<serde_json::Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reset_body: Option<serde_json::Value>,
    #[serde(default)]
    pub headers: BTreeMap<String, String>,
    pub body: serde_json::Value,
    pub name_pointer: String,
    #[serde(default)]
    pub fields: BTreeMap<String, CreateField>,
    pub id_pointer: String,
    #[serde(default)]
    pub link_pointer: Option<String>,
    #[serde(default)]
    pub link_prefix: Option<String>,
}

/// A setting an organizer may give a new event: where it goes in the body
/// and the values it takes.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateField {
    pub pointer: String,
    pub values: Vec<serde_json::Value>,
    /// What else a value needs in the body, by value (its text, or its JSON
    /// for other values): pointers to set, such as the options object a
    /// site requires with a format.
    #[serde(default)]
    pub with: BTreeMap<String, BTreeMap<String, serde_json::Value>>,
}

/// The request that lists an organizer's events, and JSON pointers into its
/// answer: `list_pointer` to the array of tournaments; `events_pointer`,
/// when a tournament holds its events in an array of its own (start.gg),
/// into each tournament; `id_pointer` and `name_pointer` into each event
/// (each tournament without `events_pointer`), the ID being what the
/// bracket connector's `tournament` takes; `tournament_name_pointer` into
/// each tournament, shown before the event's name.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TournamentsRead {
    pub method: Method,
    pub url: String,
    #[serde(default)]
    pub headers: BTreeMap<String, String>,
    #[serde(default)]
    pub body: Option<serde_json::Value>,
    pub list_pointer: String,
    #[serde(default)]
    pub events_pointer: Option<String>,
    pub id_pointer: String,
    pub name_pointer: String,
    #[serde(default)]
    pub tournament_name_pointer: Option<String>,
}

/// How the token request's body is encoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Encoding {
    Form,
    Json,
}

/// How to read who signed in, with the access token as a bearer.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UserRead {
    pub method: Method,
    pub url: String,
    #[serde(default)]
    pub headers: BTreeMap<String, String>,
    /// A JSON body, such as a GraphQL query. Sent with `POST` only.
    #[serde(default)]
    pub body: Option<serde_json::Value>,
    /// JSON pointers (RFC 6901) into the answer. A number is read as its
    /// decimal digits. Without `username_pointer` the username is the user
    /// ID.
    pub id_pointer: String,
    #[serde(default)]
    pub username_pointer: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Method {
    #[serde(rename = "GET")]
    Get,
    #[serde(rename = "POST")]
    Post,
    /// For removing participants only; a definition's reads take GET or
    /// POST.
    #[serde(rename = "DELETE")]
    Delete,
    /// For starting an event's bracket only.
    #[serde(rename = "PUT")]
    Put,
}

/// What a username may be, and how lookups compare it.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UsernameRule {
    pub characters: Characters,
    /// ASCII punctuation allowed besides `characters`' own.
    #[serde(default)]
    pub also: String,
    pub max_length: usize,
    /// Whether lookups compare usernames without ASCII case.
    #[serde(default)]
    pub case_insensitive: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Characters {
    /// ASCII letters and digits.
    LettersDigits,
    /// Any character but a control character.
    Printable,
}

/// A site's request limits: requests per calendar month and per day (UTC),
/// a sign-in's exchange (token and user read) counting two, and sign-ins
/// started per hour across the bridge.
#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Limits {
    pub monthly_request_budget: u64,
    pub daily_request_budget: u64,
    pub starts_per_hour: usize,
}

impl UsernameRule {
    pub fn allows(&self, name: &str) -> bool {
        let fits = |c: char| match self.characters {
            Characters::LettersDigits => c.is_ascii_alphanumeric() || self.also.contains(c),
            Characters::Printable => !c.is_control(),
        };
        !name.is_empty() && name.chars().count() <= self.max_length && name.chars().all(fits)
    }

    /// What `allows` allows, as the sign-in page tells a player whose
    /// username it does not: "letters, digits, '_' and '.', at most 64 characters".
    pub fn describe(&self) -> String {
        let max = self.max_length;
        match self.characters {
            Characters::LettersDigits => {
                let mut kinds = vec!["letters".to_owned(), "digits".to_owned()];
                kinds.extend(self.also.chars().map(|c| format!("'{c}'")));
                let last = kinds.pop().unwrap_or_default();
                format!("{} and {last}, at most {max} characters", kinds.join(", "))
            }
            Characters::Printable => {
                format!("at most {max} characters, none of them control characters")
            }
        }
    }

    /// The form lookups by username match.
    pub fn key(&self, name: &str) -> String {
        if self.case_insensitive {
            name.to_ascii_lowercase()
        } else {
            name.to_owned()
        }
    }
}

impl SignInSite {
    /// The origin and path of `authorize_url`, which Ember checks a sign-in
    /// page against.
    pub fn authorize_page(&self) -> (String, String) {
        let url = url::Url::parse(&self.authorize_url).expect("checked by validate");
        (url.origin().ascii_serialization(), url.path().to_owned())
    }

    pub fn validate(&self, policy: OriginPolicy) -> Result<(), String> {
        let id = &self.id;
        let fail = |what: &str| Err(format!("sign-in site {id:?}: {what}"));
        if !is_site_id(id) || id == "discord" {
            return fail("id must be 1 to 32 of a-z, 0-9 and -, and not discord");
        }
        if self.display_name.is_empty()
            || self.display_name.chars().count() > 64
            || self.display_name.chars().any(char::is_control)
        {
            return fail("display_name must be 1 to 64 characters on one line");
        }
        if !is_token(&self.client_id, 256) {
            return fail("client_id must be 1 to 256 visible ASCII characters");
        }
        let scope = |scope: &str| {
            scope.len() <= 256
                && scope
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b" :._-".contains(&b))
        };
        if !scope(&self.scope) || !self.organizer.as_ref().is_none_or(|o| scope(&o.scope)) {
            return fail("scope must be space-separated scopes");
        }
        if self.hosts.is_empty() || !self.hosts.iter().all(|host| is_host(host)) {
            return fail("hosts must name at least one lowercase host");
        }
        // A definition chooses where the client secret and players' tokens
        // go, so outside local tests its hosts are public DNS names: no IP
        // address, localhost or private name the bridge's own network may
        // answer.
        if policy == OriginPolicy::HttpsOnly && !self.hosts.iter().all(|host| is_public_host(host))
        {
            return fail(
                "hosts must be public DNS names, not IP addresses, localhost or internal names",
            );
        }
        let refresh_url = self.organizer.as_ref().and_then(|o| o.refresh_url.as_ref());
        let tournaments = self.organizer.as_ref().and_then(|o| o.tournaments.as_ref());
        let create = self.organizer.as_ref().and_then(|o| o.create.as_ref());
        let participants = self
            .organizer
            .as_ref()
            .and_then(|o| o.participants.as_ref());
        for (name, url) in [
            ("authorize_url", Some(&self.authorize_url)),
            ("token_url", Some(&self.token_url)),
            ("user.url", Some(&self.user.url)),
            ("organizer.refresh_url", refresh_url),
            (
                "organizer.tournaments.url",
                tournaments.map(|read| &read.url),
            ),
            ("organizer.create.url", create.map(|create| &create.url)),
            (
                "organizer.create.delete_url",
                create.and_then(|create| create.delete_url.as_ref()),
            ),
            (
                "organizer.create.start_url",
                create.and_then(|create| create.start_url.as_ref()),
            ),
            (
                "organizer.participants.url",
                participants.map(|add| &add.url),
            ),
            (
                "organizer.participants.remove_url",
                participants.map(|add| &add.remove_url),
            ),
        ]
        .into_iter()
        .filter_map(|(name, url)| Some((name, url?)))
        {
            if !self.allows(url, policy) {
                return fail(&format!(
                    "{name} must be https on one of hosts, with no credentials or fragment"
                ));
            }
        }
        if url::Url::parse(&self.authorize_url).is_ok_and(|url| url.query().is_some()) {
            return fail("authorize_url takes its parameters from authorize_params");
        }
        // The parameters the bridge sets itself, which a definition cannot.
        const AUTHORIZE: [&str; 5] = [
            "response_type",
            "client_id",
            "scope",
            "redirect_uri",
            "state",
        ];
        const TOKEN: [&str; 5] = [
            "grant_type",
            "code",
            "redirect_uri",
            "client_id",
            "client_secret",
        ];
        let extra = |params: &BTreeMap<String, String>, standard: &[&str]| {
            params.iter().all(|(name, value)| {
                !standard.contains(&name.as_str())
                    && is_token(name, 64)
                    && value.len() <= 256
                    && value.bytes().all(|b| b == b' ' || b.is_ascii_graphic())
            })
        };
        if !extra(&self.authorize_params, &AUTHORIZE) || !extra(&self.token_params, &TOKEN) {
            return fail(
                "authorize_params and token_params must be extra parameters, visible ASCII",
            );
        }
        // Headers the request sets itself. A definition's `Content-Type`
        // replaces the JSON one a body sends.
        const OWN: [&str; 5] = [
            "authorization",
            "host",
            "content-length",
            "transfer-encoding",
            "connection",
        ];
        let header_ok = |(name, value): (&String, &String)| {
            is_token(name, 64)
                && reqwest::header::HeaderName::from_bytes(name.as_bytes()).is_ok()
                && !OWN.iter().any(|own| name.eq_ignore_ascii_case(own))
                && value.len() <= 256
                && value.bytes().all(|b| b == b' ' || b.is_ascii_graphic())
        };
        let headers_ok = |headers: &BTreeMap<String, String>| {
            let mut names: Vec<String> = headers
                .keys()
                .map(|name| name.to_ascii_lowercase())
                .collect();
            names.sort();
            names.dedup();
            headers.iter().all(header_ok) && names.len() == headers.len()
        };
        if !headers_ok(&self.user.headers) {
            return fail(
                "user.headers must be visible ASCII, each named once, without authorization, \
                 host, content-length, transfer-encoding or connection",
            );
        }
        let write = |method: Method| matches!(method, Method::Delete | Method::Put);
        if write(self.user.method) || tournaments.is_some_and(|read| write(read.method)) {
            return fail("user and organizer.tournaments are read with GET or POST");
        }
        if self.user.method == Method::Get && self.user.body.is_some() {
            return fail("user.body goes with POST only");
        }
        let pointer_ok = |pointer: &str| pointer.starts_with('/') && pointer.len() <= 256;
        if !pointer_ok(&self.user.id_pointer)
            || !self.user.username_pointer.as_deref().is_none_or(pointer_ok)
        {
            return fail("user pointers must be JSON pointers starting with /");
        }
        if let Some(read) = tournaments {
            let optional = |pointer: &Option<String>| pointer.as_deref().is_none_or(pointer_ok);
            // The list may be the whole answer.
            if !(read.list_pointer.is_empty() || pointer_ok(&read.list_pointer))
                || !pointer_ok(&read.id_pointer)
                || !pointer_ok(&read.name_pointer)
                || !optional(&read.events_pointer)
                || !optional(&read.tournament_name_pointer)
            {
                return fail(
                    "organizer.tournaments pointers must be JSON pointers starting with /",
                );
            }
            if !headers_ok(&read.headers) || (read.method == Method::Get && read.body.is_some()) {
                return fail(
                    "organizer.tournaments needs headers as user.headers, and a body with POST only",
                );
            }
        }
        if let Some(create) = create {
            // A field goes into an object the body already has, under a
            // plain name.
            let settable = |pointer: &str| {
                pointer_ok(pointer)
                    && !pointer.contains('~')
                    && pointer.rsplit_once('/').is_some_and(|(parent, _)| {
                        create.body.pointer(parent).is_some_and(|v| v.is_object())
                    })
            };
            let value_ok = |value: &serde_json::Value| {
                !value.is_object() && !value.is_array() && value.to_string().len() <= 64
            };
            if !create.body.is_object()
                || !settable(&create.name_pointer)
                || !pointer_ok(&create.id_pointer)
                || !create.link_pointer.as_deref().is_none_or(pointer_ok)
                || create.fields.len() > 16
                || !create.fields.iter().all(|(name, field)| {
                    is_token(name, 64)
                        && settable(&field.pointer)
                        && (1..=16).contains(&field.values.len())
                        && field.values.iter().all(value_ok)
                        && field.with.len() <= 16
                        && field.with.values().all(|extra| {
                            extra.len() <= 8 && extra.keys().all(|pointer| settable(pointer))
                        })
                })
            {
                return fail(
                    "organizer.create needs an object body holding every pointer's parent, \
                     and fields of 1 to 16 plain values",
                );
            }
            let prefix_ok = create.link_prefix.as_deref().is_none_or(|prefix| {
                url::Url::parse(prefix).is_ok_and(|url| url.scheme() == "https")
            });
            let named =
                |url: &Option<String>| url.as_ref().is_none_or(|url| url.contains("{tournament}"));
            let start_ok = create.start_url.is_some() == create.start_body.is_some()
                && create
                    .start_body
                    .as_ref()
                    .is_none_or(|body| body.is_object())
                && create
                    .reset_body
                    .as_ref()
                    .is_none_or(|body| body.is_object() && create.start_url.is_some());
            if !headers_ok(&create.headers)
                || !prefix_ok
                || !named(&create.delete_url)
                || !named(&create.start_url)
                || !start_ok
            {
                return fail(
                    "organizer.create needs headers as user.headers, an https link_prefix, \
                     {tournament} in delete_url and start_url, and an object start_body \
                     with start_url",
                );
            }
        }
        if let Some(add) = participants {
            let settable = |pointer: &str| {
                pointer_ok(pointer)
                    && !pointer.contains('~')
                    && pointer.rsplit_once('/').is_some_and(|(parent, _)| {
                        add.body.pointer(parent).is_some_and(|v| v.is_object())
                    })
            };
            if !add.body.is_object()
                || !settable(&add.name_pointer)
                || !settable(&add.username_pointer)
                || !settable(&add.marker_pointer)
                || !pointer_ok(&add.id_pointer)
                || !add.url.contains("{tournament}")
                || !add.remove_url.contains("{participant}")
                || !headers_ok(&add.headers)
            {
                return fail(
                    "organizer.participants needs an object body holding every pointer's \
                     parent, {tournament} in url, {participant} in remove_url, and headers \
                     as user.headers",
                );
            }
        }
        if self.lookup_by == LookupBy::Username && self.user.username_pointer.is_none() {
            return fail("lookup_by username needs user.username_pointer");
        }
        let rule = &self.username_rule;
        if !(1..=256).contains(&rule.max_length)
            || !rule.also.bytes().all(|b| b.is_ascii_punctuation())
        {
            return fail("username_rule must allow 1 to 256 characters, `also` ASCII punctuation");
        }
        if let Some(limits) = self.limits
            && (limits.monthly_request_budget < 2
                || limits.daily_request_budget < 2
                || limits.starts_per_hour == 0)
        {
            return fail("limits must allow at least one sign-in");
        }
        Ok(())
    }

    /// Whether the bridge may call `url` for this site, or send a browser
    /// there: https on one of its hosts, or loopback http where the policy
    /// allows it (local tests).
    fn allows(&self, url: &str, policy: OriginPolicy) -> bool {
        let Ok(url) = url::Url::parse(url) else {
            return false;
        };
        let clean =
            url.username().is_empty() && url.password().is_none() && url.fragment().is_none();
        let on_host = url.scheme() == "https"
            && url.port().is_none()
            && url
                .host_str()
                .is_some_and(|host| self.hosts.iter().any(|allowed| allowed == host));
        let loopback = policy == OriginPolicy::AllowLoopbackHttp
            && url.scheme() == "http"
            && matches!(url.host_str(), Some("127.0.0.1" | "localhost" | "[::1]"));
        clean && (on_host || loopback)
    }
}

/// 1 to `max` visible ASCII characters.
fn is_token(text: &str, max: usize) -> bool {
    !text.is_empty() && text.len() <= max && text.bytes().all(|b| b.is_ascii_graphic())
}

/// A lowercase DNS name.
fn is_host(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= 253
        && text.split('.').all(|label| {
            !label.is_empty()
                && label.len() <= 63
                && !label.starts_with('-')
                && label
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        })
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    /// The shipped example definitions, as an operator copies them.
    fn example(name: &str) -> SignInSite {
        let path = format!("{}/deploy/sites/{name}.json", env!("CARGO_MANIFEST_DIR"));
        serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
    }

    #[test]
    fn the_shipped_definitions_are_valid() {
        for name in ["challonge", "startgg", "ember-bracket-site"] {
            let site = example(name);
            assert_eq!(site.id, name);
            assert_eq!(site.validate(OriginPolicy::HttpsOnly), Ok(()), "{name}");
        }
        assert_eq!(
            example("startgg").authorize_page(),
            ("https://start.gg".into(), "/oauth/authorize".into())
        );
    }

    #[test]
    fn participants_name_the_event_and_set_into_the_body() {
        let participants = |change: fn(&mut ParticipantsRequest)| {
            let mut site = example("challonge");
            change(
                site.organizer
                    .as_mut()
                    .unwrap()
                    .participants
                    .as_mut()
                    .unwrap(),
            );
            site.validate(OriginPolicy::HttpsOnly)
        };
        assert_eq!(participants(|_| {}), Ok(()));
        assert!(participants(|add| add.url = add.url.replace("{tournament}", "1")).is_err());
        assert!(
            participants(|add| add.remove_url = add.remove_url.replace("{participant}", "1"))
                .is_err()
        );
        assert!(participants(|add| add.marker_pointer = "/data/nowhere/misc".into()).is_err());
    }

    #[test]
    fn a_definition_keeps_the_bridge_on_its_hosts() {
        let refused = |change: serde_json::Value| {
            let mut site = serde_json::to_value(example("challonge")).unwrap();
            site.as_object_mut()
                .unwrap()
                .extend(change.as_object().unwrap().clone());
            let site: SignInSite = serde_json::from_value(site).unwrap();
            site.validate(OriginPolicy::HttpsOnly).is_err()
        };
        for change in [
            json!({ "id": "discord" }),
            json!({ "id": "Challonge" }),
            json!({ "token_url": "https://evil.example/oauth/token" }),
            json!({ "token_url": "http://api.challonge.com/oauth/token" }),
            json!({ "token_url": "https://api.challonge.com:8443/oauth/token" }),
            json!({ "authorize_url": "https://user@api.challonge.com/oauth/authorize" }),
            json!({ "authorize_url": "https://api.challonge.com/oauth/authorize?x=1" }),
            json!({ "authorize_params": { "state": "fixed" } }),
            json!({ "token_params": { "client_secret": "in the config" } }),
            json!({ "hosts": [] }),
            json!({ "hosts": ["API.challonge.com"] }),
            json!({ "display_name": "" }),
            json!({ "scope": "me\nall" }),
            json!({ "organizer": { "scope": "me\nall" } }),
            json!({ "organizer": { "scope": "me", "refresh_url": "https://evil.example/oauth/token" } }),
            json!({ "organizer": { "scope": "me", "tournaments": { "method": "GET",
                "url": "https://evil.example/tournaments.json", "list_pointer": "/data",
                "id_pointer": "/id", "name_pointer": "/name" } } }),
            json!({ "organizer": { "scope": "me", "tournaments": { "method": "GET",
                "url": "https://api.challonge.com/v2.1/tournaments.json", "list_pointer": "/data",
                "id_pointer": "id", "name_pointer": "/name" } } }),
            json!({ "limits": { "monthly_request_budget": 1, "daily_request_budget": 2, "starts_per_hour": 1 } }),
            json!({ "organizer": { "scope": "me", "create": { "url": "https://evil.example/t.json",
                "body": { "data": {} }, "name_pointer": "/data/name", "id_pointer": "/data/id" } } }),
            // A field must land in an object the body has.
            json!({ "organizer": { "scope": "me", "create": {
                "url": "https://api.challonge.com/v2.1/tournaments.json",
                "body": { "data": {} }, "name_pointer": "/data/name", "id_pointer": "/data/id",
                "fields": { "format": { "pointer": "/data/options/type", "values": ["swiss"] } } } } }),
            // A start names the event it starts, with a body.
            json!({ "organizer": { "scope": "me", "create": {
                "url": "https://api.challonge.com/v2.1/tournaments.json",
                "start_url": "https://api.challonge.com/v2.1/tournaments/{tournament}/change_state.json",
                "body": { "data": {} }, "name_pointer": "/data/name", "id_pointer": "/data/id" } } }),
            // A delete names the event it deletes.
            json!({ "organizer": { "scope": "me", "create": {
                "url": "https://api.challonge.com/v2.1/tournaments.json",
                "delete_url": "https://api.challonge.com/v2.1/tournaments.json",
                "body": { "data": {} }, "name_pointer": "/data/name", "id_pointer": "/data/id" } } }),
        ] {
            assert!(refused(change.clone()), "{change}");
        }
        // Hosts the bridge's own network may answer, refused outside local
        // tests even with every URL on them.
        for host in [
            "10.0.0.1",
            "169.254.169.254",
            "127.1",
            "0x7f.0x1",
            "localhost",
            "intranet",
            "auth.localhost",
            "metadata.google.internal",
            "printer.local",
            "router.home.arpa",
        ] {
            let mut site = example("challonge");
            site.hosts = vec![host.into()];
            site.authorize_url = format!("https://{host}/oauth/authorize");
            site.token_url = format!("https://{host}/oauth/token");
            site.user.url = format!("https://{host}/me");
            if let Some(organizer) = site.organizer.as_mut() {
                for url in organizer
                    .tournaments
                    .iter_mut()
                    .map(|read| &mut read.url)
                    .chain(organizer.create.iter_mut().map(|create| &mut create.url))
                {
                    *url = format!("https://{host}/tournaments.json");
                }
                if let Some(create) = organizer.create.as_mut() {
                    create.delete_url = Some(format!("https://{host}/{{tournament}}.json"));
                    create.start_url = Some(format!("https://{host}/{{tournament}}/start.json"));
                }
                if let Some(add) = organizer.participants.as_mut() {
                    add.url = format!("https://{host}/{{tournament}}/participants.json");
                    add.remove_url = format!("https://{host}/{{tournament}}/{{participant}}.json");
                }
            }
            let refused = site.validate(OriginPolicy::HttpsOnly).unwrap_err();
            assert!(refused.contains("public DNS names"), "{host}: {refused}");
            // Local tests may use them, where a URL keeps its host as written
            // (the URL parser reads "127.1" as 127.0.0.1).
            if url::Url::parse(&site.token_url).unwrap().host_str() == Some(host) {
                assert_eq!(
                    site.validate(OriginPolicy::AllowLoopbackHttp),
                    Ok(()),
                    "{host}"
                );
            }
        }
        // Headers the request sets itself, or one named twice.
        for headers in [
            json!({ "Authorization": "Bearer x" }),
            json!({ "Host": "evil.example" }),
            json!({ "Content-Length": "0" }),
            json!({ "Accept": "a", "accept": "b" }),
            json!({ "Bad Name": "x" }),
        ] {
            let mut site = example("challonge");
            site.user.headers = serde_json::from_value(headers.clone()).unwrap();
            assert!(site.validate(OriginPolicy::HttpsOnly).is_err(), "{headers}");
        }
        // Lookups by username need a username to match; by user ID, not.
        let mut site = example("challonge");
        site.user.username_pointer = None;
        assert!(site.validate(OriginPolicy::HttpsOnly).is_err());
        let mut site = example("startgg");
        site.user.username_pointer = None;
        assert_eq!(site.validate(OriginPolicy::HttpsOnly), Ok(()));
        // Loopback http only where the policy allows it.
        let mut site = example("challonge");
        site.token_url = "http://127.0.0.1:9000/oauth/token".into();
        assert!(site.validate(OriginPolicy::HttpsOnly).is_err());
        assert!(site.validate(OriginPolicy::AllowLoopbackHttp).is_ok());
    }

    #[test]
    fn a_username_rule_is_the_sites_own() {
        let rule = example("challonge").username_rule;
        assert!(rule.allows("Player_1") && !rule.allows("has space") && !rule.allows(""));
        assert_eq!(rule.key("Player_1"), "player_1");
        assert_eq!(
            rule.describe(),
            "letters, digits, '_', '-' and '.', at most 64 characters"
        );
        // Challonge usernames: 1 to 64 ASCII letters, digits, `_`, `-` and
        // `.`, compared without case.
        assert_eq!(rule.characters, Characters::LettersDigits);
        assert_eq!((rule.also.as_str(), rule.max_length), ("_-.", 64));
        assert!(rule.case_insensitive);
        let long = "x".repeat(65);
        assert!(rule.allows("a.b-c") && rule.allows(&long[1..]));
        assert!(!rule.allows(&long) && !rule.allows("player1@x") && !rule.allows("K\u{e9}t"));
        assert_eq!(rule.key("PLAYER_1"), "player_1");
        let printable = UsernameRule {
            characters: Characters::Printable,
            also: String::new(),
            max_length: 4,
            case_insensitive: false,
        };
        assert!(
            printable.allows("K t\u{e9}")
                && !printable.allows("long1")
                && !printable.allows("a\nb")
        );
        assert_eq!(printable.key("Player 1"), "Player 1");
        assert_eq!(
            printable.describe(),
            "at most 4 characters, none of them control characters"
        );
    }
}
