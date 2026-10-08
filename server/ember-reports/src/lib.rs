pub mod bugsink;
pub mod config;
pub mod event;
pub mod intake;
pub mod limits;
pub mod store;
pub mod symbolicate;

use crate::{
    bugsink::Bugsink,
    config::Config,
    limits::{Gate, RateLimiter, Ticket},
    store::Store,
};
use axum::{
    Json, Router,
    body::Body,
    extract::{ConnectInfo, DefaultBodyLimit, FromRequest, Multipart, Request, State},
    http::{StatusCode, header},
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::post,
};
use futures_util::StreamExt;
use std::{
    net::SocketAddr,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};
use tokio::sync::Notify;
use tower_http::limit::RequestBodyLimitLayer;

pub struct App {
    pub config: Config,
    pub store: Arc<Store>,
    pub bugsink: Arc<Bugsink>,
    pub rate: RateLimiter,
    pub gate: Gate,
    pub wake: Arc<Notify>,
}
#[derive(Clone)]
struct ReportId(String);
impl App {
    pub async fn new(config: Config, key: &str) -> Result<Arc<Self>, &'static str> {
        config.validate()?;
        let store = Arc::new(
            Store::new(config.state_dir.clone(), config.limits.clone())
                .await
                .map_err(|_| "cannot initialise state directory")?,
        );
        let bugsink = Arc::new(Bugsink::new(&config, key)?);
        Ok(Arc::new(Self {
            gate: Gate::new(config.limits.workers, config.limits.queue),
            rate: RateLimiter::new(config.limits.clone()),
            config,
            store,
            bugsink,
            wake: Arc::new(Notify::new()),
        }))
    }
    pub async fn background(self: Arc<Self>) {
        let mut interval = tokio::time::interval(Duration::from_secs(60));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            tokio::select! { _ = interval.tick() => {}, _ = self.wake.notified() => {} }
            let _ = self.store.prune_dumps(std::time::SystemTime::now()).await;
            self.bugsink.replay(&self.store).await;
        }
    }
}
fn refusal(status: StatusCode, reason: &'static str) -> Response {
    (status, Json(serde_json::json!({ "reason": reason }))).into_response()
}
pub fn router(app: Arc<App>) -> Router {
    Router::new()
        .route("/report/v1", post(receive))
        .layer(DefaultBodyLimit::max(intake::BODY_BYTES))
        // DefaultBodyLimit constrains Multipart; this also constrains the HTTP
        // body stream, including chunked requests without Content-Length.
        .layer(RequestBodyLimitLayer::new(intake::BODY_BYTES))
        .layer(middleware::from_fn_with_state(app.clone(), admission))
        .with_state(app)
}
async fn admission(State(app): State<Arc<App>>, mut request: Request, next: Next) -> Response {
    let started = Instant::now();
    let method = request.method().clone();
    let count = Arc::new(AtomicUsize::new(0));
    let observed = count.clone();
    let body = std::mem::replace(request.body_mut(), Body::empty());
    *request.body_mut() = Body::from_stream(body.into_data_stream().map(move |chunk| {
        if let Ok(bytes) = &chunk {
            observed.fetch_add(bytes.len(), Ordering::Relaxed);
        }
        chunk
    }));
    let result = async {
        if method != axum::http::Method::POST {
            return next.run(request).await;
        }
        let Some(ConnectInfo(peer)) = request
            .extensions()
            .get::<ConnectInfo<SocketAddr>>()
            .copied()
        else {
            return refusal(StatusCode::INTERNAL_SERVER_ERROR, "missing_peer");
        };
        let address = limits::client_address(peer.ip(), request.headers());
        if let Err(retry) = app.rate.admit(address, Instant::now()) {
            let mut response = refusal(StatusCode::TOO_MANY_REQUESTS, "rate_limit");
            response
                .headers_mut()
                .insert(header::RETRY_AFTER, retry.to_string().parse().unwrap());
            return response;
        }
        let Ok(reservation) = app.gate.reserve() else {
            return refusal(StatusCode::SERVICE_UNAVAILABLE, "queue_full");
        };
        let ticket = match app
            .gate
            .start(
                reservation,
                Duration::from_secs(app.config.limits.queue_wait_secs),
            )
            .await
        {
            Ok(ticket) => ticket,
            Err(reason) => return refusal(StatusCode::SERVICE_UNAVAILABLE, reason),
        };
        request.extensions_mut().insert(ticket.clone());
        // Keep a permit while the downstream future runs, including upload.
        let response = next.run(request).await;
        drop(ticket);
        response
    }
    .await;
    let mut response = result;
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, "no-store".parse().unwrap());
    let id = response
        .extensions()
        .get::<ReportId>()
        .map_or("-", |id| id.0.as_str());
    eprintln!(
        "method={} status={} size={} report_id={} processing_ms={}",
        method,
        response.status().as_u16(),
        count.load(Ordering::Relaxed),
        id,
        started.elapsed().as_millis()
    );
    response
}
async fn receive(State(app): State<Arc<App>>, request: Request) -> Response {
    let ticket = request.extensions().get::<Arc<Ticket>>().cloned();
    let (parts, body) = request.into_parts();
    let read = async {
        // Read the complete bounded body, including multipart epilogue. The
        // multipart parser may stop at its last boundary before reaching EOF.
        let bytes = axum::body::to_bytes(body, intake::BODY_BYTES)
            .await
            .map_err(|e| {
                let mut cause: Option<&(dyn std::error::Error + 'static)> = Some(&e);
                while let Some(error) = cause {
                    if error.is::<http_body_util::LengthLimitError>() {
                        return (StatusCode::PAYLOAD_TOO_LARGE, "body_too_large");
                    }
                    cause = error.source();
                }
                (StatusCode::BAD_REQUEST, "invalid_body")
            })?;
        let request = Request::from_parts(parts, Body::from(bytes));
        let multipart = Multipart::from_request(request, &app)
            .await
            .map_err(|_| (StatusCode::BAD_REQUEST, "invalid_multipart"))?;
        intake::read(multipart).await
    };
    let report = match tokio::time::timeout(
        Duration::from_secs(app.config.limits.upload_secs),
        read,
    )
    .await
    {
        Ok(Ok(report)) => report,
        Ok(Err((status, reason))) => return refusal(status, reason),
        Err(_) => return refusal(StatusCode::REQUEST_TIMEOUT, "upload_timeout"),
    };
    // Finish bounded processing/storage even if the client disconnects after
    // upload. This prevents interrupted atomic writes accumulating on disk.
    tokio::spawn(process(app, report, ticket))
        .await
        .unwrap_or_else(|_| refusal(StatusCode::SERVICE_UNAVAILABLE, "processing_failed"))
}
async fn process(
    app: Arc<App>,
    mut report: intake::Report,
    ticket: Option<Arc<Ticket>>,
) -> Response {
    let _ticket = ticket.clone();
    let id = uuid::Uuid::new_v4().simple().to_string();
    let mut dump_path = None;
    let walk = if let Some(bytes) = report.minidump.take() {
        match app.store.dump(&id, &bytes).await {
            Ok(path) => dump_path = Some(path.to_string_lossy().into_owned()),
            Err(_) => return refusal(StatusCode::SERVICE_UNAVAILABLE, "storage_unavailable"),
        }
        symbolicate::walk(
            bytes,
            app.config.state_dir.join("symbols"),
            Duration::from_secs(app.config.limits.processing_secs),
            ticket,
        )
        .await
    } else {
        symbolicate::Walk {
            status: "no_dump",
            ..Default::default()
        }
    };
    let event = event::build(&id, &report, &walk, dump_path.as_deref());
    // 202 means the event is durable locally, independent of Bugsink uptime.
    // When capacity is exhausted, refuse instead of evicting accepted events.
    if app.store.enqueue(&id, &event).await.is_err() {
        app.store.remove_dump(&id).await;
        return refusal(StatusCode::SERVICE_UNAVAILABLE, "outbox_unavailable");
    }
    app.wake.notify_one();
    let mut response =
        (StatusCode::ACCEPTED, Json(serde_json::json!({ "id": id }))).into_response();
    response.extensions_mut().insert(ReportId(id));
    response
}
