use crate::{config::Config, store::Store};
use std::time::Duration;
use tokio::sync::Mutex;

#[derive(Debug, PartialEq, Eq)]
pub enum Delivery {
    Delivered,
    AlreadyDelivered,
    BackendUnavailable,
    Rejected(u16),
    ReadFailed,
    DeleteFailed,
}

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
    async fn send(&self, id: &str, event: &[u8]) -> Delivery {
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
            if let Ok(response) = result {
                let status = response.status();
                if status.is_success() {
                    return Delivery::Delivered;
                }
                // Transport, 5xx, throttling, authentication/configuration and
                // request timeout are backend-wide: retry, then pause a batch.
                // Other permanent rejections are specific to this event.
                if !status.is_server_error()
                    && !status.is_redirection()
                    && !matches!(status.as_u16(), 401 | 403 | 404 | 405 | 408 | 429)
                {
                    return Delivery::Rejected(status.as_u16());
                }
            }
        }
        Delivery::BackendUnavailable
    }
    async fn deliver_locked(&self, store: &Store, id: &str) -> Delivery {
        match store.event(id).await {
            Ok(Some(event)) => match self.send(id, &event).await {
                Delivery::Delivered => match store.delivered(id).await {
                    Ok(()) => Delivery::Delivered,
                    Err(_) => Delivery::DeleteFailed,
                },
                failure => failure,
            },
            Ok(None) => Delivery::AlreadyDelivered,
            Err(_) => Delivery::ReadFailed,
        }
    }
    pub async fn deliver(&self, store: &Store, id: &str) -> Delivery {
        let _lock = self.delivery.lock().await;
        self.deliver_locked(store, id).await
    }
    pub async fn replay(&self, store: &Store) {
        let _lock = self.delivery.lock().await;
        if let Ok(ids) = store.pending().await {
            for id in ids {
                // Event-specific failures stay capped on disk and rotate.
                // Only a backend-wide outage pauses the rest of this batch.
                if self.deliver_locked(store, &id).await == Delivery::BackendUnavailable {
                    break;
                }
            }
        }
    }
}
