//! The native checkpoint's transfer over the helper pipe, in bounded
//! chunks bound to its SHA-256.
use std::{io, sync::Arc};

use sha2::{Digest, Sha256};

use super::{CHECKPOINT_CHUNK_BYTES, MAX_CHECKPOINT_BYTES};
use crate::coordination::Committed;

#[derive(Clone, Debug)]
pub struct CheckpointTransfer {
    pub room: [u8; 16],
    pub transfer: u64,
    pub term: u64,
    pub base_revision: u64,
    pub revision: u64,
    /// Shared, not copied, with the commit it was read from.
    pub bytes: Arc<[u8]>,
    pub digest: [u8; 32],
    /// The same bytes as text when they are UTF-8: the one buffer a proposal,
    /// its log entries and the commit share.
    pub(super) text: Option<Arc<str>>,
}

impl CheckpointTransfer {
    pub fn new(
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        bytes: Vec<u8>,
    ) -> io::Result<Self> {
        Self::check_size(bytes.len())?;
        let digest = sha256(&bytes);
        Self::with_digest(room, transfer, term, base_revision, revision, bytes, digest)
    }

    /// `new` for bytes whose SHA-256 is already known to be `digest`.
    fn with_digest(
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        bytes: Vec<u8>,
        digest: [u8; 32],
    ) -> io::Result<Self> {
        Self::check_size(bytes.len())?;
        let (bytes, text) = match String::from_utf8(bytes) {
            Ok(text) => {
                let text = Arc::<str>::from(text);
                (Arc::<[u8]>::from(text.clone()), Some(text))
            }
            Err(error) => (Arc::<[u8]>::from(error.into_bytes()), None),
        };
        Ok(Self {
            room,
            transfer,
            term,
            base_revision,
            revision,
            bytes,
            digest,
            text,
        })
    }

    /// The transfer of `committed`, sharing its checkpoint and the SHA-256
    /// it already holds.
    pub fn from_committed(
        room: [u8; 16],
        transfer: u64,
        base_revision: u64,
        committed: &Committed,
    ) -> io::Result<Self> {
        Self::check_size(committed.checkpoint().len())?;
        let text = committed.checkpoint_shared();
        Ok(Self {
            room,
            transfer,
            term: committed.term(),
            base_revision,
            revision: committed.revision(),
            bytes: Arc::<[u8]>::from(text.clone()),
            digest: committed.digest(),
            text: Some(text),
        })
    }

    fn check_size(length: usize) -> io::Result<()> {
        if length == 0 || length > MAX_CHECKPOINT_BYTES {
            return Err(io::Error::other("checkpoint size out of bounds"));
        }
        Ok(())
    }
    pub fn digest_hex(&self) -> String {
        hex_digest(&self.digest)
    }
    /// The chunk of the checkpoint that starts at `offset`.
    pub fn chunk(&self, offset: usize) -> io::Result<&[u8]> {
        let end = offset
            .checked_add(CHECKPOINT_CHUNK_BYTES)
            .unwrap_or(self.bytes.len())
            .min(self.bytes.len());
        if offset >= end || offset > self.bytes.len() {
            return Err(io::Error::other("invalid checkpoint chunk offset"));
        }
        Ok(&self.bytes[offset..end])
    }
}

#[derive(Debug)]
pub struct IncomingTransfer {
    pub room: [u8; 16],
    pub transfer: u64,
    pub term: u64,
    pub base_revision: u64,
    pub revision: u64,
    pub length: usize,
    pub digest: [u8; 32],
    pub bytes: Vec<u8>,
    pub started: tokio::time::Instant,
}

impl IncomingTransfer {
    pub(crate) fn validate_begin(
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: usize,
        digest: &str,
    ) -> io::Result<[u8; 32]> {
        if room == [0; 16]
            || transfer == 0
            || term == 0
            || revision != base_revision.saturating_add(1)
            || !(1..=MAX_CHECKPOINT_BYTES).contains(&length)
        {
            return Err(io::Error::other("invalid checkpoint transfer"));
        }
        parse_digest(digest)
    }

    pub fn begin(
        room: [u8; 16],
        transfer: u64,
        term: u64,
        base_revision: u64,
        revision: u64,
        length: usize,
        digest: &str,
    ) -> io::Result<Self> {
        let digest = Self::validate_begin(
            room,
            transfer,
            term,
            base_revision,
            revision,
            length,
            digest,
        )?;
        Ok(Self {
            room,
            transfer,
            term,
            base_revision,
            revision,
            length,
            digest,
            bytes: Vec::with_capacity(length),
            started: tokio::time::Instant::now(),
        })
    }
    /// Appends the chunk `decoded` at `offset`, or checks an exact retry of
    /// bytes already held, and returns the chunk's end.
    pub fn push(&mut self, offset: usize, decoded: &[u8]) -> io::Result<usize> {
        let end = offset
            .checked_add(decoded.len())
            .ok_or_else(|| io::Error::other("checkpoint chunk exceeds bounds"))?;
        if decoded.is_empty() || decoded.len() > CHECKPOINT_CHUNK_BYTES || end > self.length {
            return Err(io::Error::other("checkpoint chunk exceeds bounds"));
        }
        if offset == self.bytes.len() {
            self.bytes.extend_from_slice(decoded);
        } else if offset > self.bytes.len()
            || end > self.bytes.len()
            || self.bytes[offset..end] != *decoded
        {
            return Err(io::Error::other("checkpoint chunks must be contiguous"));
        }
        // Exact retries acknowledge only the replayed chunk end. Returning the
        // retained body's full offset would violate the sender's four-credit
        // in-flight fence when the checkpoint spans more than one window.
        Ok(end)
    }
    pub fn finish(self) -> io::Result<CheckpointTransfer> {
        if self.bytes.len() != self.length || sha256(&self.bytes) != self.digest {
            return Err(io::Error::other("checkpoint digest mismatch"));
        }
        // Hashed just above.
        CheckpointTransfer::with_digest(
            self.room,
            self.transfer,
            self.term,
            self.base_revision,
            self.revision,
            self.bytes,
            self.digest,
        )
    }
}

pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    hasher.finalize().into()
}

pub fn hex_digest(bytes: &[u8; 32]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn parse_digest(value: &str) -> io::Result<[u8; 32]> {
    if value.len() != 64 || !value.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(io::Error::other("invalid SHA-256 digest"));
    }
    let mut digest = [0; 32];
    let (pairs, remainder) = value.as_bytes().as_chunks::<2>();
    debug_assert!(remainder.is_empty());
    for (index, pair) in pairs.iter().enumerate() {
        digest[index] = u8::from_str_radix(std::str::from_utf8(pair).unwrap_or(""), 16)
            .map_err(|_| io::Error::other("invalid SHA-256 digest"))?;
    }
    Ok(digest)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn transfer_is_contiguous_and_sha256_bound() {
        let bytes = vec![9; CHECKPOINT_CHUNK_BYTES + 3];
        let source = CheckpointTransfer::new([4; 16], 2, 7, 8, 9, bytes).unwrap();
        let mut incoming = IncomingTransfer::begin(
            source.room,
            source.transfer,
            source.term,
            source.base_revision,
            source.revision,
            source.bytes.len(),
            &source.digest_hex(),
        )
        .unwrap();
        let first = source.chunk(0).unwrap();
        assert_eq!(incoming.push(0, first).unwrap(), CHECKPOINT_CHUNK_BYTES);
        assert_eq!(
            incoming.push(0, first).unwrap(),
            CHECKPOINT_CHUNK_BYTES,
            "an exact retained chunk returns only its own end offset"
        );
        assert!(
            incoming.push(0, &[8; CHECKPOINT_CHUNK_BYTES]).is_err(),
            "a changed retry must not receive credit for retained bytes"
        );
        assert_eq!(
            incoming
                .push(
                    CHECKPOINT_CHUNK_BYTES,
                    source.chunk(CHECKPOINT_CHUNK_BYTES).unwrap(),
                )
                .unwrap(),
            source.bytes.len()
        );
        assert_eq!(incoming.finish().unwrap().bytes, source.bytes);
    }
}
