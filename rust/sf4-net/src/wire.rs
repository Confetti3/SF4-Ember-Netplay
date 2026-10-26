//! Explicit framing shared by local IPC and reliable QUIC control streams.
//!
//! Integers are big endian. A control frame is a 4-byte body length followed by
//! a 2-byte version, an 8-byte application message ID, and the original payload.
//! IDs are assigned by the sending application, never by QUIC write counts.
use std::io;

use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

/// Heads every IPC frame, QUIC control frame and game packet. It never
/// changes for an IPC change; `IPC_VERSION` does.
pub const VERSION: u16 = 1;
/// The game-to-helper IPC contract. It is never in a frame header: the helper
/// sends it after `SF4N` in its answer to the pipe's authentication frame, and
/// the game opens a room only with a helper of its own version, so a game and
/// a helper from different builds fail closed when they connect.
pub const IPC_VERSION: u16 = 2;
/// First payload byte of an IPC frame that carries a JSON header and a raw
/// body: this byte, the header's length as a big-endian u32, the header, then
/// the body. A JSON payload starts with `{`, so the kinds cannot be confused.
/// The body is a field the header leaves out: `data` of a checkpoint chunk,
/// `payload` of a room message or send.
pub const IPC_RAW_BODY: u8 = 0;
const RAW_PREFIX: usize = 1 + 4;
pub const CONTROL_ALPN: &[u8] = b"sf4e/control/1";
pub const GAME_ALPN: &[u8] = b"sf4e/game/1";
pub const MAX_CONTROL_PAYLOAD: usize = 64 * 1024;
// A JSON IPC envelope can escape each byte of a control payload six times.
pub const MAX_IPC_PAYLOAD: usize = 512 * 1024;
const CONTROL_HEADER: usize = 10;
pub const GAME_HEADER: usize = 2 + 16 + 8;
// UDP's protocol ceiling is an allocation bound, not a promise that QUIC's MTU
// supports this size. Every send also checks the current QUIC datagram limit.
pub const MAX_UDP_PAYLOAD: usize = 65_507;

#[derive(Debug, PartialEq, Eq)]
pub struct ControlFrame {
    pub message_id: u64,
    pub payload: Vec<u8>,
}

fn invalid(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

pub async fn read_control<R: AsyncRead + Unpin>(reader: &mut R) -> io::Result<ControlFrame> {
    read_frame(reader, MAX_CONTROL_PAYLOAD).await
}

pub async fn read_ipc<R: AsyncRead + Unpin>(reader: &mut R) -> io::Result<ControlFrame> {
    read_frame(reader, MAX_IPC_PAYLOAD).await
}

async fn read_frame<R: AsyncRead + Unpin>(
    reader: &mut R,
    maximum: usize,
) -> io::Result<ControlFrame> {
    let length = reader.read_u32().await? as usize;
    if !(CONTROL_HEADER + 1..=CONTROL_HEADER + maximum).contains(&length) {
        return Err(invalid("control frame length out of bounds"));
    }
    if reader.read_u16().await? != VERSION {
        return Err(invalid("unsupported protocol version"));
    }
    let message_id = reader.read_u64().await?;
    if message_id == 0 || message_id > i64::MAX as u64 {
        return Err(invalid("invalid application message ID"));
    }
    let mut payload = vec![0; length - CONTROL_HEADER];
    reader.read_exact(&mut payload).await?;
    Ok(ControlFrame {
        message_id,
        payload,
    })
}

pub async fn write_control<W: AsyncWrite + Unpin>(
    writer: &mut W,
    frame: &ControlFrame,
) -> io::Result<()> {
    write_frame(writer, frame, MAX_CONTROL_PAYLOAD).await
}

pub async fn write_ipc<W: AsyncWrite + Unpin>(
    writer: &mut W,
    frame: &ControlFrame,
) -> io::Result<()> {
    write_frame(writer, frame, MAX_IPC_PAYLOAD).await
}

async fn write_frame<W: AsyncWrite + Unpin>(
    writer: &mut W,
    frame: &ControlFrame,
    maximum: usize,
) -> io::Result<()> {
    if frame.payload.is_empty()
        || frame.payload.len() > maximum
        || frame.message_id == 0
        || frame.message_id > i64::MAX as u64
    {
        return Err(invalid("invalid control frame"));
    }
    writer
        .write_u32((CONTROL_HEADER + frame.payload.len()) as u32)
        .await?;
    writer.write_u16(VERSION).await?;
    writer.write_u64(frame.message_id).await?;
    writer.write_all(&frame.payload).await?;
    writer.flush().await
}

/// An IPC frame's payload, as `split_ipc_payload` finds it.
#[derive(Debug, PartialEq, Eq)]
pub enum IpcPayload<'a> {
    Json(&'a [u8]),
    Raw { header: &'a [u8], body: &'a [u8] },
}

/// Tells a JSON payload from a raw-body one and splits the latter. A raw
/// payload needs a JSON object header and a body of at least one byte.
pub fn split_ipc_payload(payload: &[u8]) -> io::Result<IpcPayload<'_>> {
    if payload.first() != Some(&IPC_RAW_BODY) {
        return Ok(IpcPayload::Json(payload));
    }
    let length = payload
        .get(1..RAW_PREFIX)
        .map(|bytes| u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as usize)
        .ok_or_else(|| invalid("truncated raw-body IPC payload"))?;
    let header_end = RAW_PREFIX
        .checked_add(length)
        .filter(|end| length >= 2 && *end < payload.len())
        .ok_or_else(|| invalid("raw-body IPC header out of bounds"))?;
    let header = &payload[RAW_PREFIX..header_end];
    if header[0] != b'{' {
        return Err(invalid("raw-body IPC header is not an object"));
    }
    Ok(IpcPayload::Raw {
        header,
        body: &payload[header_end..],
    })
}

/// The payload of a raw-body IPC frame: `header` (a JSON object without the
/// body's field) and `body` as they are, within `MAX_IPC_PAYLOAD`.
pub fn raw_ipc_payload(header: &[u8], body: &[u8]) -> io::Result<Vec<u8>> {
    let size = RAW_PREFIX + header.len() + body.len();
    if header.len() < 2 || header[0] != b'{' || body.is_empty() || size > MAX_IPC_PAYLOAD {
        return Err(invalid("invalid raw-body IPC payload"));
    }
    let mut payload = Vec::with_capacity(size);
    payload.push(IPC_RAW_BODY);
    payload.extend_from_slice(&(header.len() as u32).to_be_bytes());
    payload.extend_from_slice(header);
    payload.extend_from_slice(body);
    Ok(payload)
}

/// Bytes in an IPC message: unpadded URL-safe base64 in a JSON payload, and
/// the bytes themselves as the body of a raw-body frame.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct IpcBytes(pub Vec<u8>);

impl Serialize for IpcBytes {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&URL_SAFE_NO_PAD.encode(&self.0))
    }
}

impl<'de> Deserialize<'de> for IpcBytes {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let encoded = std::borrow::Cow::<str>::deserialize(deserializer)?;
        URL_SAFE_NO_PAD
            .decode(encoded.as_bytes())
            .map(IpcBytes)
            .map_err(|_| serde::de::Error::custom("invalid base64 bytes"))
    }
}

/// A fresh room instance plus match generation prevents rematch packet replay.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MatchKey {
    pub room: [u8; 16],
    pub generation: u64,
}

impl MatchKey {
    pub fn encode(self, payload: &[u8], max_datagram: usize) -> io::Result<Vec<u8>> {
        if self.generation == 0
            || self.room == [0; 16]
            || payload.is_empty()
            || payload.len() > MAX_UDP_PAYLOAD
            || payload.len() + GAME_HEADER > max_datagram
        {
            return Err(invalid(
                "gameplay packet exceeds negotiated limit or has invalid identity",
            ));
        }
        let mut packet = Vec::with_capacity(GAME_HEADER + payload.len());
        packet.extend_from_slice(&VERSION.to_be_bytes());
        packet.extend_from_slice(&self.room);
        packet.extend_from_slice(&self.generation.to_be_bytes());
        packet.extend_from_slice(payload);
        Ok(packet)
    }

    /// The authenticated connection supplies the peer identity. No sender field
    /// in an incoming packet can select a different local forwarding target.
    pub fn decode(self, packet: &[u8]) -> io::Result<&[u8]> {
        if self.generation == 0
            || self.room == [0; 16]
            || packet.len() <= GAME_HEADER
            || packet.len() > GAME_HEADER + MAX_UDP_PAYLOAD
        {
            return Err(invalid("invalid gameplay packet length or identity"));
        }
        if packet[..2] != VERSION.to_be_bytes()
            || packet[2..18] != self.room
            || packet[18..26] != self.generation.to_be_bytes()
        {
            return Err(invalid("stale or incompatible gameplay packet"));
        }
        Ok(&packet[GAME_HEADER..])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn partial_reads_and_adjacent_frames_preserve_application_ids() {
        // Capacity one forces every multi-byte write through partial reads.
        let (mut tx, mut rx) = tokio::io::duplex(1);
        let sender = tokio::spawn(async move {
            for id in [7, 8192] {
                write_control(
                    &mut tx,
                    &ControlFrame {
                        message_id: id,
                        payload: b"lobby_ready".to_vec(),
                    },
                )
                .await
                .unwrap();
            }
        });
        for id in [7, 8192] {
            let frame = read_control(&mut rx).await.unwrap();
            assert_eq!(frame.message_id, id);
            assert_eq!(frame.payload, b"lobby_ready");
        }
        sender.await.unwrap();
    }

    #[tokio::test]
    async fn malformed_lengths_versions_ids_and_truncated_frames_fail() {
        for length in [
            0u32,
            10,
            u32::MAX,
            (MAX_CONTROL_PAYLOAD + CONTROL_HEADER + 1) as u32,
        ] {
            assert!(read_control(&mut &length.to_be_bytes()[..]).await.is_err());
        }
        for (version, id, body) in [
            (2u16, 1u64, vec![1]),
            (1, 0, vec![1]),
            (1, u64::MAX, vec![1]),
            (1, 1, vec![]),
        ] {
            let mut bytes = 11u32.to_be_bytes().to_vec();
            bytes.extend(version.to_be_bytes());
            bytes.extend(id.to_be_bytes());
            bytes.extend(body);
            assert!(read_control(&mut &bytes[..]).await.is_err());
        }
    }

    #[tokio::test]
    async fn maximum_frame_roundtrips_and_oversized_send_fails_before_writing() {
        let frame = ControlFrame {
            message_id: i64::MAX as u64,
            payload: vec![42; MAX_CONTROL_PAYLOAD],
        };
        let mut bytes = Vec::new();
        write_control(&mut bytes, &frame).await.unwrap();
        assert_eq!(read_control(&mut &bytes[..]).await.unwrap(), frame);
        bytes.clear();
        let invalid = ControlFrame {
            message_id: 1,
            payload: vec![0; MAX_CONTROL_PAYLOAD + 1],
        };
        assert!(write_control(&mut bytes, &invalid).await.is_err());
        assert!(bytes.is_empty());
    }

    #[test]
    fn raw_body_payloads_split_back_and_json_passes_through() {
        let header = br#"{"type":"checkpoint_chunk","offset":0}"#;
        let body = [0u8, 255, b'{', b'"', 0];
        let payload = raw_ipc_payload(header, &body).unwrap();
        assert_eq!(payload[0], IPC_RAW_BODY);
        assert_eq!(
            split_ipc_payload(&payload).unwrap(),
            IpcPayload::Raw {
                header: &header[..],
                body: &body[..],
            }
        );
        let json = br#"{"type":"status"}"#;
        assert_eq!(
            split_ipc_payload(json).unwrap(),
            IpcPayload::Json(&json[..])
        );
        // A raw frame whose body would push it past the IPC bound is refused
        // before anything is built.
        assert!(raw_ipc_payload(header, &vec![1; MAX_IPC_PAYLOAD]).is_err());
        assert!(raw_ipc_payload(header, &[]).is_err());
        assert!(raw_ipc_payload(b"[1]", &body).is_err());
    }

    #[test]
    fn malformed_raw_body_payloads_fail() {
        let mut no_body = vec![IPC_RAW_BODY];
        no_body.extend(2u32.to_be_bytes());
        no_body.extend(b"{}");
        let mut not_object = vec![IPC_RAW_BODY];
        not_object.extend(2u32.to_be_bytes());
        not_object.extend(b"[]x");
        let mut too_long = vec![IPC_RAW_BODY];
        too_long.extend(u32::MAX.to_be_bytes());
        too_long.extend(b"{}x");
        let mut too_short = vec![IPC_RAW_BODY];
        too_short.extend(1u32.to_be_bytes());
        too_short.extend(b"{x");
        for payload in [
            vec![IPC_RAW_BODY],
            vec![IPC_RAW_BODY, 0, 0, 0],
            no_body,
            not_object,
            too_long,
            too_short,
        ] {
            assert!(split_ipc_payload(&payload).is_err(), "{payload:?}");
        }
    }

    #[test]
    fn ipc_bytes_are_base64_in_json() {
        let bytes = IpcBytes(vec![0, 1, 2, 250, 255]);
        let text = serde_json::to_string(&bytes).unwrap();
        assert_eq!(text, "\"AAEC-v8\"");
        assert_eq!(serde_json::from_str::<IpcBytes>(&text).unwrap(), bytes);
        assert!(serde_json::from_str::<IpcBytes>("\"not base64!\"").is_err());
        assert_eq!(
            serde_json::from_str::<IpcBytes>("\"\"").unwrap(),
            IpcBytes::default()
        );
    }

    #[test]
    fn gameplay_packet_is_exact_and_generation_scoped() {
        let key = MatchKey {
            room: [3; 16],
            generation: 50,
        };
        let packet = key.encode(&[0, 255, 1, 128], GAME_HEADER + 4).unwrap();
        assert_eq!(key.decode(&packet).unwrap(), [0, 255, 1, 128]);
        assert!(key.encode(&[1; 5], GAME_HEADER + 4).is_err());
        for n in 0..=GAME_HEADER {
            assert!(key.decode(&packet[..n]).is_err());
        }
        assert!(
            MatchKey {
                generation: 51,
                ..key
            }
            .decode(&packet)
            .is_err()
        );
        assert!(
            MatchKey {
                room: [4; 16],
                ..key
            }
            .decode(&packet)
            .is_err()
        );
        let mut bad_version = packet;
        bad_version[1] = 2;
        assert!(key.decode(&bad_version).is_err());
    }
}
