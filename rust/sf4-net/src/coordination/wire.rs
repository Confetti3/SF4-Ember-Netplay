//! The coordination wire: the binary append and snapshot RPC bodies, the
//! compressed checkpoints they carry, and the snapshot encoding.

use super::*;

pub(super) fn invalid_body() -> io::Error {
    io::Error::other("invalid room coordination envelope")
}

pub(super) fn too_large() -> io::Error {
    io::Error::other("room coordination message too large")
}

/// Reads the fields of a binary RPC body in order, never past its end.
pub(super) struct BodyReader<'a>(&'a [u8]);

impl<'a> BodyReader<'a> {
    pub(super) fn take(&mut self, length: usize) -> io::Result<&'a [u8]> {
        if length > self.0.len() {
            return Err(invalid_body());
        }
        let (head, rest) = self.0.split_at(length);
        self.0 = rest;
        Ok(head)
    }

    pub(super) fn u8(&mut self) -> io::Result<u8> {
        Ok(self.take(1)?[0])
    }

    pub(super) fn u32(&mut self) -> io::Result<u32> {
        let bytes = self.take(size_of::<u32>())?;
        Ok(u32::from_le_bytes(
            bytes.try_into().map_err(|_| invalid_body())?,
        ))
    }

    pub(super) fn u64(&mut self) -> io::Result<u64> {
        let bytes = self.take(size_of::<u64>())?;
        Ok(u64::from_le_bytes(
            bytes.try_into().map_err(|_| invalid_body())?,
        ))
    }

    pub(super) fn rest(self) -> &'a [u8] {
        self.0
    }
}

/// The start of a binary RPC body: the coordination wire version, then the
/// JSON header's length (u32, little endian) and the header. `extra` is the
/// room to reserve for what follows it.
pub(super) fn body_with_header<T: Serialize>(header: &T, extra: usize) -> io::Result<Vec<u8>> {
    let header = serde_json::to_vec(header).map_err(io::Error::other)?;
    if header.len() > MAX_SNAPSHOT {
        return Err(too_large());
    }
    let mut body = Vec::with_capacity(1 + size_of::<u32>() + header.len() + extra);
    body.push(COORDINATION_WIRE_VERSION);
    body.extend_from_slice(&(header.len() as u32).to_le_bytes());
    body.extend_from_slice(&header);
    Ok(body)
}

/// The JSON header of a binary RPC body, and a reader of what follows it.
/// A body of another wire version, such as wire 1's JSON, is refused here.
pub(super) fn read_header<T: DeserializeOwned>(bytes: &[u8]) -> io::Result<(T, BodyReader<'_>)> {
    let mut reader = BodyReader(bytes);
    if reader.u8()? != COORDINATION_WIRE_VERSION {
        return Err(invalid_body());
    }
    let length = reader.u32()? as usize;
    let header = serde_json::from_slice(reader.take(length)?).map_err(|_| invalid_body())?;
    Ok((header, reader))
}

/// A checkpoint compressed for the wire.
pub(super) fn compress_checkpoint(text: &str) -> io::Result<Arc<[u8]>> {
    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
    encoder.write_all(text.as_bytes())?;
    let compressed = encoder.finish()?;
    if compressed.len() > MAX_SNAPSHOT {
        return Err(too_large());
    }
    Ok(compressed.into())
}

/// The checkpoint text `compressed` holds. It must inflate to exactly
/// `length` bytes of UTF-8, and its zlib stream must end, adler32 trailer
/// included, where the bytes do. zlib's checksum covers the inflated bytes
/// only when the stream end is required: a reader that stops at the end of
/// its input accepts a stream whose trailer is cut off. The output never
/// grows past `length` + 1 bytes.
pub(super) fn inflate_checkpoint(compressed: &[u8], length: u64) -> io::Result<Arc<str>> {
    if length > MAX_SNAPSHOT as u64 {
        return Err(too_large());
    }
    let digest = || io::Error::other("invalid room coordination digest");
    let mut raw = Vec::with_capacity(length as usize + 1);
    let mut inflater = Decompress::new(true);
    loop {
        let progress = (inflater.total_in(), inflater.total_out());
        let input = compressed
            .get(inflater.total_in() as usize..)
            .ok_or_else(digest)?;
        let status = inflater
            .decompress_vec(input, &mut raw, FlushDecompress::Finish)
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
        if status == Status::StreamEnd {
            break;
        }
        // Out of room (the stream is longer than declared), or out of input
        // before the stream ended (a cut stream or a missing trailer).
        if raw.len() as u64 > length || progress == (inflater.total_in(), inflater.total_out()) {
            return Err(digest());
        }
    }
    if raw.len() as u64 != length || inflater.total_in() as usize != compressed.len() {
        return Err(digest());
    }
    String::from_utf8(raw)
        .map(Arc::from)
        .map_err(|_| invalid_body())
}

/// How many compressed checkpoints a node keeps: as many as the encoded
/// appends it keeps.
pub(super) const COMPRESSED_CHECKPOINTS: usize = APPEND_BODIES;

/// The checkpoints this node compressed for the wire, each kept beside the
/// text it was compressed from. The leader compresses its proposal once, on
/// the proposing task (`Coordinator::propose`), and every append that carries
/// that entry takes the same bytes. A log entry and its clones share the
/// text, so the text is found by its pointer; holding it here keeps that
/// pointer from naming any other text while it is kept.
#[derive(Default)]
pub(super) struct CompressedCheckpoints {
    pub(super) kept: std::sync::Mutex<VecDeque<(Arc<str>, Arc<[u8]>)>>,
    #[cfg(test)]
    pub(super) compressions: std::sync::atomic::AtomicU64,
}

impl CompressedCheckpoints {
    pub(super) fn kept(&self) -> std::sync::MutexGuard<'_, VecDeque<(Arc<str>, Arc<[u8]>)>> {
        self.kept
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// The compressed bytes of `text`, compressed now, outside the lock,
    /// when they are not kept already.
    pub(super) fn compress(&self, text: &Arc<str>) -> io::Result<Arc<[u8]>> {
        let find = |kept: &VecDeque<(Arc<str>, Arc<[u8]>)>| {
            kept.iter()
                .find(|(source, _)| Arc::ptr_eq(source, text))
                .map(|(_, compressed)| compressed.clone())
        };
        if let Some(compressed) = find(&self.kept()) {
            return Ok(compressed);
        }
        #[cfg(test)]
        self.compressions
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let compressed = compress_checkpoint(text)?;
        let mut kept = self.kept();
        if find(&kept).is_none() {
            kept.push_back((text.clone(), compressed.clone()));
            if kept.len() > COMPRESSED_CHECKPOINTS {
                kept.pop_front();
            }
        }
        Ok(compressed)
    }
}

/// An append's RPC body. After the version and header (`body_with_header`),
/// whose JSON is the request with every checkpoint left empty, each normal
/// entry's checkpoint follows in entry order as
/// `text length: u64 | zlib length: u32 | zlib bytes`, little endian; an
/// empty checkpoint is two zero lengths. The checkpoint goes in compressed,
/// once, and is never escaped into the JSON.
pub(super) fn encode_append(
    request: &AppendEntriesRequest<RoomTypes>,
    checkpoints: &CompressedCheckpoints,
) -> io::Result<Vec<u8>> {
    // Cloning shares each checkpoint; the header's copies are then emptied.
    let mut header = request.clone();
    let mut texts = Vec::new();
    for entry in &mut header.entries {
        if let EntryPayload::Normal(proposal) = &mut entry.payload {
            let text = std::mem::replace(&mut proposal.checkpoint, Arc::from(""));
            let compressed = if text.is_empty() {
                None
            } else {
                Some(checkpoints.compress(&text)?)
            };
            texts.push((text.len() as u64, compressed));
        }
    }
    let extra = texts
        .iter()
        .map(|(_, compressed)| 12 + compressed.as_ref().map_or(0, |bytes| bytes.len()))
        .sum();
    let mut body = body_with_header(&header, extra)?;
    for (length, compressed) in &texts {
        let compressed = compressed.as_deref().unwrap_or_default();
        body.extend_from_slice(&length.to_le_bytes());
        body.extend_from_slice(&(compressed.len() as u32).to_le_bytes());
        body.extend_from_slice(compressed);
    }
    if body.len() > MAX_SNAPSHOT {
        return Err(too_large());
    }
    Ok(body)
}

/// Reads an `encode_append` body, inflating and checking each checkpoint:
/// `read_append`, then `inflate_append`. The RPC's dispatch runs the two
/// itself, before Raft and so outside the store lock, with its own checks
/// between them.
#[cfg(test)]
pub(super) fn decode_append(bytes: &[u8]) -> io::Result<AppendEntriesRequest<RoomTypes>> {
    let (request, checkpoints) = read_append(bytes)?;
    inflate_append(request, checkpoints)
}

/// The header of an `encode_append` body and each normal entry's declared
/// text length and zlib bytes, in entry order. Every check that needs no
/// inflating runs here, so a body that fails one is refused before any
/// checkpoint is inflated. The declared text lengths together stay within
/// `MAX_SNAPSHOT`, the bound wire 1 put on the whole inflated request.
pub(super) fn read_append(
    bytes: &[u8],
) -> io::Result<(AppendEntriesRequest<RoomTypes>, Vec<(u64, &[u8])>)> {
    let (request, mut reader) = read_header::<AppendEntriesRequest<RoomTypes>>(bytes)?;
    let mut declared = 0u64;
    let mut checkpoints = Vec::new();
    for entry in &request.entries {
        if let EntryPayload::Normal(proposal) = &entry.payload {
            let length = reader.u64()?;
            let compressed_length = reader.u32()? as usize;
            let compressed = reader.take(compressed_length)?;
            declared = declared.saturating_add(length);
            if !proposal.checkpoint.is_empty() || declared > MAX_SNAPSHOT as u64 {
                return Err(invalid_body());
            }
            if length == 0 && !compressed.is_empty() {
                return Err(invalid_body());
            }
            checkpoints.push((length, compressed));
        }
    }
    if !reader.rest().is_empty() {
        return Err(invalid_body());
    }
    Ok((request, checkpoints))
}

/// Puts each checkpoint `read_append` found back into its entry, inflated.
pub(super) fn inflate_append(
    mut request: AppendEntriesRequest<RoomTypes>,
    checkpoints: Vec<(u64, &[u8])>,
) -> io::Result<AppendEntriesRequest<RoomTypes>> {
    let mut checkpoints = checkpoints.into_iter();
    for entry in &mut request.entries {
        if let EntryPayload::Normal(proposal) = &mut entry.payload {
            let (length, compressed) = checkpoints.next().ok_or_else(invalid_body)?;
            if length != 0 {
                proposal.checkpoint = inflate_checkpoint(compressed, length)?;
            }
        }
    }
    Ok(request)
}

/// A snapshot RPC's header: everything but its data.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct InstallSnapshotHeader {
    pub(super) vote: Vote<u64>,
    pub(super) meta: SnapshotMeta<u64, BasicNode>,
    pub(super) offset: u64,
    pub(super) done: bool,
}

/// A snapshot RPC's body: the version and header (`body_with_header`), then
/// the snapshot data as it is.
pub(super) fn encode_install_snapshot(
    request: InstallSnapshotRequest<RoomTypes>,
) -> io::Result<Vec<u8>> {
    let header = InstallSnapshotHeader {
        vote: request.vote,
        meta: request.meta,
        offset: request.offset,
        done: request.done,
    };
    let mut body = body_with_header(&header, request.data.len())?;
    body.extend_from_slice(&request.data);
    Ok(body)
}

pub(super) fn decode_install_snapshot(
    bytes: &[u8],
) -> io::Result<InstallSnapshotRequest<RoomTypes>> {
    let (header, reader) = read_header::<InstallSnapshotHeader>(bytes)?;
    let data = reader.rest();
    if data.len() > SNAPSHOT_FRAGMENT_BYTES * SNAPSHOT_CREDIT_WINDOW {
        return Err(io::Error::other("room snapshot window too large"));
    }
    Ok(InstallSnapshotRequest {
        vote: header.vote,
        meta: header.meta,
        offset: header.offset,
        data: data.to_vec(),
        done: header.done,
    })
}

pub(super) fn encode_snapshot(machine: &Machine) -> io::Result<Vec<u8>> {
    let raw = serde_json::to_vec(machine).map_err(io::Error::other)?;
    if raw.len() > MAX_SNAPSHOT {
        return Err(io::Error::other("room snapshot too large"));
    }
    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
    encoder.write_all(&raw)?;
    let compressed = encoder.finish()?;
    let total = SNAPSHOT_HEADER_BYTES
        .checked_add(compressed.len())
        .ok_or_else(|| io::Error::other("room snapshot too large"))?;
    if total > MAX_SNAPSHOT {
        return Err(io::Error::other("room snapshot too large"));
    }
    let mut encoded = Vec::with_capacity(total);
    encoded.extend_from_slice(&SNAPSHOT_MAGIC);
    encoded.extend_from_slice(&(raw.len() as u64).to_le_bytes());
    encoded.extend_from_slice(blake3::hash(&raw).as_bytes());
    encoded.extend_from_slice(&compressed);
    Ok(encoded)
}

pub(super) fn decode_snapshot(encoded: &[u8]) -> io::Result<Machine> {
    if encoded.len() < SNAPSHOT_HEADER_BYTES || encoded.len() > MAX_SNAPSHOT {
        return Err(io::Error::other("invalid room snapshot envelope"));
    }
    if encoded[..SNAPSHOT_MAGIC.len()] != SNAPSHOT_MAGIC {
        return Err(io::Error::other("invalid room snapshot version"));
    }
    let length_offset = SNAPSHOT_MAGIC.len();
    let digest_offset = length_offset + size_of::<u64>();
    let data_offset = digest_offset + 32;
    let expected_length = u64::from_le_bytes(
        encoded[length_offset..digest_offset]
            .try_into()
            .map_err(|_| io::Error::other("invalid room snapshot length"))?,
    );
    if expected_length > MAX_SNAPSHOT as u64 {
        return Err(io::Error::other("room snapshot too large"));
    }
    let expected_digest = &encoded[digest_offset..data_offset];
    let mut decoder = ZlibDecoder::new(&encoded[data_offset..]);
    let mut raw = Vec::with_capacity(expected_length as usize);
    decoder
        .by_ref()
        .take(MAX_SNAPSHOT as u64 + 1)
        .read_to_end(&mut raw)?;
    if raw.len() as u64 != expected_length
        || decoder.total_in() as usize != encoded.len() - data_offset
        || blake3::hash(&raw).as_bytes() != expected_digest
    {
        return Err(io::Error::other("invalid room snapshot digest"));
    }
    serde_json::from_slice(&raw).map_err(io::Error::other)
}
