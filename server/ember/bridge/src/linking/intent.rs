//! Intents: the short-lived code an account holder creates, who may act on
//! it, and how it reads back.
use ember_protocol::EmberId;
use rusqlite::{OptionalExtension, Transaction, params};
use serde::Serialize;

use super::{
    code::{CODE_LIFETIME_SECS, code_hmac, display_code, generate_code},
    expire,
};
use crate::{
    auth::Browser,
    ctx::Ctx,
    error::{ApiFailure, Result},
    util::new_id,
};

const MAX_LIVE_INTENTS: i64 = 3;

#[derive(Serialize)]
pub struct IntentCreated {
    pub intent_id: String,
    /// Shown once. Only its HMAC is stored.
    pub code: String,
    pub expires_at: u64,
    pub connection_id: String,
    pub provider: String,
}

pub fn create_intent(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    account_id: &str,
    browser_session: Option<&[u8; 32]>,
) -> Result<IntentCreated> {
    expire(tx, ctx.now)?;
    let live: i64 = tx.query_row(
        "SELECT COUNT(*) FROM link_intents WHERE account_id = ?1 AND state IN ('created', 'claim_pending')",
        [account_id],
        |row| row.get(0),
    )?;
    if live >= MAX_LIVE_INTENTS {
        return Err(ApiFailure::rate_limited(CODE_LIFETIME_SECS));
    }
    let intent_id = new_id("lki");
    let expires_at = ctx.now + CODE_LIFETIME_SECS;
    for _ in 0..4 {
        let code = generate_code();
        let inserted = tx.execute(
            "INSERT OR IGNORE INTO link_intents
                (id, connection_id, account_id, code_hmac, created_via, browser_session, state, created_at, expires_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'created', ?7, ?8)",
            params![
                intent_id,
                connection_id,
                account_id,
                code_hmac(&ctx.keys, &code).as_slice(),
                if browser_session.is_some() { "browser" } else { "provider" },
                browser_session.map(|hash| hash.to_vec()),
                ctx.now,
                expires_at
            ],
        )?;
        if inserted == 1 {
            return Ok(IntentCreated {
                intent_id,
                code: display_code(&code),
                expires_at,
                connection_id: connection_id.to_owned(),
                provider: ctx.provider_label(connection_id),
            });
        }
    }
    Err(ApiFailure::unavailable())
}

#[derive(Serialize)]
pub struct ClaimSummary {
    pub claim_id: String,
    pub ember_id: EmberId,
    pub fingerprint: String,
    pub state: String,
    pub consented_at: u64,
}

#[derive(Serialize)]
pub struct IntentView {
    pub intent_id: String,
    pub state: String,
    pub expires_at: u64,
    pub connection_id: String,
    pub provider: String,
    pub claim: Option<ClaimSummary>,
}

pub(super) struct Intent {
    pub(super) id: String,
    pub(super) connection_id: String,
    pub(super) account_id: String,
    pub(super) browser_session: Option<Vec<u8>>,
    pub(super) state: String,
    pub(super) failed_attempts: i64,
    pub(super) expires_at: u64,
}

pub(super) fn load_intent(
    tx: &Transaction<'_>,
    by: &str,
    value: &dyn rusqlite::ToSql,
) -> Result<Option<Intent>> {
    Ok(tx
        .query_row(
            &format!(
                "SELECT id, connection_id, account_id, browser_session, state, failed_attempts, expires_at
                 FROM link_intents WHERE {by} = ?1"
            ),
            [value],
            |row| {
                Ok(Intent {
                    id: row.get(0)?,
                    connection_id: row.get(1)?,
                    account_id: row.get(2)?,
                    browser_session: row.get(3)?,
                    state: row.get(4)?,
                    failed_attempts: row.get(5)?,
                    expires_at: row.get(6)?,
                })
            },
        )
        .optional()?)
}

pub(super) fn pending_claim(tx: &Transaction<'_>, intent_id: &str) -> Result<Option<ClaimSummary>> {
    Ok(tx
        .query_row(
            "SELECT id, ember_id, state, consented_at FROM link_claims
             WHERE intent_id = ?1 ORDER BY consented_at DESC, rowid DESC LIMIT 1",
            [intent_id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get(3)?,
                ))
            },
        )
        .optional()?
        .and_then(|(claim_id, ember_id, state, consented_at)| {
            let ember_id = EmberId::parse(&ember_id).ok()?;
            Some(ClaimSummary {
                claim_id,
                fingerprint: ember_id.fingerprint(),
                ember_id,
                state,
                consented_at,
            })
        }))
}

/// Who is acting on an intent on the account side.
pub enum Owner<'a> {
    Browser(&'a Browser),
    Provider { connection_id: &'a str },
}

impl Owner<'_> {
    fn owns(&self, tx: &Transaction<'_>, intent: &Intent) -> Result<bool> {
        Ok(match self {
            // A browser-created intent belongs to the session that made it.
            Self::Browser(browser) => {
                browser.account_id == intent.account_id
                    && intent
                        .browser_session
                        .as_deref()
                        .is_none_or(|session| session == browser.token_hash.as_slice())
            }
            Self::Provider { connection_id } => {
                *connection_id == intent.connection_id
                    && tx.query_row(
                        "SELECT EXISTS (SELECT 1 FROM external_accounts WHERE id = ?1 AND connection_id = ?2)",
                        params![intent.account_id, connection_id],
                        |row| row.get::<_, bool>(0),
                    )?
            }
        })
    }

    pub(super) fn via(&self) -> &'static str {
        match self {
            Self::Browser(_) => "browser",
            Self::Provider { .. } => "provider_proxy",
        }
    }

    pub(super) fn actor(&self) -> (&'static str, String) {
        match self {
            Self::Browser(browser) => ("browser", browser.account_id.clone()),
            Self::Provider { connection_id } => ("provider", (*connection_id).to_owned()),
        }
    }
}

pub(super) fn owned_intent(
    tx: &Transaction<'_>,
    owner: &Owner<'_>,
    intent_id: &str,
) -> Result<Intent> {
    let intent = load_intent(tx, "id", &intent_id)?.ok_or_else(ApiFailure::not_found)?;
    if !owner.owns(tx, &intent)? {
        return Err(ApiFailure::not_found());
    }
    Ok(intent)
}

pub fn view_intent(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    owner: &Owner<'_>,
    intent_id: &str,
) -> Result<IntentView> {
    let intent = owned_intent(tx, owner, intent_id)?;
    let claim = pending_claim(tx, &intent.id)?
        .filter(|claim| claim.state == "pending" || intent.state == "approved");
    let state = if intent.expires_at <= ctx.now
        && matches!(intent.state.as_str(), "created" | "claim_pending")
    {
        "expired".to_owned()
    } else {
        intent.state.clone()
    };
    Ok(IntentView {
        intent_id: intent.id,
        state,
        expires_at: intent.expires_at,
        provider: ctx.provider_label(&intent.connection_id),
        connection_id: intent.connection_id,
        claim,
    })
}

/// The live intent most recently created by this browser session, if any.
pub fn browser_intent(tx: &Transaction<'_>, browser: &Browser, now: u64) -> Result<Option<String>> {
    Ok(tx
        .query_row(
            "SELECT id FROM link_intents WHERE account_id = ?1 AND state IN ('created', 'claim_pending', 'approved')
               AND expires_at > ?2 AND (browser_session IS NULL OR browser_session = ?3)
             ORDER BY created_at DESC, rowid DESC LIMIT 1",
            params![browser.account_id, now, browser.token_hash.as_slice()],
            |row| row.get(0),
        )
        .optional()?)
}

pub fn cancel_intent(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    owner: &Owner<'_>,
    intent_id: &str,
) -> Result<IntentView> {
    expire(tx, ctx.now)?;
    let intent = owned_intent(tx, owner, intent_id)?;
    tx.execute(
        "UPDATE link_intents SET state = 'cancelled' WHERE id = ?1 AND state IN ('created', 'claim_pending')",
        [&intent.id],
    )?;
    tx.execute(
        "UPDATE link_claims SET state = 'cancelled', decided_at = ?1 WHERE intent_id = ?2 AND state = 'pending'",
        params![ctx.now, intent.id],
    )?;
    view_intent(tx, ctx, owner, intent_id)
}
