//! Logical matches (spec 13, 16.5, 17): creating them, reading them,
//! cancelling them and organizer adjudication. Fighters' claims, permits and
//! reports are in `play`. Each decided game is an attempt row with exactly one
//! score effect, so retries and duplicate deliveries cannot add a win (SEC-09).
mod adjudicate;
mod cancel;
mod create;
mod read;

pub use adjudicate::adjudicate;
pub use cancel::cancel;
pub use create::create;
pub use read::{assignments, get};

use crate::error::{ApiFailure, Result};

const MAX_REASON: usize = 512;

fn check_reason(reason: &str) -> Result<()> {
    if reason.trim().is_empty()
        || reason.len() > MAX_REASON
        || reason.chars().any(|c| c.is_control() && c != '\n')
    {
        return Err(ApiFailure::invalid(
            "A reason of 1 to 512 bytes is required.",
        ));
    }
    Ok(())
}
