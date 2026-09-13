//! Explicit framing shared by local IPC and reliable QUIC control streams.
//!
//! Integers are big endian. A control frame is a 4-byte body length followed by
//! a 2-byte version, an 8-byte application message ID, and the original payload.
//! IDs are assigned by the sending application, never by QUIC write counts.
use std::io;

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

pub const VERSION: u16 = 1;
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
