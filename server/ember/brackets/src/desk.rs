//! Organizers' own events (docs/design/TOURNAMENT_DESK.md): read from the
//! bridge's desk connection (`/v1/desk/brackets`) and run as configured
//! tournaments are, each with the access token the bridge hands out with it.
//! The token is kept in memory only; a newer one, or changed settings,
//! replaces the event's worker. An event the bridge no longer lists stops being
//! read, its sets left as they are, as at the end of `active_until`.
//! With `desk.site`, the events are the bracket site's live tournaments
//! instead, run with its connector token, and their sets stay on the site.
use std::{
    collections::{HashMap, HashSet},
    sync::{Arc, Mutex},
};

use serde_json::{Value, json};
use tokio::task::JoinHandle;
use zeroize::Zeroizing;

use crate::{
    Sites,
    bracket::{Bracket, Context},
    bridge::DeskEvent,
    config::{self, Tournament},
    log,
};

/// The settings of a configured tournament an event may carry: the bridge
/// keeps them to these keys too (`routes::organizer::SETTINGS`).
const SETTINGS: [&str; 5] = [
    "rounds",
    "hold",
    "active_from",
    "active_until",
    "mark_underway",
];

pub(crate) struct Desk {
    pub sites: Sites,
    pub context: Arc<Context>,
    pub config: config::Desk,
    pub allow_loopback_http: bool,
    /// The configured tournaments' keys, which an event cannot take over.
    pub configured: HashSet<String>,
    running: Mutex<HashMap<String, Running>>,
    /// Events that could not be run, each said once until they can.
    refused: Mutex<HashSet<String>>,
    failing: std::sync::atomic::AtomicBool,
    /// The bracket site the events come from, and its connector token.
    site: Option<(ember_protocol::client::BridgeClient, Zeroizing<String>)>,
}

/// A tournament the bracket site lists as live.
#[derive(serde::Deserialize)]
struct SiteTournament {
    id: String,
    games_to_win: u8,
}

#[derive(serde::Deserialize)]
struct SiteTournaments {
    tournaments: Vec<SiteTournament>,
}

struct Running {
    token: Zeroizing<String>,
    /// The settings it runs with: changed ones (a set held from the desk)
    /// replace the worker too.
    settings: serde_json::Map<String, Value>,
    bracket: Arc<Bracket>,
    task: Option<JoinHandle<()>>,
}

impl Drop for Running {
    fn drop(&mut self) {
        if let Some(task) = self.task.take() {
            task.abort();
        }
    }
}

impl Desk {
    pub fn new(
        sites: Sites,
        context: Arc<Context>,
        config: config::Desk,
        allow_loopback_http: bool,
        configured: HashSet<String>,
    ) -> Result<Self, String> {
        let site = match &config.site {
            Some(site) => {
                let token = config::read_secret(&site.credential_file)?;
                let base = &config.services[&site.service];
                let client = ember_protocol::client::BridgeClient::new(
                    crate::http::http_client()?,
                    base,
                    token.clone(),
                );
                Some((client, token))
            }
            None => None,
        };
        Ok(Self {
            sites,
            context,
            config,
            allow_loopback_http,
            configured,
            running: Mutex::default(),
            refused: Mutex::default(),
            failing: Default::default(),
            site,
        })
    }

    /// The running events: the bridge's, or the bracket site's live
    /// tournaments as events on its service.
    async fn events(&self) -> Result<Vec<DeskEvent>, String> {
        let (Some((client, token)), Some(site)) = (&self.site, &self.config.site) else {
            return self
                .context
                .bridge
                .desk()
                .await
                .map_err(|error| error.to_string());
        };
        let listed: SiteTournaments = client
            .answer(client.get("/ember/tournaments"), 256 * 1024)
            .await
            .map_err(|_| "the bracket site did not answer".to_owned())?;
        Ok(listed
            .tournaments
            .into_iter()
            .map(|tournament| DeskEvent {
                id: tournament.id.clone(),
                site: site.service.clone(),
                settings: json!({
                    "rounds": { "games_to_win": tournament.games_to_win },
                    "mark_underway": true,
                })
                .as_object()
                .cloned()
                .unwrap_or_default(),
                tournament: tournament.id,
                access_token: token.to_string(),
            })
            .collect())
    }

    fn running(&self) -> std::sync::MutexGuard<'_, HashMap<String, Running>> {
        self.running
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// The brackets of the running events.
    pub fn brackets(&self) -> Vec<Arc<Bracket>> {
        self.running()
            .values()
            .map(|running| running.bracket.clone())
            .collect()
    }

    /// Reads the running events and starts, replaces or stops their
    /// brackets; with `spawn`, a started one gets a worker of its own. A
    /// failed read changes nothing.
    pub async fn sync(&self, spawn: bool) {
        use std::sync::atomic::Ordering;
        let events = match self.events().await {
            Ok(events) => {
                if self.failing.swap(false, Ordering::Relaxed) {
                    log!("desk: reading its events works again");
                }
                events
            }
            Err(error) => {
                if !self.failing.swap(true, Ordering::Relaxed) {
                    log!("desk: cannot read its events ({error}); tried again each poll");
                }
                return;
            }
        };
        let mut running = self.running();
        running.retain(|id, run| {
            let listed = events.iter().any(|event| event.id == *id);
            if !listed {
                log!("{}: no longer run by its organizer", run.bracket.key);
            }
            listed
        });
        for event in events {
            let token = Zeroizing::new(event.access_token.clone());
            if running
                .get(&event.id)
                .is_some_and(|run| run.token == token && run.settings == event.settings)
            {
                continue;
            }
            let taken = |key: &str, running: &HashMap<String, Running>| {
                self.configured.contains(key)
                    || running
                        .iter()
                        .any(|(id, run)| *id != event.id && run.bracket.key == key)
            };
            let made = self.tournament(&event).and_then(|tournament| {
                if taken(&tournament.key(), &running) {
                    return Err(format!("{} is already run here", tournament.key()));
                }
                // The budget follows the event, not its token, which changes.
                let name = format!("desk event {}", event.id);
                let site = self.sites.site(&tournament, &token, &name, name.clone())?;
                let bracket = Bracket::new(&tournament, site, self.context.clone())?;
                // The bracket site keeps its own sets: none go to the bridge.
                Ok(if self.site.is_some() {
                    bracket
                } else {
                    bracket.for_desk(event.id.clone(), held(&event))
                })
            });
            let bracket = match made {
                Ok(bracket) => Arc::new(bracket),
                Err(error) => {
                    if self.refused().insert(event.id.clone()) {
                        log!("desk event {}: {error}; not run", event.id);
                    }
                    continue;
                }
            };
            self.refused().remove(&event.id);
            if !running.contains_key(&event.id) {
                log!("{}: run for its organizer", bracket.key);
            }
            let task = spawn.then(|| tokio::spawn(bracket.clone().run()));
            running.insert(
                event.id.clone(),
                Running {
                    token,
                    settings: event.settings.clone(),
                    bracket,
                    task,
                },
            );
        }
    }

    fn refused(&self) -> std::sync::MutexGuard<'_, HashSet<String>> {
        self.refused
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// The event as a configured tournament: its site's adapter, its
    /// settings, and the bearer token in place of a credential file. A site
    /// in `services` is played on the `http` adapter.
    fn tournament(&self, event: &DeskEvent) -> Result<Tournament, String> {
        let service = self.config.services.get(&event.site);
        if service.is_none() && !matches!(event.site.as_str(), "challonge" | "startgg") {
            return Err(format!("site {} has no adapter here", event.site));
        }
        let mut fields: serde_json::Map<String, Value> = event
            .settings
            .iter()
            .filter(|(key, _)| SETTINGS.contains(&key.as_str()))
            .map(|(key, value)| (key.clone(), value.clone()))
            .collect();
        // The desk's holds are the bracket's own, not a configured setting.
        if let Some(hold) = fields.get_mut("hold").and_then(Value::as_object_mut) {
            hold.remove("sets");
        }
        fields.insert("tournament".into(), json!(event.tournament));
        fields.insert("credential".into(), json!({ "kind": "bearer", "file": "" }));
        if let Some(base) = service {
            fields.insert("adapter".into(), json!("http"));
            let base = base.trim_end_matches('/');
            fields.insert(
                "api_base".into(),
                json!(format!("{base}/tournaments/{}/ember", event.tournament)),
            );
        } else {
            fields.insert("adapter".into(), json!(event.site));
            if let Some(base) = self.config.api_bases.get(&event.site) {
                fields.insert("api_base".into(), json!(base));
            }
        }
        let tournament: Tournament = serde_json::from_value(Value::Object(fields))
            .map_err(|error| format!("its settings do not read: {error}"))?;
        tournament.check(self.allow_loopback_http)?;
        Ok(tournament)
    }
}

/// The sets the event's organizer holds from the desk (`hold.sets`).
fn held(event: &DeskEvent) -> Vec<String> {
    event
        .settings
        .get("hold")
        .and_then(|hold| hold.get("sets"))
        .and_then(Value::as_array)
        .map(|sets| {
            sets.iter()
                .filter_map(|set| set.as_str().map(str::to_owned))
                .collect()
        })
        .unwrap_or_default()
}
