//! Wire rules for Ember identity and the tournament bridge (EMBER-TB-001).
//!
//! Everything here is pure and platform independent: identifier derivation,
//! strict JSON and RFC 8785 canonical bytes, signature domains, the signed
//! challenge and report objects, the generic match objects, tournament play
//! (claims, room bindings and game permits), public room tickets, CloudEvents and
//! Standard Webhooks. Key storage, HTTP and databases live in the crates that
//! use this one.
pub mod api;
pub mod challenge;
pub mod discord;
pub mod encoding;
mod error;
pub mod event;
pub mod id;
pub mod json;
pub mod lobby;
pub mod matches;
pub mod partner;
pub mod play;
pub mod relay;
pub mod report;
pub mod rooms;
pub mod sign;
pub mod tournament;
pub mod webhook;

pub use error::Error;
pub use id::{EmberId, PublicKey, SigningIdentity};

pub type Result<T> = std::result::Result<T, Error>;
