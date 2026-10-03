//! The room host child protocol and the request body that feeds it.
//!
//! The supervisor starts `<room_host>` with no arguments, stdin and stdout
//! piped, stderr inherited and kill-on-drop. Over stdin it writes exactly one
//! line of JSON and then keeps the pipe open:
//!
//! ```json
//! { "room_id": "...", "name": "...", "capacity": 8, "build_id": "...",
//!   "creator": "...", "bridge_id": "...", "ticket_key": "...",
//!   "ticket_kid": "...", "helper": "/path/to/sf4-net", "port": 45800,
//!   "coordination_port": 45801 }
//! ```
//!
//! `port` is the primary endpoint's UDP port and `coordination_port` the
//! second one the room needs. The supervisor picks the two lowest free ports
//! of its range (not necessarily adjacent) and holds both until the child
//! has exited.
//!
//! Closing the child's stdin (end of file) means "close the room and exit".
//! The child answers on stdout with one JSON object per line:
//!
//! - `{ "type": "hosted", "invitation": "...", "region": "..." }`, exactly
//!   once and before anything else. The supervisor waits 30 s for it.
//! - `{ "type": "status", "members": 0, "tables_playing": 0,
//!   "invitation": "...", "banned": ["emb_..."] }` whenever something
//!   changes. It always carries the current invitation, which changes as it
//!   renews; `tables_playing` and `banned` may be left out.
//! - `{ "type": "closed", "reason": "..." }`, optionally, just before the
//!   child exits on its own.
//!
//! A line over 128 KiB, a line that is not a JSON object of a known shape, a
//! `status` before `hosted`, an empty invitation and a `status` naming more
//! than 512 banned accounts are protocol errors: the supervisor kills the
//! child. Objects with an unknown `type` and blank lines
//! are ignored so the child can grow new messages.
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncBufReadExt, AsyncRead, BufReader};

/// The most accounts a room may ban in its lifetime, and so the most a
/// `status` line can list. The same number is `ember_protocol::rooms::
/// MAX_ROOM_BANS` (this crate does not depend on ember-protocol), the helper's
/// ban set and the room model in `src/roomhost`; keep them equal.
pub const MAX_ROOM_BANS: usize = 512;

/// The longest status line, not counting the newline. The worst case a valid
/// `status` can reach is `MAX_ROOM_BANS` Ember IDs (57 bytes each, 60 with
/// quotes and a comma: 30 720 bytes) plus an invitation of at most 4096 bytes
/// (`ember_protocol::play::MAX_INVITATION`, 4112 with its key and quotes) and
/// about 100 bytes of the rest of the object: roughly 35 000 bytes. 128 KiB
/// is 3.7 times that.
pub const MAX_LINE_BYTES: usize = 128 * 1024;

const MAX_TOKEN_BYTES: usize = 128;
const MAX_KEY_BYTES: usize = 256;

/// The body of `POST /rooms`. It is also the first half of the child's
/// configuration line.
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CreateRoom {
    /// The room's 16-byte id as 32 hex characters.
    pub room_id: String,
    pub name: String,
    pub capacity: u8,
    pub build_id: String,
    /// The creator's Ember ID.
    pub creator: String,
    pub bridge_id: String,
    pub ticket_key: String,
    pub ticket_kid: String,
}

fn is_token(text: &str, limit: usize) -> bool {
    !text.is_empty() && text.len() <= limit && text.bytes().all(|byte| byte.is_ascii_graphic())
}

impl CreateRoom {
    /// The name of the first field that is malformed.
    pub fn validate(&self) -> Result<(), &'static str> {
        if self.room_id.len() != 32 || !self.room_id.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err("room_id");
        }
        if self.name.is_empty() || self.name.len() > 64 || self.name.chars().any(char::is_control) {
            return Err("name");
        }
        if !(2..=16).contains(&self.capacity) {
            return Err("capacity");
        }
        for (field, text, limit) in [
            ("build_id", &self.build_id, MAX_TOKEN_BYTES),
            ("creator", &self.creator, MAX_TOKEN_BYTES),
            ("bridge_id", &self.bridge_id, MAX_TOKEN_BYTES),
            ("ticket_key", &self.ticket_key, MAX_KEY_BYTES),
            ("ticket_kid", &self.ticket_kid, MAX_TOKEN_BYTES),
        ] {
            if !is_token(text, limit) {
                return Err(field);
            }
        }
        Ok(())
    }
}

/// The configuration line written to the child's stdin.
#[derive(Serialize)]
pub struct ChildConfig<'a> {
    #[serde(flatten)]
    pub room: &'a CreateRoom,
    pub helper: &'a str,
    /// The primary endpoint port.
    pub port: u16,
    pub coordination_port: u16,
}

/// One status line from the child.
#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum ChildMessage {
    Hosted {
        invitation: String,
        #[serde(default)]
        region: String,
    },
    Status {
        members: u32,
        #[serde(default)]
        tables_playing: u32,
        invitation: String,
        #[serde(default)]
        banned: Vec<String>,
    },
    Closed {
        #[serde(default)]
        reason: String,
    },
    #[serde(other)]
    Unknown,
}

/// Parses one line, without its newline.
pub fn parse_line(line: &[u8]) -> Result<ChildMessage, String> {
    let line = line.trim_ascii();
    if line.is_empty() {
        return Ok(ChildMessage::Unknown);
    }
    serde_json::from_slice(line).map_err(|error| error.to_string())
}

#[derive(Debug)]
pub enum ReadError {
    TooLong,
    Io(std::io::Error),
}

impl From<std::io::Error> for ReadError {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error)
    }
}

/// Reads newline-delimited lines of bounded length. `next` can be dropped
/// mid-line (it sits in a `select!`) and resumed without losing bytes.
pub struct LineReader<R> {
    reader: BufReader<R>,
    line: Vec<u8>,
}

impl<R: AsyncRead + Unpin> LineReader<R> {
    pub fn new(reader: R) -> Self {
        Self {
            reader: BufReader::new(reader),
            line: Vec::new(),
        }
    }

    /// The next line without its newline, or `None` at end of file. A final
    /// line with no newline is dropped.
    pub async fn next(&mut self) -> Result<Option<Vec<u8>>, ReadError> {
        loop {
            let available = self.reader.fill_buf().await?;
            if available.is_empty() {
                return Ok(None);
            }
            let end = available.iter().position(|byte| *byte == b'\n');
            let take = end.unwrap_or(available.len());
            if self.line.len() + take > MAX_LINE_BYTES {
                return Err(ReadError::TooLong);
            }
            self.line.extend_from_slice(&available[..take]);
            self.reader.consume(take + usize::from(end.is_some()));
            if end.is_some() {
                return Ok(Some(std::mem::take(&mut self.line)));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn room() -> CreateRoom {
        CreateRoom {
            room_id: "0123456789abcdef0123456789ABCDEF".to_owned(),
            name: "Friday night".to_owned(),
            capacity: 8,
            build_id: "b1".to_owned(),
            creator: "emb_abc".to_owned(),
            bridge_id: "brg_abc".to_owned(),
            ticket_key: "a2V5".to_owned(),
            ticket_kid: "k1".to_owned(),
        }
    }

    #[test]
    fn a_well_formed_room_validates() {
        assert_eq!(room().validate(), Ok(()));
    }

    #[test]
    fn each_malformed_field_is_named() {
        type Change = fn(&mut CreateRoom);
        let cases: [(&str, Change); 9] = [
            ("room_id", |r| r.room_id = "abc".into()),
            ("room_id", |r| r.room_id = "g".repeat(32)),
            ("name", |r| r.name = String::new()),
            ("name", |r| r.name = "a".repeat(65)),
            ("name", |r| r.name = "two\nlines".into()),
            ("capacity", |r| r.capacity = 1),
            ("capacity", |r| r.capacity = 17),
            ("creator", |r| r.creator = "has space".into()),
            ("ticket_key", |r| r.ticket_key = String::new()),
        ];
        for (field, change) in cases {
            let mut candidate = room();
            change(&mut candidate);
            assert_eq!(candidate.validate(), Err(field));
        }
        let mut long_name = room();
        long_name.name = "é".repeat(32);
        assert_eq!(long_name.validate(), Ok(()), "64 bytes is allowed");
    }

    #[test]
    fn the_child_config_is_one_flat_object() {
        let room = room();
        let text = serde_json::to_string(&ChildConfig {
            room: &room,
            helper: "/bin/helper",
            port: 45800,
            coordination_port: 45803,
        })
        .unwrap();
        assert!(!text.contains('\n'));
        let value: serde_json::Value = serde_json::from_str(&text).unwrap();
        assert_eq!(value["room_id"], room.room_id);
        assert_eq!(value["helper"], "/bin/helper");
        assert_eq!(value["port"], 45800);
        assert_eq!(value["coordination_port"], 45803);
        assert_eq!(value["ticket_kid"], "k1");
    }

    #[test]
    fn messages_parse_and_unknown_types_are_ignored() {
        let hosted = parse_line(br#"{"type":"hosted","invitation":"i","region":"use1"}"#);
        assert!(matches!(hosted, Ok(ChildMessage::Hosted { .. })));
        let status = parse_line(br#"{"type":"status","members":2,"invitation":"i"}"#).unwrap();
        let ChildMessage::Status {
            members,
            tables_playing,
            banned,
            ..
        } = status
        else {
            panic!("expected a status");
        };
        assert_eq!((members, tables_playing, banned.len()), (2, 0, 0));
        assert!(matches!(
            parse_line(br#"{"type":"closed","reason":"done"}"#),
            Ok(ChildMessage::Closed { .. })
        ));
        assert!(matches!(
            parse_line(br#"{"type":"future","x":1}"#),
            Ok(ChildMessage::Unknown)
        ));
        assert!(matches!(parse_line(b"  \r"), Ok(ChildMessage::Unknown)));
    }

    #[test]
    fn malformed_lines_are_errors() {
        for line in [
            &b"not json"[..],
            br#"{"members":1}"#,
            br#"{"type":"status","members":-1,"invitation":"i"}"#,
            br#"{"type":"status","invitation":"i"}"#,
            br#"["type","status"]"#,
        ] {
            assert!(
                parse_line(line).is_err(),
                "{}",
                String::from_utf8_lossy(line)
            );
        }
    }

    #[test]
    fn the_line_limit_has_room_for_the_largest_status() {
        // 512 Ember IDs of 57 bytes and an invitation of 4096 bytes.
        let id = format!("emb1_{}a", "q".repeat(51));
        assert_eq!(id.len(), 57);
        let line = serde_json::to_string(&serde_json::json!({
            "type": "status", "members": u32::MAX, "tables_playing": u32::MAX,
            "invitation": "i".repeat(4096), "banned": vec![id; MAX_ROOM_BANS],
        }))
        .unwrap();
        assert!(line.len() > 34_000, "{}", line.len());
        assert!(line.len() * 3 < MAX_LINE_BYTES, "{}", line.len());
        let Ok(ChildMessage::Status { banned, .. }) = parse_line(line.as_bytes()) else {
            panic!("expected a status");
        };
        assert_eq!(banned.len(), MAX_ROOM_BANS);
    }

    #[tokio::test]
    async fn the_reader_splits_lines_and_enforces_the_limit() {
        let mut reader = LineReader::new(&b"one\r\ntwo\n\nlast"[..]);
        assert_eq!(reader.next().await.unwrap().unwrap(), b"one\r");
        assert_eq!(reader.next().await.unwrap().unwrap(), b"two");
        assert_eq!(reader.next().await.unwrap().unwrap(), b"");
        // A final line with no newline never completes.
        assert!(reader.next().await.unwrap().is_none());

        let mut exact = vec![b'a'; MAX_LINE_BYTES];
        exact.push(b'\n');
        let mut reader = LineReader::new(&exact[..]);
        assert_eq!(reader.next().await.unwrap().unwrap().len(), MAX_LINE_BYTES);

        let mut long = vec![b'a'; MAX_LINE_BYTES + 1];
        long.push(b'\n');
        let mut reader = LineReader::new(&long[..]);
        assert!(matches!(reader.next().await, Err(ReadError::TooLong)));

        // A long line with no newline at all is still stopped.
        let endless = vec![b'a'; MAX_LINE_BYTES * 3];
        let mut reader = LineReader::new(&endless[..]);
        assert!(matches!(reader.next().await, Err(ReadError::TooLong)));
    }
}
