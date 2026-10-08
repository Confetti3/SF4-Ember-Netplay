//! Only raw dumps and pending event JSON are persisted. Atomic writes use 0600.
use crate::{config::Limits, event::EVENT_BYTES};
use std::{
    io,
    path::{Path, PathBuf},
    time::{Duration, SystemTime},
};
use tokio::{fs, io::AsyncWriteExt, sync::Mutex};

pub struct Store {
    pub root: PathBuf,
    limits: Limits,
    lock: Mutex<()>,
}
struct Entry {
    path: PathBuf,
    modified: SystemTime,
    bytes: u64,
}
fn event_id(id: &str) -> bool {
    id.len() == 32 && id.bytes().all(|b| b.is_ascii_hexdigit())
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
            && path
                .file_stem()
                .and_then(|s| s.to_str())
                .is_some_and(event_id)
        {
            result.push(Entry {
                path,
                modified: metadata.modified()?,
                bytes: metadata.len(),
            });
        }
    }
    result.sort_by_key(|e| e.modified);
    Ok(result)
}
async fn atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
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
        fs::rename(&temporary, path).await?;
        #[cfg(unix)]
        {
            let directory = path.parent().unwrap().to_owned();
            tokio::task::spawn_blocking(move || std::fs::File::open(directory)?.sync_all())
                .await
                .map_err(|_| error("directory sync task failed"))??;
        }
        Ok(())
    }
    .await;
    if result.is_err() {
        let _ = fs::remove_file(&temporary).await;
    }
    result
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
            lock: Mutex::new(()),
        };
        // A killed write has no accepted report ID; remove its private temporary
        // file on restart. Durable .json events remain untouched.
        for sub in ["dumps", "outbox"] {
            let mut dir = fs::read_dir(store.root.join(sub)).await?;
            while let Some(entry) = dir.next_entry().await? {
                let path = entry.path();
                if path.extension().is_some_and(|s| s == "tmp")
                    && path
                        .file_stem()
                        .and_then(|s| s.to_str())
                        .is_some_and(event_id)
                    && fs::symlink_metadata(&path).await?.is_file()
                {
                    fs::remove_file(path).await?;
                }
            }
        }
        store.prune(SystemTime::now(), 0, 0).await?;
        Ok(store)
    }
    async fn prune(
        &self,
        now: SystemTime,
        reserve_bytes: u64,
        reserve_count: usize,
    ) -> io::Result<()> {
        let mut files = entries(&self.root.join("dumps"), "dmp").await?;
        let age = Duration::from_secs(self.limits.dump_days * 86400);
        let mut total: u64 = files.iter().map(|e| e.bytes).sum();
        let mut count = files.len();
        for file in files.drain(..) {
            if now.duration_since(file.modified).unwrap_or_default() >= age
                || total + reserve_bytes > self.limits.dump_bytes
                || count + reserve_count > self.limits.dump_count
            {
                fs::remove_file(file.path).await?;
                total -= file.bytes;
                count -= 1;
            }
        }
        Ok(())
    }
    pub async fn prune_dumps(&self, now: SystemTime) -> io::Result<()> {
        let _lock = self.lock.lock().await;
        self.prune(now, 0, 0).await
    }
    pub async fn dump(&self, id: &str, bytes: &[u8]) -> io::Result<PathBuf> {
        if !event_id(id) || bytes.len() > crate::intake::DUMP_BYTES {
            return Err(error("invalid dump"));
        }
        let _lock = self.lock.lock().await;
        self.prune(SystemTime::now(), bytes.len() as u64, 1).await?;
        let path = self.root.join("dumps").join(format!("{id}.dmp"));
        atomic(&path, bytes).await?;
        Ok(path)
    }
    pub async fn remove_dump(&self, id: &str) {
        if event_id(id) {
            let _lock = self.lock.lock().await;
            let _ = fs::remove_file(self.root.join("dumps").join(format!("{id}.dmp"))).await;
        }
    }
    pub async fn enqueue(&self, id: &str, bytes: &[u8]) -> io::Result<()> {
        if !event_id(id) || bytes.len() >= EVENT_BYTES {
            return Err(error("invalid event"));
        }
        let _lock = self.lock.lock().await;
        let files = entries(&self.root.join("outbox"), "json").await?;
        if files.len() >= self.limits.outbox_count
            || files.iter().map(|e| e.bytes).sum::<u64>() + bytes.len() as u64
                > self.limits.outbox_bytes
        {
            return Err(error("outbox full"));
        }
        atomic(&self.root.join("outbox").join(format!("{id}.json")), bytes).await
    }
    pub async fn pending(&self) -> io::Result<Vec<String>> {
        let _lock = self.lock.lock().await;
        Ok(entries(&self.root.join("outbox"), "json")
            .await?
            .iter()
            .take(16)
            .filter_map(|e| e.path.file_stem()?.to_str().map(str::to_owned))
            .collect())
    }
    pub async fn event(&self, id: &str) -> io::Result<Option<Vec<u8>>> {
        if !event_id(id) {
            return Err(error("invalid id"));
        }
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
    pub async fn delivered(&self, id: &str) -> io::Result<()> {
        if !event_id(id) {
            return Err(error("invalid id"));
        }
        let _lock = self.lock.lock().await;
        fs::remove_file(self.root.join("outbox").join(format!("{id}.json"))).await
    }
}
