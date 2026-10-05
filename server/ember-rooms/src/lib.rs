//! Public room supervisor for SF4 Ember Netplay.
//!
//! The bridge asks this loopback-only service to start, list and close
//! public rooms. Each room is one `sf4e-room-host` child process; the
//! supervisor allocates its UDP port, feeds it a JSON configuration line,
//! reads its status lines, closes it when it has been empty too long and
//! reaps it however it ends. See `docs/design/PUBLIC_ROOMS.md`.
//!
//! HTTP, all behind `Authorization: Bearer <secret>` except `/health`:
//!
//! - `POST /rooms` with `{ room_id, name, capacity, build_id, creator,
//!   bridge_id, ticket_key, ticket_kid }` answers 201 `{ invitation, region }`
//!   once the host reports `hosted`. Refusals are JSON `{ "reason": ... }`:
//!   409 `room_limit` (at the room limit, out of ports or draining), 409
//!   `unsupported_build`, 409 `exists`, 400 `invalid_request` and 502
//!   `host_failed`.
//! - `GET /rooms` answers 200 with `[{ room_id, members, capacity,
//!   tables_playing, invitation, banned, opened, details? }]` for live rooms;
//!   `opened` latches the first time the host reports a member and `details`
//!   is the host's latest listing details, when it sent usable ones.
//! - `DELETE /rooms/{room_id}` asks the host to close and answers 204, or 404.
//! - `GET /health` answers `ok`.
//!
//! The child protocol is documented in [`protocol`].
pub mod api;
pub mod config;
pub mod protocol;
pub mod supervisor;

pub use api::router;
pub use config::{Build, Config, Settings, Tuning};
pub use supervisor::{CreateError, Hosted, RoomInfo, Supervisor};

/// Serves the API on `listener`. When `signal` completes the supervisor
/// drains (see [`Supervisor::drain`]) and the server then stops. The API
/// stays up during the drain so the bridge keeps seeing the rooms.
pub async fn serve_until(
    listener: tokio::net::TcpListener,
    supervisor: Supervisor,
    signal: impl std::future::Future<Output = ()> + Send + 'static,
) -> std::io::Result<()> {
    let draining = supervisor.clone();
    axum::serve(listener, router(supervisor))
        .with_graceful_shutdown(async move {
            signal.await;
            eprintln!("ember-rooms: draining");
            draining.drain().await;
        })
        .await
}
