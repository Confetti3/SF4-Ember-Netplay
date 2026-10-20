//! `POST /v1/blumint/matches`: BluMint asks for a set between two teams of
//! one player, played in Ember.
use std::collections::BTreeMap;

use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId, json as wire,
    matches::{MAX_GAMES_TO_WIN, Participant, Rules},
    play::{PROFILE, play_url},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use super::{
    BluMintBody, BluMintRefusal, CREATE_BODY, MATCHES_PATH, connection, keys, outcome, parse,
};
use crate::{
    AppState,
    ctx::Ctx,
    error::ApiFailure,
    http::{Body, idempotency_key},
    routes::matches::{NewMatch, busy, idempotent, insert_match, into_response},
};

#[derive(Deserialize)]
struct CreateRequest {
    teams: Vec<Team>,
    /// Present on every request for a whole set. BluMint's `matchSettings`,
    /// `spectators` and team names are its own, and not read.
    series: Option<Series>,
}

#[derive(Deserialize)]
struct Series {
    #[serde(rename = "gamesToWin")]
    games_to_win: Value,
}

#[derive(Deserialize)]
struct Team {
    players: Vec<TeamPlayer>,
    /// Games the team has already won: present on every team when BluMint
    /// asks for a set again after its match ended without a result, and 0
    /// on every team after its host reset the set.
    #[serde(rename = "startingWins")]
    starting_wins: Option<Value>,
}

/// What makes two requests the same match. Team names and BluMint's own
/// settings may change under one key without changing it.
#[derive(Serialize)]
struct Set {
    players: [Seat; 2],
    #[serde(rename = "gamesToWin")]
    games_to_win: u8,
    #[serde(rename = "startingWins")]
    starting_wins: [u8; 2],
}

/// Every account BluMint knows the player by. For Ember that is the one
/// Ember ID its lookup answered; other entries are not Ember's to read.
#[derive(Deserialize)]
struct TeamPlayer {
    identities: Vec<Identity>,
}

#[derive(Deserialize)]
struct Identity {
    #[serde(rename = "inGameId")]
    in_game_id: String,
}

/// Who a seat is, from the identities BluMint names its player by. An Ember
/// seat is its bare ID in the digest, as every seat was before an unmatched one
/// could be asked for, so the digests stored with live matches still match.
#[derive(Serialize)]
#[serde(untagged)]
enum Seat {
    /// The one Ember ID among the identities.
    Ember(EmberId),
    /// None of the identities is an Ember ID, so the bridge cannot match the
    /// player: BluMint asks again with the ids as it sent them.
    Unmatched(Vec<String>),
}

/// The seat a player's identities name. A player with no identity, or with
/// two Ember IDs, is not what BluMint's guide describes, and is refused.
fn seat(player: &TeamPlayer) -> Result<Seat, BluMintRefusal> {
    if player.identities.is_empty() {
        return Err(BluMintRefusal::InvalidRequest);
    }
    let mut found = player
        .identities
        .iter()
        .filter_map(|identity| EmberId::parse(&identity.in_game_id).ok());
    match (found.next(), found.next()) {
        (Some(id), None) => Ok(Seat::Ember(id)),
        (None, _) => Ok(Seat::Unmatched(
            player
                .identities
                .iter()
                .map(|identity| identity.in_game_id.clone())
                .collect(),
        )),
        _ => Err(BluMintRefusal::InvalidRequest),
    }
}

/// The set's length: a whole number of games from 1 up. Above the longest
/// set Ember plays, BluMint hands the match to its host instead.
fn games_to_win(series: Option<&Series>) -> Result<u8, BluMintRefusal> {
    let games = series
        .and_then(|series| series.games_to_win.as_u64())
        .filter(|&games| games >= 1)
        .ok_or(BluMintRefusal::InvalidRequest)?;
    u8::try_from(games)
        .ok()
        .filter(|&games| games <= MAX_GAMES_TO_WIN)
        .ok_or(BluMintRefusal::MatchUnsupported)
}

/// Where the set starts: no team's `startingWins` for a new set, or every
/// team's, each a whole number below the set's length.
fn starting_wins(teams: [&Team; 2], games_to_win: u8) -> Result<[u8; 2], BluMintRefusal> {
    let won = |team: &Team| {
        team.starting_wins
            .as_ref()
            .map(|wins| {
                wins.as_u64()
                    .filter(|&wins| wins < u64::from(games_to_win))
                    .map(|wins| wins as u8)
                    .ok_or(BluMintRefusal::InvalidRequest)
            })
            .transpose()
    };
    match (won(teams[0])?, won(teams[1])?) {
        (None, None) => Ok([0, 0]),
        (Some(first), Some(second)) => Ok([first, second]),
        _ => Err(BluMintRefusal::InvalidRequest),
    }
}

/// `POST /v1/blumint/matches`. `series.gamesToWin` (1 to 10, best of 1 to 19)
/// sets the set's length, and `startingWins` the score it starts from, which
/// every report counts. BluMint sends an `Idempotency-Key` with every
/// creation and asks again under it until it has an answer, so every request
/// under one key answers the match it made, even once that match has ended;
/// BluMint sends a new key when it wants a new match. A key replays its match
/// for a day (`routes::maintenance_once`), past the three hours BluMint
/// needs, and refuses any other creation for as long as the match exists.
/// The key is answered before the players: a stored answer, a key a close
/// refused or one that made another match is settled before any player is
/// looked at. Refusals answer in BluMint's format (`BluMintRefusal`), and are
/// never stored against the key.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: BluMintBody<CREATE_BODY>,
) -> std::result::Result<Response, BluMintRefusal> {
    let connection_id = connection(&state, &headers).await?;
    let key = idempotency_key(&headers)?;
    let asked = body
        .map_err(BluMintRefusal::from)
        .and_then(|body| requested(&body));
    let ctx = Ctx::of(&state);
    let answer = state
        .db
        .write(move |tx| {
            outcome(|| {
                let (set, digest) = asked?;
                make(tx, &ctx, &connection_id, &key, &set, &digest)
            })
        })
        .await??;
    state.committed();
    Ok(into_response(answer))
}

/// The set BluMint asks for, and the digest a key's requests are compared by.
fn requested(body: &Body<CREATE_BODY>) -> std::result::Result<(Set, String), BluMintRefusal> {
    let request: CreateRequest = parse(body)?;
    let [first, second] = request.teams.as_slice() else {
        return Err(BluMintRefusal::MatchUnsupported);
    };
    let ([a], [b]) = (first.players.as_slice(), second.players.as_slice()) else {
        return Err(BluMintRefusal::MatchUnsupported);
    };
    let seats = [seat(a)?, seat(b)?];
    if matches!(&seats, [Seat::Ember(a), Seat::Ember(b)] if a == b) {
        return Err(BluMintRefusal::InvalidRequest);
    }
    let games_to_win = games_to_win(request.series.as_ref())?;
    let starting_wins = starting_wins([first, second], games_to_win)?;
    let set = Set {
        players: seats,
        games_to_win,
        starting_wins,
    };
    let digest = wire::digest(&set).map_err(ApiFailure::from)?;
    Ok((set, digest))
}

/// The answer under `key`: the match it made, or a new one. A key whose
/// answer is forgotten while its match still exists makes no other.
fn make(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    key: &str,
    set: &Set,
    digest: &str,
) -> std::result::Result<(StatusCode, Value), BluMintRefusal> {
    let actor = keys::owner(connection_id);
    idempotent(tx, &actor, MATCHES_PATH, key, digest, ctx.now, |tx| {
        let tenant_id = ctx.tenant_of(connection_id)?;
        if keys::made_under(tx, &tenant_id, connection_id, key)?.is_some() {
            return Err(BluMintRefusal::KeyReused);
        }
        let id = insert(tx, ctx, &tenant_id, connection_id, key, set, digest)?;
        Ok((
            StatusCode::OK,
            json!({ "matchId": id, "matchUrl": play_url(&ctx.config.bridge_id, &id) }),
        ))
    })
}

/// Seats the two players and records the match `key` made. Returns its ID.
fn insert(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tenant_id: &str,
    connection_id: &str,
    key: &str,
    set: &Set,
    digest: &str,
) -> std::result::Result<String, BluMintRefusal> {
    let roster = seats(tx, connection_id, &set.players)?;
    Ok(insert_match(
        tx,
        ctx,
        &NewMatch {
            tenant_id,
            connection_id,
            external_match_id: &keys::external_id(key),
            digest,
            rules: &Rules::standard(set.games_to_win, PROFILE),
            required_build_id: "",
            metadata: &BTreeMap::new(),
            participants: [&roster[0], &roster[1]],
            starting_wins: set.starting_wins,
            lobby_id: None,
            tournament_id: None,
        },
    )?)
}

/// The two players as the participants they are linked as on the connection,
/// once neither is in another match there. Every id the bridge cannot match is
/// named, as BluMint sent it: the ids of a seat with no Ember ID among them and
/// any Ember ID with no current link, so BluMint can tell each player and ask
/// again under the key; then every player still playing, so BluMint holds the
/// match until both are free.
fn seats(
    tx: &Transaction<'_>,
    connection_id: &str,
    players: &[Seat; 2],
) -> std::result::Result<Vec<Participant>, BluMintRefusal> {
    let mut roster = Vec::new();
    let mut unmatched: Vec<String> = Vec::new();
    for (slot, seat) in players.iter().enumerate() {
        let ids = match seat {
            Seat::Ember(ember_id) => match participant(tx, connection_id, ember_id)? {
                Some(participant_id) => {
                    roster.push(Participant {
                        participant_id,
                        ember_id: ember_id.clone(),
                        slot: slot as u8,
                    });
                    continue;
                }
                None => vec![ember_id.as_str().to_owned()],
            },
            Seat::Unmatched(ids) => ids.clone(),
        };
        for id in ids {
            if !unmatched.contains(&id) {
                unmatched.push(id);
            }
        }
    }
    if !unmatched.is_empty() {
        return Err(BluMintRefusal::PlayerUnlinked(unmatched));
    }
    let mut playing = Vec::new();
    for seated in &roster {
        if busy(tx, connection_id, &seated.ember_id, "")? {
            playing.push(seated.ember_id.clone());
        }
    }
    if !playing.is_empty() {
        return Err(BluMintRefusal::PlayerBusy(playing));
    }
    Ok(roster)
}

/// The participant an Ember ID is linked as on the connection, if it is.
fn participant(
    tx: &Transaction<'_>,
    connection_id: &str,
    ember_id: &EmberId,
) -> rusqlite::Result<Option<String>> {
    tx.query_row(
        "SELECT a.participant_id FROM links l JOIN external_accounts a ON a.id = l.account_id
         WHERE l.ember_id = ?1 AND l.connection_id = ?2 AND l.revoked_at IS NULL",
        params![ember_id.as_str(), connection_id],
        |row| row.get(0),
    )
    .optional()
}
