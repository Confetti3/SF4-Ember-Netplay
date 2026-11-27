//! The website's signed-in routes (docs/design/SITE_PLAYER_CARDS.md): a
//! browser signs in with Discord on embernetplay.link, and its session reads
//! its player's own card, points, places and rating, and other players'
//! cards. The site passes these routes on under `/account/`, so the cookies
//! are the site's own.
//!
//! Signing in connects nothing: the session only names a Discord account,
//! and what it reads belongs to that account because its player connected it
//! in the game. A browser whose account no Ember ID holds is told so and
//! reads nothing. Nothing here writes to the database, and a session is not
//! stored: it reads what its player already sees in the game and changes
//! nothing, so signing out clears the cookie in that browser only, and
//! `website.sessions_from` ends every session at once.
use std::sync::Arc;

use axum::{
    Router,
    extract::{Query, State, rejection::QueryRejection},
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Redirect, Response},
    routing::{get, post},
};
use ember_protocol::{
    api::ErrorCode,
    levels::{CardSearch, MAX_SEARCH, OwnLevels, PlayerCard, SearchHit, is_card_token},
    sign_in::SIGN_IN_SECS,
};
use rusqlite::Transaction;
use serde::{Deserialize, Serialize};

use crate::{
    AppState, cards,
    cards::{Purpose, Reader, Viewer},
    config::Website,
    error::{ApiFailure, Result},
    friends::{self, Classes},
    http::{Body, Cookies, GENERAL_BODY, cookie, cookie_is, ok},
    linking::Site,
    routes::{
        account::Bucket,
        boards::{self, Boards},
        profiles,
        rooms::{self, Address},
        sign_in::{self, OAuth, SignedIn},
    },
    sealed::Sealed,
    secrets::Keys,
    skill::{
        boards::Places,
        explain::{self, OwnRating, Rules},
        levels,
    },
    util::random,
};

/// Where the site passes these routes on, and so where its cookies go: no
/// other request to the site carries a session.
const ACCOUNT_PATH: &str = "/account";
/// The cookie that holds a session.
const SESSION_COOKIE: &str = "ember_site";
/// The cookie that ties a sign-in to the browser that started it.
const STATE_COOKIE: &str = "ember_site_state";
/// Sign-ins started in an hour from one network address, and across the
/// bridge. Each started sign-in is good for one question to Discord
/// (`signed_in`), so these bound what the website asks Discord, whose answers
/// the game's own sign-in needs too.
const STARTS_PER_ADDRESS: usize = 10;
const STARTS_PER_HOUR: usize = 240;
/// The longest code Discord's answer may carry.
const MAX_CODE: usize = 256;
/// The reason a signed-in browser is refused a card: its Discord account is
/// connected to no Ember ID.
const NO_EMBER_ID: &str = "no_ember_id";
/// A player's own page. It is read once a visit, and again after signing in.
const OWN: Bucket = Bucket {
    name: "website-me",
    uses: 30,
    window: 10 * 60,
};
/// Other players' cards: as many as the game's own look-up allows
/// (`routes::levels`), for someone clicking through a board.
const CARDS: Bucket = Bucket {
    name: "website-card",
    uses: 120,
    window: 10 * 60,
};

pub fn routes(app: Router<AppState>) -> Router<AppState> {
    app.route("/v1/website/sign-in", get(start))
        .route("/v1/website/discord/callback", get(callback))
        .route("/v1/website/sign-out", post(sign_out))
        .route("/v1/website/me", get(own))
        .route("/v1/website/card", post(card))
}

/// The website, where this bridge serves its signed-in pages: it is
/// configured, public rooms are on (only their games earn anything to show)
/// and Discord sign-in is offered. Elsewhere every route here is 404.
fn website(state: &AppState) -> Result<&Website> {
    state
        .config
        .website
        .as_ref()
        .filter(|_| rooms::enabled(state) && sign_in::offered(state, &Site::Discord))
        .ok_or_else(ApiFailure::not_found)
}

impl Website {
    /// Where Discord sends the browser back to: the site's own address for
    /// the callback, which the Discord application's redirect list holds.
    fn redirect_uri(&self) -> String {
        format!("{}{ACCOUNT_PATH}/discord/callback", self.origin)
    }

    /// Sets one of the site's cookies; `max_age` 0 removes it.
    fn set_cookie(&self, response: &mut Response, name: &str, value: &str, max_age: u64) {
        let cookies = Cookies {
            path: ACCOUNT_PATH,
            secure: self.origin.starts_with("https://"),
        };
        cookies.set(response, name, value, max_age);
    }

    /// Whether a request came from the site's own pages: its browser says
    /// so (`Sec-Fetch-Site`), or, from a browser that does not say, its
    /// `Origin` is the site's. Scripts on other sites can set neither.
    fn sent(&self, headers: &HeaderMap) -> bool {
        let said = |name| headers.get(name).and_then(|value| value.to_str().ok());
        match said("sec-fetch-site") {
            Some(from) => from == "same-origin",
            None => said(header::ORIGIN.as_str()) == Some(self.origin.as_str()),
        }
    }
}

/// `GET /v1/website/sign-in`: sends the browser to Discord to sign in. The
/// state is a token only this bridge seals, good for ten minutes, and the
/// browser keeps it in a cookie.
async fn start(State(state): State<AppState>, Address(address): Address) -> Result<Response> {
    let site = website(&state)?;
    let oauth = OAuth::of(&state, &Site::Discord).ok_or_else(ApiFailure::not_found)?;
    let now = state.now();
    for (key, limit) in [
        (format!("website-start:{address}"), STARTS_PER_ADDRESS),
        ("website-starts".to_owned(), STARTS_PER_HOUR),
    ] {
        state
            .rate
            .check(&key, limit, 3600, now)
            .map_err(ApiFailure::rate_limited)?;
    }
    let sign_in = Sealed::SignIn.seal(&state.keys, u64::from_le_bytes(random()), now);
    let authorize = oauth
        .authorize_url(&site.redirect_uri(), &sign_in)
        .ok_or_else(ApiFailure::unavailable)?;
    let mut response = Redirect::to(&authorize).into_response();
    site.set_cookie(&mut response, STATE_COOKIE, &sign_in, SIGN_IN_SECS);
    Ok(response)
}

/// Discord's answer to a sign-in.
#[derive(Deserialize)]
struct Answer {
    code: Option<String>,
    state: Option<String>,
    error: Option<String>,
}

/// `GET /v1/website/discord/callback`: the browser back from Discord. It
/// leaves for the site's own page signed in, or with a mark that says the
/// sign-in did not work, and either way without the state it started with.
async fn callback(
    State(state): State<AppState>,
    headers: HeaderMap,
    answer: std::result::Result<Query<Answer>, QueryRejection>,
) -> Result<Response> {
    let site = website(&state)?;
    let now = state.now();
    let session = match answer {
        Ok(Query(answer)) => signed_in(&state, site, &headers, &answer, now)
            .await
            .and_then(|account| Sealed::Session.name(&state.keys, &account, now)),
        Err(_) => None,
    };
    let mark = if session.is_some() {
        ""
    } else {
        "#sign-in-failed"
    };
    let mut response = Redirect::to(&format!("{}/me{mark}", site.origin)).into_response();
    site.set_cookie(&mut response, STATE_COOKIE, "", 0);
    if let Some(session) = session {
        let valid = Sealed::Session.valid_secs();
        site.set_cookie(&mut response, SESSION_COOKIE, &session, valid);
    }
    Ok(response)
}

/// The Discord account that signed in. Discord is asked only for a sign-in
/// this bridge started in the last ten minutes, come back to the browser
/// that started it, and only once for each: so the starts' limits bound what
/// anyone can make the bridge ask Discord.
async fn signed_in(
    state: &AppState,
    site: &Website,
    headers: &HeaderMap,
    answer: &Answer,
    now: u64,
) -> Option<String> {
    let (code, returned) = match (&answer.error, &answer.code, &answer.state) {
        (None, Some(code), Some(state)) if !code.is_empty() && code.len() <= MAX_CODE => {
            (code, state)
        }
        _ => return None,
    };
    let started = Sealed::SignIn.open(&state.keys, returned, now)?;
    if !cookie_is(headers, STATE_COOKIE, returned) {
        return None;
    }
    let once = format!("website-finish:{}", started.subject);
    state.rate.check(&once, 1, SIGN_IN_SECS, now).ok()?;
    let oauth = OAuth::of(state, &Site::Discord)?;
    match oauth.user(&state.http, &site.redirect_uri(), code).await? {
        (SignedIn::User(user), _) => Some(user.id),
        (SignedIn::Unusable(_), _) => None,
    }
}

/// `POST /v1/website/sign-out`: this browser's session ends. Only the site's
/// own pages may ask, so no other site signs a player out.
async fn sign_out(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let site = website(&state)?;
    if !site.sent(&headers) {
        return Err(ApiFailure::forbidden());
    }
    let mut response = StatusCode::NO_CONTENT.into_response();
    site.set_cookie(&mut response, SESSION_COOKIE, "", 0);
    Ok(response)
}

/// Who a browser is to the signed-in routes.
enum Browser {
    SignedOut,
    /// Signed in with a Discord account that no Ember ID holds.
    Unconnected,
    /// Signed in as the player with this Discord account.
    Player(String),
}

impl Browser {
    /// The browser behind a request, within `bucket` once it is signed in:
    /// its session, read while the site still honours it, and whether an
    /// Ember ID holds its account.
    async fn admit(
        state: &AppState,
        site: &Website,
        headers: &HeaderMap,
        bucket: &Bucket,
        now: u64,
    ) -> Result<Self> {
        let session = cookie(headers, SESSION_COOKIE)
            .and_then(|session| Sealed::Session.open(&state.keys, &session, now))
            .filter(|session| session.sealed_at >= site.sessions_from);
        let Some(session) = session else {
            return Ok(Self::SignedOut);
        };
        let account = session.account();
        bucket.spend(state, &account, now)?;
        let held = account.clone();
        let connected = state
            .db
            .read(move |tx| Ok(Site::Discord.owner(tx, &held)?.is_some()))
            .await?;
        Ok(if connected {
            Self::Player(account)
        } else {
            Self::Unconnected
        })
    }
}

/// What every signed-in read needs of the bridge beyond the transaction.
struct Reading {
    keys: Arc<Keys>,
    classes: Classes,
    friends_offered: bool,
    boards: Arc<Boards>,
    now: u64,
}

impl Reading {
    async fn of(state: &AppState, now: u64) -> Result<Self> {
        Ok(Self {
            keys: state.keys.clone(),
            classes: Classes::of(state),
            friends_offered: friends::offered(state),
            boards: boards::boards(state).await?,
            now,
        })
    }

    fn viewer<'a>(&'a self, caller: &'a str) -> Viewer<'a> {
        Viewer {
            caller,
            friends_offered: self.friends_offered,
            classes: &self.classes,
            now: self.now,
        }
    }

    /// `target`'s card as `caller` sees it, with their places on the boards.
    fn card(
        &self,
        tx: &Transaction<'_>,
        caller: &str,
        target: &str,
    ) -> Result<Option<(PlayerCard, Places)>> {
        let read = Reader::read(tx, &self.viewer(caller), &[target], Purpose::Card)?;
        Ok(read
            .card(target)
            .map(|card| (card, self.boards.snapshot.places(target))))
    }
}

/// What `GET /v1/website/me` answers.
#[derive(Serialize)]
#[serde(tag = "state", rename_all = "snake_case")]
enum Me {
    SignedOut,
    /// Signed in with a Discord account that no Ember ID holds.
    NoAccount,
    SignedIn(Box<Own>),
}

/// A signed-in player's own page.
#[derive(Serialize)]
struct Own {
    card: PlayerCard,
    places: Places,
    /// As `GET /v1/points/me` answers them, without a rival.
    points: OwnLevels,
    /// A fresh token of the player's own card, for a link to share.
    link: String,
    /// Absent before a rated game and while the bridge hides classes.
    #[serde(skip_serializing_if = "Option::is_none")]
    rating: Option<OwnRating>,
    rules: &'static Rules,
}

/// `GET /v1/website/me`: who the browser is signed in as, and their own
/// page.
async fn own(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let site = website(&state)?;
    let now = state.now();
    let account = match Browser::admit(&state, site, &headers, &OWN, now).await? {
        Browser::SignedOut => return Ok(ok(&Me::SignedOut)),
        Browser::Unconnected => return Ok(ok(&Me::NoAccount)),
        Browser::Player(account) => account,
    };
    let reading = Reading::of(&state, now).await?;
    let own = state
        .db
        .read(move |tx| {
            // Both are there for an account an Ember ID holds, which this one
            // was a moment ago.
            let (card, places) = reading
                .card(tx, &account, &account)?
                .ok_or_else(ApiFailure::unavailable)?;
            let link =
                cards::issue(&reading.keys, &account, now).ok_or_else(ApiFailure::unavailable)?;
            let rating = if reading.classes.shown {
                explain::own_rating(tx, &account, now)?
            } else {
                None
            };
            Ok(Own {
                card,
                places,
                points: levels::own_of(tx, &account)?,
                link,
                rating,
                rules: &explain::RULES,
            })
        })
        .await?;
    Ok(ok(&Me::SignedIn(Box::new(own))))
}

/// Whose card is asked for: by its token, or by its player's name.
#[derive(Deserialize)]
#[serde(deny_unknown_fields, rename_all = "snake_case")]
enum Asked {
    Card(String),
    Name(String),
}

/// What `POST /v1/website/card` answers: a card with its player's places
/// (`card` is null when there is none to show), or the players to choose
/// from when several have the name.
#[derive(Serialize)]
#[serde(untagged)]
enum Found {
    Card {
        card: Option<Box<PlayerCard>>,
        #[serde(skip_serializing_if = "Option::is_none")]
        places: Option<Places>,
    },
    Players {
        players: Vec<SearchHit>,
    },
}

impl From<Option<(PlayerCard, Places)>> for Found {
    fn from(shown: Option<(PlayerCard, Places)>) -> Self {
        let (card, places) = shown.unzip();
        Self::Card {
            card: card.map(Box::new),
            places,
        }
    }
}

/// `POST /v1/website/card`: another player's card for the signed-in player,
/// who has a Discord account an Ember ID holds: a block is between such
/// accounts, so nobody else is shown a card (docs/design/PROFILE.md,
/// "Privacy").
async fn card(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let site = website(&state)?;
    let now = state.now();
    let caller = match Browser::admit(&state, site, &headers, &CARDS, now).await? {
        Browser::SignedOut => return Err(ApiFailure::unauthenticated()),
        Browser::Unconnected => {
            return Err(ApiFailure::new(
                ErrorCode::Forbidden,
                "Connect this Discord account in Ember to open player cards.",
            )
            .detail("reason", NO_EMBER_ID));
        }
        Browser::Player(account) => account,
    };
    let asked: Asked = body.parse()?;
    let reading = Reading::of(&state, now).await?;
    let found = state
        .db
        .read(move |tx| find(tx, &reading, &caller, asked))
        .await?;
    Ok(ok(&found))
}

/// The card `asked` names for `caller`. A token names its player. A name
/// finds who the game's own search finds (`cards::find_by_name`,
/// `profiles::shown_to`), so never the caller, a player who blocked them or
/// one who left search: the one player with exactly that name, in any case,
/// or the first `MAX_SEARCH` of several to choose from.
fn find(tx: &Transaction<'_>, reading: &Reading, caller: &str, asked: Asked) -> Result<Found> {
    let target = match asked {
        Asked::Card(token) if is_card_token(&token) => {
            cards::open(&reading.keys, &token, reading.now)
        }
        Asked::Card(_) => return Err(ApiFailure::invalid("card must be a card token")),
        Asked::Name(name) => {
            let search = CardSearch {
                name: Some(name),
                code: None,
            };
            search.check()?;
            let wanted = search.trimmed_name().unwrap_or_default();
            let named = cards::find_by_name(tx, wanted, profiles::SCAN)?
                .into_iter()
                .filter(|found| found.name.eq_ignore_ascii_case(wanted))
                .collect();
            let (mut shown, read) = profiles::shown_to(tx, &reading.viewer(caller), named)?;
            if shown.len() > 1 {
                return Ok(Found::Players {
                    players: shown
                        .iter()
                        .filter_map(|found| profiles::hit(&reading.keys, &read, found, reading.now))
                        .take(MAX_SEARCH)
                        .collect(),
                });
            }
            shown.pop().map(|only| only.account)
        }
    };
    let shown = match target {
        Some(target) => reading.card(tx, caller, &target)?,
        None => None,
    };
    Ok(shown.into())
}
