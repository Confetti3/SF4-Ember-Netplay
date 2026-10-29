//! The latency endpoint: one per room incarnation, apart from the primary
//! endpoint, with its own ALPN and small QUIC limits (MEMBER_LATENCY.md 3.5).
use std::{io, sync::Arc, time::Duration};

use iroh::{
    Endpoint,
    endpoint::{
        Builder, ConnectOptions, IdleTimeout, PortmapperConfig, QuicTransportConfig, VarInt,
    },
};
use tokio::time::timeout;

/// The latency endpoint's only ALPN.
pub(crate) const LATENCY_ALPN: &[u8] = b"sf4e/latency/1";
/// How long the join and host tasks give the latency endpoint to bind.
pub(crate) const LATENCY_BIND_TIMEOUT: Duration = Duration::from_secs(3);
/// How long a dropped endpoint is given to close.
const CLOSE_TIMEOUT: Duration = Duration::from_secs(2);

/// How latency endpoints are built. Production uses the N0 preset; tests
/// give a local relay and address lookup instead, or none at all.
#[derive(Clone)]
pub(crate) struct LatencyNet(
    Option<Arc<dyn Fn(crate::transport::Transport) -> Builder + Send + Sync>>,
);

impl Default for LatencyNet {
    fn default() -> Self {
        Self(Some(Arc::new(crate::transport::endpoint_builder)))
    }
}

impl LatencyNet {
    #[cfg(test)]
    pub(crate) fn new(builder: impl Fn() -> Builder + Send + Sync + 'static) -> Self {
        Self(Some(Arc::new(move |_| builder())))
    }

    /// Tests only: binds nothing, so a room has no latency route.
    #[cfg(test)]
    pub(crate) fn off() -> Self {
        Self(None)
    }

    fn builder(&self, policy: crate::transport::Transport) -> Option<Builder> {
        self.0.as_ref().map(|builder| builder(policy))
    }
}

/// The transport limits of a latency connection. `remote_bidi` is how many
/// bidirectional streams the peer may open: one for the accepting side, none
/// for the dialling side. Neither side takes unidirectional streams or
/// datagrams.
pub(crate) fn latency_transport(remote_bidi: u32) -> QuicTransportConfig {
    QuicTransportConfig::builder()
        .max_concurrent_bidi_streams(VarInt::from_u32(remote_bidi))
        .max_concurrent_uni_streams(VarInt::from_u32(0))
        .stream_receive_window(VarInt::from_u32(1024))
        .receive_window(VarInt::from_u32(4096))
        .send_window(4096)
        .datagram_receive_buffer_size(None)
        .max_idle_timeout(Some(
            IdleTimeout::try_from(Duration::from_secs(20)).expect("idle timeout fits"),
        ))
        .build()
}

/// The options a dial uses: the dialling side's limits.
pub(crate) fn dial_options() -> ConnectOptions {
    ConnectOptions::new().with_transport_config(latency_transport(0))
}

/// Binds a latency endpoint on a random port with port mapping off; a
/// relay-only one has no IP transport at all.
pub(crate) async fn bind_latency(
    net: &LatencyNet,
    policy: crate::transport::Transport,
) -> io::Result<Endpoint> {
    let builder = net
        .builder(policy)
        .ok_or_else(|| io::Error::other("latency_off"))?
        .alpns(vec![LATENCY_ALPN.to_vec()])
        .transport_config(latency_transport(1))
        .portmapper_config(PortmapperConfig::Disabled);
    let builder = if policy == crate::transport::Transport::RelayOnly {
        builder.clear_ip_transports()
    } else {
        builder
    };
    builder
        .bind()
        .await
        .map_err(|_| io::Error::other("latency_bind"))
}

/// Owns a room incarnation's latency endpoint and closes it when dropped.
pub(crate) struct LatencyEndpoint(Option<Endpoint>);

impl LatencyEndpoint {
    pub(crate) fn new(endpoint: Endpoint) -> Self {
        Self(Some(endpoint))
    }

    pub(crate) fn endpoint(&self) -> &Endpoint {
        self.0.as_ref().expect("held until dropped")
    }
}

impl Drop for LatencyEndpoint {
    fn drop(&mut self) {
        let Some(endpoint) = self.0.take() else {
            return;
        };
        if let Ok(runtime) = tokio::runtime::Handle::try_current() {
            runtime.spawn(async move {
                let _ = timeout(CLOSE_TIMEOUT, endpoint.close()).await;
            });
        }
    }
}

/// The join and host tasks' latency endpoint, bound within
/// `LATENCY_BIND_TIMEOUT`. It does not wait for its relay: the region hint is
/// sent only if the relay is known by then. None when it could not bind; the
/// room goes on without a latency route.
pub(crate) async fn bind_for_room(
    net: LatencyNet,
    policy: crate::transport::Transport,
) -> Option<LatencyEndpoint> {
    let endpoint = timeout(LATENCY_BIND_TIMEOUT, bind_latency(&net, policy))
        .await
        .ok()?
        .ok()?;
    Some(LatencyEndpoint::new(endpoint))
}
