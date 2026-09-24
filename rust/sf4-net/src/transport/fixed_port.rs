//! A well-known UDP port as one more direct candidate before the relay.
//!
//! The endpoint prefers the first free port in [`PORTS`] and silently falls
//! back to a random one. A fixed bind port turns on iroh's own hard-NAT
//! candidate (the reflexive IP with the bind port), keeps a UPnP mapping on a
//! stable internal port, and lets a player forward that port in the router;
//! a router that forwards it usually keeps the port outbound too, so iroh's
//! address discovery reports it. Hole punching probes every candidate in the
//! same round, and a validated direct path always outranks the relay, so no
//! path policy changes here.
//!
//! The helper does not add `public_ip:port` itself. For any other router it
//! would only be a guess, and every peer would spend hole-punch probes on it.
use std::net::{Ipv4Addr, SocketAddr};

use iroh::{
    Endpoint, TransportAddr,
    endpoint::{BindError, Builder, Connection},
};

/// 45760 is USF4's Steam app ID; every slot stays below the OS dynamic range.
pub const PORTS: [u16; 4] = [45760, 45761, 45762, 45763];

/// Binds on the first free port in `ports`, else on a random port. Any bind
/// error moves on: Windows reports a port reserved for Hyper-V or WSL as
/// access denied, not as in use.
pub async fn bind(builder: impl Fn() -> Builder, ports: &[u16]) -> Result<Endpoint, BindError> {
    for &port in ports {
        let Ok(fixed) = builder().bind_addr((Ipv4Addr::UNSPECIFIED, port)) else {
            continue;
        };
        if let Ok(endpoint) = fixed.bind().await {
            return Ok(endpoint);
        }
    }
    builder().bind().await
}

/// The fixed port this endpoint's IPv4 socket holds, or 0 on a random port.
pub fn bound(endpoint: &Endpoint) -> u16 {
    endpoint
        .bound_sockets()
        .into_iter()
        .filter(SocketAddr::is_ipv4)
        .map(|socket| socket.port())
        .find(|port| PORTS.contains(port))
        .unwrap_or(0)
}

/// Whether the connection's selected path reaches the peer on one of
/// [`PORTS`], which shows the fixed-port path is the one that connected.
pub fn selected_path_uses_fixed_port(connection: &Connection) -> bool {
    connection.paths().iter().any(|path| {
        path.is_selected()
            && matches!(path.remote_addr(), TransportAddr::Ip(addr) if PORTS.contains(&addr.port()))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use iroh::endpoint::presets;

    fn builder() -> Builder {
        Endpoint::builder(presets::Minimal)
    }

    #[tokio::test]
    async fn bind_skips_a_taken_port_and_falls_back_to_random() {
        let taken = std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).unwrap();
        let taken_port = taken.local_addr().unwrap().port();
        let free_port = std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0))
            .unwrap()
            .local_addr()
            .unwrap()
            .port();

        let next = bind(builder, &[taken_port, free_port]).await.unwrap();
        let ports: Vec<_> = next.bound_sockets().iter().map(SocketAddr::port).collect();
        assert!(ports.contains(&free_port));

        let random = bind(builder, &[taken_port, free_port]).await.unwrap();
        let ports: Vec<_> = random
            .bound_sockets()
            .iter()
            .map(SocketAddr::port)
            .collect();
        assert!(!ports.contains(&taken_port) && !ports.contains(&free_port));

        next.close().await;
        random.close().await;
    }
}
