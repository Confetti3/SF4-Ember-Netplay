//! `POST /v1/players/lookup`: the players of a connection allowed to find
//! them by Discord account or by their account on a sign-in site
//! (`Connection::finds_by`). Each one found is linked on the connection the
//! way BluMint's lookup links it (`linking::find_and_link`), so the platform
//! can create matches and rooms for them.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::{
    partner::{FoundPlayer, Lookup, LookupAnswer, MAX_LOOKUP},
    sites::{self, FoundAccount, LookupBy},
};

use crate::{
    AppState, auth,
    config::Connection,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    linking::{Ask, Site, find_and_link},
};

pub async fn lookup(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<GENERAL_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let connection: Option<&Connection> = state
        .config
        .connection(&connection_id)
        .map(|(_, connection)| connection);
    let allowed = |site: &Site| connection.is_some_and(|connection| connection.finds_by(site));
    if !connection.is_some_and(|connection| {
        connection.discord_lookup() || !connection.lookup_sites.is_empty()
    }) {
        return Err(ApiFailure::forbidden());
    }
    let request: Lookup = body.parse()?;
    if request.discord.len() + request.accounts.len() > MAX_LOOKUP {
        return Err(ApiFailure::invalid(
            "A lookup names at most 32 accounts, Discord's and sites' together.",
        ));
    }
    // Each list needs its own setting: Discord's, and each site's.
    if !request.discord.is_empty() && !allowed(&Site::Discord) {
        return Err(ApiFailure::forbidden());
    }
    let mut asks: Vec<Ask> = request.discord.iter().map(|id| Ask::discord(id)).collect();
    for account in request.accounts {
        let site = Site::Configured(account.site);
        let definition = match &site {
            Site::Configured(id) if allowed(&site) => state.config.sign_in_site(id),
            _ => None,
        }
        .ok_or_else(ApiFailure::forbidden)?;
        // An ID the site's accounts cannot have finds nothing, as a Discord
        // ID that is no snowflake finds nothing.
        let key = match definition.lookup_by {
            LookupBy::UserId => sites::is_user_id(&account.id).then(|| account.id.clone()),
            LookupBy::Username => definition
                .username_rule
                .allows(&account.id)
                .then(|| definition.username_rule.key(&account.id)),
        };
        if let Some(key) = key {
            asks.push(Ask {
                site,
                by: definition.lookup_by,
                asked: account.id,
                key,
            });
        }
    }
    let ctx = Ctx::of(&state);
    let answer = state
        .db
        .write(move |tx| {
            let mut answer = LookupAnswer {
                players: Vec::new(),
                accounts: Vec::new(),
            };
            for found in find_and_link(tx, &ctx, &connection_id, asks)? {
                match &found.ask.site {
                    Site::Discord => answer.players.push(FoundPlayer {
                        discord_user_id: found.ask.asked,
                        ember_id: found.ember_id,
                        participant_id: found.participant_id,
                    }),
                    Site::Configured(site) => answer.accounts.push(FoundAccount {
                        site: site.clone(),
                        id: found.ask.asked,
                        user_id: found.user_id,
                        ember_id: found.ember_id,
                        participant_id: found.participant_id,
                        connected_at: found.connected_at,
                    }),
                }
            }
            Ok(answer)
        })
        .await?;
    state.committed();
    Ok(ok(&answer))
}
