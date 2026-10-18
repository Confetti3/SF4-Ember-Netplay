//! Sign-in sites as data (the sign-in sites design, GENERIC_SITES.md). A site
//! is an OAuth authorization-code sign-in the operator defines: where the
//! browser goes, where the code is exchanged, how to read who signed in, what
//! a username may be, and what lookups match. The bridge's code knows no site
//! by name; the example definitions in `deploy/sites/` are only data.
use std::collections::BTreeMap;

use ember_protocol::{
    encoding::OriginPolicy,
    sites::{LookupBy, is_site_id},
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
        if self.scope.len() > 256
            || !self
                .scope
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b" :._-".contains(&b))
        {
            return fail("scope must be space-separated scopes");
        }
        if self.hosts.is_empty() || !self.hosts.iter().all(|host| is_host(host)) {
            return fail("hosts must name at least one lowercase host");
        }
        for (name, url) in [
            ("authorize_url", &self.authorize_url),
            ("token_url", &self.token_url),
            ("user.url", &self.user.url),
        ] {
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
        let header_ok = |(name, value): (&String, &String)| {
            is_token(name, 64)
                && !name.eq_ignore_ascii_case("authorization")
                && value.len() <= 256
                && value.bytes().all(|b| b == b' ' || b.is_ascii_graphic())
        };
        if !self.user.headers.iter().all(header_ok) {
            return fail("user.headers must be visible ASCII, without authorization");
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
        for name in ["challonge", "startgg"] {
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
            json!({ "limits": { "monthly_request_budget": 1, "daily_request_budget": 2, "starts_per_hour": 1 } }),
        ] {
            assert!(refused(change.clone()), "{change}");
        }
        // Loopback http only where the policy allows it.
        let mut site = example("challonge");
        site.token_url = "http://127.0.0.1:9000/oauth/token".into();
        assert!(site.validate(OriginPolicy::HttpsOnly).is_err());
        assert!(site.validate(OriginPolicy::AllowLoopbackHttp).is_ok());
    }

    #[test]
    fn a_username_rule_is_the_sites_own() {
        let rule = example("challonge").username_rule;
        assert!(rule.allows("Kate_99") && !rule.allows("has space") && !rule.allows(""));
        assert_eq!(rule.key("Kate_99"), "kate_99");
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
        assert!(!rule.allows(&long) && !rule.allows("kate@x") && !rule.allows("K\u{e9}t"));
        assert_eq!(rule.key("KATE_99"), "kate_99");
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
        assert_eq!(printable.key("Kate"), "Kate");
        assert_eq!(
            printable.describe(),
            "at most 4 characters, none of them control characters"
        );
    }
}
