//! Scenario tests of the helper's service actor, one file per family. The
//! fixtures several families use are in `support`.

use super::probes::run_probe;
use super::*;
use iroh::TransportAddr;
use iroh::endpoint::{PortmapperConfig, presets};
use tokio::net::UdpSocket;

mod admission;
mod checkpoint_export;
mod checkpoint_pipe;
mod departures;
mod dials;
mod game_links;
mod invitations;
mod join_deadline;
mod join_failures;
mod match_watch;
mod membership_retirement;
mod probe_checks;
mod public_admission;
mod public_rooms;
mod public_support;
mod reservations;
mod server_owned;
mod short_link_sharing;
mod support;
mod tick;
mod wire_limits;

// The latency tests share the endpoint and actor fixtures.
use support::*;
pub(super) use support::{endpoint, test_actor};
