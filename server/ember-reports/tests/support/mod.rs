// Each test binary compiles this module and uses only some of it.
#![allow(dead_code)]
use ember_reports::{intake::Report, store::ReportId, worker::Worker};
use serde_json::{Value, json};
use std::{collections::BTreeMap, path::PathBuf};

pub struct Temp(pub PathBuf);
impl Temp {
    pub fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("ember-reports-test-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        assert_eq!(self.0.parent(), Some(std::env::temp_dir().as_path()));
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

pub fn id(text: &str) -> ReportId {
    ReportId::parse(text).unwrap()
}
pub fn meta() -> Value {
    json!({ "schema": 1, "kind": "crash", "app_version": "0.8.0", "channel": "beta", "build_id": "a".repeat(64),
        "source_revision": "0123456789abcdef", "windows_version": "Windows 11 24H2", "exception_code": "0xc0000005",
        "crash_address": "0x401234", "comment": "It stopped after joining." })
}
pub fn report() -> Report {
    Report {
        meta: serde_json::from_value(meta()).unwrap(),
        logs: BTreeMap::new(),
        minidump: None,
    }
}
pub fn worker() -> Worker {
    Worker::new(PathBuf::from(env!("CARGO_BIN_EXE_ember-reports")))
}
