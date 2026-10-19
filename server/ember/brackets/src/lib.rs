//! Plays bracket sites' sets in Ember: README.md says what it does, and
//! ADAPTERS.md how a site is added. This module builds the connector from
//! its configuration (one worker per tournament, sharing the bridge client,
//! the database and each start.gg token's rate limit) and serves the
//! results URL.
mod bracket;
mod bridge;
mod budget;
mod challonge;
mod config;
mod http;
mod limit;
mod results;
mod roster;
mod schedule;
mod service;
mod sets;
pub mod site;
mod startgg;
mod store;

use std::{
    net::SocketAddr,
    sync::{
        Arc,
        atomic::{AtomicI64, Ordering},
    },
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use axum::{Router, extract::DefaultBodyLimit, routing::post};
use ember_protocol::webhook::Secret;
use tokio::{net::TcpListener, task::JoinHandle};

pub use config::{
    Adapter, Bridge, Config, Credential, CredentialKind, Results, RoundRule, Rounds, Side,
    Tournament,
};

use bracket::{Bracket, Context};
use bridge::BridgeClient;
use budget::Budget;
use challonge::Challonge;
use config::read_secret;
use limit::Limiters;
use results::Gate;
use service::Service;
use startgg::Startgg;
use store::Store;

/// One human-readable line on standard error. Never given a secret.
/// Control characters (from a site's or service's text) are shown as `?`,
/// so no answer can forge a line.
macro_rules! log {
    ($($arg:tt)*) => {
        eprintln!("ember-brackets: {}", $crate::printable(&format!($($arg)*)))
    };
}
pub(crate) use log;

/// `line` with each control character replaced by `?`.
pub(crate) fn printable(line: &str) -> String {
    line.chars()
        .map(|c| if c.is_control() { '?' } else { c })
        .collect()
}

/// A new, empty folder of its own for one test, so no two runs share one.
#[cfg(test)]
pub(crate) fn test_dir(name: &str) -> std::path::PathBuf {
    let mut bytes = [0u8; 8];
    getrandom::fill(&mut bytes).unwrap();
    let dir = std::env::temp_dir().join(format!(
        "ember-brackets-{name}-{}",
        ember_protocol::encoding::b64u(&bytes)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[cfg(test)]
mod tests {
    #[test]
    fn a_log_line_stays_one_line() {
        assert_eq!(
            super::printable("set s-1: round\nember-brackets: forged\u{1b}[2J"),
            "set s-1: round?ember-brackets: forged?[2J"
        );
        assert_eq!(super::printable("Pools \u{fc}"), "Pools \u{fc}");
    }
}

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
        Self::with_startgg_rate(config, clock, startgg::RATE)
    }

    /// As `new`, with each start.gg token allowed `rate.0` requests in any
    /// `rate.1` seconds. For tests, which make many requests in a moment.
    #[doc(hidden)]
    pub fn with_startgg_rate(
        config: Config,
        clock: Clock,
        rate: (usize, u64),
    ) -> Result<Self, String> {
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
        let (count, secs) = rate;
        let mut limiters = Limiters::new(count, Duration::from_secs(secs));
        let brackets = config
            .tournaments
            .iter()
            .map(|tournament| {
                let file = &tournament.credential.file;
                let credential = read_secret(file)?;
                let site: Box<dyn site::Site> = match tournament.adapter {
                    Adapter::Challonge => Box::new(Challonge::new(
                        tournament
                            .api_base
                            .as_deref()
                            .unwrap_or(challonge::API_ORIGIN),
                        &tournament.tournament,
                        tournament.bridge_site(),
                        tournament.credential.kind,
                        &credential,
                        // Challonge's free tier counts requests a month.
                        Budget::new(
                            store.clone(),
                            &credential,
                            file.display().to_string(),
                            config.monthly_request_budget,
                            clock.clone(),
                        ),
                        clock.clone(),
                    )?),
                    Adapter::Http => Box::new(Service::new(
                        tournament.api_base.as_deref().unwrap_or_default(),
                        &credential,
                    )?),
                    Adapter::Startgg => Box::new(Startgg::new(
                        tournament
                            .api_base
                            .as_deref()
                            .unwrap_or(startgg::API_ORIGIN),
                        &tournament.tournament,
                        tournament.bridge_site(),
                        &credential,
                        // One limit for everything that uses this token.
                        limiters.for_token(&credential),
                        clock.clone(),
                    )?),
                };
                Ok(Arc::new(Bracket::new(tournament, site, context.clone())?))
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
