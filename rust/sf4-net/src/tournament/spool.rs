//! The helper's report spool (spec 16.7). A signed game report is written to
//! its own file and flushed to disk before the helper says it is safe, then
//! sent from there until the bridge answers for it. A report that cannot be
//! written is reported as not saved; nothing here makes a native game wait.
use std::{
    fs, io,
    path::{Path, PathBuf},
};

use ember_protocol::{json, report::SignedReport};
use serde::{Deserialize, Serialize};

use super::{Code, Failure};
use crate::identity::durable::{self, Existing};

const FORMAT: &str = "ember-game-report";
const PREFIX: &str = "report-";
/// Pending reports kept at once; a full spool refuses new ones.
pub const MAX_PENDING: usize = 256;
const MAX_FILE: usize = 16 * 1024;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Entry {
    format: String,
    version: u8,
    pub bridge_id: String,
    pub match_id: String,
    pub report: SignedReport,
    pub queued_at: u64,
}

impl Entry {
    pub fn new(bridge_id: &str, match_id: &str, report: SignedReport, queued_at: u64) -> Self {
        Self {
            format: FORMAT.into(),
            version: 1,
            bridge_id: bridge_id.into(),
            match_id: match_id.into(),
            report,
            queued_at,
        }
    }
}

pub struct Spool {
    dir: PathBuf,
}

impl Spool {
    pub fn open(dir: PathBuf) -> Self {
        Self { dir }
    }

    pub(crate) fn usable(&self) -> bool {
        !self.dir.as_os_str().is_empty() && self.dir.is_absolute()
    }

    fn path(&self, entry: &Entry) -> PathBuf {
        self.dir.join(format!(
            "{PREFIX}{}.json",
            entry.report.report.observation_id
        ))
    }

    /// Writes `entry` durably: a flushed temporary file moved into place,
    /// written through to disk. Rewriting the same observation is a no-op.
    pub fn put(&self, entry: &Entry) -> Result<(), Failure> {
        let not_saved = || Failure::new(Code::ReportNotSaved);
        if !self.usable() {
            return Err(not_saved());
        }
        fs::create_dir_all(&self.dir).map_err(|_| not_saved())?;
        let path = self.path(entry);
        if path.exists() {
            return Ok(());
        }
        if self.pending().len() >= MAX_PENDING {
            return Err(not_saved());
        }
        let bytes = serde_json::to_vec(entry).map_err(|_| not_saved())?;
        match durable::replace(&path, &bytes, Existing::Keep) {
            Ok(()) => Ok(()),
            // The same observation, written meanwhile.
            Err(durable::Error::Io(error)) if error.kind() == io::ErrorKind::AlreadyExists => {
                Ok(())
            }
            Err(_) => Err(not_saved()),
        }
    }

    /// Every pending entry, oldest first. Unreadable files are skipped and
    /// left for inspection; they never block the others.
    pub fn pending(&self) -> Vec<Entry> {
        if !self.usable() {
            return Vec::new();
        }
        let Ok(listing) = fs::read_dir(&self.dir) else {
            return Vec::new();
        };
        let mut entries: Vec<Entry> = listing
            .filter_map(Result::ok)
            .filter(|item| item.file_name().to_string_lossy().starts_with(PREFIX))
            .filter_map(|item| read(&item.path()))
            .collect();
        entries.sort_by_key(|entry| entry.queued_at);
        entries
    }

    /// Removes an entry once the bridge has answered for it.
    pub fn done(&self, entry: &Entry) {
        let _ = fs::remove_file(self.path(entry));
    }
}

fn read(path: &Path) -> Option<Entry> {
    let bytes = fs::read(path)
        .ok()
        .filter(|bytes| bytes.len() <= MAX_FILE)?;
    json::parse_as::<Entry>(&bytes, MAX_FILE)
        .ok()
        .filter(|entry| entry.format == FORMAT && entry.version == 1)
}

#[cfg(test)]
mod tests {
    use ember_protocol::{
        SigningIdentity,
        encoding::Counter,
        play::{Observation, PERMIT_START_SECS, Permit, TABLE, VERSION, roster_digest},
        report::Outcome,
    };
    use zeroize::Zeroizing;

    use super::*;

    pub(super) fn report(observation: &str) -> SignedReport {
        let a = SigningIdentity::from_seed(&Zeroizing::new([1; 32])).unwrap();
        let b = SigningIdentity::from_seed(&Zeroizing::new([2; 32])).unwrap();
        let permit = Permit {
            version: VERSION,
            bridge_id: "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11".into(),
            match_id: "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12".into(),
            assignment_generation: Counter(1),
            attempt_id: "ega_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a13".into(),
            permit_id: "per_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a14".into(),
            room_id: "0123456789abcdef0123456789abcdef".into(),
            table_id: TABLE,
            match_generation: Counter(7),
            p1_id: a.ember_id().clone(),
            p2_id: b.ember_id().clone(),
            rules_digest: "A".repeat(43),
            roster_digest: roster_digest(a.ember_id(), b.ember_id()).unwrap(),
            build_id: "build".into(),
            issued_at: 1,
            start_by: 1 + PERMIT_START_SECS,
        };
        permit
            .report(
                a.ember_id(),
                Observation {
                    observation_id: observation.into(),
                    result: Outcome::Cancel,
                    capture_frame: None,
                    confirmed_input_frame: None,
                    observed_at: 2,
                    helper_instance_id: "ins_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a15".into(),
                },
            )
            .unwrap()
            .sign(&a)
            .unwrap()
    }

    #[test]
    fn reports_survive_reopening_until_done() {
        let dir = std::env::temp_dir().join(format!("sf4-spool-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let spool = Spool::open(dir.clone());
        let first = Entry::new(
            "brg_x",
            "emt_x",
            report("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a01"),
            10,
        );
        let second = Entry::new(
            "brg_x",
            "emt_x",
            report("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a02"),
            5,
        );
        spool.put(&first).unwrap();
        spool.put(&second).unwrap();
        // The same observation again is a no-op.
        spool.put(&first).unwrap();
        let reopened = Spool::open(dir.clone());
        let pending = reopened.pending();
        assert_eq!(pending.len(), 2);
        assert_eq!(pending[0].queued_at, 5);
        assert_eq!(pending[0].report, second.report);
        reopened.done(&pending[0]);
        assert_eq!(Spool::open(dir.clone()).pending().len(), 1);
        // A stray unreadable file never blocks the rest.
        fs::write(dir.join("report-broken.json"), b"{").unwrap();
        assert_eq!(Spool::open(dir.clone()).pending().len(), 1);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn no_folder_means_not_saved() {
        let spool = Spool::open(PathBuf::new());
        let entry = Entry::new(
            "brg_x",
            "emt_x",
            report("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a03"),
            1,
        );
        assert!(spool.put(&entry).is_err());
        assert!(spool.pending().is_empty());
    }
}

#[cfg(test)]
mod resend_tests {
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };

    use ember_protocol::{SigningIdentity, encoding::prefixed_id};
    use zeroize::Zeroizing;

    use super::tests::report;
    use super::*;
    use crate::{
        identity::Protection,
        tournament::{builtin::Builtin, play::deliver_pending, test_support::shared_pinning},
    };

    /// A loopback origin that takes each connection and closes it at once,
    /// counting them: a bridge that cannot answer.
    async fn dropping_origin() -> (String, Arc<AtomicUsize>) {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        let taken = Arc::new(AtomicUsize::new(0));
        let counted = taken.clone();
        tokio::spawn(async move {
            while let Ok((socket, _)) = listener.accept().await {
                counted.fetch_add(1, Ordering::SeqCst);
                drop(socket);
            }
        });
        (origin, taken)
    }

    fn pin(origin: String, seed: u8) -> Builtin {
        let key = SigningIdentity::from_seed(&Zeroizing::new([seed; 32]))
            .unwrap()
            .public_key()
            .to_b64u();
        Builtin {
            bridge_id: prefixed_id("brg", [seed; 16]),
            origin,
            display_name: "Bridge".into(),
            keys: vec![(format!("k{seed}"), key)],
        }
    }

    // A pass stops sending to a bridge at its first failure to answer, and
    // still sends the other bridges' reports; the reports all stay.
    #[tokio::test]
    async fn a_pass_stops_at_a_bridge_that_does_not_answer() {
        let dir = std::env::temp_dir().join(prefixed_id("sf4-net-resend", [8; 16]));
        let _ = fs::remove_dir_all(&dir);
        let ((one, asked_one), (two, asked_two)) =
            (dropping_origin().await, dropping_origin().await);
        let (first, second) = (pin(one, 7), pin(two, 9));
        let shared = shared_pinning(&dir, &[first.clone(), second.clone()]);
        shared
            .identity
            .lock()
            .unwrap()
            .enable(Protection::Passphrase("a test passphrase"))
            .unwrap();
        let queued = [
            (&first, 1),
            (&first, 2),
            (&second, 3),
            (&first, 4),
            (&second, 5),
        ];
        for (n, (bridge, at)) in queued.into_iter().enumerate() {
            let entry = Entry::new(
                &bridge.bridge_id,
                "emt_x",
                report(&format!("obs_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9b{n:02}")),
                at,
            );
            shared.spool.put(&entry).unwrap();
        }
        deliver_pending(&shared).await;
        assert_eq!(asked_one.load(Ordering::SeqCst), 1, "one try at the first");
        assert_eq!(asked_two.load(Ordering::SeqCst), 1, "one try at the second");
        assert_eq!(shared.spool.pending().len(), 5);
        let _ = fs::remove_dir_all(&dir);
    }
}
