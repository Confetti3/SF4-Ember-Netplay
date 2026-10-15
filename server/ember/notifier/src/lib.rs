//! A reference subscriber for Ember bridge events.
//!
//! It verifies each Standard Webhooks delivery before reading the body,
//! accepts only events from the configured bridge, records each event ID once
//! in a durable inbox, and answers 2xx only after that write (spec 20.4).
//! A worker then posts each event to a Discord channel webhook and Twitch
//! chat, retrying on its own schedule. Any other consumer of the bridge's
//! webhooks can work the same way.
//!
//! With a `bot` it is also a room bot: Discord's `/room` command (HTTP
//! interactions) and Twitch's `!room` (EventSub over WebSocket) open a public
//! room through the bridge and answer with its link.
mod bot;
mod bridge;
mod config;
mod discord;
mod eventsub;
mod format;
mod inbox;
mod interactions;
mod sent;
mod twitch;
mod twitch_room;

use std::{
    net::SocketAddr,
    sync::{
        Arc, Mutex,
        atomic::{AtomicI64, Ordering},
    },
    time::{SystemTime, UNIX_EPOCH},
};

use axum::{Router, routing::post};
use bridge::BridgeClient;
use ed25519_dalek::VerifyingKey;
use ember_protocol::webhook::Secret;
use rusqlite::Connection;
use tokio::{net::TcpListener, sync::Notify, task::JoinHandle};
use zeroize::Zeroizing;

pub use config::{Bot, Config, Discord, Interactions, Twitch};
pub use format::Message;
pub use interactions::register_commands;

use config::resolve;
use sent::http_client;

#[derive(Clone, Default)]
pub struct Clock(Arc<AtomicI64>);

impl Clock {
    pub fn now(&self) -> u64 {
        let wall = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_secs() as i64);
        (wall + self.0.load(Ordering::Relaxed)).max(0) as u64
    }

    pub fn advance(&self, seconds: i64) {
        self.0.fetch_add(seconds, Ordering::Relaxed);
    }
}

struct Inner {
    config: Config,
    secrets: Vec<Secret>,
    discord_url: Option<Zeroizing<String>>,
    twitch_token: Option<Zeroizing<String>>,
    db: Mutex<Connection>,
    clock: Clock,
    wake: Notify,
    http: reqwest::Client,
    /// The bridge connection the room commands act through (`bot`).
    bridge: Option<BridgeClient>,
    /// The application's public key, when `/room` is served.
    interaction_key: Option<VerifyingKey>,
}

#[derive(Clone)]
pub struct Notifier(Arc<Inner>);

impl Notifier {
    pub fn new(config: Config, clock: Clock) -> Result<Self, String> {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let secrets = config
            .secrets
            .iter()
            .map(|secret| {
                Secret::parse(&resolve(secret)?)
                    .map_err(|_| "invalid subscription secret".to_owned())
            })
            .collect::<Result<Vec<_>, _>>()?;
        if secrets.is_empty() {
            return Err("at least one subscription secret is required".into());
        }
        let discord_url = config
            .discord
            .as_ref()
            .and_then(|d| d.webhook_url.as_deref())
            .map(resolve)
            .transpose()?;
        let twitch_token = config
            .twitch
            .as_ref()
            .map(|t| resolve(&t.token))
            .transpose()?;
        let db = Connection::open(&config.database)
            .map_err(|error| format!("cannot open database: {error}"))?;
        db.execute_batch(
            "PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL;
             CREATE TABLE IF NOT EXISTS inbox (
                 id TEXT PRIMARY KEY, type TEXT NOT NULL, body BLOB NOT NULL, received_at INTEGER NOT NULL,
                 discord_done INTEGER NOT NULL, twitch_done INTEGER NOT NULL,
                 attempts INTEGER NOT NULL DEFAULT 0, next_at INTEGER NOT NULL, last_error TEXT);
             CREATE TABLE IF NOT EXISTS matches (id TEXT PRIMARY KEY, label TEXT NOT NULL);",
        )
        .map_err(|error| format!("cannot prepare database: {error}"))?;
        let bridge = config
            .bot
            .as_ref()
            .map(|bot| BridgeClient::new(&bot.bridge_api, resolve(&bot.credential)?))
            .transpose()?;
        let interactions = config
            .discord
            .as_ref()
            .and_then(|discord| discord.interactions.as_ref());
        let room_creator = config
            .twitch
            .as_ref()
            .is_some_and(|twitch| twitch.room_creator.is_some());
        if bridge.is_none() && (interactions.is_some() || room_creator) {
            return Err("the room commands need a `bot` section".into());
        }
        let interaction_key = interactions
            .map(|interactions| interactions::public_key(&interactions.public_key))
            .transpose()?;
        Ok(Self(Arc::new(Inner {
            discord_url,
            twitch_token,
            secrets,
            config,
            db: Mutex::new(db),
            clock,
            wake: Notify::new(),
            http: http_client()?,
            bridge,
            interaction_key,
        })))
    }

    /// Serves `POST /webhook` (and `POST /discord/interactions` when `/room`
    /// is configured) on `listener`, and starts the sender and the Twitch
    /// listener.
    pub fn start(
        &self,
        listener: TcpListener,
    ) -> std::io::Result<(SocketAddr, Vec<JoinHandle<()>>)> {
        let address = listener.local_addr()?;
        let mut app = Router::new().route("/webhook", post(inbox::receive));
        if self.0.interaction_key.is_some() {
            app = app.route("/discord/interactions", post(interactions::receive));
        }
        let app = app.with_state(self.clone());
        let server = tokio::spawn(async move {
            let _ = axum::serve(listener, app).await;
        });
        let mut tasks = vec![server, tokio::spawn(self.clone().run())];
        if self.twitch_room_enabled() {
            tasks.push(tokio::spawn(self.clone().eventsub()));
        }
        Ok((address, tasks))
    }
}
