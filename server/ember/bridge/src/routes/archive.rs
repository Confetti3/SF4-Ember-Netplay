//! The public replay archive's routes (docs/design/REPLAY_ARCHIVE.md): any
//! game a player sends, `PUT /v1/replays/{crc}`, which `crate::archive`
//! keeps, and `GET /v1/archive`, the stored games for the archive page,
//! which downloads them from the bucket.
use axum::{
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    replays::{self, Replay, Sides},
    rooms::MAX_HOST_NAME as MAX_NAME,
};
use rusqlite::{OptionalExtension, Transaction};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{
    AppState,
    archive::{
        self, Admission, Attribution,
        listing::{self, Cursor, Filter, LIMITS, Order, Page},
    },
    error::{ApiFailure, Result},
    http::{Body, json as respond},
    routes::{
        ledger::scores,
        play::playing,
        rooms::{self, classes_shown},
    },
};

/// `PUT /v1/replays/{crc}?a=&b=&side=&country=`: a player, with their play
/// session, sends a game's `.emberreplay` for the archive. 201 when it is
/// new, 200 when the archive already holds it. Off (404) without an archive.
/// Only a player signed in with Discord shares their games (403
/// `discord_required` otherwise, which every game takes as final for the
/// file); a tournament set's games go through the set's own route.
pub async fn put(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(crc): Path<String>,
    Query(sides): Query<Sides>,
    body: Body<{ replays::MAX_REPLAY }>,
) -> Result<Response> {
    if state.config.archive.is_none() {
        return Err(ApiFailure::not_found());
    }
    let player = playing(&state, &headers).await?;
    let file = body.0;
    let replay = Replay::parse(&file)
        .filter(|replay| replay.crc_name() == crc)
        .ok_or_else(|| ApiFailure::invalid("That is not a whole replay with this CRC."))?;
    let now = state.now();
    let admitted = state
        .db
        .write(move |tx| {
            signed_in_with_discord(tx, &player.ember_id)?;
            archive::admit(
                tx,
                now,
                &file,
                &replay,
                &player.ember_id,
                Attribution::Online(&sides),
            )
        })
        .await?;
    let status = match admitted {
        Admission::New => StatusCode::CREATED,
        Admission::Held => StatusCode::OK,
        Admission::Refused(refused) => return Err(refused.into()),
    };
    Ok(respond(status, &json!({})))
}

/// Refuses a player with no Discord account (`discord_required`).
fn signed_in_with_discord(tx: &Transaction<'_>, player: &EmberId) -> Result<()> {
    let signed_in = tx
        .query_row(
            "SELECT 1 FROM discord_accounts WHERE ember_id = ?1",
            [player.as_str()],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if signed_in {
        return Ok(());
    }
    Err(ApiFailure::new(
        ErrorCode::Forbidden,
        "Sign in with Discord to share replays to the archive.",
    )
    .detail("reason", "discord_required"))
}

/// `GET /v1/archive`'s query: a filter (`archive::listing::Filter`), the
/// order (`sort=class`, else newest first) and where to go on from.
#[derive(Deserialize, Default)]
pub struct Listing {
    #[serde(default)]
    event: Option<String>,
    #[serde(default)]
    fighter: Option<u8>,
    #[serde(default)]
    country: Option<String>,
    #[serde(default)]
    round: Option<String>,
    #[serde(default)]
    q: Option<String>,
    /// A side's exact name, 1 to 32 bytes (`archive::listing::Filter::name`).
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    class: Option<u8>,
    #[serde(default)]
    sort: Option<String>,
    /// A listing's `next`, to go on from where it stopped.
    #[serde(default)]
    cursor: Option<String>,
}

/// `GET /v1/archive`: the stored games the filter keeps, newest first or by
/// class, with no credential, as the archive page shows them: each game's
/// file in the bucket and each side's class, and each tournament set's score
/// and pack once the bucket holds it. `next`, when there is more to look
/// through, is the `cursor` that goes on. Also the events and countries of
/// the archive's games, for the page's filters, and how many games the
/// archive holds (`total`) and the filter keeps (`matching`, null when that
/// would take reading too many games), all of them, not only this page's.
/// Classes are the public rankings board's, as the archive's games carry
/// them (`archive::classes`): none while the bridge hides classes, and the
/// class order is then the newest. `name` keeps only the games a side's exact
/// name is on, which a player's profile lists their sets by; it combines with
/// every other filter.
pub async fn list(State(state): State<AppState>, Query(query): Query<Listing>) -> Result<Response> {
    let Some(archive) = state.config.archive.clone() else {
        return Err(ApiFailure::not_found());
    };
    let shown = rooms::enabled(&state) && classes_shown(&state);
    let order = match query.sort.as_deref() {
        Some("class") if shown => Order::Class,
        _ => Order::Newest,
    };
    let cursor = match query.cursor.as_deref() {
        None => Cursor::first(order),
        Some(text) => Cursor::parse(order, text)
            .ok_or_else(|| ApiFailure::invalid("That is not a cursor of this listing."))?,
    };
    if query
        .name
        .as_deref()
        .is_some_and(|name| name.is_empty() || name.len() > MAX_NAME)
    {
        return Err(ApiFailure::invalid("A name is 1 to 32 bytes."));
    }
    let filter = Filter {
        event: query.event,
        fighter: query.fighter,
        country: query.country,
        round: query.round,
        q: query.q,
        name: query.name,
        class: query.class,
    };
    let base = archive.public_base;
    let listed = state
        .db
        .read(move |tx| {
            let Page { games, next } = listing::page(tx, &filter, shown, order, &cursor, LIMITS)?;
            let (total, matching) = listing::counts(tx, &filter, shown, LIMITS)?;
            let mut sets = serde_json::Map::new();
            for id in games.iter().filter_map(|game| game.set.as_ref()) {
                if !sets.contains_key(id) {
                    sets.insert(id.clone(), set_listed(tx, &base, id)?);
                }
            }
            let games: Vec<Value> = games
                .into_iter()
                .map(|game| {
                    let classes = game.classes.map(|band| band.filter(|_| shown));
                    json!({
                        "digest": game.digest, "crc": game.crc,
                        "url": format!("{base}/games/{}.emberreplay", game.digest),
                        "saved_at": game.saved_at, "size": game.size, "fighters": game.fighters,
                        "names": game.names, "countries": game.countries,
                        "set": game.set, "event": game.event, "round": game.round, "phase": game.phase,
                        "classes": classes,
                    })
                })
                .collect();
            let (events, countries) = listing::choices(tx)?;
            Ok(json!({
                "games": games, "sets": sets, "events": events, "countries": countries,
                "total": total, "matching": matching,
                "sort": if order == Order::Class { "class" } else { "newest" },
                "more": next.is_some(), "next": next.map(|next| next.text(order)),
            }))
        })
        .await?;
    Ok(respond(StatusCode::OK, &listed))
}

/// What the listing says of set `id`: its score, and its pack's address
/// once the bucket holds one.
fn set_listed(tx: &Transaction<'_>, base: &str, id: &str) -> Result<Value> {
    let score = scores(tx, id).map(|(wins, _)| wins).unwrap_or([0, 0]);
    let packed: bool = tx
        .query_row(
            "SELECT uploaded_version > 0 FROM archive_sets WHERE match_id = ?1",
            [id],
            |row| row.get(0),
        )
        .optional()?
        .unwrap_or(false);
    let pack = packed.then(|| format!("{base}/sets/{id}.emberpack"));
    Ok(json!({ "score": score, "pack": pack }))
}
