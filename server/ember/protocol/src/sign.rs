//! Frozen signature and encryption domains (spec 9.3).
use serde::Serialize;

use crate::{
    Error, PublicKey, Result, SigningIdentity,
    json::{self, Value},
};

/// Additional-data prefix of the local encrypted key store.
pub const KEY_LOCAL_DOMAIN: &[u8] = b"EMBER:KEY-LOCAL:1\n";
/// Additional-data prefix of an exported backup.
pub const KEY_BACKUP_DOMAIN: &[u8] = b"EMBER:KEY-BACKUP:1\n";

/// What a signature is for. There is deliberately no variant for arbitrary
/// bytes. Players sign challenges and game reports; a bridge signs room
/// bindings, room tickets and game permits with its own key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Domain {
    Challenge,
    GameReport,
    Binding,
    Permit,
    RoomTicket,
}

impl Domain {
    pub const fn prefix(self) -> &'static [u8] {
        match self {
            Self::Challenge => b"EMBER:CHALLENGE:1\n",
            Self::GameReport => b"EMBER:GAME-REPORT:1\n",
            Self::Binding => b"EMBER:BINDING:1\n",
            Self::Permit => b"EMBER:PERMIT:1\n",
            Self::RoomTicket => b"EMBER:ROOM-TICKET:1\n",
        }
    }

    /// `prefix || JCS(value)`.
    pub fn signing_bytes(self, value: &Value) -> Vec<u8> {
        let mut bytes = self.prefix().to_vec();
        bytes.extend_from_slice(&value.canonical());
        bytes
    }
}

/// An object a bridge signs with its own key under one domain: a room
/// binding, a game permit or a room ticket. It is checked, then signed over
/// its canonical bytes, and verified the same way in the other direction.
pub(crate) trait BridgeSigned: Serialize + Clone {
    const DOMAIN: Domain;

    fn check(&self) -> Result<()>;
}

/// Checks `value`, then signs it: the value, the key ID and the signature,
/// for the signed wrapper to hold.
pub(crate) fn bridge_sign<T: BridgeSigned>(
    value: &T,
    key: &SigningIdentity,
    kid: &str,
) -> Result<(T, String, String)> {
    value.check()?;
    let signature = key.sign(T::DOMAIN, &json::to_value(value)?);
    Ok((value.clone(), kid.to_owned(), signature))
}

/// The key ID must be the trusted one, then the value's own consistency must
/// hold, then the signature must be good, in that order.
pub(crate) fn bridge_verify<'a, T: BridgeSigned>(
    value: &'a T,
    kid: &str,
    signature: &str,
    key: &PublicKey,
    expected_kid: &str,
) -> Result<&'a T> {
    if kid != expected_kid {
        return Err(Error::Mismatch("kid"));
    }
    value.check()?;
    key.verify(T::DOMAIN, &json::to_value(value)?, signature)?;
    Ok(value)
}
