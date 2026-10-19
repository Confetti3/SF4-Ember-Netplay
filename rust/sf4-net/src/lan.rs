//! Local discovery and bounded invitation exchange. This is transport only:
//! the native room authority still owns seats, capacity and match admission.
use futures_util::StreamExt;
use iroh::{
    Endpoint, EndpointAddr, EndpointId, RelayMode,
    address_lookup::AddrFilter,
    endpoint::{Builder, Connection, PortmapperConfig, presets},
};
use iroh_mdns_address_lookup::{DiscoveryEvent, MdnsAddressLookup};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    io,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::{
    task::JoinHandle,
    time::{Instant, timeout},
};

pub const ALPN: &[u8] = b"sf4e/lan/1";
pub const LOCATOR: &str = "sf4lan:";
pub const MAX_ROOMS: usize = 64;
const SERVICE: &str = "sf4e-lan-v1";
const REQUEST: &[u8; 8] = b"SF4ELAN1";
const DEADLINE: Duration = Duration::from_secs(5);
// The discovery library's query cadence is ten seconds with jitter. Allow
// missed announcements; clean departures send a tombstone immediately.
const MAX_AGE: Duration = Duration::from_secs(45);

fn failed() -> io::Error {
    io::Error::other("lan_unavailable")
}

/// Every LAN endpoint uses only local discovery and direct UDP. In particular,
/// N0DisableRelay still uses internet DNS and is not an offline preset.
pub(crate) fn local_builder() -> Builder {
    Endpoint::builder(presets::Minimal)
        .relay_mode(RelayMode::Disabled)
        .portmapper_config(PortmapperConfig::Disabled)
        .address_lookup(
            MdnsAddressLookup::builder()
                .service_name(SERVICE)
                .addr_filter(AddrFilter::new(|addresses| {
                    std::borrow::Cow::Owned(
                        addresses
                            .iter()
                            .filter(|a| matches!(a, iroh::TransportAddr::Ip(_)))
                            .cloned()
                            .collect(),
                    )
                })),
        )
}

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Advertisement {
    #[serde(rename = "n")]
    pub name: String,
    #[serde(rename = "b")]
    pub build: String,
    #[serde(rename = "m")]
    pub members: u8,
    #[serde(rename = "c")]
    pub capacity: u8,
    /// Forces a local refresh even when the visible details are unchanged.
    #[serde(rename = "p", default)]
    pub pulse: u64,
}

impl Advertisement {
    pub fn valid(&self) -> bool {
        !self.name.is_empty()
            && self.name.len() <= 64
            && !self.name.chars().any(char::is_control)
            && !self.build.is_empty()
            && self.build.len() <= 128
            && (2..=16).contains(&self.capacity)
            && self.members > 0
            && self.members <= self.capacity
            && serde_json::to_string(self)
                .is_ok_and(|s| s.len() + 20 - self.pulse.to_string().len() <= 245)
    }
}

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Room {
    pub endpoint: EndpointId,
    #[serde(flatten)]
    pub info: Advertisement,
}

/// An expiring, bounded view; metadata is untrusted until room admission.
#[derive(Default)]
pub struct Rooms(BTreeMap<EndpointId, (Room, Instant)>);
impl Rooms {
    fn discovered(&mut self, id: EndpointId, text: Option<&str>, now: Instant) {
        self.0
            .retain(|_, (_, seen)| now.duration_since(*seen) < MAX_AGE);
        let info = text
            .and_then(|s| serde_json::from_str::<Advertisement>(s).ok())
            .filter(Advertisement::valid);
        match info {
            Some(info) if self.0.contains_key(&id) || self.0.len() < MAX_ROOMS => {
                self.0.insert(id, (Room { endpoint: id, info }, now));
            }
            _ => {
                self.0.remove(&id);
            }
        }
    }
    pub fn snapshot(&mut self, now: Instant) -> Vec<Room> {
        self.0
            .retain(|_, (_, seen)| now.duration_since(*seen) < MAX_AGE);
        self.0.values().map(|(room, _)| room.clone()).collect()
    }
}

/// The primary endpoint's lifetime also owns its browser subscriber.
pub struct Discovery {
    pub rooms: Arc<Mutex<Rooms>>,
    task: JoinHandle<()>,
    // The lookup library suppresses passive subscriber events while the
    // same instance is actively resolving that peer. Keep browsing separate
    // from Iroh's resolver so joining/reconnecting cannot hide a room.
    _browser: MdnsAddressLookup,
}
impl Drop for Discovery {
    fn drop(&mut self) {
        self.task.abort();
    }
}

pub async fn bind() -> io::Result<(Endpoint, Discovery)> {
    // Subscribe before publication so an initial answer cannot be missed.
    let endpoint = crate::transport::primary_builder(crate::transport::Transport::Lan)
        .bind()
        .await
        .map_err(|_| failed())?;
    let browser = MdnsAddressLookup::builder()
        .service_name(SERVICE)
        .advertise(false)
        .build(endpoint.id())
        .map_err(|_| failed())?;
    let mut events = browser.subscribe().await;
    let rooms = Arc::new(Mutex::new(Rooms::default()));
    let shared = rooms.clone();
    let own = endpoint.id();
    let task = tokio::spawn(async move {
        while let Some(event) = events.next().await {
            let mut rooms = shared.lock().unwrap_or_else(|e| e.into_inner());
            match event {
                DiscoveryEvent::Discovered { endpoint_info, .. }
                    if endpoint_info.endpoint_id != own =>
                {
                    let text = endpoint_info.data.user_data().map(ToString::to_string);
                    rooms.discovered(endpoint_info.endpoint_id, text.as_deref(), Instant::now());
                }
                DiscoveryEvent::Expired { endpoint_id } => {
                    rooms.0.remove(&endpoint_id);
                }
                _ => (),
            }
        }
    });
    Ok((
        endpoint,
        Discovery {
            rooms,
            task,
            _browser: browser,
        },
    ))
}

pub fn advertise(endpoint: &Endpoint, info: Option<&Advertisement>) -> io::Result<()> {
    let data = match info {
        Some(info) if info.valid() => Some(
            serde_json::to_string(info)
                .map_err(|_| failed())?
                .parse()
                .map_err(|_| failed())?,
        ),
        Some(_) => return Err(failed()),
        // The lookup's publisher retains its previous TXT attribute when
        // user_data is None. An explicit tombstone withdraws the room now.
        None => Some("{}".parse().map_err(|_| failed())?),
    };
    endpoint.set_user_data_for_address_lookup(data);
    Ok(())
}

pub fn locator(text: &str) -> Option<EndpointId> {
    text.trim().strip_prefix(LOCATOR)?.parse().ok()
}

/// A host exposes a capability only while it explicitly advertises a LAN
/// room. QUIC authenticates the endpoint selected by the browser; it does not
/// assert that an advertised display name belongs to a trusted person.
pub async fn serve(connection: Connection, invitation: String) {
    let _ = timeout(DEADLINE, async {
        let (mut send, mut recv) = connection.accept_bi().await.map_err(|_| failed())?;
        let mut request = [0; 8];
        recv.read_exact(&mut request).await.map_err(|_| failed())?;
        if &request != REQUEST {
            return Err(failed());
        }
        send.write_all(invitation.as_bytes())
            .await
            .map_err(|_| failed())?;
        send.finish().map_err(|_| failed())?;
        let _ = send.stopped().await;
        Ok::<(), io::Error>(())
    })
    .await;
    connection.close(0u32.into(), b"LAN invitation complete");
}

pub async fn invitation(
    endpoint: &Endpoint,
    host: EndpointId,
    build: &str,
    now: u64,
) -> io::Result<String> {
    timeout(DEADLINE, async {
        let connection = endpoint
            .connect(EndpointAddr::new(host), ALPN)
            .await
            .map_err(|_| failed())?;
        let result = async {
            let (mut send, mut recv) = connection.open_bi().await.map_err(|_| failed())?;
            send.write_all(REQUEST).await.map_err(|_| failed())?;
            send.finish().map_err(|_| failed())?;
            let bytes = recv
                .read_to_end(crate::invite::MAX_INVITE_LENGTH)
                .await
                .map_err(|_| failed())?;
            let text = String::from_utf8(bytes).map_err(|_| failed())?;
            let invite = crate::invite::Invite::parse_for_build(&text, now, build)?;
            if !invite.is_lan() || invite.endpoint() != host {
                return Err(failed());
            }
            Ok(text)
        }
        .await;
        connection.close(0u32.into(), b"LAN invitation received");
        result
    })
    .await
    .map_err(|_| failed())?
}

#[cfg(test)]
mod tests;
