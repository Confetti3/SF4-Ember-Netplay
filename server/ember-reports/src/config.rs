use serde::Deserialize;
use std::{net::SocketAddr, path::PathBuf};

/// The port deploy/nginx proxies to. nginx hard-codes it, so any other bind
/// would start cleanly yet receive no traffic.
pub const BIND_PORT: u16 = 47850;

#[derive(Clone, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    pub bind: SocketAddr,
    pub state_dir: PathBuf,
    pub bugsink_base: String,
    pub bugsink_host: String,
    pub project_id: u64,
    pub limits: Limits,
}

#[derive(Clone, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Limits {
    pub per_address: usize,
    pub address_window_secs: u64,
    pub global_reports: usize,
    pub global_window_secs: u64,
    pub tracked_addresses: usize,
    pub workers: usize,
    pub queue: usize,
    pub upload_secs: u64,
    pub queue_wait_secs: u64,
    pub processing_secs: u64,
    pub outbox_count: usize,
    pub outbox_bytes: u64,
    pub dump_days: u64,
    pub dump_bytes: u64,
    pub dump_count: usize,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            bind: SocketAddr::from((std::net::Ipv4Addr::LOCALHOST, BIND_PORT)),
            state_dir: "/var/lib/ember-reports".into(),
            bugsink_base: "http://127.0.0.1:47860".into(),
            bugsink_host: "bugs.embernetplay.link".into(),
            project_id: 1,
            limits: Limits::default(),
        }
    }
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            per_address: 5,
            address_window_secs: 600,
            global_reports: 300,
            global_window_secs: 3600,
            tracked_addresses: 4096,
            workers: 4,
            queue: 32,
            upload_secs: 20,
            queue_wait_secs: 60,
            processing_secs: 20,
            outbox_count: 512,
            outbox_bytes: 256 * 1024 * 1024,
            dump_days: 30,
            dump_bytes: 2 * 1024 * 1024 * 1024,
            dump_count: 4096,
        }
    }
}

impl Config {
    pub fn validate(&self) -> Result<(), &'static str> {
        if self.bind.ip() != std::net::Ipv4Addr::LOCALHOST || self.bind.port() != BIND_PORT {
            return Err("bind must be 127.0.0.1:47850, the port nginx proxies to");
        }
        let url = reqwest::Url::parse(&self.bugsink_base).map_err(|_| "invalid bugsink_base")?;
        if url.scheme() != "http"
            || url.host_str() != Some("127.0.0.1")
            || !url.username().is_empty()
            || url.password().is_some()
            || url.query().is_some()
            || url.fragment().is_some()
            || url.path() != "/"
            || self.project_id == 0
        {
            return Err("Bugsink must use plain HTTP on 127.0.0.1, without a path or credentials");
        }
        if self.bugsink_host.is_empty()
            || self.bugsink_host.len() > 253
            || !self
                .bugsink_host
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-' || b == b':')
        {
            return Err("invalid bugsink_host");
        }
        if !self.state_dir.is_absolute() {
            return Err("state_dir must be absolute");
        }
        let l = &self.limits;
        if l.workers == 0
            || l.workers > 4
            || l.queue > 32
            || l.per_address == 0
            || l.per_address > 5
            || l.global_reports == 0
            || l.global_reports > 300
            || !(600..=86400).contains(&l.address_window_secs)
            || !(3600..=86400).contains(&l.global_window_secs)
            || l.tracked_addresses == 0
            || l.tracked_addresses > 4096
            || !(1..=20).contains(&l.processing_secs)
            || !(1..=30).contains(&l.upload_secs)
            || !(1..=60).contains(&l.queue_wait_secs)
            || l.outbox_count == 0
            || l.outbox_count > 512
            || !(900 * 1024..=256 * 1024 * 1024).contains(&l.outbox_bytes)
            || l.dump_days == 0
            || l.dump_days > 30
            || !(4 * 1024 * 1024..=2 * 1024 * 1024 * 1024).contains(&l.dump_bytes)
            || l.dump_count == 0
            || l.dump_count > 4096
        {
            return Err("limits outside supported bounds (may tighten, never exceed hard caps)");
        }
        Ok(())
    }

    pub fn credential() -> Result<String, &'static str> {
        let directory =
            std::env::var_os("CREDENTIALS_DIRECTORY").ok_or("missing credentials directory")?;
        let bytes = std::fs::read(PathBuf::from(directory).join("bugsink_dsn_key"))
            .map_err(|_| "cannot read Bugsink credential")?;
        let key = std::str::from_utf8(&bytes)
            .map_err(|_| "invalid Bugsink credential")?
            .trim();
        if key.is_empty() || key.len() > 128 || !key.bytes().all(|b| b.is_ascii_alphanumeric()) {
            return Err("invalid Bugsink credential");
        }
        Ok(key.to_owned())
    }
}
