#![cfg(windows)]
//! Explicit, network-dependent process test. Uses the production helper,
//! authenticated IPC, default Iroh relays, and raw UDP bridge end to end.
use sf4_net::{
    service::{Command, Event},
    wire::{self, ControlFrame},
};
use std::{
    io::{self, Write},
    net::Ipv4Addr,
    os::windows::{io::AsRawHandle, process::CommandExt},
    process::{Child, Command as Process, Stdio},
    time::{Duration, Instant},
};
use tokio::{
    net::{
        UdpSocket,
        windows::named_pipe::{ClientOptions, NamedPipeClient},
    },
    time::{sleep, timeout},
};
use windows_sys::Win32::System::Pipes::GetNamedPipeServerProcessId;

struct Helper {
    child: Child,
    pipe: NamedPipeClient,
    request_id: u64,
}
impl Drop for Helper {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
struct Starting(Option<Child>);
impl Drop for Starting {
    fn drop(&mut self) {
        if let Some(child) = &mut self.0 {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

impl Helper {
    async fn start(relay_only: bool) -> io::Result<Self> {
        let name = format!(r"\\.\pipe\sf4-net-{:032x}", rand::random::<u128>());
        let nonce: [u8; 32] = rand::random();
        let mut process = Process::new(env!("CARGO_BIN_EXE_sf4-net"));
        process.args(["--pipe", &name]);
        if relay_only {
            process.arg("--relay-only");
        }
        let child = process
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(0x0800_0000)
            .spawn()?;
        let mut starting = Starting(Some(child));
        let child = starting.0.as_mut().unwrap();
        let mut startup = b"SF4N".to_vec();
        startup.extend(wire::VERSION.to_be_bytes());
        startup.extend(std::process::id().to_be_bytes());
        startup.extend(nonce);
        child.stdin.take().unwrap().write_all(&startup)?;
        let until = Instant::now() + Duration::from_secs(15);
        let mut pipe = loop {
            match ClientOptions::new().open(&name) {
                Ok(pipe) => break pipe,
                Err(_) if Instant::now() < until && child.try_wait()?.is_none() => {
                    sleep(Duration::from_millis(5)).await
                }
                Err(error) => return Err(error),
            }
        };
        let mut pid = 0;
        if unsafe { GetNamedPipeServerProcessId(pipe.as_raw_handle(), &mut pid) } == 0
            || pid != child.id()
        {
            return Err(io::Error::other("incorrect pipe server PID"));
        }
        wire::write_control(
            &mut pipe,
            &ControlFrame {
                message_id: 1,
                payload: nonce.to_vec(),
            },
        )
        .await?;
        let response = wire::read_control(&mut pipe).await?;
        if response.message_id != 1 || response.payload != b"SF4N" {
            return Err(io::Error::other("authentication failed"));
        }
        Ok(Self {
            child: starting.0.take().unwrap(),
            pipe,
            request_id: 2,
        })
    }
    async fn send(&mut self, command: Command) -> io::Result<()> {
        let payload = serde_json::to_vec(&command)?;
        wire::write_ipc(
            &mut self.pipe,
            &ControlFrame {
                message_id: self.request_id,
                payload,
            },
        )
        .await?;
        self.request_id += 1;
        Ok(())
    }
    async fn next(&mut self, kind: &str) -> io::Result<Event> {
        timeout(Duration::from_secs(20), async {
            loop {
                let frame = wire::read_ipc(&mut self.pipe).await?;
                let event: Event = serde_json::from_slice(&frame.payload)?;
                if let Event::Error { code, .. } = &event {
                    return Err(io::Error::other(format!("helper: {code}")));
                }
                if serde_json::to_value(&event)?["type"] == kind {
                    return Ok(event);
                }
            }
        })
        .await
        .map_err(|_| io::Error::other("helper event timed out"))?
    }
    async fn shutdown(mut self) -> io::Result<()> {
        self.send(Command::Shutdown).await?;
        self.next("stopped").await?;
        let until = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(status) = self.child.try_wait()? {
                return if status.success() {
                    Ok(())
                } else {
                    Err(io::Error::other("helper exited unsuccessfully"))
                };
            }
            if Instant::now() >= until {
                return Err(io::Error::other("helper failed to stop"));
            }
            sleep(Duration::from_millis(5)).await;
        }
    }
}

#[tokio::test]
#[ignore = "requires public Iroh relay/address-lookup access; run explicitly"]
async fn production_helpers_host_join_rematch_and_bridge_without_gns_or_broker() {
    exercise_helpers(false).await;
}

#[tokio::test]
#[ignore = "requires public Iroh relay/address-lookup access; run explicitly"]
async fn production_helpers_force_relay_without_ip_transports() {
    exercise_helpers(true).await;
}

async fn exercise_helpers(relay_only: bool) {
    timeout(Duration::from_secs(120), async {
        let mut host = Helper::start(relay_only).await.unwrap();
        let mut guest = Helper::start(relay_only).await.unwrap();
        for helper in [&mut host, &mut guest] {
            helper.send(Command::Status).await.unwrap();
            match helper.next("status").await.unwrap() {
                Event::Status { ip_transports, fixed_port, .. } => { if relay_only { assert_eq!((ip_transports, fixed_port), (0, 0)); } else { assert!(ip_transports > 0); } },
                _ => unreachable!(),
            }
        }
        host.send(Command::Host { epoch: 1, build: "process-test-build".into() }).await.unwrap();
        let (invitation, room) = match host.next("hosted").await.unwrap() {
            Event::Hosted { invitation, room, .. } => (invitation, room), _ => unreachable!(),
        };
        guest.send(Command::Join { epoch: 7, invitation, build: "process-test-build".into() }).await.unwrap();
        let guest_id = match host.next("connected").await.unwrap() { Event::Connected { peer, .. } => peer, _ => unreachable!() };
        let host_id = match guest.next("connected").await.unwrap() { Event::Connected { peer, .. } => peer, _ => unreachable!() };
        // Leave and successor tests below are valid only after the exact
        // committed two-member coordination roster is visible on both helper
        // control streams. A transient one-member rebound is expected while
        // the admission is still being committed and is deliberately skipped.
        for helper in [&mut host, &mut guest] {
            loop {
                match helper.next("control_rebound").await.unwrap() {
                    Event::ControlRebound { members, leader, .. } if members.len() == 2 => {
                        assert!(!leader.is_empty());
                        break;
                    }
                    Event::ControlRebound { .. } => continue,
                    _ => unreachable!(),
                }
            }
        }
        host.send(Command::Send { epoch: 1, peer: guest_id, message_id: 2, payload: "cpp lobby control".into() }).await.unwrap();
        host.next("sent").await.unwrap();
        match guest.next("message").await.unwrap() {
            Event::Message { message_id, payload, .. } => { assert_eq!(message_id, 2); assert_eq!(payload, "cpp lobby control"); }, _ => unreachable!(),
        }
        let mut timings = Vec::new();
        for generation in 1..=3 {
            // A fresh local GGPO socket and virtual mapping on every rematch.
            let a = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            let b = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            let capability: [u8; 32] = rand::random();
            host.send(Command::PrepareGame { epoch: 1, peer: guest_id, room, generation, capability,
                local_port: a.local_addr().unwrap().port(), max_packet: 1024, dial: false }).await.unwrap();
            host.next("game_waiting").await.unwrap();
            guest.send(Command::PrepareGame { epoch: 7, peer: host_id, room, generation, capability,
                local_port: b.local_addr().unwrap().port(), max_packet: 1024, dial: true }).await.unwrap();
            guest.next("game_waiting").await.unwrap();
            let (a_port, a_route) = match host.next("game_ready").await.unwrap() { Event::GameReady { virtual_port, route, .. } => (virtual_port, route), _ => unreachable!() };
            let (b_port, b_route) = match guest.next("game_ready").await.unwrap() { Event::GameReady { virtual_port, route, .. } => (virtual_port, route), _ => unreachable!() };
            println!("helper generation {generation} selected routes: host={a_route} guest={b_route}");
            if generation == 3 {
                host.send(Command::CloseControl { epoch: 1, peer: guest_id }).await.unwrap();
                host.next("control_closed").await.unwrap(); guest.next("control_closed").await.unwrap();
            }
            for sequence in 0..50u32 {
                let mut payload = vec![generation as u8; 1024]; payload[..4].copy_from_slice(&sequence.to_be_bytes());
                let start = Instant::now();
                a.send_to(&payload, (Ipv4Addr::LOCALHOST, a_port)).await.unwrap();
                let mut received = [0; 2048];
                let (size, source) = timeout(Duration::from_secs(5), b.recv_from(&mut received)).await.unwrap().unwrap();
                assert_eq!(source.port(), b_port); assert_eq!(&received[..size], &payload);
                b.send_to(&payload, (Ipv4Addr::LOCALHOST, b_port)).await.unwrap();
                let (size, source) = timeout(Duration::from_secs(5), a.recv_from(&mut received)).await.unwrap().unwrap();
                assert_eq!(source.port(), a_port); assert_eq!(&received[..size], &payload);
                timings.push(start.elapsed().as_micros());
            }
            host.send(Command::EndMatch { epoch: 1, generation }).await.unwrap();
            guest.send(Command::EndMatch { epoch: 7, generation }).await.unwrap();
            host.next("game_closed").await.unwrap(); guest.next("game_closed").await.unwrap();
        }
        timings.sort_unstable();
        println!("Two helper processes (relay_only={relay_only}), 3 match generations, 150 exact 1024-byte round trips; observed round-trip us p50={} p95={} p99={}. This is not two-PC or SF4 gameplay evidence.",
            timings[timings.len()/2], timings[timings.len()*95/100], timings[timings.len()*99/100]);
        host.send(Command::Leave { epoch: 1, abandon: false }).await.unwrap(); guest.send(Command::Leave { epoch: 7, abandon: false }).await.unwrap();
        host.next("room_closed").await.unwrap(); guest.next("room_closed").await.unwrap();
        host.shutdown().await.unwrap(); guest.shutdown().await.unwrap();
    }).await.unwrap();
}
