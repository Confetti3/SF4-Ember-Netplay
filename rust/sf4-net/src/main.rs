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

#[cfg(not(windows))]
fn main() {
    eprintln!("sf4-net requires 64-bit Windows and supervised launcher startup");
    std::process::exit(1);
}
