use crate::{config::Config, store::Store};
use std::time::Duration;
use tokio::sync::Mutex;

pub struct Bugsink {
    client: reqwest::Client,
    url: String,
    host: reqwest::header::HeaderValue,
    auth: reqwest::header::HeaderValue,
    // Serialise disk claim -> send -> remove across requests and replay. Never
    // log reqwest errors: they can carry a URL/header or remote response text.
    delivery: Mutex<()>,
}
impl Bugsink {
    pub fn new(config: &Config, key: &str) -> Result<Self, &'static str> {
        if key.is_empty() || key.len() > 128 || !key.bytes().all(|b| b.is_ascii_alphanumeric()) {
            return Err("invalid credential");
        }
        let mut auth = reqwest::header::HeaderValue::from_str(&format!(
            "Sentry sentry_version=7, sentry_key={key}, sentry_client=ember-reports/{}",
            env!("CARGO_PKG_VERSION")
        ))
        .map_err(|_| "invalid credential")?;
        auth.set_sensitive(true);
        Ok(Self {
            client: reqwest::Client::builder()
                .no_proxy()
                .redirect(reqwest::redirect::Policy::none())
                .connect_timeout(Duration::from_secs(1))
                .timeout(Duration::from_secs(3))
                .build()
                .map_err(|_| "cannot create HTTP client")?,
            url: format!(
                "{}/api/{}/envelope/",
                config.bugsink_base.trim_end_matches('/'),
                config.project_id
            ),
            host: config
                .bugsink_host
                .parse()
                .map_err(|_| "invalid Bugsink host")?,
            auth,
            delivery: Mutex::new(()),
        })
    }
    pub fn envelope(id: &str, event: &[u8]) -> Vec<u8> {
        let header = serde_json::json!({ "event_id": id });
        let item = serde_json::json!({ "type": "event", "length": event.len(), "content_type": "application/json" });
        let mut bytes = format!("{header}\n{item}\n").into_bytes();
        bytes.extend_from_slice(event);
        bytes.push(b'\n');
        bytes
    }
    async fn send(&self, id: &str, event: &[u8]) -> bool {
        for attempt in 0..3 {
            if attempt > 0 {
                tokio::time::sleep(Duration::from_millis(if attempt == 1 { 250 } else { 1000 }))
                    .await;
            }
            let result = self
                .client
                .post(&self.url)
                .header(reqwest::header::HOST, self.host.clone())
                .header("x-sentry-auth", self.auth.clone())
                .header(
                    reqwest::header::CONTENT_TYPE,
                    "application/x-sentry-envelope",
                )
                .body(Self::envelope(id, event))
                .send()
                .await;
            if let Ok(response) = result
                && response.status().is_success()
            {
                return true;
            }
            // Even persistent 4xx stays bounded on disk for operator repair.
        }
        false
    }
    pub async fn deliver(&self, store: &Store, id: &str) -> bool {
        let _lock = self.delivery.lock().await;
        match store.event(id).await {
            Ok(Some(event)) => self.send(id, &event).await && store.delivered(id).await.is_ok(),
            Ok(None) => true,
            Err(_) => false,
        }
    }
    pub async fn replay(&self, store: &Store) {
        if let Ok(ids) = store.pending().await {
            for id in ids {
                // Stop a batch at the first failure; don't multiply retries
                // across hundreds of pending events while Bugsink is down.
                if !self.deliver(store, &id).await {
                    break;
                }
            }
        }
    }
}
