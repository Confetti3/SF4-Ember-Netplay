//! `POST /v1/players/lookup`: the players of a connection allowed to find
//! them by Discord account (`Connection::discord_lookup`) or by Challonge
//! username (`Connection::challonge_lookup`). Each one found is linked on the
//! connection the way BluMint's lookup links it (`discord::find_and_link`,
//! `challonge::find_and_link`), so the platform can create matches and rooms
//! for them.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::{
    challonge::is_username,
    partner::{Lookup, LookupAnswer, MAX_LOOKUP},
};

use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::{challonge, discord},
};

pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let (discord_allowed, challonge_allowed) = state
        .config
        .connection(&connection_id)
        .map_or((false, false), |(_, connection)| {
            (connection.discord_lookup(), connection.challonge_lookup())
        });
    if !discord_allowed && !challonge_allowed {
        return Err(ApiFailure::forbidden());
    }
    let request: Lookup = body.parse()?;
    // Each list needs its own setting.
    if (!request.discord.is_empty() && !discord_allowed)
        || (!request.challonge.is_empty() && !challonge_allowed)
    {
        return Err(ApiFailure::forbidden());
    }
    if request.discord.len() + request.challonge.len() > MAX_LOOKUP {
        return Err(ApiFailure::invalid(
            "A lookup names at most 32 accounts, Discord and Challonge together.",
        ));
    }
    if !request
        .challonge
        .iter()
        .all(|username| is_username(username))
    {
        return Err(ApiFailure::invalid(
            "Challonge usernames are 1 to 64 letters, digits, '_', '-' or '.'.",
        ));
    }
    let ctx = Ctx::of(&state);
    let answer = state
        .db
        .write(move |tx| {
            Ok(LookupAnswer {
                players: discord::find_and_link(tx, &ctx, &connection_id, &request.discord)?,
                challonge: challonge::find_and_link(tx, &ctx, &connection_id, &request.challonge)?,
            })
        })
        .await?;
    state.committed();
    Ok(ok(&answer))
}
