//! Who each entry of a bracket is: a site's participant (or entrant) ID to
//! the accounts the bridge can find its player by. A site lists its sets
//! with participant IDs only, so the adapter reads its list of entries when
//! a set names an ID not known yet, or when what it knows is older than
//! the adapter allows.
use std::{collections::HashMap, sync::Mutex};

use crate::{
    Clock,
    site::{Account, Entry},
};

pub(crate) struct Roster {
    clock: Clock,
    /// How long a read of the entries holds. With `None` it holds until a
    /// set names an ID it lacks.
    max_age: Option<u64>,
    known: Mutex<Known>,
}

#[derive(Default)]
struct Known {
    accounts: HashMap<String, Vec<Account>>,
    read_at: Option<u64>,
}

impl Roster {
    pub fn new(clock: Clock, max_age: Option<u64>) -> Self {
        Self {
            clock,
            max_age,
            known: Mutex::default(),
        }
    }

    fn known(&self) -> std::sync::MutexGuard<'_, Known> {
        self.known
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Whether the entries must be read again for sets naming these IDs:
    /// one is not known, or the last read is too old.
    pub fn stale<'a>(&self, ids: impl IntoIterator<Item = &'a String>) -> bool {
        let known = self.known();
        let old = match (self.max_age, known.read_at) {
            (_, None) => true,
            (Some(max_age), Some(at)) => self.clock.now().saturating_sub(at) >= max_age,
            (None, Some(_)) => false,
        };
        ids.into_iter()
            .any(|id| old || !known.accounts.contains_key(id))
    }

    /// Replaces what is known with a fresh read of the entries. Each of
    /// `asked` the read still lacks is kept as an entry with no accounts, so
    /// an ID the list never shows costs one read, not one on every read of
    /// the sets.
    pub fn replace<'a>(
        &self,
        mut accounts: HashMap<String, Vec<Account>>,
        asked: impl IntoIterator<Item = &'a String>,
    ) {
        for id in asked {
            accounts.entry(id.clone()).or_default();
        }
        *self.known() = Known {
            accounts,
            read_at: Some(self.clock.now()),
        };
    }

    /// The two entries of a set naming these IDs, player 1 first.
    pub fn entries(&self, ids: Option<&[String; 2]>) -> Option<[Entry; 2]> {
        let known = self.known();
        ids.map(|ids| {
            ids.clone().map(|id| Entry {
                accounts: known.accounts.get(&id).cloned().unwrap_or_default(),
                participant_id: id,
            })
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn account(id: &str) -> Account {
        Account::Site {
            site: "startgg".into(),
            id: id.into(),
        }
    }

    #[test]
    fn unknown_ids_are_read_once_and_old_reads_again() {
        let clock = Clock::frozen(1_000);
        let roster = Roster::new(clock.clone(), Some(900));
        let ids = ["1".to_owned(), "2".to_owned()];
        assert!(roster.stale(&ids));
        // "2" is not on the list: it is known as having no accounts.
        roster.replace(HashMap::from([("1".to_owned(), vec![account("u1")])]), &ids);
        assert!(!roster.stale(&ids));
        let [one, two] = roster.entries(Some(&ids)).unwrap();
        assert_eq!(
            (one.participant_id.as_str(), one.accounts),
            ("1", vec![account("u1")])
        );
        assert_eq!((two.participant_id.as_str(), two.accounts), ("2", vec![]));
        assert!(roster.stale(&["3".to_owned()]));
        assert!(roster.entries(None).is_none());
        clock.advance(899);
        assert!(!roster.stale(&ids));
        clock.advance(1);
        assert!(roster.stale(&ids));
        // Nothing to ask about: nothing to read, however old.
        assert!(!roster.stale(&[]));
    }

    #[test]
    fn without_an_age_only_an_unknown_id_reads_again() {
        let clock = Clock::frozen(1_000);
        let roster = Roster::new(clock.clone(), None);
        let ids = ["1".to_owned()];
        roster.replace(HashMap::new(), &ids);
        clock.advance(1_000_000);
        assert!(!roster.stale(&ids));
        assert!(roster.stale(&["2".to_owned()]));
    }
}
