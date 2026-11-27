//! Sealed tokens: a number only the bridge reads back, named for one purpose
//! and good for a time. A card token (`cards`) and the website's session
//! (`routes::website`) each name a Discord account this way; the website's
//! sign-in state names nothing but itself.
//!
//! A token is its purpose's prefix and the unpadded base64url of the bridge's
//! seal (`Keys::seal`, a fresh nonce each time) over 17 bytes: the purpose
//! byte, then the subject and the time it was sealed, both little-endian u64.
//! Two tokens of one subject cannot be matched by anyone else, and a token
//! opens only as what it was sealed for, so a card token anyone in a room is
//! shown is never a session.
use ember_protocol::{
    encoding::{b64u, decode_b64u},
    levels::{CARD_PREFIX, CARD_VALID_SECS},
    sign_in::SIGN_IN_SECS,
};

use crate::secrets::Keys;

const PLAIN_LEN: usize = 17;
/// The nonce, the plain bytes and the tag.
const SEALED_LEN: usize = 24 + PLAIN_LEN + 16;

/// What a token is for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sealed {
    /// A player's card, by their Discord account: a room shows it, and
    /// anyone may hold one.
    Card,
    /// A browser signed in on the website, by its Discord account: only that
    /// browser holds it.
    Session,
    /// A sign-in the website started, by a random number.
    SignIn,
}

/// What a token held.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Opened {
    pub subject: u64,
    /// When it was sealed, unix seconds.
    pub sealed_at: u64,
}

impl Opened {
    /// The subject as Discord writes a user ID.
    pub fn account(&self) -> String {
        self.subject.to_string()
    }
}

impl Sealed {
    const fn byte(self) -> u8 {
        match self {
            Self::Card => 0x01,
            Self::Session => 0x02,
            Self::SignIn => 0x03,
        }
    }

    /// What every token of this purpose starts with; the digit names its
    /// format.
    const fn prefix(self) -> &'static str {
        match self {
            Self::Card => CARD_PREFIX,
            Self::Session => "s1.",
            Self::SignIn => "w1.",
        }
    }

    /// How long after it was sealed a token opens.
    pub const fn valid_secs(self) -> u64 {
        match self {
            Self::Card => CARD_VALID_SECS,
            Self::Session => 30 * 24 * 60 * 60,
            Self::SignIn => SIGN_IN_SECS,
        }
    }

    /// A token of `subject`, sealed at `now`.
    pub fn seal(self, keys: &Keys, subject: u64, now: u64) -> String {
        let mut plain = [0; PLAIN_LEN];
        plain[0] = self.byte();
        plain[1..9].copy_from_slice(&subject.to_le_bytes());
        plain[9..].copy_from_slice(&now.to_le_bytes());
        format!("{}{}", self.prefix(), b64u(&keys.seal(&plain)))
    }

    /// A token of Discord account `account`, sealed at `now`. None for an
    /// account that is not a Discord user ID as Discord writes one.
    pub fn name(self, keys: &Keys, account: &str, now: u64) -> Option<String> {
        let id = account
            .parse::<u64>()
            .ok()
            .filter(|id| id.to_string() == account)?;
        Some(self.seal(keys, id, now))
    }

    /// What `token` held, if it opens under `keys` as one of this purpose
    /// sealed at most `valid_secs` before `now`. A clock that stepped back
    /// does not refuse it.
    pub fn open(self, keys: &Keys, token: &str, now: u64) -> Option<Opened> {
        let sealed: [u8; SEALED_LEN] =
            decode_b64u(token.strip_prefix(self.prefix())?, "token").ok()?;
        let plain: [u8; PLAIN_LEN] = keys.open(&sealed)?.as_slice().try_into().ok()?;
        let [purpose, rest @ ..] = plain;
        let (subject, sealed_at) = rest.split_at(8);
        let opened = Opened {
            subject: u64::from_le_bytes(subject.try_into().ok()?),
            sealed_at: u64::from_le_bytes(sealed_at.try_into().ok()?),
        };
        (purpose == self.byte() && now.saturating_sub(opened.sealed_at) <= self.valid_secs())
            .then_some(opened)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW: u64 = 1_795_000_000;
    const ACCOUNT: &str = "123456789012345678";
    const ALL: [Sealed; 3] = [Sealed::Card, Sealed::Session, Sealed::SignIn];

    #[test]
    fn a_token_opens_for_as_long_as_its_purpose_says() {
        let keys = Keys::generate();
        for purpose in ALL {
            let token = purpose.name(&keys, ACCOUNT, NOW).unwrap();
            let opened = purpose.open(&keys, &token, NOW).unwrap();
            assert_eq!(
                (opened.account().as_str(), opened.sealed_at),
                (ACCOUNT, NOW)
            );
            assert!(
                purpose
                    .open(&keys, &token, NOW + purpose.valid_secs())
                    .is_some()
            );
            assert!(
                purpose
                    .open(&keys, &token, NOW + purpose.valid_secs() + 1)
                    .is_none()
            );
            assert!(purpose.open(&keys, &token, NOW - 60).is_some());
            // Two of one subject, which nobody else can match.
            assert_ne!(token, purpose.name(&keys, ACCOUNT, NOW).unwrap());
        }
    }

    #[test]
    fn a_token_opens_only_as_what_it_was_sealed_for() {
        let keys = Keys::generate();
        for sealed_for in ALL {
            let token = sealed_for.seal(&keys, 7, NOW);
            let body = token.split_once('.').unwrap().1;
            for purpose in ALL {
                let opens = purpose == sealed_for;
                assert_eq!(purpose.open(&keys, &token, NOW).is_some(), opens);
                // Under the other purpose's own prefix, too.
                let renamed = format!("{}{body}", purpose.prefix());
                assert_eq!(purpose.open(&keys, &renamed, NOW).is_some(), opens);
            }
            assert!(sealed_for.open(&Keys::generate(), &token, NOW).is_none());
            assert!(sealed_for.open(&keys, body, NOW).is_none());
            assert!(sealed_for.open(&keys, sealed_for.prefix(), NOW).is_none());
        }
    }

    #[test]
    fn only_a_discord_user_id_is_named() {
        let keys = Keys::generate();
        for account in ["", "abc", "-1", "+1", "01", "18446744073709551616"] {
            assert!(
                Sealed::Card.name(&keys, account, NOW).is_none(),
                "{account:?}"
            );
        }
        let largest = u64::MAX.to_string();
        let token = Sealed::Session.name(&keys, &largest, NOW).unwrap();
        let opened = Sealed::Session.open(&keys, &token, NOW).unwrap();
        assert_eq!(opened.account(), largest);
    }
}
