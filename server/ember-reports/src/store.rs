//! Only raw dumps and pending event JSON are persisted. Atomic writes use 0600.
use crate::{config::Limits, event::EVENT_BYTES};
use std::{
    fmt, io,
    path::{Path, PathBuf},
    time::{Duration, SystemTime},
};
use tokio::{
    fs,
    io::AsyncWriteExt,
    sync::{Mutex, MutexGuard},
};

pub struct Store {
    pub root: PathBuf,
    limits: Limits,
    lock: Mutex<Cursor>,
    #[cfg(test)]
    deletion_fault: std::sync::Mutex<Option<PathBuf>>,
}
#[derive(Default)]
struct Cursor {
    after: Option<PathBuf>,
}

// The lock is the capacity reservation. Competing reservations and delivery
// cannot change capacity until commit/drop. It is held only for storage I/O,
// never for symbolication or backend HTTP.
pub struct Reservation<'a> {
    store: &'a Store,
    id: ReportId,
    bytes: usize,
    _lock: MutexGuard<'a, Cursor>,
}
struct Entry {
    id: ReportId,
    path: PathBuf,
    modified: SystemTime,
    bytes: u64,
}
/// A report and outbox file name: 32 hex digits, validated once where it enters.
/// Generated IDs are lowercase UUID hex, the form clients receive.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ReportId(String);
impl ReportId {
    pub fn generate() -> Self {
        Self(uuid::Uuid::new_v4().simple().to_string())
    }
    pub fn parse(text: &str) -> Option<Self> {
        (text.len() == 32 && text.bytes().all(|b| b.is_ascii_hexdigit()))
            .then(|| Self(text.to_owned()))
    }
    pub fn as_str(&self) -> &str {
        &self.0
    }
}
impl fmt::Display for ReportId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
fn report_id(path: &Path) -> Option<ReportId> {
    ReportId::parse(path.file_stem()?.to_str()?)
}
fn error(message: &'static str) -> io::Error {
    io::Error::other(message)
}

async fn entries(directory: &Path, extension: &str) -> io::Result<Vec<Entry>> {
    let mut reader = fs::read_dir(directory).await?;
    let mut result = Vec::new();
    while let Some(entry) = reader.next_entry().await? {
        let path = entry.path();
        let metadata = fs::symlink_metadata(&path).await?;
        // The service owns dumps/outbox (0700); never follow symlinks or delete
        // arbitrary files placed there by an operator.
        if metadata.is_file()
            && path.extension().is_some_and(|e| e == extension)
            && let Some(id) = report_id(&path)
        {
            result.push(Entry {
                id,
                path,
                modified: metadata.modified()?,
                bytes: metadata.len(),
            });
        }
    }
    result.sort_by(|a, b| (a.modified, &a.path).cmp(&(b.modified, &b.path)));
    Ok(result)
}
async fn stage(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let temporary = path.with_extension("tmp");
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    options.mode(0o600);
    let mut file = options.open(&temporary).await?;
    let result = async {
        file.write_all(bytes).await?;
        file.sync_all().await?;
        drop(file);
        Ok(())
    }
    .await;
    if result.is_err() {
        let _ = fs::remove_file(&temporary).await;
    }
    result
}

async fn publish(path: &Path) -> io::Result<()> {
    fs::rename(path.with_extension("tmp"), path).await?;
    #[cfg(unix)]
    {
        let directory = path.parent().unwrap().to_owned();
        tokio::task::spawn_blocking(move || std::fs::File::open(directory)?.sync_all())
            .await
            .map_err(|_| error("directory sync task failed"))??;
    }
    Ok(())
}

impl Reservation<'_> {
    pub async fn commit(self, event: &[u8], dump: Option<&[u8]>) -> io::Result<()> {
        if event.len() != self.bytes
            || dump.is_some_and(|d| {
                d.len() > crate::intake::DUMP_BYTES || d.len() as u64 > self.store.limits.dump_bytes
            })
        {
            return Err(error("invalid reserved report"));
        }
        let event_path = self
            .store
            .root
            .join("outbox")
            .join(format!("{}.json", self.id));
        let dump_path = self.store.dump_path(&self.id);
        if dump.is_some() {
            self.store.admit_dump().await?;
        }
        let result = async {
            // Complete both staged, synced writes before publishing or pruning.
            if let Some(dump) = dump {
                stage(&dump_path, dump).await?;
            }
            stage(&event_path, event).await?;
            if dump.is_some() {
                publish(&dump_path).await?;
            }
            publish(&event_path).await
        }
        .await;
        if result.is_err() {
            for path in [&event_path, &dump_path] {
                let _ = fs::remove_file(path.with_extension("tmp")).await;
                let _ = fs::remove_file(path).await;
            }
            return result;
        }
        // The report is now durable. Retention failure cannot turn an accepted
        // write into a rejection after older evidence has already been pruned;
        // periodic retention retries it. Further dump commits are blocked while
        // storage is over budget. Protect this report from clock skew.
        if let Some(dump) = dump {
            let _ = self
                .store
                .prune(SystemTime::now(), dump.len() as u64, 1, Some(&dump_path))
                .await;
        }
        Ok(())
    }
}

impl Store {
    pub async fn new(root: PathBuf, limits: Limits) -> io::Result<Self> {
        for sub in ["dumps", "outbox", "symbols"] {
            let path = root.join(sub);
            fs::create_dir_all(&path).await?;
            #[cfg(unix)]
            if sub != "symbols" {
                use std::os::unix::fs::PermissionsExt;
                fs::set_permissions(path, std::fs::Permissions::from_mode(0o700)).await?;
            }
        }
        let store = Self {
            root,
            limits,
            lock: Mutex::new(Cursor::default()),
            #[cfg(test)]
            deletion_fault: std::sync::Mutex::new(None),
        };
        // A killed write has no accepted report ID; remove its private temporary
        // file on restart. Durable .json events remain untouched.
        for sub in ["dumps", "outbox"] {
            let mut dir = fs::read_dir(store.root.join(sub)).await?;
            while let Some(entry) = dir.next_entry().await? {
                let path = entry.path();
                if path.extension().is_some_and(|s| s == "tmp")
                    && report_id(&path).is_some()
                    && fs::symlink_metadata(&path).await?.is_file()
                {
                    fs::remove_file(path).await?;
                }
            }
        }
        store.prune(SystemTime::now(), 0, 0, None).await?;
        Ok(store)
    }
    // Called under the reservation lock, before staging either file. Permit
    // one replacement dump beyond the retained budget so failed publication
    // never requires deleting older evidence. Headroom is at most one file and
    // min(DUMP_BYTES, dump_bytes) bytes, enforced by commit's input validation.
    // Count leftover staging files too: failed cleanup must not renew headroom.
    async fn admit_dump(&self) -> io::Result<()> {
        let directory = self.root.join("dumps");
        let mut files = entries(&directory, "dmp").await?;
        files.extend(entries(&directory, "tmp").await?);
        if self.limits.dump_count == 0
            || files.len() > self.limits.dump_count
            || files.iter().map(|e| e.bytes).sum::<u64>() > self.limits.dump_bytes
        {
            return Err(error("dump storage over budget"));
        }
        Ok(())
    }
    async fn prune(
        &self,
        now: SystemTime,
        reserve_bytes: u64,
        reserve_count: usize,
        protected: Option<&Path>,
    ) -> io::Result<()> {
        let mut files = entries(&self.root.join("dumps"), "dmp").await?;
        files.retain(|entry| Some(entry.path.as_path()) != protected);
        let age = Duration::from_secs(self.limits.dump_days * 86400);
        let mut total: u64 = files.iter().map(|e| e.bytes).sum();
        let mut count = files.len();
        for file in files.drain(..) {
            if now.duration_since(file.modified).unwrap_or_default() >= age
                || total + reserve_bytes > self.limits.dump_bytes
                || count + reserve_count > self.limits.dump_count
            {
                // Per-store fault injection keeps retention tests deterministic,
                // including when run as root; staging/publication still use disk.
                #[cfg(test)]
                if self.deletion_fault.lock().unwrap().as_ref() == Some(&file.path) {
                    return Err(io::Error::new(
                        io::ErrorKind::PermissionDenied,
                        "injected dump deletion failure",
                    ));
                }
                fs::remove_file(file.path).await?;
                total -= file.bytes;
                count -= 1;
            }
        }
        Ok(())
    }
    pub async fn prune_dumps(&self, now: SystemTime) -> io::Result<()> {
        let _lock = self.lock.lock().await;
        self.prune(now, 0, 0, None).await
    }
    pub fn dump_path(&self, id: &ReportId) -> PathBuf {
        self.root.join("dumps").join(format!("{id}.dmp"))
    }
    pub async fn reserve(&self, id: &ReportId, bytes: usize) -> io::Result<Reservation<'_>> {
        if bytes >= EVENT_BYTES {
            return Err(error("oversized event"));
        }
        let lock = self.lock.lock().await;
        let files = entries(&self.root.join("outbox"), "json").await?;
        if files.len() >= self.limits.outbox_count
            || files.iter().map(|e| e.bytes).sum::<u64>() + bytes as u64 > self.limits.outbox_bytes
        {
            return Err(error("outbox full"));
        }
        for path in [
            self.dump_path(id),
            self.root.join("outbox").join(format!("{id}.json")),
        ] {
            for path in [&path, &path.with_extension("tmp")] {
                match fs::symlink_metadata(path).await {
                    Err(e) if e.kind() == io::ErrorKind::NotFound => {}
                    Err(e) => return Err(e),
                    Ok(_) => return Err(error("report id already exists")),
                }
            }
        }
        Ok(Reservation {
            store: self,
            id: id.clone(),
            bytes,
            _lock: lock,
        })
    }
    pub async fn enqueue(&self, id: &ReportId, bytes: &[u8]) -> io::Result<()> {
        self.reserve(id, bytes.len())
            .await?
            .commit(bytes, None)
            .await
    }
    pub async fn pending(&self) -> io::Result<Vec<ReportId>> {
        let mut cursor = self.lock.lock().await;
        let mut files = entries(&self.root.join("outbox"), "json").await?;
        // Stable lexical cursor, independent of mtime and deletion of the last
        // delivered entry. Every retained failure is retried without starving
        // files outside the first batch of 16.
        files.sort_by(|a, b| a.path.cmp(&b.path));
        let start = cursor
            .after
            .as_ref()
            .map_or(0, |after| files.partition_point(|e| &e.path <= after));
        let selected: Vec<_> = files[start..]
            .iter()
            .chain(files[..start].iter())
            .take(16)
            .collect();
        cursor.after = selected.last().map(|e| e.path.clone());
        Ok(selected.into_iter().map(|e| e.id.clone()).collect())
    }
    pub async fn event(&self, id: &ReportId) -> io::Result<Option<Vec<u8>>> {
        let _lock = self.lock.lock().await;
        let path = self.root.join("outbox").join(format!("{id}.json"));
        let mut file = match fs::File::open(path).await {
            Ok(file) => file,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(e),
        };
        let mut bytes = Vec::new();
        use tokio::io::AsyncReadExt;
        (&mut file)
            .take(EVENT_BYTES as u64)
            .read_to_end(&mut bytes)
            .await?;
        if bytes.len() >= EVENT_BYTES {
            return Err(error("oversized outbox event"));
        }
        Ok(Some(bytes))
    }
    pub async fn delivered(&self, id: &ReportId) -> io::Result<()> {
        let _lock = self.lock.lock().await;
        fs::remove_file(self.root.join("outbox").join(format!("{id}.json"))).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Temp(PathBuf);
    impl Temp {
        fn new() -> Self {
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
    fn repeated(digit: char) -> ReportId {
        ReportId::parse(&digit.to_string().repeat(32)).unwrap()
    }

    #[test]
    fn report_ids_are_exactly_32_hex_digits() {
        for valid in [
            "0123456789abcdef0123456789ABCDEF".to_owned(),
            "f".repeat(32),
        ] {
            assert_eq!(ReportId::parse(&valid).unwrap().as_str(), valid);
        }
        for invalid in [
            String::new(),
            "a".repeat(31),
            "a".repeat(33),
            format!("{}g", "a".repeat(31)),
            format!("../{}", "a".repeat(29)),
            "00112233-4455-6677-8899-aabbccdd".to_owned(),
        ] {
            assert!(ReportId::parse(&invalid).is_none(), "{invalid}");
        }
        let generated = ReportId::generate();
        assert!(ReportId::parse(generated.as_str()).is_some());
        assert!(!generated.as_str().bytes().any(|b| b.is_ascii_uppercase()));
    }

    #[tokio::test]
    async fn persistent_retention_failure_bounds_dump_admission_until_recovery() {
        // Isolate count and byte limits, then exercise both together. Outbox
        // delivery frees slots throughout; it cannot mask dump admission bugs.
        for (dump_count, dump_bytes) in [(1, 32), (4, 8), (1, 8)] {
            let temp = Temp::new();
            let store = Store::new(
                temp.0.clone(),
                Limits {
                    outbox_count: 1,
                    dump_count,
                    dump_bytes,
                    ..Limits::default()
                },
            )
            .await
            .unwrap();
            let a = repeated('a');
            let b = repeated('b');
            store
                .reserve(&a, 2)
                .await
                .unwrap()
                .commit(b"{}", Some(b"MDMP0001"))
                .await
                .unwrap();
            std::fs::File::options()
                .write(true)
                .open(store.dump_path(&a))
                .unwrap()
                .set_modified(SystemTime::now() - Duration::from_secs(60))
                .unwrap();
            store.delivered(&a).await.unwrap();
            *store.deletion_fault.lock().unwrap() = Some(store.dump_path(&a));

            // B remains accepted even though pruning A fails after publication.
            store
                .reserve(&b, 2)
                .await
                .unwrap()
                .commit(b"{}", Some(b"MDMP0002"))
                .await
                .unwrap();
            assert_eq!(store.event(&b).await.unwrap().unwrap(), b"{}");
            store.delivered(&b).await.unwrap();

            for attempt in 0..16 {
                let id = ReportId::parse(&format!("{attempt:032x}")).unwrap();
                assert!(store.prune_dumps(SystemTime::now()).await.is_err());
                assert!(
                    store
                        .reserve(&id, 2)
                        .await
                        .unwrap()
                        .commit(b"{}", Some(b"MDMPmore"))
                        .await
                        .is_err()
                );
                assert!(store.event(&id).await.unwrap().is_none());
                let files: Vec<_> = std::fs::read_dir(temp.0.join("dumps"))
                    .unwrap()
                    .map(|e| e.unwrap())
                    .collect();
                assert_eq!(files.len(), 2); // No new dump or staging leftovers.
                let bytes: u64 = files.iter().map(|e| e.metadata().unwrap().len()).sum();
                assert_eq!(bytes, 16);
                assert!(files.len() <= dump_count + 1);
                assert!(bytes <= dump_bytes + 8);
                assert_eq!(std::fs::read(store.dump_path(&a)).unwrap(), b"MDMP0001");
                assert_eq!(std::fs::read(store.dump_path(&b)).unwrap(), b"MDMP0002");
                // New event writes still succeed while deletion keeps failing.
                store.enqueue(&id, b"{}").await.unwrap();
                store.delivered(&id).await.unwrap();
            }

            *store.deletion_fault.lock().unwrap() = None;
            store.prune_dumps(SystemTime::now()).await.unwrap();
            assert!(!store.dump_path(&a).exists());
            assert_eq!(std::fs::read(store.dump_path(&b)).unwrap(), b"MDMP0002");
            let c = repeated('c');
            store
                .reserve(&c, 2)
                .await
                .unwrap()
                .commit(b"{}", Some(b"MDMP0003"))
                .await
                .unwrap();
            assert_eq!(store.event(&c).await.unwrap().unwrap(), b"{}");
            assert_eq!(std::fs::read(store.dump_path(&c)).unwrap(), b"MDMP0003");
            assert_eq!(std::fs::read_dir(temp.0.join("dumps")).unwrap().count(), 1);
        }
    }
}
