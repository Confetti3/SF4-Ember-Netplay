//! `POST /v1/players/lookup`: the players of a connection allowed to find
//! them by Discord account or by Challonge username
//! (`Connection::finds_by`). Each one found is linked on the connection the
//! way BluMint's lookup links it (`Site::find_and_link`), so the platform can
//! create matches and rooms for them.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::{
    challonge::is_username,
    partner::{FoundChallongePlayer, FoundPlayer, Lookup, LookupAnswer, MAX_LOOKUP},
};

use crate::{
    AppState, auth,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    linking::Site,
};

pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let allowed = |site| {
        state
            .config
            .connection(&connection_id)
            .is_some_and(|(_, connection)| connection.finds_by(site))
    };
    if !allowed(Site::Discord) && !allowed(Site::Challonge) {
        return Err(ApiFailure::forbidden());
    }
    let request: Lookup = body.parse()?;
    // Each list needs its own setting.
    if (!request.discord.is_empty() && !allowed(Site::Discord))
        || (!request.challonge.is_empty() && !allowed(Site::Challonge))
    {
        return Err(ApiFailure::forbidden());
    }
    if request.discord.len() + request.challonge.len() > MAX_LOOKUP {
        return Err(ApiFailure::invalid(
            "A lookup names at most 32 accounts, Discord and Challonge together.",
        ));
    }
    if !request.challonge.iter().all(|name| is_username(name)) {
        return Err(ApiFailure::invalid(
            "Challonge usernames are 1 to 64 letters, digits, '_', '-' or '.'.",
        ));
    }
    let ctx = Ctx::of(&state);
    let answer = state
        .db
        .write(move |tx| {
            let players = Site::Discord
                .find_and_link(tx, &ctx, &connection_id, &request.discord)?
                .into_iter()
                .map(|found| FoundPlayer {
                    discord_user_id: found.user_id,
                    ember_id: found.ember_id,
                    participant_id: found.participant_id,
                })
                .collect();
            let challonge = Site::Challonge
                .find_and_link(tx, &ctx, &connection_id, &request.challonge)?
                .into_iter()
                .map(|found| FoundChallongePlayer {
                    username: found.asked.to_owned(),
                    challonge_user_id: found.user_id,
                    ember_id: found.ember_id,
                    participant_id: found.participant_id,
                })
                .collect();
            Ok(LookupAnswer { players, challonge })
        })
        .await?;
    state.committed();
    Ok(ok(&answer))
}
