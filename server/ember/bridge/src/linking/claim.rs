//! Claims: the player's signed request to be linked through a code.
use ember_protocol::{EmberId, api::ErrorCode, event::Kind};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use super::{
    code::{code_hmac, code_rejected, normalize_code},
    expire,
    intent::{load_intent, pending_claim},
};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    util::new_id,
};

const MAX_FAILED_ATTEMPTS: i64 = 5;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClaimCommand {
    pub code: String,
    pub connection_id: String,
    /// The player confirmed the named provider in Ember.
    pub consent: bool,
}

#[derive(Serialize)]
pub struct ClaimView {
    pub claim_id: Option<String>,
    pub intent_id: Option<String>,
    pub state: &'static str,
    pub ember_id: EmberId,
    pub fingerprint: String,
    pub connection_id: String,
    pub provider: String,
    pub expires_at: Option<u64>,
}

/// Records the player's signed claim. Rejections that count against a code
/// are committed even though the request fails, so they return `Ok(Err)`.
pub fn claim(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    ember_id: &EmberId,
    challenge_id: &str,
    command: &ClaimCommand,
) -> Result<Result<ClaimView>> {
    expire(tx, ctx.now)?;
    if !command.consent {
        return Ok(Err(ApiFailure::invalid(
            "Confirm the provider in Ember before linking.",
        )));
    }
    let Some(code) = normalize_code(&command.code) else {
        return Ok(Err(code_rejected()));
    };
    let hmac = code_hmac(&ctx.keys, &code);
    let Some(intent) = load_intent(tx, "code_hmac", &hmac.as_slice())? else {
        return Ok(Err(code_rejected()));
    };
    if intent.connection_id != command.connection_id {
        // A code from another provider or environment (LINK-07). It counts
        // against that code so a leaked one cannot be retried forever.
        let attempts = intent.failed_attempts + 1;
        tx.execute(
            "UPDATE link_intents SET failed_attempts = ?1,
                state = CASE WHEN ?1 >= ?2 AND state IN ('created', 'claim_pending') THEN 'expired' ELSE state END
             WHERE id = ?3",
            params![attempts, MAX_FAILED_ATTEMPTS, intent.id],
        )?;
        // An intent that just ran out of attempts takes its pending claim with it.
        expire(tx, ctx.now)?;
        return Ok(Err(code_rejected()));
    }
    if !matches!(intent.state.as_str(), "created" | "claim_pending") || intent.expires_at <= ctx.now
    {
        return Ok(Err(code_rejected()));
    }
    let view = |claim_id: String, state: &'static str| ClaimView {
        claim_id: Some(claim_id),
        intent_id: Some(intent.id.clone()),
        state,
        ember_id: ember_id.clone(),
        fingerprint: ember_id.fingerprint(),
        connection_id: intent.connection_id.clone(),
        provider: ctx.provider_label(&intent.connection_id),
        expires_at: Some(intent.expires_at),
    };
    if let Some(existing) = pending_claim(tx, &intent.id)?.filter(|claim| claim.state == "pending")
    {
        // A pending claim is never swapped for another key (LINK-05).
        return Ok(if existing.ember_id == *ember_id {
            Ok(view(existing.claim_id, "pending"))
        } else {
            Err(ApiFailure::new(
                ErrorCode::LinkPendingApproval,
                "This code is already waiting for approval of another request.",
            ))
        });
    }
    let linked: Option<String> = tx
        .query_row(
            "SELECT account_id FROM links WHERE ember_id = ?1 AND connection_id = ?2 AND revoked_at IS NULL",
            params![ember_id.as_str(), intent.connection_id],
            |row| row.get(0),
        )
        .optional()?;
    match linked {
        Some(account) if account == intent.account_id => {
            return Ok(Ok(ClaimView {
                claim_id: None,
                state: "linked",
                ..view(String::new(), "linked")
            }));
        }
        Some(_) => {
            return Ok(Err(ApiFailure::new(
                ErrorCode::LinkConflict,
                "This Ember ID is already linked to another account on this provider. Unlink it first.",
            )));
        }
        None => {}
    }
    let claim_id = new_id("lkc");
    tx.execute(
        "INSERT INTO link_claims (id, intent_id, ember_id, challenge_id, state, consented_at)
         VALUES (?1, ?2, ?3, ?4, 'pending', ?5)",
        params![
            claim_id,
            intent.id,
            ember_id.as_str(),
            challenge_id,
            ctx.now
        ],
    )?;
    tx.execute(
        "UPDATE link_intents SET state = 'claim_pending' WHERE id = ?1",
        [&intent.id],
    )?;
    let tenant = ctx.tenant_of(&intent.connection_id)?;
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind: Kind::LinkPending,
            tenant_id: &tenant,
            connection_id: Some(&intent.connection_id),
            subject: format!("link-intents/{}", intent.id),
            match_id: None,
            ember_id: Some(ember_id),
            lobby_id: None,
            tournament_id: None,
            data: json!({
                "intent_id": intent.id,
                "claim_id": claim_id,
                "ember_id": ember_id,
                "fingerprint": ember_id.fingerprint(),
                "connection_id": intent.connection_id,
            }),
        },
    )?;
    Ok(Ok(view(claim_id, "pending")))
}

/// The player withdraws their own claim. A claim the account side already
/// answered stays as it was, and a retried cancellation changes nothing.
pub fn cancel_claim(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    ember_id: &EmberId,
    claim_id: &str,
) -> Result<()> {
    let (intent, claim_state): (String, String) = tx
        .query_row(
            "SELECT intent_id, state FROM link_claims WHERE id = ?1 AND ember_id = ?2",
            params![claim_id, ember_id.as_str()],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?
        .ok_or_else(ApiFailure::not_found)?;
    match claim_state.as_str() {
        "pending" => {
            tx.execute(
                "UPDATE link_claims SET state = 'cancelled', decided_at = ?1 WHERE id = ?2 AND state = 'pending'",
                params![ctx.now, claim_id],
            )?;
            tx.execute("UPDATE link_intents SET state = 'created' WHERE id = ?1 AND state = 'claim_pending'", [&intent])?;
            Ok(())
        }
        // A retried cancellation.
        "cancelled" => Ok(()),
        // The account side answered first: say so instead of claiming
        // a cancellation that did not happen.
        decided => Err(ApiFailure::new(
            ErrorCode::LinkConflict,
            "The request was already answered. Check your links.",
        )
        .detail("state", decided.to_owned())),
    }
}
