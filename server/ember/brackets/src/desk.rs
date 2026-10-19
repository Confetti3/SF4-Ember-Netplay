//! Organizers' own events (docs/design/TOURNAMENT_DESK.md): read from the
//! bridge's desk connection (`/v1/desk/brackets`) and run as configured
//! tournaments are, each with the access token the bridge hands out with it.
//! The token is kept in memory only; a newer one, or changed settings,
//! replaces the event's worker. An event the bridge no longer lists stops being
//! read, its sets left as they are, as at the end of `active_until`.
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
    ) -> Self {
        Self {
            sites,
            context,
            config,
            allow_loopback_http,
            configured,
            running: Mutex::default(),
            refused: Mutex::default(),
            failing: Default::default(),
        }
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
        let events = match self.context.bridge.desk().await {
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
                Bracket::new(&tournament, site, self.context.clone())
                    .map(|bracket| bracket.for_desk(event.id.clone(), held(&event)))
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
    /// settings, and the bearer token in place of a credential file.
    fn tournament(&self, event: &DeskEvent) -> Result<Tournament, String> {
        if !matches!(event.site.as_str(), "challonge" | "startgg") {
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
        fields.insert("adapter".into(), json!(event.site));
        fields.insert("tournament".into(), json!(event.tournament));
        fields.insert("credential".into(), json!({ "kind": "bearer", "file": "" }));
        if let Some(base) = self.config.api_bases.get(&event.site) {
            fields.insert("api_base".into(), json!(base));
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
