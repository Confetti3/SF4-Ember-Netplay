//! Event-driven raw GGPO packet forwarding. Each bridge owns exactly one
//! authorized remote peer and one immutable loopback mapping for one match.
use std::{
    io,
    net::{Ipv4Addr, SocketAddr},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use crate::{transport::GameConnection, wire::MAX_UDP_PAYLOAD};
use tokio::{net::UdpSocket, sync::watch};

#[derive(Clone, Copy, Debug, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Failure {
    PacketLimit,
    DatagramLimit,
    SendFailed,
    PeerClosed,
    LocalSocket,
}

#[derive(Default)]
pub struct BridgeStats {
    pub sent_packets: AtomicU64,
    pub sent_bytes: AtomicU64,
    pub received_packets: AtomicU64,
    pub received_bytes: AtomicU64,
    pub rejected_packets: AtomicU64,
    pub congestion_events: AtomicU64,
    pub local_drops: AtomicU64,
}

pub struct Bridge {
    game: GameConnection,
    socket: UdpSocket,
    ggpo: SocketAddr,
    pub stats: Arc<BridgeStats>,
}

impl Bridge {
    /// `ggpo` must come from authenticated local registration, never a remote
    /// packet. Inbound datagrams from any other source are rejected.
    ///
    /// The socket is deliberately left unconnected (F-008). When GGPO closes
    /// its socket at match end while the peer is still sending, a connected
    /// socket receives ICMP port-unreachable errors. Under Wine that stalled
    /// every runtime worker, so the helper stopped answering and the room
    /// closed on match_teardown_timeout.
    pub async fn bind(game: GameConnection, ggpo: SocketAddr) -> io::Result<Self> {
        if !ggpo.ip().is_loopback() || !ggpo.is_ipv4() || ggpo.port() == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "GGPO mapping must be IPv4 loopback with a nonzero port",
            ));
        }
        let socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await?;
        Ok(Self {
            game,
            socket,
            ggpo,
            stats: Arc::default(),
        })
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.socket.local_addr()
    }

    pub async fn run(self, mut stop: watch::Receiver<bool>) -> Result<(), Failure> {
        // Hold the full UDP ceiling to detect oversized packets, not a small
        // receive buffer that silently truncates before validation.
        let mut buffer = vec![0; MAX_UDP_PAYLOAD + 1];
        loop {
            if *stop.borrow() {
                return Ok(());
            }
            tokio::select! {
                changed = stop.changed() => {
                    if changed.is_err() || *stop.borrow() { return Ok(()); }
                }
                incoming = self.socket.recv_from(&mut buffer) => {
                    let size = match incoming {
                        Ok((size, source)) if source == self.ggpo => size,
                        Ok(_) => {
                            self.stats.rejected_packets.fetch_add(1, Ordering::Relaxed);
                            continue;
                        }
                        // UDP ICMP errors describe a previous local delivery,
                        // not failure of the independently authenticated QUIC
                        // connection. GGPO can retire its socket before the
                        // game-thread EndMatch acknowledgment reaches us.
                        Err(error) if local_unreachable(&error) => {
                            self.stats.local_drops.fetch_add(1, Ordering::Relaxed);
                            continue;
                        }
                        Err(_) => return Err(Failure::LocalSocket),
                    };
                    if size == 0 { self.stats.rejected_packets.fetch_add(1, Ordering::Relaxed); continue; }
                    if size > self.game.authorization.max_packet {
                        return Err(Failure::PacketLimit);
                    }
                    let maximum = self.game.connection.max_datagram_size().unwrap_or(0);
                    let packet = self.game.authorization.key.encode(&buffer[..size], maximum).map_err(|_|Failure::DatagramLimit)?;
                    if self.game.connection.datagram_send_buffer_space() < packet.len() {
                        // send_datagram evicts older queued datagrams when full.
                        // Record pressure without waiting for older traffic.
                        self.stats.congestion_events.fetch_add(1, Ordering::Relaxed);
                    }
                    self.game.connection.send_datagram(packet.into())
                        .map_err(|_| Failure::SendFailed)?;
                    self.stats.sent_packets.fetch_add(1, Ordering::Relaxed);
                    self.stats.sent_bytes.fetch_add(size as u64, Ordering::Relaxed);
                }
                incoming = self.game.connection.read_datagram() => {
                    let packet = incoming.map_err(|_| Failure::PeerClosed)?;
                    let payload = match self.game.authorization.key.decode(&packet) {
                        Ok(payload) if payload.len() <= self.game.authorization.max_packet => payload,
                        _ => { self.stats.rejected_packets.fetch_add(1, Ordering::Relaxed); continue; }
                    };
                    match self.socket.try_send_to(payload, self.ggpo) {
                        Ok(size) if size == payload.len() => {
                            self.stats.received_packets.fetch_add(1, Ordering::Relaxed);
                            self.stats.received_bytes.fetch_add(size as u64, Ordering::Relaxed);
                        }
                        Err(error) if error.kind() != io::ErrorKind::WouldBlock && !local_unreachable(&error) => return Err(Failure::LocalSocket),
                        _ => { self.stats.local_drops.fetch_add(1, Ordering::Relaxed); }
                    }
                }
            }
        }
    }
}

impl Drop for Bridge {
    fn drop(&mut self) {
        self.game.close();
    }
}

fn local_unreachable(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::ConnectionReset | io::ErrorKind::ConnectionRefused
    )
}
