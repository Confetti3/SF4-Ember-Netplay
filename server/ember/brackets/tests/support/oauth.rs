//! A site's OAuth authorization-code flow, for the fake sites: the user the
//! test's browser is signed in as consents at the authorize page, the code is
//! exchanged with the client secret (in a form or a JSON body), and tokens are
//! refreshed. Each token keeps the scope asked for at the authorize page, so a
//! fake can refuse what the token was not granted.
use std::collections::HashMap;

use axum::{
    body::Bytes,
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Redirect, Response},
};
use serde_json::{Value, json};

/// A user on the site: ID and name.
pub type User = (String, String);

pub struct OAuth {
    pub client_id: &'static str,
    pub client_secret: &'static str,
    /// Who the test's browser is signed in to the site as.
    pub browser: Option<User>,
    /// Code: user, redirect URI and scope.
    codes: HashMap<String, (User, String, String)>,
    /// Access token: user and scope.
    tokens: HashMap<String, (User, String)>,
    /// Refresh token: user and scope.
    refreshes: HashMap<String, (User, String)>,
    issued: usize,
}

/// The query string's or a form body's parameters.
pub fn params(text: &[u8]) -> HashMap<String, String> {
    url::form_urlencoded::parse(text).into_owned().collect()
}

fn refusal(error: &str) -> Response {
    (
        StatusCode::BAD_REQUEST,
        [(header::CONTENT_TYPE, "application/json")],
        json!({ "error": error }).to_string(),
    )
        .into_response()
}

impl OAuth {
    pub fn new(client_id: &'static str, client_secret: &'static str) -> Self {
        Self {
            client_id,
            client_secret,
            browser: None,
            codes: HashMap::new(),
            tokens: HashMap::new(),
            refreshes: HashMap::new(),
            issued: 0,
        }
    }

    /// The authorize page: the browser's user approves at once and is sent
    /// back to the redirect URI with a code and the state unchanged.
    pub fn authorize(&mut self, query: &str) -> Response {
        let query = params(query.as_bytes());
        let get = |key: &str| query.get(key).cloned().unwrap_or_default();
        let (Some(user), true, true) = (
            self.browser.clone(),
            get("response_type") == "code",
            get("client_id") == self.client_id,
        ) else {
            return refusal("invalid_request");
        };
        let redirect = get("redirect_uri");
        if !redirect.starts_with("http") {
            return refusal("invalid_request");
        }
        self.issued += 1;
        let code = format!("code-{}", self.issued);
        self.codes
            .insert(code.clone(), (user, redirect.clone(), get("scope")));
        let back = url::form_urlencoded::Serializer::new(String::new())
            .append_pair("code", &code)
            .append_pair("state", &get("state"))
            .finish();
        Redirect::to(&format!("{redirect}?{back}")).into_response()
    }

    /// The token endpoint: a code or a refresh token, from this client.
    pub fn token(&mut self, headers: &HeaderMap, body: &Bytes) -> Response {
        let json = headers
            .get(header::CONTENT_TYPE)
            .is_some_and(|value| value == "application/json");
        let fields: HashMap<String, String> = if json {
            serde_json::from_slice::<serde_json::Map<String, Value>>(body)
                .unwrap_or_default()
                .into_iter()
                .filter_map(|(key, value)| Some((key, value.as_str()?.to_owned())))
                .collect()
        } else {
            params(body)
        };
        let get = |key: &str| fields.get(key).map(String::as_str).unwrap_or_default();
        if (get("client_id"), get("client_secret")) != (self.client_id, self.client_secret) {
            return refusal("invalid_client");
        }
        let granted = match get("grant_type") {
            "authorization_code" => self
                .codes
                .remove(get("code"))
                .filter(|(_, redirect, _)| redirect == get("redirect_uri"))
                .map(|(user, _, scope)| (user, scope)),
            "refresh_token" => self.refreshes.get(get("refresh_token")).cloned(),
            _ => None,
        };
        let Some((user, scope)) = granted else {
            return refusal("invalid_grant");
        };
        self.issued += 1;
        let access = format!("access-{}-{}", user.0, self.issued);
        let refresh = format!("refresh-{}-{}", user.0, self.issued);
        self.tokens
            .insert(access.clone(), (user.clone(), scope.clone()));
        self.refreshes.insert(refresh.clone(), (user, scope));
        (
            [(header::CONTENT_TYPE, "application/json")],
            json!({ "access_token": access, "token_type": "Bearer", "expires_in": 604800,
                    "refresh_token": refresh })
            .to_string(),
        )
            .into_response()
    }

    /// The user whose bearer token `headers` carry, and the token's scope.
    pub fn bearer(&self, headers: &HeaderMap) -> Option<&(User, String)> {
        let token = headers
            .get(header::AUTHORIZATION)?
            .to_str()
            .ok()?
            .strip_prefix("Bearer ")?;
        self.tokens.get(token)
    }

    /// Whether the bearer token in `headers` was granted `scope`.
    pub fn allows(&self, headers: &HeaderMap, scope: &str) -> bool {
        self.bearer(headers)
            .is_some_and(|(_, granted)| granted.split(' ').any(|have| have == scope))
    }

    /// The latest access token `user_id` holds with `scope`, for a test
    /// acting as them on the site's own pages.
    pub fn token_of(&self, user_id: &str, scope: &str) -> String {
        self.tokens
            .iter()
            .filter(|(_, ((id, _), granted))| {
                id == user_id && granted.split(' ').any(|have| have == scope)
            })
            .max_by_key(|(token, _)| token.rsplit('-').next().unwrap().parse::<usize>().unwrap())
            .map(|(token, _)| token.clone())
            .unwrap()
    }
}

/// What the browser does with an authorize URL: the site sends it back to
/// the bridge; the query it comes back with.
pub async fn approve(authorize_url: &str) -> String {
    let client = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .unwrap();
    let answer = client.get(authorize_url).send().await.unwrap();
    assert!(answer.status().is_redirection(), "{}", answer.status());
    let back = answer.headers()[header::LOCATION.as_str()]
        .to_str()
        .unwrap();
    back.split_once('?').unwrap().1.to_owned()
}
