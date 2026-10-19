//! The web desk's page and its files (docs/design/TOURNAMENT_DESK.md, 4a):
//! served with their types, and a policy that lets them load nothing from
//! anywhere else.
mod common;

use common::Bridge;
use reqwest::StatusCode;

#[tokio::test]
async fn the_desk_page_and_its_files_are_served_with_a_strict_policy() {
    let bridge = Bridge::start().await;
    for (path, kind) in [
        ("/desk", "text/html; charset=utf-8"),
        ("/desk/desk.css", "text/css; charset=utf-8"),
        ("/desk/desk.js", "text/javascript; charset=utf-8"),
        ("/desk/ember-emblem.png", "image/png"),
        ("/desk/inter-latin-400.woff2", "font/woff2"),
    ] {
        let served = bridge.client.get(bridge.url(path)).send().await.unwrap();
        assert_eq!(served.status(), StatusCode::OK, "{path}");
        assert_eq!(served.headers()["content-type"], kind, "{path}");
        let policy = served.headers()["content-security-policy"]
            .to_str()
            .unwrap();
        assert!(
            policy.starts_with("default-src 'none'; script-src 'self'"),
            "{path}"
        );
        assert_eq!(served.headers()["x-content-type-options"], "nosniff");
    }
    let page = bridge.client.get(bridge.url("/desk")).send().await.unwrap();
    let page = page.text().await.unwrap();
    // Nothing inline for the policy to refuse.
    assert!(!page.contains("<script>") && !page.contains("style="));
    let missing = bridge
        .client
        .get(bridge.url("/desk/OFL.txt"))
        .send()
        .await
        .unwrap();
    assert_eq!(missing.status(), StatusCode::NOT_FOUND);
}
