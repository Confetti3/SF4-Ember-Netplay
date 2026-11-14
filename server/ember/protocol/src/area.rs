//! Where a player plays from, coarsely: the player's own claim, which their
//! card's region and the public rooms they create carry. It is not how they
//! connect: a relay code (`relay::REGIONS`) is a routing fact and never names
//! a place.
//!
//! The game holds the same twelve areas in `src/common/Area.hxx` (with their
//! names, their neighbours and the default from Windows' time zone) and the
//! rooms page in `server/ember-short/deploy/site/assets/rooms.js`; the tests
//! here read both and check that the three lists agree.
use serde::{Deserialize, Serialize};

use crate::wire_enum::wire_enum;

wire_enum! {
    serde
    /// An area by its wire code: a to z only, so a reader that checks the
    /// shape of a relay code takes it too.
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum Area {
        NaWest = "naw",
        NaCentral = "nac",
        NaEast = "nae",
        CentralAmerica = "cam",
        SouthAmerica = "sam",
        EuropeWest = "euw",
        EuropeEast = "eue",
        MiddleEastAfrica = "mea",
        SouthAsia = "sas",
        EastAsia = "eas",
        SouthEastAsia = "sea",
        Oceania = "oce",
    }
}

/// Whether `code` is an area's wire code.
pub fn is_area(code: &str) -> bool {
    Area::parse(code).is_some()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A file of the repository this crate sits in, or None when the crate is
    /// built outside it.
    fn repository_file(path: &str) -> Option<String> {
        std::fs::read_to_string(format!("{}/../../../{path}", env!("CARGO_MANIFEST_DIR"))).ok()
    }

    /// The codes of the game's table, in its order: each row is
    /// `{Area::Name, "code", "area.code"}`.
    fn game_codes(header: &str) -> Vec<String> {
        header
            .match_indices("\"area.")
            .map(|(at, found)| {
                let rest = &header[at + found.len()..];
                rest[..rest.find('"').unwrap()].to_owned()
            })
            .collect()
    }

    /// The English name of `key` in a gettext catalog.
    fn catalog_name(catalog: &str, key: &str) -> Option<String> {
        let catalog = catalog.replace("\r\n", "\n");
        let entry = format!("msgid \"{key}\"\nmsgstr \"");
        let at = catalog.find(&entry)?;
        let rest = &catalog[at + entry.len()..];
        Some(rest[..rest.find('"')?].to_owned())
    }

    #[test]
    fn every_area_has_a_plain_code_and_none_repeats() {
        let mut seen = std::collections::BTreeSet::new();
        for area in Area::ALL {
            let code = area.as_str();
            assert!(seen.insert(code), "{code} repeats");
            assert!(
                (1..=8).contains(&code.len()) && code.bytes().all(|b| b.is_ascii_lowercase()),
                "{code}"
            );
            assert!(!crate::relay::is_region(code), "{code} is a relay's code");
            assert!(is_area(code));
        }
        for code in ["", "use1", "euc1", "other", "NAW", "naw ", "na-w"] {
            assert!(!is_area(code), "{code}");
        }
    }

    #[test]
    fn the_games_table_lists_the_same_areas_in_the_same_order_with_their_names() {
        let Some(header) = repository_file("src/common/Area.hxx") else {
            return;
        };
        assert_eq!(game_codes(&header), Area::NAMES.map(str::to_owned));
        let catalog = repository_file("locales/en.po").expect("the English catalog");
        for code in Area::NAMES {
            let name = catalog_name(&catalog, &format!("area.{code}"));
            assert!(
                name.is_some_and(|name| !name.is_empty()),
                "{code} has no name"
            );
        }
    }
}
