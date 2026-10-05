//! Bounded, unordered gameplay-datagram measurements. Reliable streams only
//! negotiate the measurement and retire its reader before GGPO takes ownership.
use iroh::endpoint::{Connection, RecvStream, SendStream};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeSet, io, time::Duration};
use tokio::time::{Instant, timeout};

pub const INTERVAL: Duration = Duration::from_millis(50);
pub const MAX_SAMPLES: u64 = 600;
const BENCHMARK_PACKET_BYTES: usize = 1064 + crate::wire::GAME_HEADER;
pub fn duration(benchmark: bool) -> Duration {
    Duration::from_secs(if benchmark { 30 } else { 5 })
}

/// How long a check's reservation lets the peer it measures answer it: the
/// measurement, its setup and final reply, and the dial before it.
pub fn window(benchmark: bool) -> Duration {
    duration(benchmark) + Duration::from_secs(5) + crate::transport::HANDSHAKE_TIMEOUT
}

#[derive(Clone, Default, Debug, Serialize, Deserialize)]
pub struct Metrics {
    pub expected: u32,
    pub sent: u32,
    pub replies: u32,
    pub p50_rtt_us: u64,
    pub p99_rtt_us: u64,
    pub jitter_us: u64,
    pub send_pressure: u32,
    pub packet_bytes: u32,
    pub duration_ms: u64,
}

pub struct Measurement {
    pub samples: Vec<u64>,
    pub route_changed: bool,
    /// The selected route when the wait for late replies ended: the route this
    /// measurement observed on its probe connection.
    pub route: String,
    pub metrics: Metrics,
}

/// The transport a route string names: `direct` or `relay`.
fn route_kind(route: &str) -> &str {
    route.split(':').next().unwrap_or(route)
}

/// Whether the route stayed usable for one measurement. Latency differs by
/// transport, so a move between transports, such as relay to direct, mixes two
/// distributions into one recommendation. iroh also moves a healthy direct
/// connection between addresses of the same peer, for example from its IPv4
/// LAN address to an IPv6 one, while a check runs. Those paths measure alike
/// and the move discards nothing, so it does not count; routes carry no
/// address, so such a move is not even visible here. A moment with no
/// selected path, which a move passes through, says nothing either way.
struct RouteWatch {
    initial: String,
    last: String,
    changed: bool,
}

impl RouteWatch {
    fn new(initial: String) -> Self {
        Self {
            changed: initial == "unavailable",
            last: initial.clone(),
            initial,
        }
    }

    /// Records the route seen at one packet of the measurement.
    fn packet(&mut self, current: String) {
        if current == "unavailable" {
            return;
        }
        if route_kind(&current) != route_kind(&self.initial) {
            self.changed = true;
        }
        self.last = current;
    }

    /// The route to report when the measurement ends: the current one, or the
    /// last one seen if none is selected at this instant.
    fn reported(&self, current: String) -> String {
        if current == "unavailable" {
            self.last.clone()
        } else {
            current
        }
    }
}

fn failed() -> io::Error {
    io::Error::other("datagram measurement unavailable")
}

fn header(room: [u8; 16], request: u64, revision: u64, sequence: u64, timestamp: u64) -> [u8; 48] {
    let mut bytes = [0; 48];
    bytes[..16].copy_from_slice(&room);
    for (offset, value) in [
        (16, request),
        (24, revision),
        (32, sequence),
        (40, timestamp),
    ] {
        bytes[offset..offset + 8].copy_from_slice(&value.to_be_bytes());
    }
    bytes
}

fn number(bytes: &[u8], offset: usize) -> u64 {
    u64::from_be_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("validated probe header"),
    )
}

pub async fn measure(
    connection: &Connection,
    mut send: SendStream,
    mut recv: RecvStream,
    room: [u8; 16],
    request: u64,
    revision: u64,
    benchmark: bool,
) -> io::Result<Measurement> {
    let expected = if benchmark { MAX_SAMPLES } else { 100 };
    // Benchmark uses the admitted worst-case GGPO payload size, including a
    // full two-player spectator input queue. Neither mode retransmits probes.
    let packet_bytes = if benchmark {
        BENCHMARK_PACKET_BYTES
    } else {
        96
    };
    let mut setup = header(room, request, revision, expected, packet_bytes as u64).to_vec();
    setup.extend_from_slice(b"DGP2");
    send.write_all(&setup).await.map_err(|_| failed())?;
    let mut ack = [0; 4];
    timeout(Duration::from_secs(2), recv.read_exact(&mut ack))
        .await
        .map_err(|_| failed())?
        .map_err(|_| failed())?;
    if &ack != b"DGP2" || connection.max_datagram_size().unwrap_or(0) < packet_bytes {
        return Err(failed());
    }
    let started = Instant::now();
    let deadline = started + duration(benchmark) + Duration::from_secs(1);
    // Only the datagram exchange is measured, so only a route seen at one of its
    // packets can taint a sample. The connection is not consulted again after
    // the last packet except to report where it ended up.
    let mut watch = RouteWatch::new(route(connection));
    let mut ticker = tokio::time::interval(INTERVAL);
    ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut timestamps = vec![None; expected as usize];
    let mut received = BTreeSet::new();
    let mut samples = Vec::new();
    let mut attempted = 0;
    let mut metrics = Metrics {
        expected: expected as u32,
        packet_bytes: packet_bytes as u32,
        duration_ms: duration(benchmark).as_millis() as u64,
        ..Metrics::default()
    };
    loop {
        tokio::select! {
            _=tokio::time::sleep_until(deadline)=>break,
            _=ticker.tick(), if attempted<expected => {
                let sequence=attempted+1;
                attempted+=1;
                let timestamp=started.elapsed().as_micros() as u64;
                let mut packet=vec![0;packet_bytes];
                packet[..48].copy_from_slice(&header(room,request,revision,sequence,timestamp));
                if connection.datagram_send_buffer_space()<packet_bytes { metrics.send_pressure+=1; }
                connection.send_datagram(packet.into()).map_err(|_|failed())?;
                timestamps[(sequence-1) as usize]=Some(timestamp);
                metrics.sent+=1;
                watch.packet(route(connection));
            },
            incoming=connection.read_datagram()=> {
                let packet=incoming.map_err(|_|failed())?;
                if packet.len()!=packet_bytes || packet[..32]!=setup[..32] { continue; }
                let sequence=number(&packet,32);
                if sequence==0 || sequence>expected { continue; }
                let timestamp=number(&packet,40);
                if timestamps[(sequence-1) as usize]!=Some(timestamp) || !received.insert(sequence) { continue; }
                samples.push((started.elapsed().as_micros() as u64).saturating_sub(timestamp));
                watch.packet(route(connection));
                // Keep the same duration for loss-free and impaired runs.
            }
        }
    }
    // Read before the stream ends: the peer closes a finished probe connection
    // as soon as it has answered, and a closed connection has no selected path.
    let final_route = watch.reported(route(connection));
    // FIN is independent of datagram loss, so even 100% loss terminates cleanly.
    send.finish().map_err(|_| failed())?;
    let mut end = [0; 1];
    if !matches!(
        timeout(Duration::from_secs(2), recv.read(&mut end)).await,
        Ok(Ok(None))
    ) {
        return Err(failed());
    }
    metrics.replies = samples.len() as u32;
    metrics.jitter_us = if samples.len() > 1 {
        samples.windows(2).map(|v| v[0].abs_diff(v[1])).sum::<u64>() / (samples.len() - 1) as u64
    } else {
        0
    };
    let mut sorted = samples.clone();
    sorted.sort_unstable();
    if !sorted.is_empty() {
        metrics.p50_rtt_us = sorted[((sorted.len() - 1) * 50).div_ceil(100)];
        metrics.p99_rtt_us = sorted[((sorted.len() - 1) * 99).div_ceil(100)];
    }
    Ok(Measurement {
        samples,
        route_changed: watch.changed,
        route: final_route,
        metrics,
    })
}

pub async fn respond(
    connection: &Connection,
    mut send: SendStream,
    mut recv: RecvStream,
    room: [u8; 16],
    request: u64,
    revision: u64,
) -> io::Result<()> {
    let mut setup = [0; 52];
    timeout(Duration::from_secs(2), recv.read_exact(&mut setup))
        .await
        .map_err(|_| failed())?
        .map_err(|_| failed())?;
    let expected = number(&setup, 32);
    let packet_bytes = number(&setup, 40) as usize;
    if setup[..32] != header(room, request, revision, 0, 0)[..32]
        || &setup[48..] != b"DGP2"
        || !matches!(
            (expected, packet_bytes),
            (100, 96) | (600, BENCHMARK_PACKET_BYTES)
        )
    {
        return Err(failed());
    }
    send.write_all(b"DGP2").await.map_err(|_| failed())?;
    let deadline = Instant::now() + duration(expected == MAX_SAMPLES) + Duration::from_secs(4);
    let mut seen = BTreeSet::new();
    let mut end = [0; 1];
    loop {
        tokio::select! {
            _=tokio::time::sleep_until(deadline)=>return Err(failed()),
            result=recv.read(&mut end)=> {
                if !matches!(result,Ok(None)) { return Err(failed()); }
                send.finish().map_err(|_|failed())?;
                // The caller closes the connection next; let the peer receive the
                // end of the stream before that close can discard it.
                let _ = timeout(Duration::from_secs(2), send.stopped()).await;
                return Ok(());
            },
            packet=connection.read_datagram()=> {
                let packet=packet.map_err(|_|failed())?;
                if packet.len()!=packet_bytes || packet[..32]!=setup[..32] { continue; }
                let sequence=number(&packet,32);
                if sequence==0 || sequence>expected || !seen.insert(sequence) { continue; }
                connection.send_datagram(packet).map_err(|_|failed())?;
            }
        }
    }
}

/// The selected path as it may leave the helper (`invite::public_route`): a
/// class and a relay region, never an address.
pub fn route(connection: &Connection) -> String {
    let paths = connection.paths();
    let selected = paths.iter().find(|p| p.is_selected());
    let remote = selected.as_ref().map(|p| p.remote_addr());
    crate::invite::public_route(remote)
}

#[cfg(test)]
mod tests {
    use super::*;
    use iroh::{Endpoint, EndpointAddr, endpoint::presets};
    use std::net::Ipv4Addr;

    #[test]
    fn a_move_between_direct_addresses_of_the_peer_is_not_a_route_change() {
        // The failing run: a complete measurement that began on the IPv4 LAN
        // address and was moved to an IPv6 one by iroh after the 17th packet.
        // Both addresses are the one route `direct`.
        let mut watch = RouteWatch::new("direct".into());
        for _ in 0..17 {
            watch.packet("direct".into());
        }
        // A move passes through a moment with no selected path.
        watch.packet("unavailable".into());
        watch.packet("direct".into());
        assert!(!watch.changed);
        assert_eq!(watch.reported("direct".into()), "direct");
        assert_eq!(watch.reported("unavailable".into()), "direct");
    }

    #[test]
    fn a_move_between_transports_or_no_route_at_all_is_a_route_change() {
        let mut upgraded = RouteWatch::new("relay:use1".into());
        upgraded.packet("relay:use1".into());
        assert!(!upgraded.changed);
        upgraded.packet("direct".into());
        assert!(upgraded.changed);
        assert!(RouteWatch::new("unavailable".into()).changed);
    }

    async fn loss_case(drop_all: bool) {
        let a = Endpoint::builder(presets::Minimal)
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .unwrap()
            .alpns(vec![b"probe-test".to_vec()])
            .bind()
            .await
            .unwrap();
        let b = Endpoint::builder(presets::Minimal)
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .unwrap()
            .alpns(vec![b"probe-test".to_vec()])
            .bind()
            .await
            .unwrap();
        let (outgoing, incoming) = tokio::join!(
            a.connect(
                EndpointAddr::new(b.id()).with_ip_addr(b.bound_sockets()[0]),
                b"probe-test"
            ),
            async { b.accept().await.unwrap().await }
        );
        let outgoing = outgoing.unwrap();
        let incoming = incoming.unwrap();
        let responder = tokio::spawn(async move {
            let (mut send, mut recv) = incoming.accept_bi().await.unwrap();
            let mut setup = [0; 52];
            recv.read_exact(&mut setup).await.unwrap();
            send.write_all(b"DGP2").await.unwrap();
            let mut end = [0; 1];
            loop {
                tokio::select! {
                    packet=incoming.read_datagram()=> {
                        let packet=packet.unwrap();
                        // Exercise missing, duplicate, and forged replies through
                        // the actual datagram reader; a stream probe cannot pass.
                        if !drop_all && number(&packet,32)%2==1 {
                            incoming.send_datagram(packet.clone()).unwrap();
                            incoming.send_datagram(packet.clone()).unwrap();
                            let mut forged=packet.to_vec();forged[16]^=1;
                            incoming.send_datagram(forged.into()).unwrap();
                        }
                    },
                    end=recv.read(&mut end)=> { assert!(matches!(end,Ok(None)));send.finish().unwrap();break; }
                }
            }
            incoming
        });
        let (send, recv) = outgoing.open_bi().await.unwrap();
        let result = timeout(
            Duration::from_secs(9),
            measure(&outgoing, send, recv, [9; 16], 7, 3, false),
        )
        .await
        .unwrap()
        .unwrap();
        let _retained = responder.await.unwrap();
        assert_eq!(result.metrics.expected, 100);
        assert_eq!(result.metrics.sent, 100);
        assert_eq!(result.samples.len(), if drop_all { 0 } else { 50 });
        assert_eq!(result.metrics.replies, result.samples.len() as u32);
        assert!(!result.route_changed);
        assert!(outgoing.close_reason().is_none());
        a.close().await;
        b.close().await;
    }

    #[tokio::test]
    async fn datagram_measurement_reports_loss_without_retransmission_or_duplicate_credit() {
        tokio::join!(loss_case(false), loss_case(true));
    }
}
