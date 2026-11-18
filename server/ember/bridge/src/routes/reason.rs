//! The one rule for the free-text reason a service or organizer gives when it
//! cancels, closes or adjudicates (matches, tournaments, lobbies).

use crate::error::{ApiFailure, Result};

const MAX_REASON: usize = 512;

/// A reason is 1 to 512 bytes, not only blank, and holds no control character
/// except a line break.
pub fn check(reason: &str) -> Result<()> {
    if reason.trim().is_empty()
        || reason.len() > MAX_REASON
        || reason.chars().any(|c| c.is_control() && c != '\n')
    {
        return Err(ApiFailure::invalid(
            "A reason of 1 to 512 bytes is required, with no control characters other than line breaks.",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_line_break_is_allowed() {
        assert!(check("No-show\nplayer left").is_ok());
    }

    #[test]
    fn blank_and_oversized_reasons_are_refused() {
        assert!(check("").is_err());
        assert!(check(" \n\t ").is_err());
        assert!(check(&"a".repeat(512)).is_ok());
        assert!(check(&"a".repeat(513)).is_err());
        // 171 three-byte characters are 513 bytes.
        assert!(check(&"\u{3042}".repeat(171)).is_err());
        assert!(check(&"\u{3042}".repeat(170)).is_ok());
    }

    #[test]
    fn other_control_characters_are_refused() {
        for bad in ["a\rb", "a\tb", "a\u{0}b", "a\u{85}b"] {
            assert!(check(bad).is_err(), "{bad:?}");
        }
    }
}
