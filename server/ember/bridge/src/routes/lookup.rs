//! `POST /v1/players/lookup`: the players of a connection allowed to find
//! them by Discord account or by their account on a sign-in site
//! (`Connection::finds_by`). Each one found is linked on the connection the
//! way BluMint's lookup links it (`linking::find_and_link`), so the platform
//! can create matches and rooms for them.
//!
//! `POST /v1/players/prover`: the player who signed a challenge the bridge's
//! tournament service issued, for that service's connection alone.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::{
    PublicKey,
    challenge::Action,
    partner::{FoundPlayer, Lookup, LookupAnswer, MAX_LOOKUP, Prover, ProverAnswer},
    sign::Domain,
    sites::{self, FoundAccount, LookupBy},
};
use rusqlite::OptionalExtension;

use crate::{
    AppState, auth,
    config::Connection,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, GENERAL_BODY, PROOF_BODY, ok},
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
    Ok(ok(&answer))
}

/// The tournament service proves who is asking with a challenge it issued
/// (audience: its own origin) and the player's signature over it. The bridge
/// checks the signature with the key the player opened their sessions with,
/// and answers their Discord account, linked on the service's connection.
/// The path, command and single use are the service's to check: the bridge
/// keeps no record of challenges it did not issue.
pub async fn prover(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection_id = service.provider_connection()?.to_owned();
    let tournaments = state
        .config
        .tournaments
        .as_ref()
        .filter(|tournaments| tournaments.connection == connection_id)
        .ok_or_else(ApiFailure::forbidden)?;
    let Prover { challenge, proof } = body.parse()?;
    let now = state.now();
    let refused = || ApiFailure::invalid("That proof is not for the tournament service.");
    challenge
        .check_shape(state.config.policy())
        .map_err(|_| refused())?;
    if challenge.action != Action::TournamentAct
        || challenge.audience != tournaments.origin
        || challenge.bridge_id != state.config.bridge_id
        || proof.challenge_id != challenge.challenge_id
        || now < challenge.issued_at
        || now >= challenge.expires_at
    {
        return Err(refused());
    }
    let ember_id = challenge.ember_id.clone();
    let signed = challenge.to_value()?;
    let ctx = Ctx::of(&state);
    let answer = state
        .db
        .write(move |tx| {
            let key: Option<String> = tx
                .query_row(
                    "SELECT public_key FROM identities WHERE ember_id = ?1",
                    [ember_id.as_str()],
                    |row| row.get(0),
                )
                .optional()?;
            let key = key.and_then(|key| PublicKey::from_b64u(&key).ok());
            if !key.is_some_and(|key| {
                key.ember_id() == ember_id
                    && key
                        .verify(Domain::Challenge, &signed, &proof.signature)
                        .is_ok()
            }) {
                return Err(refused());
            }
            let discord: Option<String> = tx
                .query_row(
                    "SELECT user_id FROM discord_accounts WHERE ember_id = ?1",
                    [ember_id.as_str()],
                    |row| row.get(0),
                )
                .optional()?;
            let player = match discord {
                Some(user_id) => {
                    find_and_link(tx, &ctx, &connection_id, vec![Ask::discord(&user_id)])?
                        .into_iter()
                        .next()
                        .map(|found| FoundPlayer {
                            discord_user_id: found.ask.asked,
                            ember_id: found.ember_id,
                            participant_id: found.participant_id,
                        })
                }
                None => None,
            };
            Ok(ProverAnswer { ember_id, player })
        })
        .await?;
    Ok(ok(&answer))
}
