//! Sign-in sites (`sign_in`): the bracket sites a bridge lists in its
//! capabilities (Challonge, start.gg and any later one), each named by the
//! bridge's own site ID. The helper knows no site in particular: it opens a
//! sign-in page only on the authorize origin and path the bridge the player
//! trusts declares for that site.
use std::sync::Arc;

use ember_protocol::{
    challenge::Action,
    sites::{account_path, is_public_host, is_site_id, organize_path, start_path},
};
use serde::{Deserialize, Serialize};
use serde_json::value::RawValue;

use super::{
    Failure, Outcome, Shared, browser,
    client::approved,
    sign_in::{self, Routes},
};

/// The most sites kept from one bridge's list.
pub const MAX_LISTED: usize = 16;
/// The longest display name kept, in characters.
const MAX_NAME: usize = 64;

/// One site a bridge lists, with only what the helper and the game use. Its
/// other fields, such as how platforms look players up (`lookup_by`), and
/// any a later bridge adds, are ignored rather than understood.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Listed {
    pub id: String,
    pub display_name: String,
    pub sign_in: bool,
    pub authorize_origin: String,
    pub authorize_path: String,
    /// Whether it offers an organizer sign-in; older bridges leave it out.
    #[serde(default)]
    pub organizer: bool,
}

/// The sites in a capabilities document's `sign_in_sites`, as written,
/// entry by entry: an entry that does not read as a site (a field missing,
/// of another type or given twice), with an ID no site can have or a name no
/// row can show, or naming a site listed earlier, is dropped. Anything but a
/// list is no sites.
pub(super) fn listed(sites: Option<&RawValue>) -> Vec<Listed> {
    let Some(entries) =
        sites.and_then(|sites| serde_json::from_str::<Vec<&RawValue>>(sites.get()).ok())
    else {
        return Vec::new();
    };
    let mut kept: Vec<Listed> = Vec::new();
    for entry in entries {
        let Ok(site) = serde_json::from_str::<Listed>(entry.get()) else {
            continue;
        };
        let named = !site.display_name.trim().is_empty()
            && site.display_name.chars().count() <= MAX_NAME
            && !site.display_name.chars().any(char::is_control);
        if is_site_id(&site.id) && named && !kept.iter().any(|known| known.id == site.id) {
            kept.push(site);
        }
        if kept.len() == MAX_LISTED {
            break;
        }
    }
    kept
}

/// The site's routes; with `organizer`, its organizer sign-in's, where one
/// path starts the sign-in, reads the grant and drops it.
fn routes(site: &str, organizer: bool) -> Result<Routes, Failure> {
    if !is_site_id(site) {
        return Err(Failure::new("invalid_request"));
    }
    let (start, account) = if organizer {
        (organize_path(site), organize_path(site))
    } else {
        (start_path(site), account_path(site))
    };
    Ok(Routes {
        start_path: start,
        account_path: account,
        connect: Action::SiteConnect,
        remove: Action::SiteRemove,
        cancel: Action::SiteCancel,
    })
}

pub async fn status(shared: &Arc<Shared>, bridge_id: &str, site: &str, organizer: bool) -> Outcome {
    let routes = routes(site, organizer)?;
    if organizer {
        return sign_in::grant(shared, bridge_id, &routes).await;
    }
    sign_in::status(shared, bridge_id, &routes).await
}

/// Starts a sign-in on `site` and opens its page, which must be on the
/// authorize origin and path the bridge's capabilities declare for it.
pub async fn connect(
    shared: &Arc<Shared>,
    bridge_id: &str,
    site: &str,
    organizer: bool,
) -> Outcome {
    let routes = routes(site, organizer)?;
    let origin = approved(shared, bridge_id)?.origin;
    // The bridge is trusted already, so capabilities it does not serve now
    // mean it is down for the moment, not that it is no tournament service.
    let offered = shared
        .client
        .capabilities(&origin)
        .await
        .map_err(|Failure(code)| match code.as_str() {
            "not_a_bridge" => Failure::new("service_unavailable"),
            _ => Failure(code),
        })?;
    let listed = offered
        .sign_in_sites
        .into_iter()
        .find(|listed| listed.id == site)
        .ok_or_else(|| Failure::new("not_found"))?;
    // Listed with its sign-in off: the bridge would refuse the start.
    if !listed.sign_in || (organizer && !listed.organizer) {
        return Err(Failure::new("sign_in_off"));
    }
    // An organizer sign-in's path drops the grant on DELETE, so one whose
    // page did not open is left to lapse rather than ended.
    sign_in::connect(
        shared,
        bridge_id,
        &routes,
        |url| is_declared_page(url, &listed.authorize_origin, &listed.authorize_path),
        !organizer,
    )
    .await
}

pub async fn remove(shared: &Arc<Shared>, bridge_id: &str, site: &str, organizer: bool) -> Outcome {
    sign_in::remove(shared, bridge_id, &routes(site, organizer)?).await
}

pub async fn cancel(shared: &Arc<Shared>, bridge_id: &str, site: &str) -> Outcome {
    sign_in::cancel(shared, bridge_id, &routes(site, false)?).await
}

/// `url` is the page at `path` on `origin`, with nothing after it but a
/// query, and nothing that could break out of a URL. `origin` must be a bare
/// https origin on a public name, by the same rule the bridge holds a site's
/// hosts to (`is_public_host`: no IP address, no name kept for local
/// networks), so with no user, port or path. `path` is an absolute path of
/// plain characters.
fn is_declared_page(url: &str, origin: &str, path: &str) -> bool {
    let bare_origin = origin.strip_prefix("https://").is_some_and(is_public_host);
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
            ("https://1.brackets.example", "/sign-in"),
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
            ("https://localhost", "/oauth/authorize"),
            ("https://challonge", "/oauth/authorize"),
            ("https://127.0.0.1", "/oauth/authorize"),
            ("https://192.168.1.1", "/oauth/authorize"),
            ("https://10.0.0.1", "/oauth/authorize"),
            ("https://0x7f.0x0.0x0.0x1", "/oauth/authorize"),
            ("https://2130706433", "/oauth/authorize"),
            ("https://brackets.0X7F", "/oauth/authorize"),
            ("https://auth.localhost", "/oauth/authorize"),
            ("https://metadata.google.Internal", "/oauth/authorize"),
            ("https://router.home.arpa", "/oauth/authorize"),
            ("https://-brackets.example", "/oauth/authorize"),
            ("https://brackets-.example", "/oauth/authorize"),
        ] {
            let url = format!("{origin}{path}?state=y");
            assert!(!is_declared_page(&url, origin, path), "{origin} {path}");
        }
    }

    fn listed(sites: Option<serde_json::Value>) -> Vec<Listed> {
        let raw = sites.map(|sites| serde_json::value::to_raw_value(&sites).unwrap());
        super::listed(raw.as_deref())
    }

    #[test]
    fn a_listed_site_this_build_cannot_read_is_dropped() {
        let site = |id: &str| {
            serde_json::json!({
                "id": id, "display_name": "Brackets", "sign_in": true,
                "lookup_by": "username",
                "authorize_origin": "https://brackets.example",
                "authorize_path": "/sign-in",
            })
        };
        let mut unknown_lookup = site("emailed");
        unknown_lookup["lookup_by"] = "email".into();
        unknown_lookup["priority"] = serde_json::json!(1.5);
        let mut missing = site("missing");
        missing.as_object_mut().unwrap().remove("authorize_path");
        let mut retyped = site("retyped");
        retyped["sign_in"] = "yes".into();
        let mut unnamed = site("unnamed");
        unnamed["display_name"] = " ".into();
        let mut long = site("long");
        long["display_name"] = "x".repeat(65).into();
        let mut duplicate = site("challonge");
        duplicate["display_name"] = "Second".into();
        let kept = listed(Some(serde_json::json!([
            site("challonge"),
            unknown_lookup,
            missing,
            retyped,
            unnamed,
            long,
            site("Bad/Id"),
            duplicate,
            42,
            "startgg",
            null,
        ])));
        let ids: Vec<&str> = kept.iter().map(|site| site.id.as_str()).collect();
        assert_eq!(ids, ["challonge", "emailed"]);
        assert_eq!(kept[0].display_name, "Brackets");

        for sites in [
            None,
            Some(serde_json::Value::Null),
            Some(serde_json::json!({ "id": "x" })),
        ] {
            assert!(listed(sites).is_empty());
        }
        let many = (0..40).map(|n| site(&format!("site-{n}"))).collect();
        assert_eq!(
            listed(Some(serde_json::Value::Array(many))).len(),
            MAX_LISTED
        );
        // An entry giving a field twice is not read as a site.
        let twice = r#"[{"id":"a","id":"b","display_name":"A","sign_in":true,
            "authorize_origin":"https://a.example","authorize_path":"/x"}]"#;
        let twice = RawValue::from_string(twice.to_owned()).unwrap();
        assert!(super::listed(Some(&twice)).is_empty());
    }

    #[test]
    fn site_ids_are_checked_before_any_request() {
        assert!(routes("challonge", false).is_ok() && routes("brackets-plus", true).is_ok());
        for site in ["", "Challonge", "a/b", "../discord", &"x".repeat(33)] {
            assert!(
                routes(site, false).is_err() && routes(site, true).is_err(),
                "{site}"
            );
        }
    }

    #[test]
    fn an_organizer_sign_in_uses_its_own_path_for_all_three() {
        let organizer = routes("challonge", true).unwrap();
        assert_eq!(organizer.start_path, "/v1/sites/challonge/organize");
        assert_eq!(organizer.account_path, organizer.start_path);
        let player = routes("challonge", false).unwrap();
        assert_ne!(player.start_path, organizer.start_path);
    }

    #[test]
    fn an_older_bridge_offers_no_organizer_sign_in() {
        let site = |extra: serde_json::Value| {
            let mut entry = serde_json::json!({
                "id": "challonge", "display_name": "Challonge", "sign_in": true,
                "authorize_origin": "https://api.challonge.com", "authorize_path": "/oauth/authorize",
            });
            entry
                .as_object_mut()
                .unwrap()
                .extend(extra.as_object().unwrap().clone());
            listed(Some(serde_json::json!([entry])))[0].organizer
        };
        assert!(!site(serde_json::json!({})));
        assert!(site(serde_json::json!({ "organizer": true })));
    }
}
