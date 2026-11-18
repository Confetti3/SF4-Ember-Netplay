//! Logical matches (spec 13, 16.5, 17): creating them, reading them,
//! cancelling them and organizer adjudication. Fighters' claims, permits and
//! reports are in `play`. Each decided game is an attempt row with exactly one
//! score effect, so retries and duplicate deliveries cannot add a win (SEC-09).
mod adjudicate;
mod cancel;
mod create;
mod read;

pub use adjudicate::adjudicate;
pub(crate) use adjudicate::{AdjudicationCommand, adjudicate_for};
pub use cancel::cancel;
pub use create::create;
pub use read::{assignments, get, link, visible};
