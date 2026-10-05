//! Ending a link: revocation, and who may ask for it.
use ember_protocol::{EmberId, event::Kind};
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::json;

use super::intent::Owner;
use crate::{
    audit::audit,
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    routes::{discord, matches},
};

#[allow(clippy::too_many_arguments)]
pub fn revoke_link(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    tenant: &str,
    link_id: &str,
    ember_id: &EmberId,
    connection_id: &str,
    participant_id: &str,
    reason: &str,
) -> Result<()> {
    tx.execute(
        "UPDATE links SET revoked_at = ?1, revoked_by = ?2 WHERE id = ?3 AND revoked_at IS NULL",
        params![ctx.now, reason, link_id],
    )?;
    emit(
        tx,
        &ctx.config,
        ctx.now,
        NewEvent {
            kind: Kind::LinkRemoved,
            tenant_id: tenant,
            connection_id: Some(connection_id),
            subject: format!("links/{link_id}"),
            match_id: None,
            ember_id: Some(ember_id),
            lobby_id: None,
            tournament_id: None,
            data: json!({
                "link_id": link_id,
                "ember_id": ember_id,
                "connection_id": connection_id,
                "participant_id": participant_id,
                "reason": reason,
            }),
        },
    )?;
    // Future permissions derived from the link end with it (spec 11.6).
    matches::on_unlink(tx, ctx, tenant, connection_id, ember_id)
}

pub enum Unlinker<'a> {
    Player(&'a EmberId),
    Account(Owner<'a>),
}

pub fn unlink(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    by: &Unlinker<'_>,
    link_id: &str,
) -> Result<serde_json::Value> {
    let row: Option<(String, String, String, String)> = tx
        .query_row(
            "SELECT l.ember_id, l.connection_id, l.account_id, a.participant_id
             FROM links l JOIN external_accounts a ON a.id = l.account_id
             WHERE l.id = ?1 AND l.revoked_at IS NULL",
            [link_id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )
        .optional()?;
    let (ember_id, connection_id, account_id, participant_id) =
        row.ok_or_else(ApiFailure::not_found)?;
    let allowed = match by {
        Unlinker::Player(player) => player.as_str() == ember_id,
        Unlinker::Account(Owner::Browser(browser)) => browser.account_id == account_id,
        Unlinker::Account(Owner::Provider { connection_id: own }) => *own == connection_id,
    };
    if !allowed {
        return Err(ApiFailure::not_found());
    }
    let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
    let tenant = ctx.tenant_of(&connection_id)?;
    let actor = match by {
        Unlinker::Player(player) => ("player", player.to_string()),
        Unlinker::Account(owner) => owner.actor(),
    };
    revoke_link(
        tx,
        ctx,
        &tenant,
        link_id,
        &ember_id,
        &connection_id,
        &participant_id,
        &format!("unlinked_by_{}", actor.0),
    )?;
    if !matches!(by, Unlinker::Account(Owner::Provider { .. })) {
        discord::withdraw(tx, link_id)?;
    }
    audit(tx, ctx.now, actor, "link.remove", link_id, "ok", None)?;
    Ok(json!({ "link_id": link_id, "state": "removed" }))
}
