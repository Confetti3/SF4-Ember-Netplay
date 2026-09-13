//! Network process components. No game state or lobby rules belong in this crate.
pub mod bridge;
pub mod control;
pub mod coordination;
pub mod coordination_iroh;
pub mod invite;
#[cfg(windows)]
pub mod ipc;
pub mod recovery;
pub mod service;
pub mod transport;
pub mod wire;
