//! `POST /v1/players/lookup`: the players of a connection allowed to find
//! them by Discord account or by their account on a sign-in site
//! (`Connection::finds_by`). Each one found is linked on the connection the
//! way BluMint's lookup links it (`linking::find_and_link`), so the platform
//! can create matches and rooms for them.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::{
    partner::{FoundPlayer, Lookup, LookupAnswer, MAX_LOOKUP},
    sites::{FoundAccount, LookupBy},
};

use crate::{
    AppState, auth,
    config::Connection,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, ok},
    linking::{Ask, Site, discord_asks, find_and_link},
    routes::sign_in::is_user_id,
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
    let sites: Vec<Site> = request
        .accounts
        .iter()
        .map(|account| Site::Configured(account.site.clone()))
        .collect();
    if (!request.discord.is_empty() && !allowed(&Site::Discord)) || !sites.iter().all(allowed) {
        return Err(ApiFailure::forbidden());
    }
    // Each ID is one its site's accounts can have.
    let mut accounts = Vec::new();
    for (account, site) in request.accounts.iter().zip(&sites) {
        let definition = state
            .config
            .sign_in_site(&account.site)
            .ok_or_else(ApiFailure::forbidden)?;
        let key = match definition.lookup_by {
            LookupBy::UserId if is_user_id(&account.id) => account.id.clone(),
            LookupBy::Username if definition.username_rule.allows(&account.id) => {
                definition.username_rule.key(&account.id)
            }
            _ => {
                return Err(ApiFailure::invalid(format!(
                    "{:?} is no account ID or username {} can have.",
                    account.id, definition.display_name
                )));
            }
        };
        accounts.push((site.clone(), definition.lookup_by, account.id.clone(), key));
    }
    let ctx = Ctx::of(&state);
    let answer = state
        .db
        .write(move |tx| {
            let players = find_and_link(tx, &ctx, &connection_id, &discord_asks(&request.discord))?
                .into_iter()
                .map(|found| FoundPlayer {
                    discord_user_id: found.ask.asked.to_owned(),
                    ember_id: found.ember_id,
                    participant_id: found.participant_id,
                })
                .collect();
            let asks: Vec<Ask<'_>> = accounts
                .iter()
                .map(|(site, by, asked, key)| Ask {
                    site,
                    by: *by,
                    asked,
                    key: key.clone(),
                })
                .collect();
            let accounts = find_and_link(tx, &ctx, &connection_id, &asks)?
                .into_iter()
                .map(|found| FoundAccount {
                    site: found.ask.site.id().to_owned(),
                    id: found.ask.asked.to_owned(),
                    user_id: found.user_id,
                    ember_id: found.ember_id,
                    participant_id: found.participant_id,
                    connected_at: found.connected_at,
                })
                .collect();
            Ok(LookupAnswer { players, accounts })
        })
        .await?;
    state.committed();
    Ok(ok(&answer))
}
