//! Plays bracket sites' sets in Ember (docs/design/challonge/PLAN.md, part 4).
//!
//! For each configured tournament it reads the open sets, finds both
//! players by their connected site account on the bridge, and creates each
//! set as an `ember-room-v1` match on its `direct` connection. The bridge
//! sends each finished match to the connector's results URL; the connector
//! reads the bracket at once and reports the score if the site still has the
//! set open without one, or makes a fresh attempt at a set that restarted or
//! expired. A set the organizer changed by hand has its Ember match
//! cancelled. Reads back off while nothing changes, stay inside the
//! tournament's window, and stop once the bracket is finished, since
//! Challonge's free tier allows 500 requests a month.
//!
//! Sites sit behind `site::Site`; Challonge is the first. Everything the
//! connector decides survives a restart in its SQLite file, and each
//! attempt's `external_match_id` is fixed, so it never makes a match twice.
mod bracket;
mod bridge;
mod budget;
mod challonge;
mod config;
mod http;
mod results;
mod schedule;
mod sets;
pub mod site;
mod store;

use std::{
    net::SocketAddr,
    sync::{
        Arc,
        atomic::{AtomicI64, Ordering},
    },
    time::{SystemTime, UNIX_EPOCH},
};

use axum::{Router, extract::DefaultBodyLimit, routing::post};
use ember_protocol::webhook::Secret;
use tokio::{net::TcpListener, task::JoinHandle};

pub use config::{
    Bridge, Config, Credential, CredentialKind, Results, RoundRule, Rounds, Side, SiteKind,
    Tournament,
};

use bracket::{Bracket, Context};
use bridge::BridgeClient;
use budget::Budget;
use challonge::Challonge;
use config::read_secret;
use results::Gate;
use store::Store;

/// One human-readable line on standard error. Never given a secret.
macro_rules! log {
    ($($arg:tt)*) => {
        eprintln!("ember-brackets: {}", format_args!($($arg)*))
    };
}
pub(crate) use log;

/// Unix seconds: the wall clock, or for tests a clock that stands still
/// until it is moved.
#[derive(Clone)]
pub struct Clock {
    offset: Arc<AtomicI64>,
    wall: bool,
}

impl Default for Clock {
    fn default() -> Self {
        Self {
            offset: Arc::default(),
            wall: true,
        }
    }
}

impl Clock {
    /// A clock that reads `at` until it is moved.
    pub fn frozen(at: u64) -> Self {
        Self {
            offset: Arc::new(AtomicI64::new(at as i64)),
            wall: false,
        }
    }

    pub fn now(&self) -> u64 {
        let wall = if self.wall {
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_or(0, |elapsed| elapsed.as_secs() as i64)
        } else {
            0
        };
        (wall + self.offset.load(Ordering::Relaxed)).max(0) as u64
    }

    pub fn advance(&self, seconds: i64) {
        self.offset.fetch_add(seconds, Ordering::Relaxed);
    }
}

struct Inner {
    gate: Gate,
    results_path: String,
    store: Arc<Store>,
    clock: Clock,
    brackets: Vec<Arc<Bracket>>,
}

#[derive(Clone)]
pub struct Connector(Arc<Inner>);

impl Connector {
    pub fn new(config: Config, clock: Clock) -> Result<Self, String> {
        let _ = rustls::crypto::ring::default_provider().install_default();
        config.check()?;
        let secrets = std::iter::once(&config.results.secret_file)
            .chain(config.results.previous_secret_file.as_ref())
            .map(|path| {
                Secret::parse(&read_secret(path)?)
                    .map_err(|_| format!("{} holds no result secret", path.display()))
            })
            .collect::<Result<Vec<_>, String>>()?;
        let store = Arc::new(Store::open(&config.database)?);
        let context = Arc::new(Context {
            store: store.clone(),
            bridge: BridgeClient::new(
                &config.bridge.api,
                read_secret(&config.bridge.credential_file)?,
            )?,
            clock: clock.clone(),
            build_id: config.bridge.build_id.clone(),
            poll_secs: config.poll_secs,
            max_poll_secs: config.max_poll_secs,
            pointed_out: Default::default(),
        });
        let brackets = config
            .tournaments
            .iter()
            .map(|tournament| {
                let file = &tournament.credential.file;
                let credential = read_secret(file)?;
                let budget = Budget::new(
                    store.clone(),
                    &credential,
                    file.display().to_string(),
                    config.monthly_request_budget,
                    clock.clone(),
                );
                let site = match tournament.site {
                    SiteKind::Challonge => Challonge::new(
                        tournament
                            .api_base
                            .as_deref()
                            .unwrap_or(challonge::API_ORIGIN),
                        &tournament.tournament,
                        tournament.credential.kind,
                        &credential,
                        budget,
                    )?,
                };
                Ok(Arc::new(Bracket::new(
                    tournament,
                    Box::new(site),
                    context.clone(),
                )?))
            })
            .collect::<Result<_, String>>()?;
        Ok(Self(Arc::new(Inner {
            gate: Gate::new(secrets, config.bridge.connection_id),
            results_path: config.results.path,
            store,
            clock,
            brackets,
        })))
    }

    /// Receives results on `listener`, without the tournaments' workers.
    pub fn serve(&self, listener: TcpListener) -> std::io::Result<(SocketAddr, JoinHandle<()>)> {
        let address = listener.local_addr()?;
        let app = Router::new()
            .route(&self.0.results_path, post(results::receive))
            .layer(DefaultBodyLimit::max(results::MAX_BODY))
            .with_state(self.clone());
        let server = tokio::spawn(async move {
            let _ = axum::serve(listener, app).await;
        });
        Ok((address, server))
    }

    /// Receives results on `listener` and starts one worker per tournament.
    pub fn start(
        &self,
        listener: TcpListener,
    ) -> std::io::Result<(SocketAddr, Vec<JoinHandle<()>>)> {
        let (address, server) = self.serve(listener)?;
        let mut tasks = vec![server];
        tasks.extend(
            self.0
                .brackets
                .iter()
                .map(|bracket| tokio::spawn(bracket.clone().run())),
        );
        Ok((address, tasks))
    }

    /// One pass of every tournament, as its worker makes them: read the
    /// bracket if a result is waiting, if `poll`, or if it is due, and act
    /// on it.
    pub async fn pass(&self, poll: bool) {
        for bracket in &self.0.brackets {
            bracket.pass(poll).await;
        }
    }

    pub fn results_path(&self) -> &str {
        &self.0.results_path
    }
}
