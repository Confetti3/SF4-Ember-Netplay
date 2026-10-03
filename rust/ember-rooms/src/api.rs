//! The loopback HTTP surface the bridge talks to.
use axum::{
    Json, Router,
    body::Bytes,
    extract::{DefaultBodyLimit, Path, Request, State},
    http::{HeaderValue, StatusCode, header},
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::{delete, get},
};
use serde_json::json;

use crate::{
    protocol::CreateRoom,
    supervisor::{CreateError, Supervisor},
};

/// A create request is a few hundred bytes; this leaves room for long keys.
const MAX_BODY_BYTES: usize = 16 * 1024;

pub fn router(supervisor: Supervisor) -> Router {
    let authed = Router::new()
        .route("/rooms", get(list_rooms).post(create_room))
        .route("/rooms/{room_id}", delete(delete_room))
        .route_layer(middleware::from_fn_with_state(
            supervisor.clone(),
            require_secret,
        ));
    Router::new()
        .route("/health", get(health))
        .merge(authed)
        .layer(DefaultBodyLimit::max(MAX_BODY_BYTES))
        .with_state(supervisor)
}

fn no_store(mut response: Response) -> Response {
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    response
}

fn refusal(code: StatusCode, reason: &str) -> Response {
    no_store((code, Json(json!({ "reason": reason }))).into_response())
}

async fn require_secret(
    State(supervisor): State<Supervisor>,
    request: Request,
    next: Next,
) -> Response {
    let presented = request
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|value| value.to_str().ok());
    if !supervisor.settings().authorized(presented) {
        return no_store(StatusCode::UNAUTHORIZED.into_response());
    }
    next.run(request).await
}

async fn health() -> Response {
    no_store("ok".into_response())
}

async fn create_room(State(supervisor): State<Supervisor>, body: Bytes) -> Response {
    let Ok(request) = serde_json::from_slice::<CreateRoom>(&body) else {
        return refusal(StatusCode::BAD_REQUEST, "invalid_request");
    };
    match supervisor.create(request).await {
        Ok(hosted) => no_store((StatusCode::CREATED, Json(hosted)).into_response()),
        Err(CreateError::Invalid(field)) => no_store(
            (
                StatusCode::BAD_REQUEST,
                Json(json!({ "reason": "invalid_request", "field": field })),
            )
                .into_response(),
        ),
        Err(CreateError::RoomLimit) => refusal(StatusCode::CONFLICT, "room_limit"),
        Err(CreateError::UnsupportedBuild) => refusal(StatusCode::CONFLICT, "unsupported_build"),
        Err(CreateError::Exists) => refusal(StatusCode::CONFLICT, "exists"),
        Err(CreateError::HostFailed) => refusal(StatusCode::BAD_GATEWAY, "host_failed"),
    }
}

async fn list_rooms(State(supervisor): State<Supervisor>) -> Response {
    no_store(Json(supervisor.list()).into_response())
}

async fn delete_room(
    State(supervisor): State<Supervisor>,
    Path(room_id): Path<String>,
) -> Response {
    if supervisor.close_room(&room_id) {
        no_store(StatusCode::NO_CONTENT.into_response())
    } else {
        refusal(StatusCode::NOT_FOUND, "room_not_found")
    }
}
