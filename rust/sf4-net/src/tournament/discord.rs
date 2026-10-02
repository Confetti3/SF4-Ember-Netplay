//! Discord sign-in from Ember (optional; bridges that list the `discord`
//! feature). The helper asks the bridge to start a sign-in with a proof and
//! opens Discord's page in the player's browser itself, so the game never
//! handles the address. Under Wine and Proton the browser is the system's.
use std::sync::Arc;

use ember_protocol::{
    challenge::{Action, Method},
    discord::{ACCOUNT_PATH, Connection, START_PATH, SignInStarted},
};
use serde_json::json;

use super::{
    Failure, Outcome, Shared,
    client::{answer, prove, with_session},
};

/// Where Discord's sign-in page lives. Anything else the bridge answers is
/// not opened.
const SIGN_IN_PAGE: &str = "https://discord.com/oauth2/authorize?";

/// The player's connected Discord account on the bridge, or `account: null`.
pub async fn status(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    with_session(shared, bridge_id, |bridge, token| async move {
        let (status, body) = shared
            .client
            .call(
                reqwest::Method::GET,
                &format!("{}{ACCOUNT_PATH}", bridge.origin),
                Some(&token),
                None,
            )
            .await?;
        let connection: Connection = serde_json::from_value(answer(status, &body, &[200])?)
            .map_err(|_| Failure::new("bridge_invalid_response"))?;
        Ok(Some(json!(connection)))
    })
    .await
}

/// Starts a sign-in and opens Discord's page in the browser. The account
/// appears once the player finishes there; `status` reads it.
pub async fn connect(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    let started = with_session(shared, bridge_id, |bridge, token| async move {
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::DiscordConnect,
            Method::Post,
            START_PATH,
            json!({}),
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::POST,
                &format!("{}{START_PATH}", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        Ok(Some(answer(status, &response, &[201])?))
    })
    .await?;
    let started: SignInStarted = started
        .and_then(|value| serde_json::from_value(value).ok())
        .filter(|started: &SignInStarted| is_sign_in_page(&started.authorize_url))
        .ok_or_else(|| Failure::new("bridge_invalid_response"))?;
    open_in_browser(&started.authorize_url)?;
    Ok(Some(json!({ "opened": true })))
}

/// Disconnects the player's Discord account on the bridge.
pub async fn remove(shared: &Arc<Shared>, bridge_id: &str) -> Outcome {
    with_session(shared, bridge_id, |bridge, token| async move {
        let body = prove(
            shared,
            &bridge,
            Some(&token),
            Action::DiscordRemove,
            Method::Delete,
            ACCOUNT_PATH,
            json!({}),
        )
        .await?;
        let (status, response) = shared
            .client
            .call(
                reqwest::Method::DELETE,
                &format!("{}{ACCOUNT_PATH}", bridge.origin),
                Some(&token),
                Some(body),
            )
            .await?;
        Ok(Some(answer(status, &response, &[200])?))
    })
    .await
}

/// Discord's own sign-in page, with nothing that could break out of a URL.
fn is_sign_in_page(url: &str) -> bool {
    url.starts_with(SIGN_IN_PAGE)
        && url.len() <= 2048
        && url
            .bytes()
            .all(|byte| byte.is_ascii_graphic() && byte != b'"')
}

#[cfg(windows)]
fn open_in_browser(url: &str) -> Result<(), Failure> {
    use windows_sys::Win32::UI::Shell::ShellExecuteW;
    let wide = |text: &str| text.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
    let (verb, target) = (wide("open"), wide(url));
    // SAFETY: both strings are NUL-terminated and outlive the call.
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            target.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
        )
    };
    if result as isize > 32 {
        Ok(())
    } else {
        Err(Failure::new("browser_unavailable"))
    }
}

#[cfg(not(windows))]
fn open_in_browser(_: &str) -> Result<(), Failure> {
    Err(Failure::new("browser_unavailable"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_discords_sign_in_page_is_opened() {
        assert!(is_sign_in_page(
            "https://discord.com/oauth2/authorize?response_type=code&client_id=1546980049692135514&scope=identify&state=x"
        ));
        for url in [
            "http://discord.com/oauth2/authorize?x",
            "https://discord.com.example/oauth2/authorize?x",
            "https://discord.com/oauth2/authorize",
            "https://example.com/?https://discord.com/oauth2/authorize?",
            "https://discord.com/oauth2/authorize?a b",
            "file:///C:/Windows/System32/calc.exe",
        ] {
            assert!(!is_sign_in_page(url), "{url}");
        }
    }
}
