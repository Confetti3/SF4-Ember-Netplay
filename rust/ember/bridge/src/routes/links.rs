//! Code-based account linking (spec 11).
//!
//! A verified external account (a provider backend, or the mock provider's
//! browser login) creates an intent and sees a short code once. The player
//! enters it in Ember, whose helper submits a signed claim. The verified
//! account then approves that exact claim and Ember ID. A code alone never
//! finishes a link.
use std::sync::Arc;

use axum::{
    extract::{Path, State},
    http::{HeaderMap, StatusCode},
    response::Response,
};
use ember_protocol::{
    EmberId,
    api::ErrorCode,
    challenge::{Action, Method},
    encoding::is_prefixed_id,
    event::Kind,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{
    AppState, Keys,
    auth::{self, Actor, Browser, FRESH_AUTH_SECS, Role},
    config::Config,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    http::{Body, PROOF_BODY, json, ok},
    routes::{discord, matches, sessions},
    util::{new_id, random},
};

pub const CODE_LIFETIME_SECS: u64 = 5 * 60;
const CODE_ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const CODE_LENGTH: usize = 10;
const MAX_LIVE_INTENTS: i64 = 3;
const MAX_FAILED_ATTEMPTS: i64 = 5;
const CLAIMS_PER_WINDOW: usize = 10;
const CLAIM_WINDOW_SECS: u64 = 10 * 60;
pub const MAX_SUBJECT: usize = 256;
const MAX_RESOLVE: usize = 100;

/// What every link operation needs, cloned into the blocking transaction.
#[derive(Clone)]
pub struct Ctx {
    pub config: Arc<Config>,
    pub keys: Arc<Keys>,
    pub now: u64,
}

impl Ctx {
    pub fn of(state: &AppState) -> Self {
        Self {
            config: state.config.clone(),
            keys: state.keys.clone(),
            now: state.now(),
        }
    }

    pub fn tenant_of(&self, connection_id: &str) -> Result<String> {
        self.config
            .connection(connection_id)
            .map(|(tenant, _)| tenant.id.clone())
            .ok_or_else(ApiFailure::not_found)
    }

    fn provider_label(&self, connection_id: &str) -> String {
        self.config
            .connection(connection_id)
            .map_or_else(String::new, |(_, connection)| {
                connection.display_name.clone()
            })
    }
}

/// The same answer for unknown, expired, used and foreign codes, so codes
/// cannot be probed through error text (spec 25.2).
fn code_rejected() -> ApiFailure {
    ApiFailure::new(
        ErrorCode::LinkExpired,
        "That code is not valid. Ask for a new code and try again.",
    )
}

fn generate_code() -> String {
    let bytes: [u8; 8] = random();
    let bits = u64::from_be_bytes(bytes);
    (0..CODE_LENGTH)
        .map(|index| CODE_ALPHABET[((bits >> (index * 5)) & 31) as usize] as char)
        .collect()
}

/// Accepts the ten symbols, in either case, with at most the single display
/// separator after the fifth. Nothing else is normalized.
pub fn normalize_code(input: &str) -> Option<String> {
    let compact = match input.len() {
        CODE_LENGTH => input.to_owned(),
        11 if input.as_bytes()[5] == b'-' => format!("{}{}", &input[..5], &input[6..]),
        _ => return None,
    };
    let upper = compact.to_ascii_uppercase();
    upper
        .bytes()
        .all(|byte| CODE_ALPHABET.contains(&byte))
        .then_some(upper)
}

fn display_code(code: &str) -> String {
    format!("{}-{}", &code[..5], &code[5..])
}

fn code_hmac(keys: &Keys, normalized: &str) -> [u8; 32] {
    keys.keyed_hash("link-code", normalized.as_bytes())
}

pub fn check_subject(subject: &str) -> Result<()> {
    if subject.is_empty() || subject.len() > MAX_SUBJECT || subject.chars().any(char::is_control) {
        return Err(ApiFailure::invalid("The external subject is invalid."));
    }
    Ok(())
}

/// The bridge record for an external subject, created on first use.
/// Subjects are opaque strings compared byte for byte (LINK-11).
pub fn account(
    tx: &Transaction<'_>,
    connection_id: &str,
    subject: &str,
    label: &str,
    now: u64,
) -> Result<(String, String)> {
    check_subject(subject)?;
    let label: String = label.chars().filter(|c| !c.is_control()).take(64).collect();
    tx.execute(
        "INSERT OR IGNORE INTO external_accounts (id, connection_id, subject, participant_id, display_label, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![new_id("ext"), connection_id, subject, new_id("epl"), label, now],
    )?;
    Ok(tx.query_row(
        "SELECT id, participant_id FROM external_accounts WHERE connection_id = ?1 AND subject = ?2",
        params![connection_id, subject],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?)
}

/// Expires intents past their time and every pending claim on an expired
/// intent. The one place an intent and its claim end together.
pub fn expire(tx: &Transaction<'_>, now: u64) -> Result<()> {
    tx.execute(
        "UPDATE link_intents SET state = 'expired' WHERE state IN ('created', 'claim_pending') AND expires_at <= ?1",
        [now],
    )?;
    tx.execute(
        "UPDATE link_claims SET state = 'expired', decided_at = ?1
         WHERE state = 'pending' AND intent_id IN (SELECT id FROM link_intents WHERE state = 'expired')",
        [now],
    )?;
    Ok(())
}

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

struct Intent {
    id: String,
    connection_id: String,
    account_id: String,
    browser_session: Option<Vec<u8>>,
    state: String,
    failed_attempts: i64,
    expires_at: u64,
}

fn load_intent(
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

fn pending_claim(tx: &Transaction<'_>, intent_id: &str) -> Result<Option<ClaimSummary>> {
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

    fn via(&self) -> &'static str {
        match self {
            Self::Browser(_) => "browser",
            Self::Provider { .. } => "provider_proxy",
        }
    }

    fn actor(&self) -> (&'static str, String) {
        match self {
            Self::Browser(browser) => ("browser", browser.account_id.clone()),
            Self::Provider { connection_id } => ("provider", (*connection_id).to_owned()),
        }
    }
}

fn owned_intent(tx: &Transaction<'_>, owner: &Owner<'_>, intent_id: &str) -> Result<Intent> {
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

pub fn audit(
    tx: &Transaction<'_>,
    now: u64,
    (class, id): (&str, String),
    action: &str,
    target: &str,
    result: &str,
    reason: Option<&str>,
) -> Result<()> {
    tx.execute(
        "INSERT INTO audit_entries (at, actor_class, actor_id, action, target, result, reason)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![now, class, id, action, target, result, reason],
    )?;
    Ok(())
}

fn csrf_header(headers: &HeaderMap) -> Option<&str> {
    headers
        .get("x-csrf-token")
        .and_then(|value| value.to_str().ok())
}

/// The account-side actor for a JSON request: a provider credential or a
/// mock browser session with its CSRF token.
async fn account_owner(state: &AppState, headers: &HeaderMap) -> Result<OwnerActor> {
    match auth::any(state, headers).await? {
        Actor::Service(service) => {
            service.require(Role::Provider)?;
            Ok(OwnerActor::Provider(
                service.provider_connection()?.to_owned(),
            ))
        }
        Actor::Browser(browser) => {
            if !browser.csrf_ok(csrf_header(headers)) {
                return Err(ApiFailure::forbidden());
            }
            Ok(OwnerActor::Browser(browser))
        }
        Actor::Player(_) => Err(ApiFailure::forbidden()),
    }
}

pub enum OwnerActor {
    Browser(Browser),
    Provider(String),
}

impl OwnerActor {
    pub fn owner(&self) -> Owner<'_> {
        match self {
            Self::Browser(browser) => Owner::Browser(browser),
            Self::Provider(connection_id) => Owner::Provider { connection_id },
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct IntentRequest {
    #[serde(default)]
    subject: Option<String>,
    #[serde(default)]
    display_label: Option<String>,
}

/// `POST /v1/link-intents`.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let request: IntentRequest = if body.0.is_empty() {
        IntentRequest {
            subject: None,
            display_label: None,
        }
    } else {
        body.parse()?
    };
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let created = state
        .db
        .write(move |tx| match &actor {
            OwnerActor::Provider(connection_id) => {
                let subject = request.subject.as_deref().ok_or_else(|| {
                    ApiFailure::invalid("A provider must name the verified subject.")
                })?;
                let (account_id, _) = account(
                    tx,
                    connection_id,
                    subject,
                    request.display_label.as_deref().unwrap_or(""),
                    ctx.now,
                )?;
                create_intent(tx, &ctx, connection_id, &account_id, None)
            }
            OwnerActor::Browser(browser) => {
                if request.subject.is_some() {
                    return Err(ApiFailure::invalid(
                        "A browser session links its own signed-in account.",
                    ));
                }
                create_intent(
                    tx,
                    &ctx,
                    &browser.connection_id,
                    &browser.account_id,
                    Some(&browser.token_hash),
                )
            }
        })
        .await?;
    state.committed();
    Ok(json(StatusCode::CREATED, &created))
}

/// `GET /v1/link-intents/{id}`.
pub async fn get(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let actor = match auth::any(&state, &headers).await? {
        Actor::Service(service) => OwnerActor::Provider(service.provider_connection()?.to_owned()),
        Actor::Browser(browser) => OwnerActor::Browser(browser),
        Actor::Player(_) => return Err(ApiFailure::forbidden()),
    };
    let ctx = Ctx::of(&state);
    let view = state
        .db
        .read(move |tx| view_intent(tx, &ctx, &actor.owner(), &id))
        .await?;
    Ok(ok(&view))
}

/// `POST /v1/link-intents/{id}/approve`.
pub async fn approve_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let command: ApproveCommand = body.parse()?;
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let link = state
        .db
        .write(move |tx| approve(tx, &ctx, &actor.owner(), &id, &command))
        .await?;
    state.committed();
    Ok(ok(&link))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RejectCommand {
    claim_id: String,
}

/// `POST /v1/link-intents/{id}/reject`.
pub async fn reject_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let command: RejectCommand = body.parse()?;
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let view = state
        .db
        .write(move |tx| reject(tx, &ctx, &actor.owner(), &id, &command.claim_id))
        .await?;
    Ok(ok(&view))
}

/// `POST /v1/link-intents/{id}/cancel`.
pub async fn cancel_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Response> {
    let actor = account_owner(&state, &headers).await?;
    let ctx = Ctx::of(&state);
    let view = state
        .db
        .write(move |tx| cancel_intent(tx, &ctx, &actor.owner(), &id))
        .await?;
    Ok(ok(&view))
}

/// `POST /v1/link-claims`: player session plus a `link.claim` proof.
pub async fn claim_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let now = state.now();
    state
        .rate
        .check(
            &format!("link-claim:{}", player.ember_id),
            CLAIMS_PER_WINDOW,
            CLAIM_WINDOW_SECS,
            now,
        )
        .map_err(ApiFailure::rate_limited)?;
    let (request, command): (_, ClaimCommand) = sessions::proven(&body.0)?;
    let ctx = Ctx::of(&state);
    let outcome = state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                sessions::Target {
                    action: Action::LinkClaim,
                    method: Method::Post,
                    path: "/v1/link-claims",
                },
                Some(&player.ember_id),
            )?;
            claim(tx, &ctx, &ember_id, &request.proof.challenge_id, &command)
        })
        .await?;
    state.committed();
    Ok(json(StatusCode::CREATED, &outcome?))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CancelClaimCommand {
    claim_id: String,
}

/// `POST /v1/link-claims/{id}/cancel`: player session plus a `link.cancel` proof.
pub async fn cancel_claim_route(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let (request, command): (_, CancelClaimCommand) = sessions::proven(&body.0)?;
    if command.claim_id != id || !is_prefixed_id(&id, "lkc") {
        return Err(ApiFailure::invalid("The command does not match the path."));
    }
    let ctx = Ctx::of(&state);
    let path = format!("/v1/link-claims/{id}/cancel");
    state
        .db
        .write(move |tx| {
            let (ember_id, _) = sessions::consume_proof(
                tx,
                &ctx.config,
                ctx.now,
                &request,
                sessions::Target { action: Action::LinkCancel, method: Method::Post, path: &path },
                Some(&player.ember_id),
            )?;
            let (intent, claim_state): (String, String) = tx
                .query_row(
                    "SELECT intent_id, state FROM link_claims WHERE id = ?1 AND ember_id = ?2",
                    params![id, ember_id.as_str()],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()?
                .ok_or_else(ApiFailure::not_found)?;
            match claim_state.as_str() {
                "pending" => {
                    tx.execute(
                        "UPDATE link_claims SET state = 'cancelled', decided_at = ?1 WHERE id = ?2 AND state = 'pending'",
                        params![ctx.now, id],
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
        })
        .await?;
    Ok(ok(
        &json!({ "claim_id": command.claim_id, "state": "cancelled" }),
    ))
}

/// `GET /v1/links`: the caller's own links and pending claims.
pub async fn list(State(state): State<AppState>, headers: HeaderMap) -> Result<Response> {
    let player = auth::player(&state, &headers).await?;
    let config = state.config.clone();
    let now = state.now();
    let body = state
        .db
        .read(move |tx| {
            let links = tx
                .prepare(
                    "SELECT l.id, l.connection_id, a.display_label, l.approved_at FROM links l
                     JOIN external_accounts a ON a.id = l.account_id
                     WHERE l.ember_id = ?1 AND l.revoked_at IS NULL ORDER BY l.approved_at",
                )?
                .query_map([player.ember_id.as_str()], |row| {
                    let connection: String = row.get(1)?;
                    let label: String = row.get(2)?;
                    Ok(json!({
                        "link_id": row.get::<_, String>(0)?,
                        "connection_id": connection,
                        "provider": config.connection(&connection).map(|(_, c)| c.display_name.clone()),
                        "account_label": mask(&label),
                        "state": "linked",
                        "approved_at": row.get::<_, u64>(3)?,
                    }))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            let pending = tx
                .prepare(
                    "SELECT c.id, i.connection_id, i.expires_at FROM link_claims c JOIN link_intents i ON i.id = c.intent_id
                     WHERE c.ember_id = ?1 AND c.state = 'pending' AND i.expires_at > ?2
                       AND i.state IN ('created', 'claim_pending')",
                )?
                .query_map(params![player.ember_id.as_str(), now], |row| {
                    let connection: String = row.get(1)?;
                    Ok(json!({
                        "claim_id": row.get::<_, String>(0)?,
                        "connection_id": connection,
                        "provider": config.connection(&connection).map(|(_, c)| c.display_name.clone()),
                        "state": "waiting_for_browser_approval",
                        "expires_at": row.get::<_, u64>(2)?,
                    }))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(json!({ "links": links, "pending": pending }))
        })
        .await?;
    Ok(ok(&body))
}

/// Shows the first two characters of an account label and masks the rest.
fn mask(label: &str) -> String {
    let visible: String = label.chars().take(2).collect();
    if label.chars().count() <= 2 {
        visible
    } else {
        format!("{visible}***")
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RemoveCommand {
    link_id: String,
}

/// `DELETE /v1/links/{id}`: the linked player with a `link.remove` proof, the
/// account's browser session, or its provider.
pub async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
    body: Body<PROOF_BODY>,
) -> Result<Response> {
    let ctx = Ctx::of(&state);
    let result = match auth::any(&state, &headers).await? {
        Actor::Player(player) => {
            let (request, command): (_, RemoveCommand) = sessions::proven(&body.0)?;
            if command.link_id != id || !is_prefixed_id(&id, "lnk") {
                return Err(ApiFailure::invalid("The command does not match the path."));
            }
            let path = format!("/v1/links/{id}");
            state
                .db
                .write(move |tx| {
                    let (ember_id, _) = sessions::consume_proof(
                        tx,
                        &ctx.config,
                        ctx.now,
                        &request,
                        sessions::Target {
                            action: Action::LinkRemove,
                            method: Method::Delete,
                            path: &path,
                        },
                        Some(&player.ember_id),
                    )?;
                    unlink(tx, &ctx, &Unlinker::Player(&ember_id), &id)
                })
                .await?
        }
        _ => {
            let actor = account_owner(&state, &headers).await?;
            state
                .db
                .write(move |tx| unlink(tx, &ctx, &Unlinker::Account(actor.owner()), &id))
                .await?
        }
    };
    state.committed();
    Ok(ok(&result))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ResolveRequest {
    subjects: Vec<String>,
}

/// `POST /v1/players/resolve`: this provider connection's own subjects only.
/// Unknown subjects are reported unlinked and nothing is created (LINK-12).
pub async fn resolve(
    State(state): State<AppState>,
    headers: HeaderMap,
    body: Body<{ crate::http::GENERAL_BODY }>,
) -> Result<Response> {
    let service = auth::service(&state, &headers).await?;
    let connection = service.provider_connection()?.to_owned();
    let request: ResolveRequest = body.parse()?;
    if request.subjects.is_empty() || request.subjects.len() > MAX_RESOLVE {
        return Err(ApiFailure::invalid("Resolve between 1 and 100 subjects."));
    }
    for subject in &request.subjects {
        check_subject(subject)?;
    }
    let players = state
        .db
        .read(move |tx| {
            let mut statement = tx.prepare(
                "SELECT a.participant_id, l.ember_id FROM external_accounts a
                 LEFT JOIN links l ON l.account_id = a.id AND l.revoked_at IS NULL
                 WHERE a.connection_id = ?1 AND a.subject = ?2",
            )?;
            request
                .subjects
                .iter()
                .map(|subject| {
                    let row: Option<(String, Option<String>)> = statement
                        .query_row(params![connection, subject], |row| Ok((row.get(0)?, row.get(1)?)))
                        .optional()?;
                    Ok(match row {
                        Some((participant, Some(ember))) => {
                            json!({ "subject": subject, "linked": true, "participant_id": participant, "ember_id": ember })
                        }
                        _ => json!({ "subject": subject, "linked": false }),
                    })
                })
                .collect::<Result<Vec<_>>>()
        })
        .await?;
    Ok(ok(&json!({ "players": players })))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_use_crockford_symbols() {
        for _ in 0..200 {
            let code = generate_code();
            assert_eq!(code.len(), 10);
            assert_eq!(normalize_code(&code).as_deref(), Some(code.as_str()));
            let shown = display_code(&code);
            assert_eq!(normalize_code(&shown).as_deref(), Some(code.as_str()));
            assert_eq!(
                normalize_code(&shown.to_lowercase()).as_deref(),
                Some(code.as_str())
            );
        }
        for bad in [
            "ABCDE FGHJK",
            "ABCDEFGHJ",
            "ABCDE--FGHJK",
            "ABCDEFGHJI",
            "ABCDO-FGHJK",
            "ABC-DEFGHJK",
            " ABCDEFGHJK",
        ] {
            assert_eq!(normalize_code(bad), None, "{bad}");
        }
    }

    #[test]
    fn labels_are_masked() {
        assert_eq!(mask("Kate"), "Ka***");
        assert_eq!(mask("K"), "K");
        assert_eq!(mask("日本語"), "日本***");
    }
}
