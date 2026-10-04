//! Approval: the account holder accepts or rejects the pending claim.
use ember_protocol::{EmberId, api::ErrorCode, event::Kind};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use super::{
    code::code_rejected,
    expire,
    intent::{IntentView, Owner, owned_intent, pending_claim, view_intent},
    revoke::revoke_link,
};
use crate::{
    audit::audit,
    auth::FRESH_AUTH_SECS,
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    util::new_id,
};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ApproveCommand {
    pub claim_id: String,
    pub ember_id: EmberId,
    #[serde(default)]
    pub replace: bool,
    /// Provider-proxy approvals name the verified subject they act for.
    #[serde(default)]
    pub subject: Option<String>,
}

#[derive(Serialize)]
pub struct LinkView {
    pub link_id: String,
    pub ember_id: EmberId,
    pub connection_id: String,
    pub participant_id: String,
    pub approved_via: String,
    pub approved_at: u64,
}

/// Approves the exact pending claim. Intent lock, uniqueness, code
/// consumption, link row and event are one transaction (LINK-04).
pub fn approve(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    owner: &Owner<'_>,
    intent_id: &str,
    command: &ApproveCommand,
) -> Result<LinkView> {
    expire(tx, ctx.now)?;
    let intent = owned_intent(tx, owner, intent_id)?;
    let (subject, participant_id): (String, String) = tx.query_row(
        "SELECT subject, participant_id FROM external_accounts WHERE id = ?1",
        [&intent.account_id],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    if let Owner::Provider { .. } = owner
        && command.subject.as_deref() != Some(subject.as_str())
    {
        return Err(ApiFailure::not_found());
    }
    if intent.state == "approved" {
        // A retried approval of the same claim returns the same link, and
        // only a claim made on this intent.
        return link_for_claim(tx, &intent.id, &command.claim_id)?
            .filter(|link| link.ember_id == command.ember_id)
            .ok_or_else(code_rejected);
    }
    if intent.state != "claim_pending" {
        return Err(code_rejected());
    }
    let claim = pending_claim(tx, &intent.id)?
        .filter(|claim| claim.state == "pending")
        .ok_or_else(code_rejected)?;
    if claim.claim_id != command.claim_id || claim.ember_id != command.ember_id {
        return Err(ApiFailure::new(
            ErrorCode::LinkConflict,
            "The approval does not match the pending request. Reload and check the Ember ID.",
        ));
    }
    let tenant = ctx.tenant_of(&intent.connection_id)?;
    let existing: Option<(String, String)> = tx
        .query_row(
            "SELECT id, ember_id FROM links WHERE account_id = ?1 AND revoked_at IS NULL",
            [&intent.account_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()?;
    if let Some((old_link, old_ember)) = existing {
        if !command.replace {
            return Err(ApiFailure::new(
                ErrorCode::LinkConflict,
                "This account is already linked to another Ember ID. Approve a replacement to change it.",
            ));
        }
        if let Owner::Browser(browser) = owner
            && browser.authenticated_at + FRESH_AUTH_SECS < ctx.now
        {
            return Err(ApiFailure::new(
                ErrorCode::Forbidden,
                "Sign in again to replace a linked Ember ID.",
            ));
        }
        let old_ember = EmberId::parse(&old_ember).map_err(|_| ApiFailure::unavailable())?;
        revoke_link(
            tx,
            ctx,
            &tenant,
            &old_link,
            &old_ember,
            &intent.connection_id,
            &participant_id,
            "replaced",
        )?;
    }
    let link_id = new_id("lnk");
    tx.execute(
        "INSERT INTO links (id, account_id, connection_id, ember_id, approved_via, claim_id, consented_at, approved_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        params![
            link_id,
            intent.account_id,
            intent.connection_id,
            claim.ember_id.as_str(),
            owner.via(),
            claim.claim_id,
            claim.consented_at,
            ctx.now
        ],
    )
    .map_err(|error| match error {
        rusqlite::Error::SqliteFailure(failure, _) if failure.code == rusqlite::ErrorCode::ConstraintViolation => {
            ApiFailure::new(ErrorCode::LinkConflict, "This Ember ID is already linked on this provider.")
        }
        other => other.into(),
    })?;
    tx.execute(
        "UPDATE link_claims SET state = 'approved', decided_at = ?1 WHERE id = ?2",
        params![ctx.now, claim.claim_id],
    )?;
    tx.execute(
        "UPDATE link_intents SET state = 'approved' WHERE id = ?1",
        [&intent.id],
    )?;
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind: Kind::LinkCompleted,
            tenant_id: &tenant,
            connection_id: Some(&intent.connection_id),
            subject: format!("links/{link_id}"),
            match_id: None,
            ember_id: Some(&claim.ember_id),
            lobby_id: None,
            tournament_id: None,
            data: json!({
                "link_id": link_id,
                "ember_id": claim.ember_id,
                "connection_id": intent.connection_id,
                "participant_id": participant_id,
                "approved_via": owner.via(),
            }),
        },
    )?;
    audit(
        tx,
        ctx.now,
        owner.actor(),
        "link.approve",
        &link_id,
        "ok",
        None,
    )?;
    Ok(LinkView {
        link_id,
        ember_id: claim.ember_id,
        connection_id: intent.connection_id,
        participant_id,
        approved_via: owner.via().into(),
        approved_at: ctx.now,
    })
}

fn link_for_claim(
    tx: &Transaction<'_>,
    intent_id: &str,
    claim_id: &str,
) -> Result<Option<LinkView>> {
    Ok(tx
        .query_row(
            "SELECT l.id, l.ember_id, l.connection_id, a.participant_id, l.approved_via, l.approved_at
             FROM links l JOIN external_accounts a ON a.id = l.account_id
             JOIN link_claims c ON c.id = l.claim_id
             WHERE l.claim_id = ?1 AND c.intent_id = ?2 AND l.revoked_at IS NULL",
            [claim_id, intent_id],
            |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get(2)?, row.get(3)?, row.get(4)?, row.get(5)?))
            },
        )
        .optional()?
        .and_then(|(link_id, ember_id, connection_id, participant_id, approved_via, approved_at)| {
            Some(LinkView {
                link_id,
                ember_id: EmberId::parse(&ember_id).ok()?,
                connection_id,
                participant_id,
                approved_via,
                approved_at,
            })
        }))
}

/// The account holder rejects the pending claim. The code stays usable
/// until it expires, so the genuine player can still claim it.
pub fn reject(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    owner: &Owner<'_>,
    intent_id: &str,
    claim_id: &str,
) -> Result<IntentView> {
    expire(tx, ctx.now)?;
    let intent = owned_intent(tx, owner, intent_id)?;
    let changed = tx.execute(
        "UPDATE link_claims SET state = 'denied', decided_at = ?1 WHERE id = ?2 AND intent_id = ?3 AND state = 'pending'",
        params![ctx.now, claim_id, intent.id],
    )?;
    if changed == 1 {
        tx.execute(
            "UPDATE link_intents SET state = 'created' WHERE id = ?1 AND state = 'claim_pending'",
            [&intent.id],
        )?;
        audit(
            tx,
            ctx.now,
            owner.actor(),
            "link.reject",
            claim_id,
            "ok",
            None,
        )?;
    }
    view_intent(tx, ctx, owner, intent_id)
}
