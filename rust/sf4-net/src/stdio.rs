//! `sf4-net --stdio`: the helper as a child of a headless room host.
//!
//! The parent created the pipes, so there is no pipe name, bootstrap or
//! authentication step: commands arrive on stdin and events leave on stdout,
//! framed like the Windows named pipe (`wire::read_ipc` / `wire::write_ipc`).
//! The first command's message ID must be above 1, as on the pipe, where ID 1
//! is the authentication exchange.
//!
//! Stdout belongs to the frames alone. Diagnostics go to stderr.
//!
//! When stdin ends the parent is gone or has closed its end, and the helper
//! stops as if it had been sent `Command::Shutdown`.
use std::io;

use iroh::Endpoint;
use tokio::io::{AsyncRead, AsyncWrite};

use crate::{
    service,
    wire::{self, ControlFrame},
};

const FEED_BUFFER: usize = 64 * 1024;
const SHUTDOWN: &[u8] = br#"{"type":"shutdown"}"#;

#[derive(Debug, Default, PartialEq, Eq)]
pub struct Options {
    /// Exact UDP port for the primary endpoint; `None` keeps the default policy.
    pub bind_port: Option<u16>,
    /// Exact UDP port for the per-room coordination endpoint when hosting;
    /// `None` keeps the default policy.
    pub coordination_port: Option<u16>,
    pub relay_only: bool,
}

/// One `--<name> <u16>` value, from 1 to 65535, given at most once.
fn port_option(
    name: &str,
    args: &mut impl Iterator<Item = String>,
    slot: &mut Option<u16>,
) -> Result<(), String> {
    let value = args.next().ok_or(format!("{name} needs a port"))?;
    let port: u16 = value
        .parse()
        .map_err(|_| format!("invalid {name} {value:?}"))?;
    if port == 0 || slot.replace(port).is_some() {
        return Err(format!("{name} takes one port from 1 to 65535"));
    }
    Ok(())
}

/// Parses everything after the program name:
/// `--stdio [--bind-port <u16>] [--coordination-port <u16>] [--relay-only]`.
pub fn parse_args<I: IntoIterator<Item = String>>(args: I) -> Result<Options, String> {
    let mut args = args.into_iter();
    if args.next().as_deref() != Some("--stdio") {
        return Err("expected --stdio".into());
    }
    let mut options = Options::default();
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--bind-port" => port_option("--bind-port", &mut args, &mut options.bind_port)?,
            "--coordination-port" => port_option(
                "--coordination-port",
                &mut args,
                &mut options.coordination_port,
            )?,
            "--relay-only" if !options.relay_only => options.relay_only = true,
            other => return Err(format!("unexpected argument {other:?}")),
        }
    }
    if options.relay_only && (options.bind_port.is_some() || options.coordination_port.is_some()) {
        return Err("a bind port cannot be combined with --relay-only".into());
    }
    // Two UDP sockets cannot share a port.
    if options.bind_port.is_some() && options.bind_port == options.coordination_port {
        return Err("--bind-port and --coordination-port must differ".into());
    }
    Ok(options)
}

/// Runs the service over `input` (commands) and `output` (events). Returns
/// `Ok` after a `shutdown` command or after `input` ends, `Err` otherwise.
pub async fn serve<R, W>(
    input: R,
    output: W,
    endpoint: Endpoint,
    relay_only: bool,
) -> io::Result<()>
where
    R: AsyncRead + Unpin + Send + 'static,
    W: AsyncWrite + Unpin + Send + 'static,
{
    serve_on_port(input, output, endpoint, relay_only, None).await
}

/// `serve` with the exact UDP port for the coordination endpoint of a room
/// this process hosts (`--coordination-port`).
pub async fn serve_on_port<R, W>(
    mut input: R,
    output: W,
    endpoint: Endpoint,
    relay_only: bool,
    coordination_port: Option<u16>,
) -> io::Result<()>
where
    R: AsyncRead + Unpin + Send + 'static,
    W: AsyncWrite + Unpin + Send + 'static,
{
    let (mut feed, commands) = tokio::io::duplex(FEED_BUFFER);
    let forwarder = tokio::spawn(async move { forward(&mut input, &mut feed).await });
    let result = service::run_on_port(
        tokio::io::join(commands, output),
        endpoint,
        relay_only,
        coordination_port,
    )
    .await;
    forwarder.abort();
    result
}

/// Copies command frames to the service and turns the end of `input` into a
/// shutdown command. After that it keeps the feed open: the service reads a
/// closed feed as a lost IPC connection, which would race the shutdown.
async fn forward<R, W>(input: &mut R, feed: &mut W) -> io::Result<()>
where
    R: AsyncRead + Unpin,
    W: AsyncWrite + Unpin,
{
    let mut last_id = 1;
    loop {
        match wire::read_ipc(input).await {
            Ok(frame) => {
                last_id = last_id.max(frame.message_id);
                wire::write_ipc(feed, &frame).await?;
            }
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => break,
            // A malformed frame closes the feed, which fails the service.
            Err(error) => return Err(error),
        }
    }
    wire::write_ipc(
        feed,
        &ControlFrame {
            message_id: last_id.saturating_add(1).min(i64::MAX as u64),
            payload: SHUTDOWN.to_vec(),
        },
    )
    .await?;
    std::future::pending().await
}

/// Process entry for `--stdio` (arguments after the program name). Returns the
/// exit code: 0 after a clean stop, 1 on failure, 2 on bad arguments.
#[cfg(unix)]
pub fn main<I: IntoIterator<Item = String>>(args: I) -> i32 {
    use std::time::Duration;

    let options = match parse_args(args) {
        Ok(options) => options,
        Err(message) => {
            eprintln!("sf4-net: {message}");
            eprintln!(
                "usage: sf4-net --stdio [--bind-port <port>] [--coordination-port <port>] [--relay-only]"
            );
            return 2;
        }
    };
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => {
            eprintln!("sf4-net: runtime: {error}");
            return 1;
        }
    };
    let result = runtime.block_on(async {
        let (input, output) = stdio_pipes()?;
        let endpoint = match options.bind_port {
            Some(port) => crate::transport::bind_endpoint_on_port(port).await?,
            None => crate::transport::bind_endpoint_with_policy(options.relay_only).await?,
        };
        serve_on_port(
            input,
            output,
            endpoint,
            options.relay_only,
            options.coordination_port,
        )
        .await
    });
    // Nothing the runtime still holds may keep a stopped helper alive.
    runtime.shutdown_timeout(Duration::from_secs(3));
    match result {
        Ok(()) => 0,
        Err(error) => {
            eprintln!("sf4-net: {error}");
            1
        }
    }
}

/// Stdin and stdout as non-blocking pipe ends. They must be pipes, which the
/// parent creates; the descriptors are duplicated so the std handles stay put.
#[cfg(unix)]
fn stdio_pipes() -> io::Result<(
    tokio::net::unix::pipe::Receiver,
    tokio::net::unix::pipe::Sender,
)> {
    use std::os::fd::AsFd;
    use tokio::net::unix::pipe::{Receiver, Sender};

    let input = io::stdin().as_fd().try_clone_to_owned()?;
    let output = io::stdout().as_fd().try_clone_to_owned()?;
    Ok((
        Receiver::from_owned_fd(input)?,
        Sender::from_owned_fd(output)?,
    ))
}

#[cfg(test)]
mod tests {
    use std::{net::Ipv4Addr, time::Duration};

    use iroh::endpoint::presets;
    use tokio::{
        io::{DuplexStream, duplex},
        time::timeout,
    };

    use super::*;

    fn args(list: &[&str]) -> Result<Options, String> {
        parse_args(list.iter().map(|argument| argument.to_string()))
    }

    #[test]
    fn arguments_are_strict() {
        assert_eq!(args(&["--stdio"]), Ok(Options::default()));
        assert_eq!(
            args(&["--stdio", "--bind-port", "45760"]),
            Ok(Options {
                bind_port: Some(45760),
                ..Options::default()
            })
        );
        assert_eq!(
            args(&["--stdio", "--relay-only"]),
            Ok(Options {
                relay_only: true,
                ..Options::default()
            })
        );
        assert_eq!(
            args(&["--stdio", "--coordination-port", "45770"]),
            Ok(Options {
                coordination_port: Some(45770),
                ..Options::default()
            })
        );
        assert_eq!(
            args(&["--stdio", "--coordination-port", "45770", "--bind-port", "45760"]),
            Ok(Options {
                bind_port: Some(45760),
                coordination_port: Some(45770),
                relay_only: false
            })
        );
        for bad in [
            &["--stdio", "--coordination-port"][..],
            &["--stdio", "--coordination-port", "0"],
            &["--stdio", "--coordination-port", "65536"],
            &["--stdio", "--coordination-port", "1", "--coordination-port", "2"],
            &["--stdio", "--coordination-port", "45760", "--relay-only"],
            &["--stdio", "--bind-port", "45760", "--coordination-port", "45760"],
        ] {
            assert!(args(bad).is_err(), "{bad:?} must be refused");
        }
        for bad in [
            &[][..],
            &["--pipe", "x"],
            &["--bind-port", "1", "--stdio"],
            &["--stdio", "--bind-port"],
            &["--stdio", "--bind-port", "0"],
            &["--stdio", "--bind-port", "65536"],
            &["--stdio", "--bind-port", "x"],
            &["--stdio", "--bind-port", "1", "--bind-port", "2"],
            &["--stdio", "--relay-only", "--relay-only"],
            &["--stdio", "--relay-only", "--bind-port", "45760"],
            &["--stdio", "--bind-port", "45760", "--relay-only"],
            &["--stdio", "extra"],
        ] {
            assert!(args(bad).is_err(), "{bad:?} must be refused");
        }
    }

    async fn endpoint() -> Endpoint {
        Endpoint::builder(presets::Minimal)
            .clear_ip_transports()
            .bind_addr((Ipv4Addr::LOCALHOST, 0))
            .unwrap()
            .bind()
            .await
            .unwrap()
    }

    fn command(id: u64, json: &str) -> ControlFrame {
        ControlFrame {
            message_id: id,
            payload: json.as_bytes().to_vec(),
        }
    }

    /// Event types up to and including `stopped`, or until the stream ends.
    async fn events_until_stopped(output: &mut DuplexStream) -> Vec<String> {
        let mut seen = Vec::new();
        while let Ok(frame) = wire::read_ipc(output).await {
            let value: serde_json::Value = serde_json::from_slice(&frame.payload).unwrap();
            let kind = value["type"].as_str().unwrap().to_string();
            seen.push(kind.clone());
            if kind == "stopped" {
                break;
            }
        }
        seen
    }

    #[tokio::test]
    async fn a_shutdown_command_stops_the_service_cleanly() {
        timeout(Duration::from_secs(20), async {
            let ((mut commands, input), (mut events, output)) = (duplex(4096), duplex(4096));
            let service = tokio::spawn(serve(input, output, endpoint().await, false));
            wire::write_ipc(&mut commands, &command(2, r#"{"type":"shutdown"}"#))
                .await
                .unwrap();
            assert_eq!(
                events_until_stopped(&mut events).await.last().unwrap(),
                "stopped"
            );
            service.await.unwrap().unwrap();
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn the_end_of_input_stops_the_service_cleanly_after_earlier_commands() {
        timeout(Duration::from_secs(20), async {
            let ((mut commands, input), (mut events, output)) = (duplex(4096), duplex(4096));
            let service = tokio::spawn(serve(input, output, endpoint().await, false));
            wire::write_ipc(&mut commands, &command(2, r#"{"type":"status"}"#))
                .await
                .unwrap();
            drop(commands);
            let seen = events_until_stopped(&mut events).await;
            assert_eq!(seen.last().unwrap(), "stopped");
            assert!(seen.iter().any(|kind| kind == "status"), "{seen:?}");
            service.await.unwrap().unwrap();
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn input_that_ends_inside_a_frame_also_stops_cleanly() {
        timeout(Duration::from_secs(20), async {
            let ((mut commands, input), (mut events, output)) = (duplex(4096), duplex(4096));
            let service = tokio::spawn(serve(input, output, endpoint().await, false));
            tokio::io::AsyncWriteExt::write_all(&mut commands, &[0, 0, 0, 30, 0])
                .await
                .unwrap();
            drop(commands);
            assert_eq!(
                events_until_stopped(&mut events).await.last().unwrap(),
                "stopped"
            );
            service.await.unwrap().unwrap();
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn a_malformed_frame_fails_the_service() {
        timeout(Duration::from_secs(20), async {
            let ((mut commands, input), (_events, output)) = (duplex(4096), duplex(4096));
            let service = tokio::spawn(serve(input, output, endpoint().await, false));
            // A frame length of zero is out of bounds.
            tokio::io::AsyncWriteExt::write_all(&mut commands, &[0; 16])
                .await
                .unwrap();
            assert!(service.await.unwrap().is_err());
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn an_exact_port_binds_or_fails() {
        let free = std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0))
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        let held = std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).unwrap();
        let held_port = held.local_addr().unwrap().port();
        let refused = crate::transport::bind_endpoint_on_port(held_port).await;
        let message = refused
            .expect_err("a held port must not be bound")
            .to_string();
        assert!(message.contains(&held_port.to_string()), "{message}");
        drop(held);

        let endpoint = crate::transport::bind_endpoint_on_port(free).await.unwrap();
        let ports: Vec<_> = endpoint.bound_sockets().iter().map(|s| s.port()).collect();
        assert!(ports.contains(&free), "{ports:?}");
        endpoint.close().await;
    }
}
