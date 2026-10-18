//! Sign-in sites (`sign_in`): the bracket sites a bridge lists in its
//! capabilities (Challonge, start.gg and any later one), each named by the
//! bridge's own site ID. The helper knows no site in particular: it opens a
//! sign-in page only on the authorize origin and path the bridge the player
//! trusts declares for that site.
use std::sync::Arc;

use ember_protocol::{
    challenge::Action,
    sites::{account_path, is_site_id, start_path},
};

use super::{
    Failure, Outcome, Shared, browser,
    client::approved,
    sign_in::{self, Routes},
};

fn routes(site: &str) -> Result<Routes, Failure> {
    if !is_site_id(site) {
        return Err(Failure::new("invalid_request"));
    }
    Ok(Routes {
        start_path: start_path(site),
        account_path: account_path(site),
        connect: Action::SiteConnect,
        remove: Action::SiteRemove,
        cancel: Action::SiteCancel,
    })
}

pub async fn status(shared: &Arc<Shared>, bridge_id: &str, site: &str) -> Outcome {
    sign_in::status(shared, bridge_id, &routes(site)?).await
}

/// Starts a sign-in on `site` and opens its page, which must be on the
/// authorize origin and path the bridge's capabilities declare for it.
pub async fn connect(shared: &Arc<Shared>, bridge_id: &str, site: &str) -> Outcome {
    let routes = routes(site)?;
    let origin = approved(shared, bridge_id)?.origin;
    let capabilities = shared.client.capabilities(&origin).await?;
    let listed = capabilities
        .sign_in_sites
        .into_iter()
        .find(|listed| listed.id == site)
        .ok_or_else(|| Failure::new("not_found"))?;
    sign_in::connect(shared, bridge_id, &routes, |url| {
        is_declared_page(url, &listed.authorize_origin, &listed.authorize_path)
    })
    .await
}

pub async fn remove(shared: &Arc<Shared>, bridge_id: &str, site: &str) -> Outcome {
    sign_in::remove(shared, bridge_id, &routes(site)?).await
}

pub async fn cancel(shared: &Arc<Shared>, bridge_id: &str, site: &str) -> Outcome {
    sign_in::cancel(shared, bridge_id, &routes(site)?).await
}

/// `url` is the page at `path` on `origin`, with nothing after it but a
/// query, and nothing that could break out of a URL. `origin` must be a bare
/// https origin: a host of letters, digits, `-` and dots, with no user, port
/// or path; `path` an absolute path of plain characters.
fn is_declared_page(url: &str, origin: &str, path: &str) -> bool {
    let host = origin.strip_prefix("https://").unwrap_or_default();
    let bare_origin = host.split('.').all(|label| {
        !label.is_empty()
            && label
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    });
    let plain_path = path.starts_with('/')
        && !path.starts_with("//")
        && path
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'/' | b'.' | b'_' | b'-'));
    let rest = url
        .strip_prefix(origin)
        .and_then(|rest| rest.strip_prefix(path));
    bare_origin
        && plain_path
        && rest.is_some_and(|rest| rest.is_empty() || rest.starts_with('?'))
        && browser::is_plain(url)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_declared_page_is_opened() {
        let cases = [
            ("https://api.challonge.com", "/oauth/authorize"),
            ("https://start.gg", "/oauth/authorize"),
            ("https://brackets.example", "/sign-in"),
        ];
        for (origin, path) in cases {
            for url in [
                format!("{origin}{path}?response_type=code&client_id=x&state=y"),
                format!("{origin}{path}"),
            ] {
                assert!(is_declared_page(&url, origin, path), "{url}");
            }
            for url in [
                format!("{origin}/other?state=y"),
                format!("{origin}{path}x?state=y"),
                format!("{origin}{path}/more?state=y"),
                format!("{origin}{path}#top"),
                format!("{origin}{path}?a b"),
                format!("{origin}{path}?\"x"),
                format!("{origin}.example{path}?state=y"),
                format!("{origin}:8443{path}?state=y"),
                format!("https://evil.example{path}?state=y"),
                format!("https://evil.example/?{origin}{path}?"),
                format!("{origin}{path}?{}", "a".repeat(2048)),
                "file:///C:/Windows/System32/calc.exe".to_owned(),
            ] {
                assert!(!is_declared_page(&url, origin, path), "{url}");
            }
        }
        // Only a bare https origin and a plain path are honoured, whatever the bridge says.
        for (origin, path) in [
            ("http://api.challonge.com", "/oauth/authorize"),
            ("https://user@api.challonge.com", "/oauth/authorize"),
            ("https://api.challonge.com:8443", "/oauth/authorize"),
            ("https://api.challonge.com/", "oauth/authorize"),
            ("https://", "/oauth/authorize"),
            ("https://a..challonge.com", "/oauth/authorize"),
            ("https://api.challonge.com", "//evil.example/authorize"),
            ("https://api.challonge.com", "/oauth/authorize?next=x"),
            ("https://api.challonge.com", ""),
        ] {
            let url = format!("{origin}{path}?state=y");
            assert!(!is_declared_page(&url, origin, path), "{origin} {path}");
        }
    }

    #[test]
    fn site_ids_are_checked_before_any_request() {
        assert!(routes("challonge").is_ok() && routes("brackets-plus").is_ok());
        for site in ["", "Challonge", "a/b", "../discord", &"x".repeat(33)] {
            assert!(routes(site).is_err(), "{site}");
        }
    }
}
