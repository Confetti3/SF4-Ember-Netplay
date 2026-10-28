//! Tournament Mode by invitation (`desk.invite_only`): at first only vetted
//! organizers run events. The operator makes single-use invites
//! (`ember-bridge invite`); an Ember ID that redeems one in Ember becomes an
//! organizer, and only organizers get organizer access on a sign-in site,
//! the web desk and the desk's actions. Without `invite_only` everyone is.
//!
//! An invite is stored only as a keyed hash, like a link code, and a
//! redeemed one names who took it.
use axum::{extract::State, http::HeaderMap, response::Response};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    challenge::{Action, Method},
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Deserialize;
use serde_json::json;

use super::Caller;
use crate::{
    AppState, Keys,
    ctx::Ctx,
    error::{ApiFailure, Result},
    http::{Body, PROOF_BODY, ok},
    linking::code::{display_code, generate_code, normalize_code},
};

pub const ACCESS_PATH: &str = "/v1/organizer/access";
pub const REDEEM_PATH: &str = "/v1/organizer/invites/redeem";

/// Redeems one Ember ID may try in an hour, so codes cannot be guessed.
const REDEEMS_PER_HOUR: usize = 5;

fn hash(keys: &Keys, code: &str) -> [u8; 32] {
    keys.keyed_hash("organizer-invite", code.as_bytes())
}

/// Whether `ember_id` may organize on this bridge.
pub fn allowed(tx: &Transaction<'_>, ctx: &Ctx, ember_id: &str) -> Result<bool> {
    if !invite_only(&ctx.config) {
        return Ok(true);
    }
    Ok(tx
        .query_row(
            "SELECT 1 FROM organizers WHERE ember_id = ?1",
            [ember_id],
            |_| Ok(()),
        )
        .optional()?
        .is_some())
}

fn invite_only(config: &crate::Config) -> bool {
    config.desk.as_ref().is_some_and(|desk| desk.invite_only)
}

/// The refusal for an Ember ID that has not redeemed an invite.
pub fn not_invited() -> ApiFailure {
    ApiFailure::new(
        ErrorCode::Forbidden,
        "Tournament Mode is by invitation for now. Redeem your invite in Ember \
         (Ember ID › Tournament organizer).",
    )
    .detail("reason", "invite_required")
}

/// Refuses `ember_id` unless it may organize.
pub async fn require(state: &AppState, ember_id: &str) -> Result<()> {
    let (ember_id, ctx) = (ember_id.to_owned(), Ctx::of(state));
    let ok = state
        .db
        .read(move |tx| allowed(tx, &ctx, &ember_id))
        .await?;
    if ok { Ok(()) } else { Err(not_invited()) }
}

impl Caller {
    /// `Caller::of` for an organizer's own routes: the command, from an
    /// Ember ID that may organize.
    pub(crate) async fn organizer<C: serde::de::DeserializeOwned>(
        state: &AppState,
        headers: &HeaderMap,
        body: &Body<PROOF_BODY>,
    ) -> Result<(Self, C)> {
        let (caller, command) = Self::of(state, headers, body).await?;
        require(state, caller.ember_id()).await?;
        Ok((caller, command))
    }
}

/// `GET /v1/organizer/access` (player or web desk session): whether the
/// bridge takes organizers by invitation, and whether this Ember ID is one.
pub async fn access(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let ember_id = super::reader(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let only = invite_only(&ctx.config);
    let allowed = state
        .db
        .read(move |tx| allowed(tx, &ctx, &ember_id))
        .await?;
    Ok(ok(&json!({ "invite_only": only, "allowed": allowed })))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Redeem {
    code: String,
}

/// `POST /v1/organizer/invites/redeem` (player session, `site.connect`
/// proof): takes an unused invite for the session's Ember ID. Every wrong,
/// used or unknown code gets the same answer.
pub async fn redeem(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let (caller, command): (_, Redeem) = Caller::of(&state, &headers, &body).await?;
    caller.require_game()?;
    state
        .rate
        .check(
            &format!("organizer-invite:{}", caller.ember_id()),
            REDEEMS_PER_HOUR,
            3600,
            state.now(),
        )
        .map_err(ApiFailure::rate_limited)?;
    let wrong = || {
        ApiFailure::new(ErrorCode::InvalidRequest, "That invite is not valid.")
            .detail("reason", "invite_invalid")
    };
    let code = normalize_code(command.code.trim()).ok_or_else(wrong)?;
    let hashed = hash(&state.keys, &code);
    let ctx = Ctx::of(&state);
    let taken = state
        .db
        .write(move |tx| {
            let ember_id =
                caller.prove(tx, &ctx, Action::SiteConnect, Method::Post, REDEEM_PATH)?;
            if allowed(tx, &ctx, &ember_id)? {
                return Ok(true);
            }
            let used = tx.execute(
                "UPDATE organizer_invites SET redeemed_by = ?1, redeemed_at = ?2
                 WHERE code_hash = ?3 AND redeemed_by IS NULL",
                params![ember_id, ctx.now, hashed.as_slice()],
            )?;
            if used == 1 {
                tx.execute(
                    "INSERT INTO organizers (ember_id, since, code_hash) VALUES (?1, ?2, ?3)",
                    params![ember_id, ctx.now, hashed.as_slice()],
                )?;
            }
            Ok(used == 1)
        })
        .await?;
    if !taken {
        return Err(wrong());
    }
    Ok(ok(&json!({ "allowed": true })))
}

/// Makes `count` single-use invites, `note` saying for whom; the codes are
/// shown once, here, and stored only as hashes.
pub async fn make(state: &AppState, count: usize, note: Option<String>) -> Result<Vec<String>> {
    let codes: Vec<String> = (0..count).map(|_| generate_code()).collect();
    let hashed: Vec<[u8; 32]> = codes.iter().map(|code| hash(&state.keys, code)).collect();
    let now = state.now();
    state
        .db
        .write(move |tx| {
            for code_hash in &hashed {
                tx.execute(
                    "INSERT INTO organizer_invites (code_hash, created_at, note) VALUES (?1, ?2, ?3)",
                    params![code_hash.as_slice(), now, note],
                )?;
            }
            Ok(())
        })
        .await?;
    Ok(codes.iter().map(|code| display_code(code)).collect())
}

/// Every organizer, with when they joined and the note of their invite.
pub async fn list(state: &AppState) -> Result<Vec<(String, u64, Option<String>)>> {
    state
        .db
        .read(|tx| {
            Ok(tx
                .prepare(
                    "SELECT o.ember_id, o.since, i.note FROM organizers o
                     LEFT JOIN organizer_invites i ON i.code_hash = o.code_hash ORDER BY o.since",
                )?
                .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))?
                .collect::<rusqlite::Result<_>>()?)
        })
        .await
}

/// Makes `ember_id` an organizer without an invite, as the operator vets
/// one directly (`note` says who); false when it already is one.
pub async fn add(state: &AppState, ember_id: &str, note: Option<String>) -> Result<bool> {
    EmberId::parse(ember_id).map_err(|_| ApiFailure::invalid("That is not an Ember ID."))?;
    let (ember_id, now) = (ember_id.to_owned(), state.now());
    let added = state
        .db
        .write(move |tx| {
            // The note rides on an invite already marked as redeemed.
            let code_hash = crate::util::random::<32>();
            tx.execute(
                "INSERT INTO organizer_invites (code_hash, created_at, note, redeemed_by, redeemed_at)
                 VALUES (?1, ?2, ?3, ?4, ?2)",
                params![code_hash.as_slice(), now, note, ember_id],
            )?;
            Ok(tx.execute(
                "INSERT OR IGNORE INTO organizers (ember_id, since, code_hash) VALUES (?1, ?2, ?3)",
                params![ember_id, now, code_hash.as_slice()],
            )?)
        })
        .await?;
    Ok(added == 1)
}

/// Takes `ember_id`'s organizer standing away; their events keep running
/// until stopped, but they make no new ones.
pub async fn remove(state: &AppState, ember_id: &str) -> Result<bool> {
    let ember_id = ember_id.to_owned();
    let removed = state
        .db
        .write(move |tx| Ok(tx.execute("DELETE FROM organizers WHERE ember_id = ?1", [&ember_id])?))
        .await?;
    Ok(removed == 1)
}
