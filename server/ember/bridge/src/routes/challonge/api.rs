//! Challonge's device sign-in endpoints and its current-user endpoint, as
//! docs/design/challonge/CHALLONGE_API.md reads them. Nothing here logs or
//! formats an error: the requests carry the device code, the client secret
//! and the access token, and a reqwest error's text includes the URL.
use axum::body::Bytes;
use ember_protocol::challonge::{SignInState, is_username};
use reqwest::{StatusCode, header};
use serde::Deserialize;
use zeroize::Zeroizing;

use crate::{config::Challonge, util::outbound_client};

const DEVICE_GRANT: &str = "urn:ietf:params:oauth:grant-type:device_code";

/// A device code Challonge issued, and what the player is shown.
pub struct Device {
    pub device_code: Zeroizing<String>,
    pub user_code: String,
    pub verification_uri: String,
    pub verification_uri_complete: Option<String>,
    pub expires_in: u64,
    /// The seconds Challonge asks to wait between polls, when it says.
    pub interval: Option<u64>,
}

/// What one poll of the token endpoint says about a sign-in.
pub enum Polled {
    /// The player approved: the access token, to read who they are with.
    Token(Zeroizing<String>),
    /// Not approved yet, or no usable answer this time: ask again later.
    Wait,
    /// Ask less often (`slow_down`, or Challonge's rate limit).
    SlowDown,
    Ended(SignInState),
}

pub struct User {
    pub id: String,
    pub username: String,
}

/// POSTs `params` to `path`, in the query string, where Challonge's reference
/// puts them, and also as a form body, where RFC 8628 and Challonge's other
/// OAuth samples put them, so a server reading either finds the same values.
async fn post(
    challonge: &Challonge,
    path: &str,
    params: &[(&str, &str)],
) -> Option<(StatusCode, Bytes)> {
    let url = url::Url::parse_with_params(&format!("{}{path}", challonge.api_base), params).ok()?;
    let form = url::form_urlencoded::Serializer::new(String::new())
        .extend_pairs(params)
        .finish();
    let response = outbound_client()
        .build()
        .ok()?
        .post(url)
        .header(header::ACCEPT, "application/json")
        .header(header::CONTENT_TYPE, "application/x-www-form-urlencoded")
        .body(form)
        .send()
        .await
        .ok()?;
    let status = response.status();
    Some((status, response.bytes().await.ok()?))
}

/// Asks Challonge for a device code, or None if it gave no usable one. Its
/// pages must be Challonge's own over https, since the game opens them.
pub async fn authorize_device(challonge: &Challonge) -> Option<Device> {
    #[derive(Deserialize)]
    struct Answer {
        device_code: String,
        user_code: String,
        verification_uri: String,
        #[serde(default)]
        verification_uri_complete: Option<String>,
        expires_in: u64,
        #[serde(default)]
        interval: Option<u64>,
    }
    let params = [
        ("client_id", challonge.client_id.as_str()),
        ("scope", challonge.scope.as_str()),
    ];
    let (status, body) = post(challonge, &challonge.device_path, &params).await?;
    if !status.is_success() {
        return None;
    }
    let answer: Answer = serde_json::from_slice(&body).ok()?;
    let device_code = Zeroizing::new(answer.device_code);
    let usable = is_code(&device_code, 1024)
        && is_code(&answer.user_code, 64)
        && is_challonge_page(&answer.verification_uri)
        && answer
            .verification_uri_complete
            .as_deref()
            .is_none_or(is_challonge_page)
        && answer.expires_in > 0;
    usable.then_some(Device {
        device_code,
        user_code: answer.user_code,
        verification_uri: answer.verification_uri,
        verification_uri_complete: answer.verification_uri_complete,
        expires_in: answer.expires_in,
        interval: answer.interval,
    })
}

/// Polls the token endpoint once for `device_code`. The OAuth errors follow
/// RFC 8628; Challonge documents only `authorization_pending` and
/// `invalid_grant`. A server error or no answer at all is waited out, since
/// the sign-in ends at its expiry anyway.
pub async fn poll_token(
    challonge: &Challonge,
    client_secret: Option<&str>,
    device_code: &str,
) -> Polled {
    #[derive(Deserialize)]
    struct Token {
        access_token: String,
    }
    #[derive(Deserialize)]
    struct Refusal {
        error: String,
    }
    let mut params = vec![
        ("grant_type", DEVICE_GRANT),
        ("device_code", device_code),
        ("client_id", challonge.client_id.as_str()),
    ];
    params.extend(client_secret.map(|secret| ("client_secret", secret)));
    let Some((status, body)) = post(challonge, &challonge.token_path, &params).await else {
        return Polled::Wait;
    };
    if status.is_success() {
        return match serde_json::from_slice::<Token>(&body) {
            Ok(token) => Polled::Token(Zeroizing::new(token.access_token)),
            Err(_) => Polled::Ended(SignInState::Failed),
        };
    }
    if status == StatusCode::TOO_MANY_REQUESTS {
        return Polled::SlowDown;
    }
    if status.is_server_error() {
        return Polled::Wait;
    }
    let error = serde_json::from_slice::<Refusal>(&body).map(|refusal| refusal.error);
    match error.as_deref() {
        Ok("authorization_pending") => Polled::Wait,
        Ok("slow_down") => Polled::SlowDown,
        Ok("access_denied") => Polled::Ended(SignInState::Denied),
        Ok("expired_token" | "invalid_grant") => Polled::Ended(SignInState::Expired),
        _ => Polled::Ended(SignInState::Failed),
    }
}

/// Reads who signed in, or None on any failure or an answer that does not
/// name a user ID and a username Ember can use.
pub async fn me(challonge: &Challonge, access_token: &str) -> Option<User> {
    #[derive(Deserialize)]
    struct Me {
        data: Data,
    }
    #[derive(Deserialize)]
    struct Data {
        id: Id,
        attributes: Attributes,
    }
    /// Challonge's IDs are strings in `data.id` and numbers elsewhere.
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
    let response = outbound_client()
        .build()
        .ok()?
        .get(format!("{}{}", challonge.api_base, challonge.me_path))
        .header(header::ACCEPT, "application/json")
        .header(header::CONTENT_TYPE, "application/vnd.api+json")
        .header("Authorization-Type", "v2")
        .bearer_auth(access_token)
        .send()
        .await
        .ok()?;
    if !response.status().is_success() {
        return None;
    }
    let me: Me = serde_json::from_slice(&response.bytes().await.ok()?).ok()?;
    let id = match me.data.id {
        Id::Text(id) => id,
        Id::Number(id) => id.to_string(),
    };
    let username = me.data.attributes.username;
    let usable = !id.is_empty() && id.len() <= 32 && id.bytes().all(|b| b.is_ascii_alphanumeric());
    (usable && is_username(&username)).then_some(User { id, username })
}

/// 1 to `max` visible ASCII characters.
fn is_code(text: &str, max: usize) -> bool {
    !text.is_empty() && text.len() <= max && text.bytes().all(|b| b.is_ascii_graphic())
}

/// An https page on challonge.com or one of its subdomains, with no
/// credentials or port of its own.
fn is_challonge_page(text: &str) -> bool {
    let Ok(url) = url::Url::parse(text) else {
        return false;
    };
    let on_challonge = url
        .host_str()
        .is_some_and(|host| host == "challonge.com" || host.ends_with(".challonge.com"));
    text.len() <= 512
        && url.scheme() == "https"
        && on_challonge
        && url.port().is_none()
        && url.username().is_empty()
        && url.password().is_none()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_challonges_own_pages_are_shown() {
        for page in [
            "https://auth.challonge.com/oauth/device",
            "https://auth.challonge.com/oauth/device?user_code=FQFREU",
            "https://challonge.com/device",
        ] {
            assert!(is_challonge_page(page), "{page}");
        }
        for page in [
            "http://auth.challonge.com/oauth/device",
            "https://evilchallonge.com/oauth/device",
            "https://challonge.com.evil.example/device",
            "https://user@auth.challonge.com/oauth/device",
            "https://auth.challonge.com:8443/oauth/device",
            "javascript:alert(1)",
            "not a url",
        ] {
            assert!(!is_challonge_page(page), "{page}");
        }
    }
}
