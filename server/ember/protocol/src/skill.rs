//! A player's public room class, as the bridge signs it into their room
//! ticket (`docs/design/SKILL_BADGE.md`). The rating behind it never leaves
//! the bridge.
use serde::{Deserialize, Serialize};

use crate::{Error, Result};

/// The highest band of the skill badge. Bands are USF4's own online classes,
/// 1 (D) to 10 (Grand Master): D, D+, C, C+, B, B+, A, A+, Master, Grand Master.
pub const SKILL_BANDS: u8 = 10;

/// A player's class as a room shows it beside their name.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SkillBadge {
    /// 1 to `SKILL_BANDS`, or 0 before the player's first class.
    pub band: u8,
    /// Whether the rating is sure enough to stand behind the band: false
    /// while a new player's rating forms and again after a long break.
    pub settled: bool,
}

impl SkillBadge {
    pub fn check(&self) -> Result<()> {
        let valid = self.band <= SKILL_BANDS && (self.band != 0 || !self.settled);
        if valid {
            Ok(())
        } else {
            Err(Error::InvalidField("badge"))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_badge_is_a_class_or_none_yet() {
        let badge = |band, settled| SkillBadge { band, settled };
        for valid in [badge(0, false), badge(1, true), badge(SKILL_BANDS, false)] {
            assert!(valid.check().is_ok(), "{valid:?}");
        }
        // No class yet, so nothing to be settled about; and no class past the last.
        for invalid in [badge(0, true), badge(SKILL_BANDS + 1, true)] {
            assert_eq!(
                invalid.check(),
                Err(Error::InvalidField("badge")),
                "{invalid:?}"
            );
        }
    }
}
