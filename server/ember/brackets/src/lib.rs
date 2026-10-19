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
mod desk;
mod http;
mod limit;
mod results;
mod roster;
mod rounds;
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
use zeroize::Zeroizing;

pub use config::{Adapter, Bridge, Config, Credential, CredentialKind, Desk, Results, Tournament};
pub use rounds::{Hold, RoundRule, Rounds, Side};

use bracket::{Bracket, Context};
use bridge::BridgeClient;
use budget::Budget;
use challonge::Challonge;
use config::read_secret;
use desk::Desk as DeskEvents;
use limit::Limiters;
use results::Gate;
use service::Service;
use startgg::Startgg;
use store::Store;

/// One human-readable line on standard error. Never given a secret.
/// This is the only way the connector's library prints: a test
/// (`nothing_is_printed_but_through_log`) fails on any other print macro in
/// `src` but `main.rs`, so a new message has to come through here and its
/// review. `main.rs` prints its usage line, one startup line and the error
/// that stops it, which name a file or a setting and never a secret's value.
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

    /// The self-test prints the connector's output when a run fails, and its
    /// documents say that output never holds a secret. That holds because only
    /// `log!` prints in the library (`main.rs`, the entry point, prints its
    /// usage, a startup line and a fatal error), so no print macro may be added
    /// elsewhere without this test (and the documents) being looked at again.
    #[test]
    fn nothing_is_printed_but_through_log() {
        let macros: Vec<String> = ["print", "eprint"]
            .iter()
            .flat_map(|name| [format!("{name}!"), format!("{name}ln!")])
            .chain([format!("{}!", "dbg")])
            .collect();
        // Every source file under `src`, in its folders too.
        let mut folders = vec![std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src")];
        while let Some(folder) = folders.pop() {
            for entry in std::fs::read_dir(folder).unwrap() {
                let path = entry.unwrap().path();
                if path.is_dir() {
                    folders.push(path);
                    continue;
                }
                let is_source = path.extension().is_some_and(|extension| extension == "rs");
                if !is_source || path.ends_with("main.rs") {
                    continue;
                }
                let text = std::fs::read_to_string(&path).unwrap();
                for (number, line) in text.lines().enumerate() {
                    let line = line.trim_start();
                    let is_log = path.ends_with("lib.rs")
                        && line.starts_with(concat!("eprint", "ln!(\"ember-brackets: {}\""));
                    let printing = macros.iter().any(|name| line.contains(name.as_str()));
                    assert!(
                        !printing || is_log || line.starts_with("//"),
                        "{}:{}: print through log!",
                        path.display(),
                        number + 1
                    );
                }
            }
        }
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
    desk: Option<DeskEvents>,
}

/// Builds each tournament's site, sharing the database, the Challonge
/// budget and each start.gg token's rate limit.
pub(crate) struct Sites {
    store: Arc<Store>,
    monthly_request_budget: u32,
    clock: Clock,
    limiters: std::sync::Mutex<Limiters>,
}

impl Sites {
    /// The site of `tournament`, reached with `credential`. Its Challonge
    /// budget is kept under `budget` (the credential itself, so tournaments
    /// sharing one share it) and named `name` in the log.
    fn site(
        &self,
        tournament: &Tournament,
        credential: &Zeroizing<String>,
        budget: &str,
        name: String,
    ) -> Result<Box<dyn site::Site>, String> {
        let clock = self.clock.clone();
        Ok(match tournament.adapter {
            Adapter::Challonge => Box::new(Challonge::new(
                tournament
                    .api_base
                    .as_deref()
                    .unwrap_or(challonge::API_ORIGIN),
                &tournament.tournament,
                tournament.bridge_site(),
                tournament.credential.kind,
                credential,
                // Challonge's free tier counts requests a month.
                Budget::new(
                    self.store.clone(),
                    budget,
                    name,
                    self.monthly_request_budget,
                    clock.clone(),
                ),
                clock,
            )?),
            Adapter::Http => Box::new(Service::new(
                tournament.api_base.as_deref().unwrap_or_default(),
                credential,
            )?),
            Adapter::Startgg => Box::new(Startgg::new(
                tournament
                    .api_base
                    .as_deref()
                    .unwrap_or(startgg::API_ORIGIN),
                &tournament.tournament,
                tournament.bridge_site(),
                credential,
                // One limit for everything that uses this token.
                self.limiters
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner())
                    .for_token(credential),
                clock,
                startgg::Wanted {
                    phases: tournament.rounds.names_phases(),
                    streams: tournament.hold.streamed,
                },
            )?),
        })
    }
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
        let sites = Sites {
            store: store.clone(),
            monthly_request_budget: config.monthly_request_budget,
            clock: clock.clone(),
            limiters: std::sync::Mutex::new(Limiters::new(count, Duration::from_secs(secs))),
        };
        let brackets: Vec<Arc<Bracket>> = config
            .tournaments
            .iter()
            .map(|tournament| {
                let file = &tournament.credential.file;
                let credential = read_secret(file)?;
                let site = sites.site(
                    tournament,
                    &credential,
                    &credential,
                    file.display().to_string(),
                )?;
                Ok(Arc::new(Bracket::new(tournament, site, context.clone())?))
            })
            .collect::<Result<_, String>>()?;
        let desk = config.desk.map(|desk| {
            let configured = brackets.iter().map(|bracket| bracket.key.clone()).collect();
            DeskEvents::new(
                sites,
                context.clone(),
                desk,
                config.allow_loopback_http,
                configured,
            )
        });
        Ok(Self(Arc::new(Inner {
            gate: Gate::new(secrets, config.bridge.connection_id),
            results_path: config.results.path,
            store,
            clock,
            brackets,
            desk,
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
        if self.0.desk.is_some() {
            let connector = self.clone();
            tasks.push(tokio::spawn(async move {
                let desk = connector.0.desk.as_ref().expect("checked above");
                loop {
                    desk.sync(true).await;
                    tokio::time::sleep(Duration::from_secs(desk.context.poll_secs)).await;
                }
            }));
        }
        Ok((address, tasks))
    }

    /// One pass of every tournament, as its worker makes them: read the
    /// bracket if a result is waiting, if `poll`, or if it is due, and act
    /// on it.
    pub async fn pass(&self, poll: bool) {
        let mut brackets = self.0.brackets.clone();
        if let Some(desk) = &self.0.desk {
            desk.sync(false).await;
            brackets.extend(desk.brackets());
        }
        for bracket in &brackets {
            bracket.pass(poll).await;
        }
    }

    pub fn results_path(&self) -> &str {
        &self.0.results_path
    }
}
