use axum::{extract::Multipart, http::StatusCode};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub const BODY_BYTES: usize = 5 * 1024 * 1024;
pub const LOG_BYTES: usize = 192 * 1024;
pub const DUMP_BYTES: usize = 4 * 1024 * 1024;
pub const META_BYTES: usize = 16 * 1024;
pub const LOG_NAMES: [&str; 4] = ["sf4e.log", "launcher.log", "sf4e-crash.log", "sf4-net.log"];

#[derive(Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    Crash,
    Problem,
}
#[derive(Clone, Copy, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Channel {
    Stable,
    Beta,
    Nightly,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Meta {
    pub schema: u32,
    pub kind: Kind,
    pub app_version: String,
    pub channel: Channel,
    pub build_id: String,
    pub source_revision: String,
    pub windows_version: String,
    pub exit_code: Option<i64>,
    pub exception_code: Option<String>,
    pub crash_address: Option<String>,
    pub comment: Option<String>,
}
impl Meta {
    pub fn validate(&self) -> Result<(), &'static str> {
        fn text(s: &str, cap: usize) -> bool {
            !s.is_empty() && s.len() <= cap && !s.chars().any(char::is_control)
        }
        if self.schema != 1 {
            return Err("unsupported_schema");
        }
        if !text(&self.app_version, 128)
            || !text(&self.source_revision, 128)
            || !text(&self.windows_version, 256)
        {
            return Err("invalid_metadata");
        }
        if self.build_id.len() != 64 || !self.build_id.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err("invalid_build_id");
        }
        if self
            .comment
            .as_ref()
            .is_some_and(|s| s.chars().count() > 2000)
        {
            return Err("comment_too_long");
        }
        if self
            .exception_code
            .as_ref()
            .is_some_and(|s| hex(s, 8).is_none())
            || self
                .crash_address
                .as_ref()
                .is_some_and(|s| hex(s, 16).is_none())
            || self
                .exit_code
                .is_some_and(|n| n < i32::MIN as i64 || n > u32::MAX as i64)
        {
            return Err("invalid_crash_facts");
        }
        Ok(())
    }
}
pub fn hex(s: &str, digits: usize) -> Option<u64> {
    let s = s
        .strip_prefix("0x")
        .or_else(|| s.strip_prefix("0X"))
        .unwrap_or(s);
    if s.is_empty() || s.len() > digits || !s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    u64::from_str_radix(s, 16).ok()
}

pub struct Report {
    pub meta: Meta,
    pub logs: BTreeMap<String, String>,
    pub minidump: Option<Vec<u8>>,
}

pub type Error = (StatusCode, &'static str);
pub async fn read(mut multipart: Multipart) -> Result<Report, Error> {
    let bad = |s| (StatusCode::BAD_REQUEST, s);
    let mut meta = None;
    let mut logs = BTreeMap::new();
    let mut minidump = None;
    while let Some(mut part) = multipart
        .next_field()
        .await
        .map_err(|e| (e.status(), "invalid_multipart"))?
    {
        let name = part
            .name()
            .ok_or_else(|| bad("missing_part_name"))?
            .to_owned();
        let filename = part.file_name().map(str::to_owned);
        let cap = match name.as_str() {
            "meta" if meta.is_none() && filename.is_none() => META_BYTES,
            "minidump" if minidump.is_none() => DUMP_BYTES,
            "log" => {
                let file = filename
                    .as_deref()
                    .ok_or_else(|| bad("missing_log_filename"))?;
                if !LOG_NAMES.contains(&file) {
                    return Err(bad("unknown_log_filename"));
                }
                if logs.contains_key(file) || logs.len() >= 4 {
                    return Err(bad("duplicate_log"));
                }
                LOG_BYTES
            }
            "meta" | "minidump" => return Err(bad("duplicate_or_invalid_part")),
            _ => return Err(bad("unknown_part")),
        };
        let mut bytes = Vec::new();
        while let Some(chunk) = part
            .chunk()
            .await
            .map_err(|e| (e.status(), "invalid_multipart"))?
        {
            if bytes.len() + chunk.len() > cap {
                return Err((StatusCode::PAYLOAD_TOO_LARGE, "part_too_large"));
            }
            bytes.extend_from_slice(&chunk);
        }
        match name.as_str() {
            "meta" => {
                let parsed: Meta =
                    serde_json::from_slice(&bytes).map_err(|_| bad("invalid_meta_json"))?;
                parsed.validate().map_err(bad)?;
                meta = Some(parsed);
            }
            "log" => {
                let log = String::from_utf8(bytes).map_err(|_| bad("log_not_utf8"))?;
                logs.insert(filename.unwrap(), log);
            }
            "minidump" => {
                if !bytes.starts_with(b"MDMP") {
                    return Err(bad("invalid_minidump_signature"));
                }
                minidump = Some(bytes);
            }
            _ => unreachable!(),
        }
    }
    Ok(Report {
        meta: meta.ok_or_else(|| bad("missing_meta"))?,
        logs,
        minidump,
    })
}
