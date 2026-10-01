//! Frozen signature and encryption domains (spec 9.3).
use crate::json::Value;

/// Additional-data prefix of the local encrypted key store.
pub const KEY_LOCAL_DOMAIN: &[u8] = b"EMBER:KEY-LOCAL:1\n";
/// Additional-data prefix of an exported backup.
pub const KEY_BACKUP_DOMAIN: &[u8] = b"EMBER:KEY-BACKUP:1\n";

/// What an identity signature is for. There is deliberately no variant for
/// arbitrary bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Domain {
    Challenge,
    GameReport,
}

impl Domain {
    pub const fn prefix(self) -> &'static [u8] {
        match self {
            Self::Challenge => b"EMBER:CHALLENGE:1\n",
            Self::GameReport => b"EMBER:GAME-REPORT:1\n",
        }
    }

    /// `prefix || JCS(value)`.
    pub fn signing_bytes(self, value: &Value) -> Vec<u8> {
        let mut bytes = self.prefix().to_vec();
        bytes.extend_from_slice(&value.canonical());
        bytes
    }
}
