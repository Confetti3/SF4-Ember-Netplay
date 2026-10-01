//! The mock provider's browser pages: a stand-in for a real platform's
//! verified login, so the full browser approval path (spec 11.3) can be run
//! and tested without any provider. Served only when `mock_browser` is set,
//! and only for connections of kind `mock`.
use axum::{
    Form,
    extract::{Path, State},
    http::{HeaderMap, HeaderValue, StatusCode, header},
    response::{IntoResponse, Redirect, Response},
};
use ember_protocol::EmberId;
use serde::Deserialize;

use crate::{
    AppState,
    auth::{self, BROWSER_COOKIE, BROWSER_SECS, Browser},
    error::{ApiFailure, Result},
    routes::links::{self, ApproveCommand, Ctx, IntentCreated, Owner},
    util::html,
};

const STYLE: &str = "body{font:16px/1.5 system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;color:#1d1d1f;background:#fff}\
h1{font-size:1.4rem}code,.code{font:600 2rem ui-monospace,monospace;letter-spacing:.1em}\
.box{border:1px solid #c7c7cc;border-radius:8px;padding:1rem;margin:1rem 0}.warn{background:#fff4e5;border-color:#f5a623}\
button{font:inherit;padding:.4rem 1rem;margin-right:.5rem}input{font:inherit;padding:.3rem;width:100%;box-sizing:border-box}\
@media (prefers-color-scheme:dark){body{color:#f5f5f7;background:#1c1c1e}.box{border-color:#48484a}.warn{background:#3a2a10}}";

fn page(title: &str, body: &str, refresh: bool) -> Response {
    let refresh = if refresh {
        r#"<meta http-equiv="refresh" content="3">"#
    } else {
        ""
    };
    let document = format!(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">{refresh}<title>{}</title><style>{STYLE}</style></head><body>{body}</body></html>",
        html(title)
    );
    let mut response = (
        StatusCode::OK,
        [(header::CONTENT_TYPE, "text/html; charset=utf-8")],
        document,
    )
        .into_response();
    let headers = response.headers_mut();
    headers.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static("default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'"),
    );
    headers.insert(header::X_FRAME_OPTIONS, HeaderValue::from_static("DENY"));
    headers.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    headers.insert(
        header::REFERRER_POLICY,
        HeaderValue::from_static("no-referrer"),
    );
    response
}

fn enabled(state: &AppState, connection: &str) -> Result<()> {
    let mock = state
        .config
        .connection(connection)
        .is_some_and(|(_, connection)| connection.kind == "mock" && connection.enabled);
    if state.config.mock_browser && mock {
        Ok(())
    } else {
        Err(ApiFailure::not_found())
    }
}

async fn session(
    state: &AppState,
    headers: &HeaderMap,
    connection: &str,
) -> Result<Option<Browser>> {
    Ok(auth::browser(state, headers)
        .await?
        .filter(|browser| browser.connection_id == connection))
}

fn provider_name(state: &AppState, connection: &str) -> String {
    state
        .config
        .connection(connection)
        .map_or_else(String::new, |(_, c)| c.display_name.clone())
}

/// `GET /mock/{connection}/login`.
pub async fn login_form(
    State(state): State<AppState>,
    Path(connection): Path<String>,
) -> Result<Response> {
    enabled(&state, &connection)?;
    let provider = html(&provider_name(&state, &connection));
    Ok(page(
        "Mock provider sign-in",
        &format!(
            "<h1>{provider}: sign in</h1>\
             <p class=\"box warn\">This is a mock provider for testing Ember linking. Any name signs in.</p>\
             <form method=\"post\"><label>Account ID<input name=\"subject\" required maxlength=\"256\" autocomplete=\"username\"></label>\
             <label>Display name<input name=\"display_label\" maxlength=\"64\"></label><p><button>Sign in</button></p></form>"
        ),
        false,
    ))
}

#[derive(Deserialize)]
pub struct LoginForm {
    subject: String,
    #[serde(default)]
    display_label: String,
}

/// `POST /mock/{connection}/login`.
pub async fn login(
    State(state): State<AppState>,
    Path(connection): Path<String>,
    Form(form): Form<LoginForm>,
) -> Result<Response> {
    enabled(&state, &connection)?;
    let keys = state.keys.clone();
    let now = state.now();
    let target = connection.clone();
    let cookie = state
        .db
        .write(move |tx| {
            let (account_id, _) =
                links::account(tx, &target, &form.subject, &form.display_label, now)?;
            auth::start_browser_session(tx, &keys, &target, &account_id, now)
        })
        .await?;
    let secure = if state.config.origin.starts_with("https://") {
        "; Secure"
    } else {
        ""
    };
    let mut response = Redirect::to(&format!("/mock/{connection}/link")).into_response();
    response.headers_mut().insert(
        header::SET_COOKIE,
        HeaderValue::from_str(&format!(
            "{BROWSER_COOKIE}={cookie}; Path=/; HttpOnly; SameSite=Strict; Max-Age={BROWSER_SECS}{secure}"
        ))
        .map_err(|_| ApiFailure::unavailable())?,
    );
    Ok(response)
}

/// `GET /mock/{connection}/link`.
pub async fn link_page(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(connection): Path<String>,
) -> Result<Response> {
    enabled(&state, &connection)?;
    let Some(browser) = session(&state, &headers, &connection).await? else {
        return Ok(Redirect::to(&format!("/mock/{connection}/login")).into_response());
    };
    render(&state, browser, None).await
}

async fn render(
    state: &AppState,
    browser: Browser,
    created: Option<IntentCreated>,
) -> Result<Response> {
    let ctx = Ctx::of(state);
    let connection = browser.connection_id.clone();
    let provider = html(&provider_name(state, &connection));
    let csrf = html(browser.csrf());
    let lookup = browser.clone();
    let (intent, linked) = state
        .db
        .read(move |tx| {
            let intent = match links::browser_intent(tx, &lookup, ctx.now)? {
                Some(id) => Some(links::view_intent(tx, &ctx, &Owner::Browser(&lookup), &id)?),
                None => None,
            };
            let linked: Option<(String, String)> =
                rusqlite::OptionalExtension::optional(tx.query_row(
                    "SELECT id, ember_id FROM links WHERE account_id = ?1 AND revoked_at IS NULL",
                    [&lookup.account_id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                ))?;
            Ok((intent, linked))
        })
        .await?;
    let mut body = format!(
        "<h1>{provider}: link an Ember installation</h1><p>Signed in as <strong>{}</strong>.</p>",
        html(&browser.subject)
    );
    if let Some((link_id, ember)) = &linked {
        let fingerprint = EmberId::parse(ember)
            .map(|id| id.fingerprint())
            .unwrap_or_default();
        body += &format!(
            "<div class=\"box\"><p>Linked Ember ID <code style=\"font-size:1rem\">{}</code></p>\
             <form method=\"post\" action=\"/mock/{connection}/links/{}/remove\"><input type=\"hidden\" name=\"csrf\" value=\"{csrf}\">\
             <button>Unlink</button></form></div>",
            html(&fingerprint),
            html(link_id)
        );
    }
    let mut refresh = false;
    match (&created, &intent) {
        (Some(created), _) => {
            refresh = false;
            body += &format!(
                "<div class=\"box\"><p>Enter this code in Ember under Profile, Linked accounts. It works once and expires in five minutes.</p>\
                 <p class=\"code\">{}</p><p><a href=\"/mock/{connection}/link\">Continue</a> after entering it.</p></div>",
                html(&created.code)
            );
        }
        (None, Some(view)) if view.state == "claim_pending" => {
            let claim = view.claim.as_ref().ok_or_else(ApiFailure::unavailable)?;
            body += &format!(
                "<div class=\"box warn\"><p>An Ember installation asked to link to this account.</p>\
                 <p>Approve only if Ember shows this same fingerprint:</p><p class=\"code\">{}</p>\
                 <p>If you did not just enter a code in Ember, reject this request.</p>\
                 <form method=\"post\" action=\"/mock/{connection}/link/{intent}/approve\" style=\"display:inline\">\
                 <input type=\"hidden\" name=\"csrf\" value=\"{csrf}\"><input type=\"hidden\" name=\"claim_id\" value=\"{claim_id}\">\
                 <input type=\"hidden\" name=\"ember_id\" value=\"{ember_id}\">{replace}<button>Approve</button></form>\
                 <form method=\"post\" action=\"/mock/{connection}/link/{intent}/reject\" style=\"display:inline\">\
                 <input type=\"hidden\" name=\"csrf\" value=\"{csrf}\"><input type=\"hidden\" name=\"claim_id\" value=\"{claim_id}\">\
                 <button>Reject</button></form></div>",
                html(&claim.fingerprint),
                intent = html(&view.intent_id),
                claim_id = html(&claim.claim_id),
                ember_id = html(claim.ember_id.as_str()),
                replace = if linked.is_some() {
                    "<input type=\"hidden\" name=\"replace\" value=\"true\"><p>This replaces the Ember ID linked now.</p>"
                } else {
                    ""
                },
            );
        }
        (None, Some(view)) if view.state == "created" => {
            refresh = true;
            body += "<div class=\"box\"><p>Waiting for Ember to send the code. This page refreshes on its own.</p></div>";
        }
        _ => {}
    }
    if created.is_none()
        && !matches!(
            intent.as_ref().map(|view| view.state.as_str()),
            Some("created" | "claim_pending")
        )
    {
        body += &format!(
            "<form method=\"post\" action=\"/mock/{connection}/link/create\"><input type=\"hidden\" name=\"csrf\" value=\"{csrf}\">\
             <button>Create a link code</button></form>"
        );
    }
    body += &format!(
        "<form method=\"post\" action=\"/mock/{connection}/logout\"><input type=\"hidden\" name=\"csrf\" value=\"{csrf}\">\
         <p><button>Sign out</button></p></form>"
    );
    Ok(page("Link Ember", &body, refresh))
}

#[derive(Deserialize)]
pub struct CsrfForm {
    csrf: String,
}

async fn checked(
    state: &AppState,
    headers: &HeaderMap,
    connection: &str,
    csrf: &str,
) -> Result<Browser> {
    enabled(state, connection)?;
    let browser = session(state, headers, connection)
        .await?
        .ok_or_else(ApiFailure::unauthenticated)?;
    if !browser.csrf_ok(Some(csrf)) {
        return Err(ApiFailure::forbidden());
    }
    Ok(browser)
}

/// `POST /mock/{connection}/link/create`.
pub async fn create(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(connection): Path<String>,
    Form(form): Form<CsrfForm>,
) -> Result<Response> {
    let browser = checked(&state, &headers, &connection, &form.csrf).await?;
    let ctx = Ctx::of(&state);
    let owner = browser.clone();
    let created = state
        .db
        .write(move |tx| {
            links::create_intent(
                tx,
                &ctx,
                &owner.connection_id,
                &owner.account_id,
                Some(&owner.token_hash),
            )
        })
        .await?;
    render(&state, browser, Some(created)).await
}

#[derive(Deserialize)]
pub struct DecisionForm {
    csrf: String,
    claim_id: String,
    #[serde(default)]
    ember_id: Option<String>,
    #[serde(default)]
    replace: Option<String>,
}

/// `POST /mock/{connection}/link/{intent}/approve`.
pub async fn approve(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((connection, intent)): Path<(String, String)>,
    Form(form): Form<DecisionForm>,
) -> Result<Response> {
    let browser = checked(&state, &headers, &connection, &form.csrf).await?;
    let command = ApproveCommand {
        claim_id: form.claim_id,
        ember_id: EmberId::parse(form.ember_id.as_deref().unwrap_or(""))?,
        replace: form.replace.as_deref() == Some("true"),
        subject: None,
    };
    let ctx = Ctx::of(&state);
    state
        .db
        .write(move |tx| links::approve(tx, &ctx, &Owner::Browser(&browser), &intent, &command))
        .await?;
    state.committed();
    Ok(Redirect::to(&format!("/mock/{connection}/link")).into_response())
}

/// `POST /mock/{connection}/link/{intent}/reject`.
pub async fn reject(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((connection, intent)): Path<(String, String)>,
    Form(form): Form<DecisionForm>,
) -> Result<Response> {
    let browser = checked(&state, &headers, &connection, &form.csrf).await?;
    let ctx = Ctx::of(&state);
    state
        .db
        .write(move |tx| {
            links::reject(tx, &ctx, &Owner::Browser(&browser), &intent, &form.claim_id)
        })
        .await?;
    Ok(Redirect::to(&format!("/mock/{connection}/link")).into_response())
}

/// `POST /mock/{connection}/links/{link}/remove`.
pub async fn remove(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path((connection, link)): Path<(String, String)>,
    Form(form): Form<CsrfForm>,
) -> Result<Response> {
    let browser = checked(&state, &headers, &connection, &form.csrf).await?;
    let ctx = Ctx::of(&state);
    state
        .db
        .write(move |tx| {
            links::unlink(
                tx,
                &ctx,
                &links::Unlinker::Account(Owner::Browser(&browser)),
                &link,
            )
        })
        .await?;
    state.committed();
    Ok(Redirect::to(&format!("/mock/{connection}/link")).into_response())
}

/// `POST /mock/{connection}/logout`.
pub async fn logout(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(connection): Path<String>,
    Form(form): Form<CsrfForm>,
) -> Result<Response> {
    let browser = checked(&state, &headers, &connection, &form.csrf).await?;
    state
        .db
        .write(move |tx| {
            tx.execute(
                "DELETE FROM browser_sessions WHERE token_hash = ?1",
                [browser.token_hash.as_slice()],
            )?;
            Ok(())
        })
        .await?;
    let mut response = Redirect::to(&format!("/mock/{connection}/login")).into_response();
    response.headers_mut().insert(
        header::SET_COOKIE,
        HeaderValue::from_str(&format!(
            "{BROWSER_COOKIE}=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"
        ))
        .map_err(|_| ApiFailure::unavailable())?,
    );
    Ok(response)
}
