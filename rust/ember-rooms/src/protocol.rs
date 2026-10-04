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
//!   "invitation": "...", "banned": ["emb_..."], "details": { ... } }`
//!   whenever something changes. It always carries the current invitation,
//!   which changes as it renews; `tables_playing`, `banned` and `details` may
//!   be left out. `details` is the room as the room host sees it for the
//!   listing: `{ "name", "capacity", "locked", "host_name", "fighters",
//!   "set_format", "rotation" }`. The supervisor does not read it: it passes
//!   a JSON object of at most 2 KB on to the bridge as it is, unknown keys
//!   included, and drops anything else (another type, a larger object, one
//!   nested deeper than the supervisor allows) without treating the line as
//!   an error. It must still be well-formed JSON, like the rest of the line.
//! - `{ "type": "closed", "reason": "..." }`, optionally, before the child
//!   exits on its own (the room host says it when its last member leaves on
//!   purpose, then takes a few seconds to close). The room leaves the list at
//!   once and the child's stdin is closed, which starts the kill grace.
//!
//! A line over 128 KiB, a line that is not a JSON object of a known shape, a
//! `status` before `hosted`, an empty invitation, a member named twice and a
//! `status` naming more than 512 banned accounts are protocol errors: the
//! supervisor kills the child. Objects with an unknown `type` and blank lines
//! are ignored so the child can grow new messages.
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncBufReadExt, AsyncRead, BufReader};

/// The most accounts a room may ban in its lifetime, and so the most a
/// `status` line can list. The same number is `ember_protocol::rooms::
/// MAX_ROOM_BANS` (this crate does not depend on ember-protocol), the helper's
/// ban set and the room model in `src/roomhost`; keep them equal.
pub const MAX_ROOM_BANS: usize = 512;

/// The most a status line's `details` object may weigh as compact JSON. The
/// bridge's per-room report allowance (`MAX_ROOM_REPORT` in its supervisor
/// client) grows by the same amount; keep them equal.
pub const MAX_DETAILS_BYTES: usize = 2048;

/// How deeply nested a kept `details` may be. The real object is two levels
/// (the object and its `fighters` array); the limit keeps the supervisor's own
/// list, which the bridge parses with serde_json's depth limit of 128 one
/// level further down, from ever failing on a hostile value.
pub const MAX_DETAILS_DEPTH: usize = 8;

/// The longest status line, not counting the newline. The worst case a valid
/// `status` can reach is `MAX_ROOM_BANS` Ember IDs (57 bytes each, 60 with
/// quotes and a comma: 30 720 bytes) plus an invitation of at most 4096 bytes
/// (`ember_protocol::play::MAX_INVITATION`, 4112 with its key and quotes) and
/// about 100 bytes of the rest of the object, plus `MAX_DETAILS_BYTES` of
/// details: roughly 37 000 bytes. 128 KiB is 3.5 times that.
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
        /// Filled by `parse_line`, never by serde: the line's `details` member
        /// is read as raw text and decoded on its own, so one too deeply
        /// nested for the parser is absent instead of failing the line. Any
        /// JSON value that is left is here; `usable_details` decides whether
        /// it is kept.
        #[serde(skip)]
        details: Option<serde_json::Value>,
    },
    Closed {
        #[serde(default)]
        reason: String,
    },
    #[serde(other)]
    Unknown,
}

/// The `details` of a status as the supervisor keeps it: a JSON object whose
/// compact form is at most `MAX_DETAILS_BYTES` and whose nesting is at most
/// `MAX_DETAILS_DEPTH`. Anything else is dropped, and the room carries on
/// without it. (`parse_line` has already left out a `details` too deeply
/// nested to decode, so that is never the line's error.)
pub fn usable_details(details: Option<serde_json::Value>) -> Option<serde_json::Value> {
    fn depth(value: &serde_json::Value) -> usize {
        match value {
            serde_json::Value::Array(items) => 1 + items.iter().map(depth).max().unwrap_or(0),
            serde_json::Value::Object(fields) => 1 + fields.values().map(depth).max().unwrap_or(0),
            _ => 0,
        }
    }
    let details = details?;
    let fits = details.is_object()
        && depth(&details) <= MAX_DETAILS_DEPTH
        && serde_json::to_vec(&details).is_ok_and(|bytes| bytes.len() <= MAX_DETAILS_BYTES);
    fits.then_some(details)
}

/// A line's top-level members: `details` as raw text (the parser checks that
/// it is well-formed JSON of any depth without decoding it), everything else
/// decoded. A member named twice is an error, `details` included, whichever
/// way its name is spelled.
struct Envelope {
    rest: serde_json::Map<String, serde_json::Value>,
    details: Option<Box<serde_json::value::RawValue>>,
}

impl<'de> Deserialize<'de> for Envelope {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct Members;
        impl<'de> serde::de::Visitor<'de> for Members {
            type Value = Envelope;

            fn expecting(&self, out: &mut std::fmt::Formatter) -> std::fmt::Result {
                out.write_str("a JSON object")
            }

            fn visit_map<A: serde::de::MapAccess<'de>>(
                self,
                mut map: A,
            ) -> Result<Envelope, A::Error> {
                let mut envelope = Envelope {
                    rest: serde_json::Map::new(),
                    details: None,
                };
                while let Some(key) = map.next_key::<String>()? {
                    let duplicate = if key == "details" {
                        envelope.details.is_some()
                    } else {
                        envelope.rest.contains_key(&key)
                    };
                    if duplicate {
                        return Err(serde::de::Error::custom(format!("duplicate field `{key}`")));
                    }
                    if key == "details" {
                        envelope.details = Some(map.next_value()?);
                    } else {
                        envelope.rest.insert(key, map.next_value()?);
                    }
                }
                Ok(envelope)
            }
        }
        deserializer.deserialize_map(Members)
    }
}

/// Parses one line, without its newline. The parser owns the boundary: the
/// line must be one well-formed JSON object, and its `details` member is read
/// as raw text and decoded on its own. A `details` the decoder's own depth
/// limit refuses is left out and the line is decoded without it, so what the
/// room host says about its listing can never make a sound line an error.
pub fn parse_line(line: &[u8]) -> Result<ChildMessage, String> {
    let line = line.trim_ascii();
    if line.is_empty() {
        return Ok(ChildMessage::Unknown);
    }
    let text = |error: serde_json::Error| error.to_string();
    let Envelope { rest, details } = serde_json::from_slice(line).map_err(text)?;
    let mut message = serde_json::from_value(serde_json::Value::Object(rest)).map_err(text)?;
    if let ChildMessage::Status { details: slot, .. } = &mut message {
        *slot = details.and_then(|raw| serde_json::from_str(raw.get()).ok());
    }
    Ok(message)
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

    fn details_of(line: &[u8]) -> Option<serde_json::Value> {
        let Ok(ChildMessage::Status { details, .. }) = parse_line(line) else {
            panic!("expected a status");
        };
        usable_details(details)
    }

    #[test]
    fn details_are_kept_whole_and_unknown_keys_pass_through() {
        let line = br#"{"type":"status","members":2,"invitation":"i","details":{"name":"Room","capacity":8,"locked":false,"host_name":"Kate","fighters":[3,255],"set_format":3,"rotation":1,"future":{"x":[1]}}}"#;
        let details = details_of(line).expect("details");
        assert_eq!(details["name"], "Room");
        assert_eq!(details["fighters"], serde_json::json!([3, 255]));
        assert_eq!(details["future"]["x"][0], 1);
        // A status with none still parses, and says so.
        assert!(details_of(br#"{"type":"status","members":2,"invitation":"i"}"#).is_none());
        assert!(
            details_of(br#"{"type":"status","members":2,"invitation":"i","details":null}"#)
                .is_none()
        );
    }

    #[test]
    fn malformed_details_never_fail_the_line() {
        for details in [
            r#""text""#,
            "7",
            "true",
            "[1,2]",
            r#"{"capacity":"many","fighters":"x","locked":[]}"#,
        ] {
            let line =
                format!(r#"{{"type":"status","members":1,"invitation":"i","details":{details}}}"#);
            let parsed = parse_line(line.as_bytes());
            assert!(matches!(
                parsed,
                Ok(ChildMessage::Status { members: 1, .. })
            ));
            // Only an object is kept; what is inside it is the bridge's to judge.
            let kept = details_of(line.as_bytes()).is_some();
            assert_eq!(kept, details.starts_with('{'), "{details}");
        }
    }

    #[test]
    fn details_over_the_limit_are_dropped() {
        let build = |size: usize| {
            let name = "n".repeat(size);
            format!(
                r#"{{"type":"status","members":1,"invitation":"i","details":{{"k":"{name}"}}}}"#
            )
        };
        // `{"k":""}` is 8 bytes of the object around the text.
        let exact = build(MAX_DETAILS_BYTES - 8);
        assert!(details_of(exact.as_bytes()).is_some());
        let over = build(MAX_DETAILS_BYTES - 7);
        assert!(matches!(
            parse_line(over.as_bytes()),
            Ok(ChildMessage::Status { .. })
        ));
        assert!(details_of(over.as_bytes()).is_none());
    }

    #[test]
    fn deeply_nested_details_are_dropped() {
        let nested = |levels: usize| {
            let (open, close) = ("[".repeat(levels), "]".repeat(levels));
            format!(
                r#"{{"type":"status","members":1,"invitation":"i","details":{{"k":{open}{close}}}}}"#
            )
        };
        assert!(details_of(nested(MAX_DETAILS_DEPTH - 1).as_bytes()).is_some());
        assert!(details_of(nested(MAX_DETAILS_DEPTH).as_bytes()).is_none());
        // Past the parser's own limit the details are dropped and the line
        // still parses, with every other field.
        let Ok(ChildMessage::Status {
            members,
            invitation,
            details,
            ..
        }) = parse_line(nested(200).as_bytes())
        else {
            panic!("expected a status");
        };
        assert_eq!((members, invitation.as_str(), details), (1, "i", None));
        // The most a line can hold, about 60 000 levels, is still no error
        // and no recursion, on a status or on any other message.
        for kind in ["hosted", "status"] {
            let (open, close) = ("[".repeat(60_000), "]".repeat(60_000));
            let line = format!(
                r#"{{"type":"{kind}","members":1,"invitation":"i","details":{open}{close}}}"#
            );
            assert!(line.len() <= MAX_LINE_BYTES);
            assert!(parse_line(line.as_bytes()).is_ok(), "{kind}");
        }
    }

    #[test]
    fn the_details_member_is_found_among_strings_and_other_members() {
        // Brackets, quotes and the word in strings, and `details` deeper down,
        // are not the member. The real one sits last, deep, with escapes in
        // its name.
        let deep = format!("{}{}", "[".repeat(300), "]".repeat(300));
        let line = format!(
            r#"{{ "invitation" : "i\"}}[{{\"details\":{deep}" , "other": {{"details": 1}}, "type":"status", "members": 2, "details" : {{"k": {deep}, "s": "]}}"}} }}"#
        );
        let Ok(ChildMessage::Status {
            members,
            invitation,
            details,
            ..
        }) = parse_line(line.as_bytes())
        else {
            panic!("expected a status");
        };
        assert_eq!((members, details), (2, None));
        assert!(invitation.starts_with('i'));
        // The same member, shallow, is read whole.
        let line = r#"{"type":"status","members":2,"invitation":"i","details" : {"s": "]}\"", "n": [ 1 ] } }"#;
        let Ok(ChildMessage::Status { details, .. }) = parse_line(line.as_bytes()) else {
            panic!("expected a status");
        };
        assert_eq!(details, Some(serde_json::json!({ "s": "]}\"", "n": [1] })));
    }

    #[test]
    fn invalid_json_is_an_error_wherever_it_sits() {
        let deep = format!("{}{}", "[".repeat(300), "]".repeat(300));
        // Details that are not JSON are not repaired into absence.
        for details in [r#"{"a":}"#, "", "nope", "[1,,2]", r#"{"a":1,}"#, "[1"] {
            let line =
                format!(r#"{{"type":"status","members":1,"invitation":"i","details":{details}}}"#);
            assert!(parse_line(line.as_bytes()).is_err(), "{line}");
        }
        for line in [
            r#"{"type":"status","members":-1,"invitation":"i","details":{"a":1}}"#.to_owned(),
            format!(r#"{{"type":"status","invitation":"i","details":{deep}}}"#),
            format!(r#"{{"type":"status","members":1,"invitation":"i","details":{deep}"#),
            format!(r#"{{"type":"status","members":1,"invitation":"i","details":{deep}}} x"#),
            format!(r#"{{"type":"status","members":1,"invitation":"i","details":{deep}}}}}"#),
            format!(r#"{{"type":"status","members":1,"invitation":"i","x":{deep}"#),
            format!(r#"{{"type":"status","members":1,"invitation":"i",,"details":{deep}}}"#),
        ] {
            assert!(parse_line(line.as_bytes()).is_err(), "{line}");
        }
    }

    #[test]
    fn a_member_named_twice_is_an_error() {
        for line in [
            r#"{"type":"status","members":1,"invitation":"i","details":{},"details":{}}"#,
            r#"{"type":"status","members":1,"invitation":"i","details":{},"details":null}"#,
            r#"{"type":"status","members":1,"members":2,"invitation":"i"}"#,
            r#"{"type":"status","type":"closed","members":1,"invitation":"i"}"#,
            r#"{"type":"status","members":1,"invitation":"i","other":1,"other":2}"#,
        ] {
            let error = parse_line(line.as_bytes()).expect_err(line);
            assert!(error.contains("duplicate field"), "{line}: {error}");
        }
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
        // 512 Ember IDs of 57 bytes, an invitation of 4096 bytes and the
        // largest details.
        let id = format!("emb1_{}a", "q".repeat(51));
        assert_eq!(id.len(), 57);
        let line = serde_json::to_string(&serde_json::json!({
            "type": "status", "members": u32::MAX, "tables_playing": u32::MAX,
            "invitation": "i".repeat(4096), "banned": vec![id; MAX_ROOM_BANS],
            "details": { "name": "n".repeat(MAX_DETAILS_BYTES - 11) },
        }))
        .unwrap();
        assert!(line.len() > 36_000, "{}", line.len());
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
