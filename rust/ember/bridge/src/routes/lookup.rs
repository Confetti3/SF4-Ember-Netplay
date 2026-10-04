//! `POST /v1/players/lookup`: the players of a connection allowed to find
//! them by Discord account (`Connection::discord_lookup`). Each one found is
//! linked on the connection the way BluMint's lookup links it
//! (`discord::find_and_link`), so the platform can create matches and rooms
//! for them.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::partner::{Lookup, LookupAnswer, MAX_LOOKUP};

use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    routes::discord,
};

pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let allowed = state
        .config
        .connection(&connection_id)
        .is_some_and(|(_, connection)| connection.discord_lookup());
    if !allowed {
        return Err(ApiFailure::forbidden());
    }
    let request: Lookup = body.parse()?;
    if request.discord.len() > MAX_LOOKUP {
        return Err(ApiFailure::invalid(
            "A lookup names at most 32 Discord user IDs.",
        ));
    }
    let ctx = Ctx::of(&state);
    let players = state
        .db
        .write(move |tx| discord::find_and_link(tx, &ctx, &connection_id, &request.discord))
        .await?;
    state.committed();
    Ok(ok(&LookupAnswer { players }))
}
