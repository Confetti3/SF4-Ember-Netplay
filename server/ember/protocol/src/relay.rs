//! The relay regions a player's network is placed in: the short codes of the
//! Iroh relays the helper pins, which is what the native side shows and what a
//! room's creator reports as its region. A relay outside these has no code.

/// The known region codes, in the order of the helper's pinned relays.
pub const REGIONS: [&str; 4] = ["use1", "usw1", "euc1", "aps1"];

/// Whether `code` is one of the known region codes.
pub fn is_region(code: &str) -> bool {
    REGIONS.contains(&code)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_known_codes_are_regions() {
        for code in REGIONS {
            assert!(is_region(code), "{code}");
        }
        for code in ["", "other", "USE1", "use", "use1 ", "euc2"] {
            assert!(!is_region(code), "{code}");
        }
    }
}
