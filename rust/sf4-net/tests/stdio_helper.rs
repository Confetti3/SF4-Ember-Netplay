#![cfg(unix)]
//! The production binary as a child of a headless room host: framed commands
//! on stdin, framed events on stdout, nothing else on stdout.
use std::{
    io::{self, Read, Write},
    net::{Ipv4Addr, UdpSocket},
    process::{Child, ChildStdin, Command, Stdio},
    sync::mpsc::{self, Receiver},
    thread,
    time::{Duration, Instant},
};

const LIMIT: Duration = Duration::from_secs(60);

enum Output {
    Frame(u64, serde_json::Value),
    /// The stream ended on a frame boundary.
    End,
    /// Bytes that are not a frame.
    Garbage(String),
}

struct Helper {
    child: Child,
    stdin: Option<ChildStdin>,
    frames: Receiver<Output>,
    next_id: u64,
}
impl Drop for Helper {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn free_port() -> u16 {
    UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0))
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

fn read_frames(mut stdout: impl Read, sender: mpsc::Sender<Output>) {
    loop {
        let mut length = [0; 4];
        match stdout.read(&mut length[..1]) {
            Ok(0) => return drop(sender.send(Output::End)),
            Ok(_) => {}
            Err(error) => return drop(sender.send(Output::Garbage(error.to_string()))),
        }
        let mut rest = || -> io::Result<(u64, serde_json::Value)> {
            stdout.read_exact(&mut length[1..])?;
            let length = u32::from_be_bytes(length) as usize;
            let mut body = vec![0; length];
            stdout.read_exact(&mut body)?;
            if length < 10 || body[..2] != [0, 1] {
                return Err(io::Error::other("bad frame header"));
            }
            let id = u64::from_be_bytes(body[2..10].try_into().unwrap());
            let json = serde_json::from_slice(&body[10..]).map_err(io::Error::other)?;
            Ok((id, json))
        };
        match rest() {
            Ok((id, json)) => {
                if sender.send(Output::Frame(id, json)).is_err() {
                    return;
                }
            }
            Err(error) => return drop(sender.send(Output::Garbage(error.to_string()))),
        }
    }
}

impl Helper {
    fn start(args: &[&str]) -> Self {
        let mut child = Command::new(env!("CARGO_BIN_EXE_sf4-net"))
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let stdin = child.stdin.take();
        let stdout = child.stdout.take().unwrap();
        let (sender, frames) = mpsc::channel();
        thread::spawn(move || read_frames(stdout, sender));
        Self {
            child,
            stdin,
            frames,
            next_id: 2,
        }
    }
    fn send(&mut self, json: &str) {
        let mut frame = Vec::new();
        frame.extend(((10 + json.len()) as u32).to_be_bytes());
        frame.extend(1u16.to_be_bytes());
        frame.extend(self.next_id.to_be_bytes());
        frame.extend(json.as_bytes());
        self.next_id += 1;
        let stdin = self.stdin.as_mut().unwrap();
        stdin.write_all(&frame).unwrap();
        stdin.flush().unwrap();
    }
    fn close_stdin(&mut self) {
        self.stdin = None;
    }
    /// Event types up to and including `until`, panicking on stray bytes.
    fn events_until(&mut self, until: &str) -> Vec<String> {
        let deadline = Instant::now() + LIMIT;
        let mut seen = Vec::new();
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            match self
                .frames
                .recv_timeout(left)
                .expect("helper event timed out")
            {
                Output::Frame(id, json) => {
                    assert!(id >= 2, "event IDs start at 2");
                    let kind = json["type"].as_str().unwrap().to_string();
                    seen.push(kind.clone());
                    if kind == until {
                        return seen;
                    }
                }
                Output::End => panic!("stdout ended before {until}: {seen:?}"),
                Output::Garbage(why) => panic!("stdout held something but frames: {why}"),
            }
        }
    }
    /// After `stopped` the process exits with 0 and stdout ends on a frame boundary.
    fn expect_clean_exit(&mut self) {
        let deadline = Instant::now() + LIMIT;
        loop {
            match self.frames.recv_timeout(Duration::from_secs(1)) {
                Ok(Output::Frame(..)) => {}
                Ok(Output::End) => break,
                Ok(Output::Garbage(why)) => panic!("stdout held something but frames: {why}"),
                Err(_) => assert!(Instant::now() < deadline, "helper kept stdout open"),
            }
        }
        let status = self.wait();
        assert!(status.success(), "helper exited with {status}");
    }
    fn wait(&mut self) -> std::process::ExitStatus {
        let deadline = Instant::now() + LIMIT;
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                return status;
            }
            assert!(Instant::now() < deadline, "helper did not exit");
            thread::sleep(Duration::from_millis(20));
        }
    }
    fn stderr(&mut self) -> String {
        let mut text = String::new();
        self.child
            .stderr
            .take()
            .unwrap()
            .read_to_string(&mut text)
            .unwrap();
        text
    }
}

#[test]
fn a_shutdown_command_stops_the_helper_with_exit_zero() {
    let port = free_port();
    let mut helper = Helper::start(&["--stdio", "--bind-port", &port.to_string()]);
    helper.send(r#"{"type":"status"}"#);
    helper.events_until("status");
    // The endpoint is up, so it holds exactly the requested port.
    assert!(
        UdpSocket::bind((Ipv4Addr::UNSPECIFIED, port)).is_err(),
        "port {port} is not held by the helper"
    );
    helper.send(r#"{"type":"shutdown"}"#);
    helper.events_until("stopped");
    helper.expect_clean_exit();
}

#[test]
fn closing_stdin_stops_the_helper_with_exit_zero() {
    let mut helper = Helper::start(&["--stdio", "--bind-port", &free_port().to_string()]);
    helper.close_stdin();
    helper.events_until("stopped");
    helper.expect_clean_exit();
}

#[test]
fn closing_stdin_after_commands_still_answers_them_first() {
    let mut helper = Helper::start(&["--stdio"]);
    helper.send(r#"{"type":"status"}"#);
    helper.close_stdin();
    let seen = helper.events_until("stopped");
    assert!(seen.iter().any(|kind| kind == "status"), "{seen:?}");
    helper.expect_clean_exit();
}

#[test]
fn a_port_that_cannot_be_bound_stops_the_helper_with_an_error() {
    let held = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).unwrap();
    let port = held.local_addr().unwrap().port();
    let mut helper = Helper::start(&["--stdio", "--bind-port", &port.to_string()]);
    let status = helper.wait();
    assert_eq!(status.code(), Some(1));
    let message = helper.stderr();
    assert!(message.contains(&port.to_string()), "{message:?}");
    match helper.frames.recv_timeout(LIMIT).unwrap() {
        Output::End => {}
        _ => panic!("nothing may reach stdout when startup fails"),
    }
}

#[test]
fn bad_arguments_exit_two_without_touching_stdout() {
    for args in [&["--stdio", "--bind-port"][..], &["--pipe", "x"], &[]] {
        let mut helper = Helper::start(args);
        assert_eq!(helper.wait().code(), Some(2));
        assert!(helper.stderr().contains("usage"));
        assert!(matches!(
            helper.frames.recv_timeout(LIMIT).unwrap(),
            Output::End
        ));
    }
}
