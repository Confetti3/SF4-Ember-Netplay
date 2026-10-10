//! What the helper tells the native side about its own network: the home
//! relay's region and whether it is connected, and how UDP behaves here.
//! Nothing that identifies a network leaves this module. The native side
//! gets a region code, never a relay URL, and never an address or port.
use super::*;
use iroh::{Watcher as _, unstable_net_report::NetReport};

/// What a net report says, as plain facts. `Report` is non-exhaustive, so
/// the derivation below works on these.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct NetFacts {
    /// A UDP round trip to a relay completed, over IPv4 or IPv6.
    pub udp: bool,
    /// The public address differed between relays: `None` until two relays
    /// have answered.
    pub mapping_varies: Option<bool>,
    pub captive_portal: bool,
}

impl NetFacts {
    fn of(report: &NetReport) -> Self {
        Self {
            udp: report.has_udp(),
            mapping_varies: report.mapping_varies_by_dest(),
            captive_portal: report.captive_portal == Some(true),
        }
    }
}

/// The wire form of the summary; see `Event::NetworkReport`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct NetworkSummary {
    /// A region code from `invite::relay_region`, or "" without a home relay.
    pub relay: &'static str,
    pub relay_connected: bool,
    pub udp: bool,
    /// "open", "strict", "no_udp" or "checking".
    pub nat: &'static str,
    pub captive_portal: bool,
}

/// How this network treats the direct path to a peer. "strict" is a public
/// address that differs by destination (symmetric NAT), where hole punching
/// usually fails. When only one relay has answered the difference cannot be
/// seen yet; the report is finished, so that reads as "open" rather than
/// leaving the native side checking for good.
pub(super) fn nat_class(facts: NetFacts) -> &'static str {
    if !facts.udp {
        "no_udp"
    } else if facts.mapping_varies == Some(true) {
        "strict"
    } else {
        "open"
    }
}

/// The first net report of an endpoint arrives a few seconds after it binds.
pub(super) fn summarize(relay: Option<(&'static str, bool)>, facts: Option<NetFacts>) -> NetworkSummary {
    let (relay, relay_connected) = relay.unwrap_or(("", false));
    match facts {
        None => NetworkSummary {
            relay,
            relay_connected,
            udp: false,
            nat: "checking",
            captive_portal: false,
        },
        Some(facts) => NetworkSummary {
            relay,
            relay_connected,
            udp: facts.udp,
            nat: nat_class(facts),
            captive_portal: facts.captive_portal,
        },
    }
}

/// The region and connection of the home relay. A connected relay wins when
/// several are listed.
fn home_relay(endpoint: &Endpoint) -> Option<(&'static str, bool)> {
    let statuses = endpoint.home_relay_status().get();
    statuses
        .iter()
        .find(|status| status.is_connected())
        .or(statuses.first())
        .map(|status| {
            (
                crate::invite::relay_region(status.url()),
                status.is_connected(),
            )
        })
}

pub(super) fn home_relay_connected(endpoint: &Endpoint) -> bool {
    home_relay(endpoint).is_some_and(|(_, connected)| connected)
}

/// The home relay's region when it is a known one, which is what a public
/// room's creator gives the bridge as the room's region.
pub(super) fn known_home_region(endpoint: &Endpoint) -> Option<&'static str> {
    home_relay(endpoint)
        .map(|(region, _)| region)
        .filter(|region| ember_protocol::relay::is_region(region))
}

pub(super) fn network_summary(endpoint: &Endpoint) -> NetworkSummary {
    let facts = endpoint.net_report().get().as_ref().map(NetFacts::of);
    summarize(home_relay(endpoint), facts)
}

impl From<NetworkSummary> for Event {
    fn from(summary: NetworkSummary) -> Self {
        Event::NetworkReport {
            relay: summary.relay.into(),
            relay_connected: summary.relay_connected,
            udp: summary.udp,
            nat: summary.nat.into(),
            captive_portal: summary.captive_portal,
        }
    }
}

/// Why a host attempt failed, from the error its task ended with. Only a home
/// relay that never came online has a reason the player can act on.
pub(super) fn host_failure_reason(error: &io::Error) -> Option<&'static str> {
    (error.to_string() == "relay_unavailable").then_some("relay_unreachable")
}

/// Why the first control connection of a join failed, from the error it ended
/// with. A public host's refusal (`PublicRefused`) is `refused`; a handshake
/// that ran out of time after the connection opened is `timeout`; no connection
/// at all is as `join_failure_reason` says. A connection the host closed
/// without an answer has no reason of its own. A private join (`public` false)
/// is classified by `join_failure_reason` alone, as it always was.
pub(super) fn join_error_reason(
    error: &io::Error,
    relay_connected: bool,
    public: bool,
) -> Option<&'static str> {
    if public && transport::is_admission_refused(error) {
        Some("refused")
    } else if public && transport::is_handshake_timeout(error) {
        Some("timeout")
    } else {
        join_failure_reason(transport::is_host_unreachable(error), relay_connected)
    }
}

/// Why the first control connection of a join failed. `dial_failed` is true
/// when no connection to the host ever opened. Without a connected home relay
/// that is on this side; with one it is the host that could not be reached. A
/// connection that opened and was then refused has no reason of its own.
pub(super) fn join_failure_reason(dial_failed: bool, relay_connected: bool) -> Option<&'static str> {
    match (dial_failed, relay_connected) {
        (false, _) => None,
        (true, false) => Some("relay_unreachable"),
        (true, true) => Some("host_unreachable"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const OPEN: NetFacts = NetFacts {
        udp: true,
        mapping_varies: Some(false),
        captive_portal: false,
    };

    #[test]
    fn nat_class_reads_udp_and_mapping() {
        assert_eq!(nat_class(OPEN), "open");
        assert_eq!(
            nat_class(NetFacts {
                mapping_varies: Some(true),
                ..OPEN
            }),
            "strict"
        );
        // Blocked UDP outranks whatever a stale mapping said.
        for mapping_varies in [None, Some(false), Some(true)] {
            assert_eq!(
                nat_class(NetFacts {
                    udp: false,
                    mapping_varies,
                    captive_portal: false,
                }),
                "no_udp"
            );
        }
        // One answering relay cannot show a varying mapping.
        assert_eq!(
            nat_class(NetFacts {
                mapping_varies: None,
                ..OPEN
            }),
            "open"
        );
    }

    #[test]
    fn a_summary_checks_until_a_report_exists_and_never_names_a_relay_url() {
        let checking = summarize(Some(("use1", true)), None);
        assert_eq!(checking.nat, "checking");
        assert!(!checking.udp && !checking.captive_portal);
        assert_eq!((checking.relay, checking.relay_connected), ("use1", true));
        let none = summarize(None, None);
        assert_eq!((none.relay, none.relay_connected), ("", false));
        let strict = summarize(
            Some(("euc1", false)),
            Some(NetFacts {
                mapping_varies: Some(true),
                captive_portal: true,
                ..OPEN
            }),
        );
        assert_eq!(strict.nat, "strict");
        assert!(strict.udp && strict.captive_portal && !strict.relay_connected);
        // The wire event carries only the codes and flags.
        let json = serde_json::to_value(Event::from(strict)).unwrap();
        assert_eq!(json["type"], "network_report");
        assert_eq!(json["relay"], "euc1");
        assert_eq!(json.as_object().unwrap().len(), 6);
    }

    #[test]
    fn failure_reasons_name_the_stage() {
        assert_eq!(join_failure_reason(true, false), Some("relay_unreachable"));
        assert_eq!(join_failure_reason(true, true), Some("host_unreachable"));
        assert_eq!(join_failure_reason(false, true), None);
        assert_eq!(join_failure_reason(false, false), None);
        // Each way a join's first control can end has its own reason, whether
        // or not the home relay is connected.
        for relay in [false, true] {
            assert_eq!(
                join_error_reason(&transport::admission_refused(), relay, true),
                Some("refused")
            );
            assert_eq!(
                join_error_reason(&io::Error::other("closed"), relay, true),
                None
            );
            // A private join has neither of the public reasons.
            assert_eq!(
                join_error_reason(&transport::admission_refused(), relay, false),
                None
            );
        }
        let timed_out = io::Error::new(io::ErrorKind::TimedOut, transport::HANDSHAKE_TIMED_OUT);
        assert_eq!(join_error_reason(&timed_out, true, true), Some("timeout"));
        assert_eq!(join_error_reason(&timed_out, true, false), None);
        let unreachable = io::Error::new(io::ErrorKind::ConnectionAborted, transport::HOST_UNREACHABLE);
        for public in [false, true] {
            assert_eq!(
                join_error_reason(&unreachable, true, public),
                Some("host_unreachable")
            );
            assert_eq!(
                join_error_reason(&unreachable, false, public),
                Some("relay_unreachable")
            );
        }
        assert_eq!(
            host_failure_reason(&failed("relay_unavailable")),
            Some("relay_unreachable")
        );
        assert_eq!(host_failure_reason(&failed("clock")), None);
    }
}
