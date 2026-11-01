#![cfg_attr(windows, windows_subsystem = "windows")]

#[cfg(windows)]
fn main() {
    if start().is_err() {
        std::process::exit(1);
    }
}

#[cfg(windows)]
fn start() -> std::io::Result<()> {
    use sf4_net::{ipc, service, transport};
    let mut args = std::env::args().skip(1);
    if args.next().as_deref() != Some("--pipe") {
        return Err(std::io::Error::other("supervised startup required"));
    }
    let name = args
        .next()
        .ok_or_else(|| std::io::Error::other("missing pipe name"))?;
    let relay_only = match args.next().as_deref() {
        None => false,
        Some("--relay-only") => true,
        _ => return Err(std::io::Error::other("invalid helper option")),
    };
    if args.next().is_some() || !ipc::valid_pipe_name(&name) {
        return Err(std::io::Error::other("invalid pipe name"));
    }
    let bootstrap = ipc::read_inherited_bootstrap()?;
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()?
        .block_on(async {
            let pipe = ipc::create_server(&name)?;
            let pipe = ipc::authenticate(pipe, &bootstrap).await?;
            drop(bootstrap);
            let endpoint = transport::bind_endpoint_with_policy(relay_only).await?;
            service::run(pipe, endpoint, relay_only).await
        })
}

#[cfg(unix)]
fn main() {
    return_freed_memory();
    std::process::exit(sf4_net::stdio::main(std::env::args().skip(1)));
}

/// A room server runs one helper per room. glibc keeps a freed block that it
/// took from its heap, and raises its mmap threshold to the largest block
/// freed so far, so the short-lived buffers of every member that joins
/// (about 9 MB) stayed held for good: a room rejoined four times sat at
/// 50 MB instead of 10. One arena, a fixed 128 KiB mmap threshold and a
/// 128 KiB trim threshold hand those blocks back as soon as they are freed.
/// Set before the runtime starts any thread.
#[cfg(all(target_os = "linux", target_env = "gnu"))]
fn return_freed_memory() {
    unsafe extern "C" {
        fn mallopt(param: std::ffi::c_int, value: std::ffi::c_int) -> std::ffi::c_int;
    }
    const M_TRIM_THRESHOLD: std::ffi::c_int = -1;
    const M_MMAP_THRESHOLD: std::ffi::c_int = -3;
    const M_ARENA_MAX: std::ffi::c_int = -8;
    // SAFETY: mallopt only changes allocator tuning; it is called before any
    // other thread exists.
    unsafe {
        mallopt(M_ARENA_MAX, 1);
        mallopt(M_MMAP_THRESHOLD, 128 * 1024);
        mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    }
}

#[cfg(all(unix, not(all(target_os = "linux", target_env = "gnu"))))]
fn return_freed_memory() {}

#[cfg(not(any(windows, unix)))]
fn main() {
    eprintln!("sf4-net requires 64-bit Windows and supervised launcher startup");
    std::process::exit(1);
}
