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
        ("/desk/sets.js", "text/javascript; charset=utf-8"),
        ("/desk/public.js", "text/javascript; charset=utf-8"),
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

/// Makes event `id` with `title`, public or not, as the organizer routes
/// would leave it.
async fn event(bridge: &Bridge, id: &'static str, title: &'static str, public: bool) {
    bridge
        .state()
        .db
        .write(move |tx| {
            tx.execute(
                "INSERT INTO organizer_events (id, ember_id, title, status, starts_at, settings,
                     created_at, public)
                 VALUES (?1, 'emb_organizer', ?2, 'draft', 1792950000, '{}', 1, ?3)",
                rusqlite::params![id, title, public],
            )?;
            Ok(())
        })
        .await
        .unwrap();
}

#[tokio::test]
async fn a_public_event_has_a_page_with_its_title_and_start_for_link_previews() {
    let bridge = Bridge::start().await;
    event(&bridge, "oev_open", "Ember Weekly <13> & friends", true).await;
    event(&bridge, "oev_closed", "Private", false).await;

    let served = bridge
        .client
        .get(bridge.url("/t/oev_open"))
        .send()
        .await
        .unwrap();
    assert_eq!(served.status(), StatusCode::OK);
    assert_eq!(served.headers()["content-type"], "text/html; charset=utf-8");
    let policy = served.headers()["content-security-policy"]
        .to_str()
        .unwrap();
    assert!(policy.starts_with("default-src 'none'; script-src 'self'"));
    let page = served.text().await.unwrap();
    // Written in for Discord's preview, which runs no script; escaped.
    assert!(
        page.contains(
            r#"<meta property="og:title" content="Ember Weekly &lt;13&gt; &amp; friends">"#
        )
    );
    assert!(
        page.contains(r#"content="USF4 on Ember · 2026-10-25 17:40 UTC">"#),
        "{page}"
    );
    assert!(!page.contains("<script>") && !page.contains("style=") && !page.contains("{{"));

    for missing in ["/t/oev_closed", "/t/oev_nothing"] {
        let served = bridge.client.get(bridge.url(missing)).send().await.unwrap();
        assert_eq!(served.status(), StatusCode::NOT_FOUND, "{missing}");
    }
}
