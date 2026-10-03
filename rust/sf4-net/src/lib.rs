//! Network process components. No game state or lobby rules belong in this crate.
pub mod bridge;
pub mod control;
pub mod coordination;
pub mod coordination_iroh;
pub mod identity;
pub mod invite;
#[cfg(windows)]
pub mod ipc;
pub mod probe;
pub(crate) mod public_room;
pub mod recovery;
pub mod service;
pub mod short_invite;
pub mod stdio;
pub mod tournament;
pub mod transport;
pub mod wire;
