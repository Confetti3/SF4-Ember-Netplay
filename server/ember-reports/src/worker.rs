//! One disposable Linux process per dump. No untrusted parser runs in intake.
use crate::symbolicate::{Walk, raw_facts};
#[cfg(target_os = "linux")]
use std::io;
use std::{path::PathBuf, time::Duration};

// Four workers consume at most 256 MiB of address space in total, leaving
// headroom for intake/delivery inside the service's 512 MiB cgroup.
pub const MEMORY_BYTES: u64 = 64 * 1024 * 1024;
#[cfg(target_os = "linux")]
const OUTPUT_BYTES: usize = 8 * 1024 * 1024;

pub struct Worker {
    executable: PathBuf,
}
impl Worker {
    pub fn new(executable: PathBuf) -> Self {
        Self { executable }
    }
    pub async fn walk(&self, bytes: Vec<u8>, symbols: PathBuf, deadline: Duration) -> Walk {
        let mut fallback = raw_facts(&bytes);
        #[cfg(target_os = "linux")]
        {
            let mut command = limited_command(&self.executable, deadline);
            command.arg("symbolicate").arg(symbols);
            match exchange(command, bytes, deadline).await {
                Ok(output) => {
                    if let Ok(walk) = serde_json::from_slice(&output) {
                        return walk;
                    }
                    fallback.status = "worker_failed".into();
                }
                Err(Failure::Timeout) => fallback.status = "timeout".into(),
                Err(Failure::Io) => fallback.status = "worker_failed".into(),
            }
        }
        #[cfg(not(target_os = "linux"))]
        {
            // Fail closed: never silently fall back to in-process parsing.
            let _ = (&self.executable, symbols, deadline);
            fallback.status = "worker_unsupported".into();
        }
        fallback
    }
}

#[cfg(target_os = "linux")]
fn limited_command(executable: &std::path::Path, deadline: Duration) -> tokio::process::Command {
    use std::process::Stdio;
    let mut command = tokio::process::Command::new(executable);
    command
        .env_clear()
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .kill_on_drop(true);
    let cpu_seconds = deadline
        .as_secs()
        .saturating_add(u64::from(deadline.subsec_nanos() != 0))
        .max(1);
    // SAFETY: this post-fork hook calls only async-signal-safe setrlimit. No
    // allocation, locks, Rust runtime or parser code runs before exec.
    unsafe {
        command.pre_exec(move || {
            for (resource, value) in [
                (libc::RLIMIT_AS, MEMORY_BYTES),
                (libc::RLIMIT_CPU, cpu_seconds),
                (libc::RLIMIT_CORE, 0),
            ] {
                let limit = libc::rlimit {
                    rlim_cur: value,
                    rlim_max: value,
                };
                if libc::setrlimit(resource, &limit) != 0 {
                    return Err(io::Error::last_os_error());
                }
            }
            Ok(())
        });
    }
    command
}

#[cfg(target_os = "linux")]
#[derive(Debug)]
enum Failure {
    Timeout,
    Io,
}

#[cfg(target_os = "linux")]
async fn exchange(
    mut command: tokio::process::Command,
    bytes: Vec<u8>,
    deadline: Duration,
) -> Result<Vec<u8>, Failure> {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let expires = tokio::time::Instant::now() + deadline;
    let mut child = command.spawn().map_err(|_| Failure::Io)?;
    let (Some(mut input), Some(output)) = (child.stdin.take(), child.stdout.take()) else {
        reap(&mut child).await;
        return Err(Failure::Io);
    };
    let mut output = output.take((OUTPUT_BYTES + 1) as u64);
    let result = tokio::time::timeout_at(expires, async {
        // Drain stdout while writing stdin; neither pipe may block the other.
        let write = async {
            input.write_all(&bytes).await?;
            input.shutdown().await?;
            drop(input);
            Ok::<_, io::Error>(())
        };
        let read = async {
            let mut json = Vec::new();
            output.read_to_end(&mut json).await?;
            if json.len() > OUTPUT_BYTES {
                return Err(io::Error::other("worker output too large"));
            }
            Ok(json)
        };
        let (_, json) = tokio::try_join!(write, read)?;
        if !child.wait().await?.success() {
            return Err(io::Error::other("worker exited unsuccessfully"));
        }
        Ok(json)
    })
    .await;
    match result {
        Ok(Ok(json)) => Ok(json),
        failed => {
            // kill() waits/reaps too, but always wait explicitly even if the
            // child raced with the signal. The caller holds admission until
            // this returns; no timeout/error branch releases a live worker.
            reap(&mut child).await;
            Err(if failed.is_err() {
                Failure::Timeout
            } else {
                Failure::Io
            })
        }
    }
}

#[cfg(target_os = "linux")]
async fn reap(child: &mut tokio::process::Child) {
    loop {
        let _ = child.start_kill();
        if child.wait().await.is_ok() {
            return;
        }
        // Never release the caller's admission on an unconfirmed wait error.
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

// The binary dispatches this mode before reading configuration or credentials.
pub fn run(symbols: PathBuf) -> Result<(), &'static str> {
    use std::io::Read;
    let mut bytes = Vec::new();
    std::io::stdin()
        .lock()
        .take((crate::intake::DUMP_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|_| "cannot read dump")?;
    let runtime = tokio::runtime::Builder::new_current_thread()
        .max_blocking_threads(1)
        .enable_all()
        .build()
        .map_err(|_| "cannot start worker")?;
    let walk = runtime.block_on(crate::symbolicate::walk(bytes, symbols));
    serde_json::to_writer(std::io::stdout().lock(), &walk).map_err(|_| "cannot write frames")
}

#[cfg(all(test, target_os = "linux"))]
mod tests {
    use super::*;

    #[tokio::test]
    async fn deadline_kills_and_reaps_worker() {
        let pid_file =
            std::env::temp_dir().join(format!("ember-worker-pid-{}", uuid::Uuid::new_v4()));
        let mut command = limited_command(std::path::Path::new("/bin/sh"), Duration::from_secs(20));
        command
            .args(["-c", "echo $$ > \"$1\"; while :; do :; done", "worker-test"])
            .arg(&pid_file);
        assert!(matches!(
            exchange(command, vec![], Duration::from_secs(1)).await,
            Err(Failure::Timeout)
        ));
        let pid = std::fs::read_to_string(&pid_file)
            .unwrap()
            .trim()
            .parse::<u32>()
            .unwrap();
        std::fs::remove_file(pid_file).unwrap();
        assert!(!std::path::Path::new(&format!("/proc/{pid}")).exists());
    }

    #[tokio::test]
    async fn worker_limits_are_set_before_exec() {
        let mut command = limited_command(std::path::Path::new("/bin/sh"), Duration::from_secs(2));
        command.args(["-c", "ulimit -v; ulimit -t; ulimit -c"]);
        let output = exchange(command, vec![], Duration::from_secs(2))
            .await
            .unwrap();
        assert_eq!(
            String::from_utf8(output).unwrap(),
            format!("{}\n2\n0\n", MEMORY_BYTES / 1024)
        );
    }
}
