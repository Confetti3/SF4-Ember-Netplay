use crate::{config::Limits, refusal::Refusal};
use axum::http::HeaderMap;
use std::{
    collections::{HashMap, VecDeque},
    net::{IpAddr, Ipv4Addr},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

pub fn client_address(peer: IpAddr, headers: &HeaderMap) -> IpAddr {
    if peer == IpAddr::V4(Ipv4Addr::LOCALHOST)
        && headers.get_all("x-real-ip").iter().count() == 1
        && let Some(address) = headers
            .get("x-real-ip")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.parse().ok())
    {
        return address;
    }
    peer
}

struct Windows {
    addresses: HashMap<IpAddr, VecDeque<Instant>>,
    global: VecDeque<Instant>,
}

pub struct RateLimiter {
    settings: Limits,
    windows: Mutex<Windows>,
}

fn expire(window: &mut VecDeque<Instant>, now: Instant, duration: Duration) {
    while window
        .front()
        .is_some_and(|time| now.saturating_duration_since(*time) >= duration)
    {
        window.pop_front();
    }
}
fn retry(window: &VecDeque<Instant>, now: Instant, duration: Duration) -> u64 {
    window.front().map_or(1, |time| {
        duration
            .saturating_sub(now.saturating_duration_since(*time))
            .as_secs()
            + 1
    })
}

impl RateLimiter {
    pub fn new(settings: Limits) -> Self {
        Self {
            settings,
            windows: Mutex::new(Windows {
                addresses: HashMap::new(),
                global: VecDeque::new(),
            }),
        }
    }
    pub fn admit(&self, address: IpAddr, now: Instant) -> Result<(), u64> {
        let mut w = self.windows.lock().unwrap();
        let a = Duration::from_secs(self.settings.address_window_secs);
        let g = Duration::from_secs(self.settings.global_window_secs);
        expire(&mut w.global, now, g);
        w.addresses.retain(|_, times| {
            expire(times, now, a);
            !times.is_empty()
        });
        if w.global.len() >= self.settings.global_reports {
            return Err(retry(&w.global, now, g));
        }
        if let Some(times) = w.addresses.get(&address) {
            if times.len() >= self.settings.per_address {
                return Err(retry(times, now, a));
            }
        } else if w.addresses.len() >= self.settings.tracked_addresses {
            return Err(a.as_secs());
        }
        w.addresses.entry(address).or_default().push_back(now);
        w.global.push_back(now);
        Ok(())
    }
}

#[derive(Clone)]
pub struct Gate {
    admission: Arc<Semaphore>,
    workers: Arc<Semaphore>,
}

// The worker permit is shared with a blocking symbolication task. Cancellation
// of an HTTP request never frees a CPU slot while its task still runs.
pub struct Ticket {
    _admission: OwnedSemaphorePermit,
    _worker: OwnedSemaphorePermit,
}

impl Gate {
    pub fn new(workers: usize, queue: usize) -> Self {
        Self {
            admission: Arc::new(Semaphore::new(workers + queue)),
            workers: Arc::new(Semaphore::new(workers)),
        }
    }
    pub fn reserve(&self) -> Result<OwnedSemaphorePermit, Refusal> {
        self.admission
            .clone()
            .try_acquire_owned()
            .map_err(|_| Refusal::QueueFull)
    }
    pub async fn start(
        &self,
        admission: OwnedSemaphorePermit,
        wait: Duration,
    ) -> Result<Arc<Ticket>, Refusal> {
        let worker = tokio::time::timeout(wait, self.workers.clone().acquire_owned())
            .await
            .map_err(|_| Refusal::QueueTimeout)?
            .map_err(|_| Refusal::QueueClosed)?;
        Ok(Arc::new(Ticket {
            _admission: admission,
            _worker: worker,
        }))
    }
}
