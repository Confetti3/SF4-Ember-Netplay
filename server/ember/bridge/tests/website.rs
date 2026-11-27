//! The website's routes (docs/design/SITE_PLAYER_CARDS.md): the points
//! boards anyone reads, the Discord sign-in that ends in the site's session
//! cookie, and what a session reads: its player's own page and other players'
//! cards. No answer names a Discord account or an Ember ID (`World`).
mod common;

use std::sync::Mutex;

use common::{
    Bridge,
    profiles::*,
    sign_in::{self, Code, FakeSite},
    supervisor,
};
use ember_bridge::{config, integrations::Secrets};
use ember_protocol::levels::is_card_token;
use reqwest::{Method, StatusCode};
use serde_json::{Value as Json_, json};
use zeroize::Zeroizing;

const SITE: &str = "https://site.example";
const BOARD: &str = "/v1/points/board/public";
const ME: &str = "/v1/website/me";
const CARD: &str = "/v1/website/card";
const SESSION: &str = "ember_site";
const STATE: &str = "ember_site_state";
/// The Discord accounts the stand-in signs in, which are `account(1)` to
/// `account(3)`.
const ACCOUNTS: [&str; 3] = [
    "381906487192731649",
    "381906487192731650",
    "381906487192731651",
];
const DAY: u64 = 24 * 60 * 60;

/// A bridge with public rooms, friends, Discord sign-in and the website's
/// routes, whose clock moves only when a test moves it. Codes `good-1` to
/// `good-3` sign in `ACCOUNTS`.
async fn start(classes: bool) -> World {
    start_with(classes, |_| {}).await
}

/// `start`, with the configuration changed by `more`.
async fn start_with(classes: bool, more: impl FnOnce(&mut config::Config)) -> World {
    let discord = FakeSite::new(
        "/api/oauth2/token",
        "/api/users/@me",
        ("1546980049692135514", "test-secret"),
        "/account/discord/callback",
        vec![
            ("good-1", Code::SignsIn((ACCOUNTS[0], "one"))),
            ("good-2", Code::SignsIn((ACCOUNTS[1], "two"))),
            ("good-3", Code::SignsIn((ACCOUNTS[2], "three"))),
        ],
        &[],
        |(id, username)| json!({ "id": id, "username": username }),
    );
    let api_base = format!("{}/api", discord.serve().await);
    let (fake, url) = supervisor::fake_supervisor().await;
    let bridge = Bridge::start_with(
        |config| {
            supervisor::enable(config, url);
            let rooms = config.rooms.as_mut().unwrap();
            rooms.friends = true;
            rooms.skill_badges = classes;
            config.discord = Some(config::Discord {
                client_id: "1546980049692135514".into(),
                api_base,
            });
            config.website = Some(config::Website {
                origin: SITE.into(),
                sessions_from: 0,
            });
            more(config);
        },
        Secrets {
            discord_client_secret: Some(Zeroizing::new("test-secret".into())),
            ..supervisor::secrets()
        },
    )
    .await;
    bridge.clock.freeze();
    World {
        bridge,
        _fake: fake,
        answers: Mutex::default(),
        accounts: Mutex::default(),
        ember_ids: Mutex::default(),
    }
}

/// Player `n` (1 to 3), whose game sends `name`, takes part and is listed.
async fn listed(world: &World, n: u8, name: &'static str, country: &str) -> Member {
    let member = world.member(n, u64::from(n), name).await;
    assert_eq!(member.discord, ACCOUNTS[usize::from(n) - 1]);
    world.sync_as(&member, name, country, 20).await;
    let listing = json!({ "ranked": true, "listing": { "name": name, "country": country } });
    let (status, answer) = world
        .call(Method::PUT, &member, "/v1/ranking/me", Some(listing))
        .await;
    assert_eq!(status, StatusCode::OK, "{answer}");
    member
}

/// `member`'s counted games: the account's, and one fighter's when named.
async fn counted(world: &World, member: &Member, fighter: Option<u8>, games: u64, wins: u64) {
    let account = &member.discord;
    world
        .sql(match fighter {
            None => format!(
                "INSERT INTO account_points (discord_user_id, games, wins, first_at, played_at)
                 VALUES ('{account}', {games}, {wins}, 1, 2)"
            ),
            Some(fighter) => format!(
                "INSERT INTO fighter_levels (discord_user_id, fighter, games, wins, first_at, played_at)
                 VALUES ('{account}', {fighter}, {games}, {wins}, 1, 2)"
            ),
        })
        .await;
}

/// Gives `member` a rating last shown as `band`, played `days_ago`.
async fn rated(
    world: &World,
    member: &Member,
    rating: f64,
    deviation: f64,
    band: u8,
    days_ago: u64,
) {
    let played_at = world.now() - days_ago * DAY;
    world
        .sql(format!(
            "INSERT INTO skill_ratings (discord_user_id, rating, deviation, volatility, band, played_at)
             VALUES ('{}', {rating}, {deviation}, 0.0594, {band}, {played_at})",
            member.discord
        ))
        .await;
}

/// The whole of what `response` says of cookie `name`, after its `=`.
fn cookie_set(response: &reqwest::Response, name: &str) -> Option<String> {
    response
        .headers()
        .get_all("set-cookie")
        .iter()
        .filter_map(|value| value.to_str().ok())
        .find_map(|value| value.strip_prefix(&format!("{name}=")))
        .map(str::to_owned)
}

fn value_of(cookie: &str) -> &str {
    cookie.split(';').next().unwrap_or_default()
}

/// Starts a sign-in: the state Discord would hand back and the state cookie.
async fn start_sign_in(world: &World) -> (String, String) {
    let bridge = &world.bridge;
    let started = bridge
        .client
        .get(bridge.url("/v1/website/sign-in"))
        .send()
        .await
        .unwrap();
    assert_eq!(started.status(), StatusCode::SEE_OTHER);
    let location = url::Url::parse(started.headers()["location"].to_str().unwrap()).unwrap();
    assert_eq!(location.path(), "/oauth2/authorize");
    let query = sign_in::query(&location);
    assert_eq!(query["scope"], "identify");
    assert_eq!(
        query["redirect_uri"],
        format!("{SITE}/account/discord/callback")
    );
    let kept = cookie_set(&started, STATE).unwrap();
    assert!(
        kept.ends_with("; Path=/account; HttpOnly; SameSite=Lax; Max-Age=600; Secure"),
        "{kept}"
    );
    assert_eq!(value_of(&kept), query["state"]);
    (query["state"].clone(), value_of(&kept).to_owned())
}

/// The browser back from Discord with `query`, holding state cookie `kept`.
async fn back(world: &World, query: &str, kept: Option<&str>) -> reqwest::Response {
    let bridge = &world.bridge;
    let request = bridge
        .client
        .get(bridge.url(&format!("/v1/website/discord/callback?{query}")));
    let request = match kept {
        Some(kept) => request.header("cookie", format!("{STATE}={kept}")),
        None => request,
    };
    let back = request.send().await.unwrap();
    assert_eq!(back.status(), StatusCode::SEE_OTHER);
    // The state is spent either way.
    assert!(cookie_set(&back, STATE).unwrap().contains("Max-Age=0"));
    back
}

/// Signs a browser in with `code`: its session cookie's value.
async fn signed_in(world: &World, code: &str) -> String {
    let (state, kept) = start_sign_in(world).await;
    let back = back(world, &format!("code={code}&state={state}"), Some(&kept)).await;
    assert_eq!(back.headers()["location"], format!("{SITE}/me"));
    let session = cookie_set(&back, SESSION).unwrap();
    assert!(
        session.ends_with("; Path=/account; HttpOnly; SameSite=Lax; Max-Age=2592000; Secure"),
        "{session}"
    );
    value_of(&session).to_owned()
}

/// A request from a browser holding `session`; its answer is kept for the
/// world's scan.
async fn browse(
    world: &World,
    method: Method,
    path: &str,
    session: Option<&str>,
    body: Option<Json_>,
) -> (StatusCode, Json_) {
    let bridge = &world.bridge;
    let mut request = bridge.client.request(method, bridge.url(path));
    if let Some(session) = session {
        request = request.header("cookie", format!("{SESSION}={session}"));
    }
    if let Some(body) = body {
        request = request.body(body.to_string());
    }
    let (status, answer) = common::read(request.send().await.unwrap()).await;
    world.record(path, &answer);
    (status, answer)
}

async fn board(world: &World, query: &str) -> Json_ {
    let (status, board) = browse(world, Method::GET, &format!("{BOARD}{query}"), None, None).await;
    assert_eq!(status, StatusCode::OK, "{board}");
    board
}

async fn own(world: &World, session: &str) -> Json_ {
    let (status, own) = browse(world, Method::GET, ME, Some(session), None).await;
    assert_eq!(status, StatusCode::OK, "{own}");
    own
}

async fn card(world: &World, session: &str, asked: Json_) -> Json_ {
    let (status, found) = browse(world, Method::POST, CARD, Some(session), Some(asked)).await;
    assert_eq!(status, StatusCode::OK, "{found}");
    found
}

// A board lists the players the class board may list, most points first,
// with the place equal points share, and what the class board shows of each.
#[tokio::test]
async fn a_points_board_lists_the_listed_players_in_order() {
    let world = start(true).await;
    let one = listed(&world, 1, "Player 1", "US").await;
    let two = listed(&world, 2, "Player 2", "JP").await;
    let three = listed(&world, 3, "Player 3", "GB").await;
    // Most points of all, and not listed: its game sent no name.
    let unlisted = world.member(4, 4, "Player 4").await;
    counted(&world, &unlisted, None, 9000, 9000).await;
    counted(&world, &unlisted, Some(20), 9000, 9000).await;
    // 600 points each, 12 for the third.
    counted(&world, &one, None, 400, 200).await;
    counted(&world, &two, None, 400, 200).await;
    counted(&world, &three, None, 10, 2).await;
    counted(&world, &one, Some(20), 150, 60).await;
    counted(&world, &three, Some(20), 5, 1).await;
    // A settled class A, a rating still placing, and none.
    rated(&world, &one, 1742.0, 68.0, 7, 0).await;
    rated(&world, &two, 1600.0, 300.0, 0, 0).await;

    let account = board(&world, "").await;
    assert_eq!(account["total"], 3);
    assert_eq!(account["listed_at"], world.now());
    assert!(account.get("fighter").is_none(), "{account}");
    let lines = account["players"].as_array().unwrap();
    let shown: Vec<_> = lines
        .iter()
        .map(|line| {
            (
                &line["place"],
                &line["name"],
                &line["points"],
                &line["tier"],
            )
        })
        .collect();
    assert_eq!(
        shown,
        [
            (&json!(1), &json!("Player 1"), &json!(600), &json!(1)),
            (&json!(1), &json!("Player 2"), &json!(600), &json!(1)),
            (&json!(3), &json!("Player 3"), &json!(12), &json!(0)),
        ]
    );
    assert_eq!(
        (&lines[0]["country"], &lines[0]["band"]),
        (&json!("US"), &json!(7))
    );
    assert!(lines[1].get("band").is_none() && lines[2].get("band").is_none());
    assert!(
        lines
            .iter()
            .all(|line| is_card_token(line["card"].as_str().unwrap()))
    );

    let cammy = board(&world, "?fighter=20").await;
    assert_eq!(
        (&cammy["fighter"], &cammy["total"]),
        (&json!(20), &json!(2))
    );
    let lines = cammy["players"].as_array().unwrap();
    assert_eq!(
        (&lines[0]["name"], &lines[0]["points"], &lines[0]["tier"]),
        (&json!("Player 1"), &json!(210), &json!(1))
    );
    assert_eq!(
        (&lines[1]["place"], &lines[1]["points"]),
        (&json!(2), &json!(6))
    );
    let nobody = board(&world, "?fighter=43").await;
    assert_eq!(
        (&nobody["total"], &nobody["players"]),
        (&json!(0), &json!([]))
    );

    for query in ["?fighter=44", "?fighter=", "?fighter=x", "?board=bp"] {
        let (status, _) = browse(&world, Method::GET, &format!("{BOARD}{query}"), None, None).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{query}");
    }

    // The boards are read at most once a minute, all together. A player who
    // stops taking part is then on none of them.
    let stop = json!({ "ranked": false });
    world
        .call(Method::PUT, &one, "/v1/ranking/me", Some(stop))
        .await;
    world.bridge.clock.advance(59);
    assert_eq!(board(&world, "").await["total"], 3);
    world.bridge.clock.advance(1);
    assert_eq!(board(&world, "").await["total"], 2);
    assert_eq!(
        board(&world, "?fighter=20").await["players"][0]["name"],
        "Player 3"
    );
}

// A bridge that hides classes lists no class on a board either.
#[tokio::test]
async fn a_board_shows_no_class_while_the_bridge_hides_them() {
    let world = start(false).await;
    let one = listed(&world, 1, "Player 1", "US").await;
    counted(&world, &one, None, 10, 5).await;
    rated(&world, &one, 1742.0, 68.0, 7, 0).await;
    let line = &board(&world, "").await["players"][0];
    assert_eq!(line["points"], 15);
    assert!(line.get("band").is_none(), "{line}");
}

// Only the browser that started a sign-in finishes it, and it ends in the
// site's session cookie. Signing in connects nothing: an account no Ember ID
// holds reads nothing until its player connects it in the game.
#[tokio::test]
async fn a_browser_signs_in_with_discord_on_the_site() {
    let world = start(true).await;
    // Another browser, a wrong state, a state this bridge never sealed (in
    // the cookie too), a refused code and no code at all.
    let (state, kept) = start_sign_in(&world).await;
    let good = format!("code=good-1&state={state}");
    for (query, kept) in [
        (good.as_str(), None),
        (good.as_str(), Some("someone-elses-state")),
        ("code=good-1&state=another", Some(kept.as_str())),
        ("code=good-1&state=forged", Some("forged")),
        ("code=good-1&code=good-2&state=forged", Some("forged")),
        (&format!("code=bad&state={state}"), Some(kept.as_str())),
        (
            &format!("error=access_denied&state={state}"),
            Some(kept.as_str()),
        ),
    ] {
        let back = back(&world, query, kept).await;
        assert_eq!(
            back.headers()["location"],
            format!("{SITE}/me#sign-in-failed")
        );
        assert!(cookie_set(&back, SESSION).is_none(), "{query}");
    }

    // A sign-in asks Discord once: the refused code above spent that one,
    // and a state is good for ten minutes.
    let spent = back(&world, &good, Some(&kept)).await;
    assert!(cookie_set(&spent, SESSION).is_none());
    let (late, kept) = start_sign_in(&world).await;
    world.bridge.clock.advance(601);
    let expired = back(&world, &format!("code=good-1&state={late}"), Some(&kept)).await;
    assert!(cookie_set(&expired, SESSION).is_none());

    let session = signed_in(&world, "good-1").await;
    assert_eq!(
        own(&world, &session).await,
        json!({ "state": "no_account" })
    );
    let one = world.member(1, 1, "Player 1").await;
    let signed = own(&world, &session).await;
    assert_eq!(signed["state"], "signed_in");
    assert!(is_card_token(signed["link"].as_str().unwrap()));

    // No cookie, a cookie that is not a session, and a card token anyone in a
    // room is shown, under either prefix: nobody is signed in.
    let card = world.token(&one);
    let as_session = format!("s1.{}", card.split_once('.').unwrap().1);
    for cookie in [
        None,
        Some("s1.AAAA"),
        Some(card.as_str()),
        Some(as_session.as_str()),
    ] {
        let (status, answer) = browse(&world, Method::GET, ME, cookie, None).await;
        assert_eq!(
            (status, &answer),
            (StatusCode::OK, &json!({ "state": "signed_out" }))
        );
    }
    // A session is read for 30 days.
    world.bridge.clock.advance(30 * DAY as i64);
    assert_eq!(own(&world, &session).await["state"], "signed_in");
    world.bridge.clock.advance(1);
    assert_eq!(own(&world, &session).await["state"], "signed_out");

    // Signing out ends the session in that browser, asked from the site's
    // own pages and from nowhere else: the browser says where a request is
    // from, and one that does not say sends the page's origin. A page whose
    // policy sends no referrer sends the origin as `null`.
    let bridge = &world.bridge;
    let sign_out = |headers: &'static [(&'static str, &'static str)]| {
        let request = bridge.client.post(bridge.url("/v1/website/sign-out"));
        headers
            .iter()
            .fold(request, |request, (name, value)| {
                request.header(*name, *value)
            })
            .send()
    };
    let refused: [&[(&str, &str)]; 5] = [
        &[],
        &[("origin", "https://elsewhere.example")],
        &[("origin", "null")],
        &[("sec-fetch-site", "cross-site"), ("origin", SITE)],
        &[("sec-fetch-site", "same-site"), ("origin", SITE)],
    ];
    for headers in refused {
        let refused = sign_out(headers).await.unwrap();
        assert_eq!(refused.status(), StatusCode::FORBIDDEN, "{headers:?}");
        assert!(cookie_set(&refused, SESSION).is_none());
    }
    let allowed: [&[(&str, &str)]; 2] = [
        &[("sec-fetch-site", "same-origin"), ("origin", "null")],
        &[("origin", SITE)],
    ];
    for headers in allowed {
        let out = sign_out(headers).await.unwrap();
        assert_eq!(out.status(), StatusCode::NO_CONTENT, "{headers:?}");
        assert!(
            cookie_set(&out, SESSION)
                .unwrap()
                .starts_with("; Path=/account; HttpOnly; SameSite=Lax; Max-Age=0")
        );
    }
}

// A signed-in player's own page: their card, their points with what only
// they see of them, their place on each board, and their rating with the
// rules that made it.
#[tokio::test]
async fn a_signed_in_player_reads_their_own_page() {
    let world = start(true).await;
    let one = listed(&world, 1, "Player 1", "US").await;
    let two = listed(&world, 2, "Player 2", "JP").await;
    counted(&world, &one, None, 300, 100).await;
    counted(&world, &one, Some(20), 150, 60).await;
    counted(&world, &two, None, 400, 200).await;
    counted(&world, &two, Some(20), 10, 0).await;
    rated(&world, &one, 1742.0, 68.0, 7, 0).await;
    let session = signed_in(&world, "good-1").await;

    let own_page = own(&world, &session).await;
    let card = &own_page["card"];
    assert_eq!(
        (&card["name"], &card["country"], &card["main"]),
        (&json!("Player 1"), &json!("US"), &json!(20))
    );
    assert_eq!(card["account"], json!({ "points": 400, "tier": 0 }));
    assert_eq!(card["badge"], json!({ "band": 7, "settled": true }));
    assert!(card.get("versus").is_none(), "{card}");
    let points = &own_page["points"];
    assert_eq!(
        (
            &points["points"],
            &points["account"]["games"],
            &points["account"]["wins"]
        ),
        (&json!("counted"), &json!(300), &json!(100))
    );
    assert_eq!(
        (&points["account"]["floor"], &points["account"]["next"]),
        (&json!(0), &json!(500))
    );
    assert_eq!(
        points["fighters"],
        json!([{ "fighter": 20, "points": 210, "games": 150, "wins": 60, "tier": 1,
                 "floor": 200, "next": 700, "since": 1 }])
    );
    assert_eq!(
        own_page["places"],
        json!({ "account": 2, "fighters": [[20, 1]] })
    );

    let rating = &own_page["rating"];
    assert_eq!(
        (
            &rating["takes_part"],
            &rating["rating"],
            &rating["deviation"],
            &rating["volatility"]
        ),
        (&json!(true), &json!(1742.0), &json!(68.0), &json!(0.0594))
    );
    assert_eq!(
        (&rating["band"], &rating["settled"]),
        (&json!(7), &json!(true))
    );
    assert_eq!(
        (&rating["up_at"], &rating["down_below"]),
        (&json!(1825.0), &json!(1675.0))
    );
    assert_eq!(rating["played_at"], world.now());
    let examples = rating["examples"].as_array().unwrap();
    assert_eq!(examples.len(), 4);
    assert_eq!(
        (&examples[0]["band"], &examples[0]["win"]["weight"]),
        (&json!(7), &json!(1.0))
    );
    assert_eq!(
        (&examples[3]["band"], &examples[3]["opponent"]),
        (&json!(0), &json!(1500.0))
    );
    let rules = &own_page["rules"];
    assert_eq!(rules["floors"].as_array().unwrap().len(), 9);
    assert_eq!(
        (
            &rules["floors"][3],
            &rules["margin"],
            &rules["full_repeats"]
        ),
        (&json!(1500.0), &json!(25.0), &json!(8))
    );

    // The link is the player's own card, as another signed-in player opens it.
    let other = signed_in(&world, "good-2").await;
    let opened = card_of(&world, &other, &own_page["link"]).await;
    assert_eq!(opened["card"]["name"], "Player 1");
    // A player with no rated game has no rating to explain, and still the rules.
    let unrated = own(&world, &other).await;
    assert!(unrated.get("rating").is_none(), "{unrated}");
    assert_eq!(unrated["rules"]["start_rating"], 1500.0);

    // A break makes the rating less sure, and after most of a year its class
    // shows faint.
    world.bridge.clock.advance(200 * DAY as i64);
    let later = own(&world, &signed_in(&world, "good-1").await).await;
    assert!(
        later["rating"]["deviation"].as_f64().unwrap() > 150.0,
        "{later}"
    );
    assert_eq!(later["rating"]["settled"], true);
    world.bridge.clock.advance(200 * DAY as i64);
    let faint = own(&world, &signed_in(&world, "good-1").await).await;
    assert_eq!(
        (&faint["rating"]["band"], &faint["rating"]["settled"]),
        (&json!(7), &json!(false))
    );
    assert!(faint["rating"].get("faint_in_days").is_none(), "{faint}");
}

async fn card_of(world: &World, session: &str, token: &Json_) -> Json_ {
    card(world, session, json!({ "card": token })).await
}

// A bridge that hides classes tells no rating and shows no class on a card.
#[tokio::test]
async fn a_bridge_that_hides_classes_tells_no_rating() {
    let world = start(false).await;
    let one = listed(&world, 1, "Player 1", "US").await;
    rated(&world, &one, 1742.0, 68.0, 7, 0).await;
    let own_page = own(&world, &signed_in(&world, "good-1").await).await;
    assert_eq!(own_page["state"], "signed_in");
    assert!(own_page.get("rating").is_none(), "{own_page}");
    assert!(own_page["card"].get("badge").is_none(), "{own_page}");
}

// Another player's card opens by its token from a board and by its player's
// name, for a signed-in player whose account an Ember ID holds, and never
// for one its player blocked.
#[tokio::test]
async fn a_signed_in_player_opens_other_players_cards() {
    let world = start(true).await;
    let one = listed(&world, 1, "Player 1", "US").await;
    let two = listed(&world, 2, "Player 2", "JP").await;
    counted(&world, &one, None, 300, 100).await;
    counted(&world, &one, Some(20), 150, 60).await;
    world
        .sql(format!(
            "INSERT INTO pair_records (first, second, first_wins, second_wins, played_at)
             VALUES ('{}', '{}', 9, 14, 50)",
            one.discord, two.discord
        ))
        .await;
    let session = signed_in(&world, "good-2").await;

    let token = board(&world, "").await["players"][0]["card"].clone();
    let found = card_of(&world, &session, &token).await;
    assert_eq!(
        (&found["card"]["name"], &found["card"]["account"]["points"]),
        (&json!("Player 1"), &json!(400))
    );
    assert_eq!(
        found["card"]["fighters"],
        json!([{ "fighter": 20, "points": 210, "tier": 1 }])
    );
    assert_eq!(
        found["card"]["versus"],
        json!({ "won": 14, "lost": 9, "last_at": 50 })
    );
    assert_eq!(
        found["places"],
        json!({ "account": 1, "fighters": [[20, 1]] })
    );
    // A fighter's games and wins are the player's own to see.
    assert!(found["card"]["fighters"][0].get("games").is_none());

    // By name, in any case. A name nobody has, and the caller's own, find nobody.
    for name in ["Player 1", "player 1", " Player 1 "] {
        let named = card(&world, &session, json!({ "name": name })).await;
        assert_eq!(named["card"]["name"], "Player 1", "{name}");
        assert_eq!(named["places"]["account"], 1);
    }
    for name in ["Player 9", "Player 2", "Player"] {
        let named = card(&world, &session, json!({ "name": name })).await;
        assert_eq!(named, json!({ "card": null }), "{name}");
    }
    // Two players with one name: the caller chooses.
    let three = world.member(3, 3, "Player 1").await;
    world.tick();
    world.sync_as(&three, "Player 1", "GB", 3).await;
    let several = card(&world, &session, json!({ "name": "Player 1" })).await;
    let players = several["players"].as_array().unwrap();
    assert_eq!(players.len(), 2, "{several}");
    assert!(several.get("card").is_none());
    let opened = card_of(&world, &session, &players[1]["card"]).await;
    assert_eq!(opened["card"]["name"], "Player 1");

    // A token that does not open, and a body that asks for nothing known.
    let unopened = format!("c1.{}", "A".repeat(76));
    assert_eq!(
        card_of(&world, &session, &json!(unopened)).await,
        json!({ "card": null })
    );
    for body in [
        json!({ "card": "x" }),
        json!({ "name": "" }),
        json!({}),
        json!({ "card": token, "name": "Player 1" }),
    ] {
        let (status, _) = browse(
            &world,
            Method::POST,
            CARD,
            Some(&session),
            Some(body.clone()),
        )
        .await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "{body}");
    }

    // A player who blocked the caller shows them nothing, by token or by name.
    world
        .sql(format!(
            "INSERT INTO friend_blocks (blocker, blocked, id, name, at) VALUES ('{}', '{}', 'bl_x', 'Player 2', 1)",
            one.discord, two.discord
        ))
        .await;
    assert_eq!(
        card_of(&world, &session, &token).await,
        json!({ "card": null })
    );
    let named = card(&world, &session, json!({ "name": "Player 1" })).await;
    assert_eq!(named["card"]["country"], "GB", "{named}");

    // Nobody signed out reads a card, nor a browser whose account no Ember ID holds.
    let (status, _) = browse(
        &world,
        Method::POST,
        CARD,
        None,
        Some(json!({ "card": token })),
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    world
        .sql(format!(
            "DELETE FROM discord_accounts WHERE user_id = '{}'",
            two.discord
        ))
        .await;
    let (status, refused) = browse(
        &world,
        Method::POST,
        CARD,
        Some(&session),
        Some(json!({ "card": token })),
    )
    .await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{refused}");
    assert_eq!(refused["error"]["details"]["reason"], "no_ember_id");
}

// The website's routes exist only where the bridge is told the site; the
// boards need no more than public rooms.
#[tokio::test]
async fn a_bridge_without_the_website_has_no_signed_in_routes() {
    let world = World::start().await;
    for (method, path) in [
        (Method::GET, "/v1/website/sign-in"),
        (
            Method::GET,
            "/v1/website/discord/callback?code=good-1&state=x",
        ),
        (Method::POST, "/v1/website/sign-out"),
        (Method::GET, ME),
        (Method::POST, CARD),
    ] {
        let (status, _) = browse(&world, method, path, None, None).await;
        assert_eq!(status, StatusCode::NOT_FOUND, "{path}");
    }
    assert_eq!(board(&world, "").await["total"], 0);
    // And no board without public rooms.
    let bridge = Bridge::start().await;
    let plain = bridge.client.get(bridge.url(BOARD)).send().await.unwrap();
    assert_eq!(plain.status(), StatusCode::NOT_FOUND);
}

// A player's own page is read 30 times in ten minutes, cards are opened 120
// times, and sign-ins start 10 times an hour from one address.
#[tokio::test]
async fn the_website_routes_are_limited() {
    let world = start(true).await;
    world.member(1, 1, "Player 1").await;
    let session = signed_in(&world, "good-1").await;
    let nobody = json!({ "name": "Player 9" });
    for _ in 0..120 {
        card(&world, &session, nobody.clone()).await;
    }
    let (status, _) = browse(&world, Method::POST, CARD, Some(&session), Some(nobody)).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS);
    for _ in 0..30 {
        own(&world, &session).await;
    }
    let (status, _) = browse(&world, Method::GET, ME, Some(&session), None).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS);
    // Someone signed out costs nobody's budget.
    let (status, _) = browse(&world, Method::GET, ME, None, None).await;
    assert_eq!(status, StatusCode::OK);
    for _ in 1..10 {
        start_sign_in(&world).await;
    }
    let (status, _) = browse(&world, Method::GET, "/v1/website/sign-in", None, None).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS);
}

// What only its own player is told never rides on another player's card, and
// a board says nothing of a developer's class or of a player who is on one
// fighter's board alone.
#[tokio::test]
async fn a_card_and_a_board_tell_nothing_of_a_rating() {
    let world = start_with(true, |config| {
        config.rooms.as_mut().unwrap().developers = vec![ACCOUNTS[2].into()];
    })
    .await;
    let one = listed(&world, 1, "Player 1", "US").await;
    let two = listed(&world, 2, "Player 2", "JP").await;
    let developer = listed(&world, 3, "Player 3", "GB").await;
    counted(&world, &one, None, 300, 100).await;
    counted(&world, &one, Some(20), 150, 60).await;
    // On a fighter's board and not on the account's.
    counted(&world, &two, Some(20), 10, 0).await;
    counted(&world, &developer, None, 50, 0).await;
    for member in [&one, &two, &developer] {
        rated(&world, member, 1742.0, 68.0, 7, 0).await;
    }

    let lines = board(&world, "").await["players"].clone();
    assert_eq!(
        (&lines[0]["name"], &lines[0]["band"]),
        (&json!("Player 1"), &json!(7))
    );
    assert_eq!(lines[1]["name"], "Player 3");
    assert!(lines[1].get("band").is_none(), "{lines}");

    let session = signed_in(&world, "good-2").await;
    assert_eq!(
        own(&world, &session).await["places"],
        json!({ "fighters": [[20, 2]] })
    );
    let found = card(&world, &session, json!({ "name": "Player 1" })).await;
    assert_eq!(
        found["card"]["badge"],
        json!({ "band": 7, "settled": true })
    );
    let told = found.to_string();
    for private in [
        "rating",
        "deviation",
        "volatility",
        "1742",
        "games\":150",
        "rules",
    ] {
        assert!(!told.contains(private), "{private}: {told}");
    }
    // A developer's card shows their mark in place of a class.
    let found = card(&world, &session, json!({ "name": "Player 3" })).await;
    assert_eq!(found["card"]["badge"]["developer"], true, "{found}");
}

// A player who stopped taking part keeps their rating, is told it does not
// move, and is on no board.
#[tokio::test]
async fn a_player_who_does_not_take_part_is_told_so() {
    let world = start(true).await;
    let one = listed(&world, 1, "Player 1", "US").await;
    counted(&world, &one, None, 300, 100).await;
    rated(&world, &one, 1742.0, 68.0, 7, 0).await;
    let stop = json!({ "ranked": false });
    world
        .call(Method::PUT, &one, "/v1/ranking/me", Some(stop))
        .await;
    let own_page = own(&world, &signed_in(&world, "good-1").await).await;
    assert_eq!(
        (
            &own_page["rating"]["takes_part"],
            &own_page["rating"]["rating"]
        ),
        (&json!(false), &json!(1742.0))
    );
    assert_eq!(own_page["places"], json!({ "fighters": [] }));
    assert_eq!(own_page["points"]["account"]["points"], 400);
    assert!(own_page["card"].get("badge").is_none(), "{own_page}");
}

// The operator ends every session at once: one sealed before
// `website.sessions_from` is no longer read.
#[tokio::test]
async fn sessions_from_before_the_operators_mark_are_signed_out() {
    let world = start_with(true, |config| {
        config.website.as_mut().unwrap().sessions_from = u64::MAX;
    })
    .await;
    world.member(1, 1, "Player 1").await;
    let session = signed_in(&world, "good-1").await;
    assert_eq!(
        own(&world, &session).await,
        json!({ "state": "signed_out" })
    );
    let (status, _) = browse(
        &world,
        Method::POST,
        CARD,
        Some(&session),
        Some(json!({ "name": "Player 1" })),
    )
    .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
}
