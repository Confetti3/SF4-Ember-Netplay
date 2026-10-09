mod support;
use ember_reports::{
    event::{self, EVENT_BYTES},
    intake::{self, Kind},
    limits::Gate,
    symbolicate::{Frame, Thread, Walk, WalkStatus},
};
use serde_json::{Value, json};
use std::{path::PathBuf, time::Duration};
use support::{Temp, id, report, worker};

const SYNTHETIC_DEBUG_ID: &str = "00112233445566778899AABBCCDDEEFF1";

fn synthetic(module_name: &str) -> Vec<u8> {
    use minidump::{Minidump, MinidumpModuleList, Module as _};
    use minidump_synth::{
        DumpString, Exception, Memory, Module, SynthMinidump, SystemInfo, Thread, x86_context,
    };
    use test_assembler::{Endian, Section};
    let e = Endian::Little;
    let name = DumpString::new(module_name, e);
    // RSDS with a nonzero GUID, age 1, and debug filename. rust-minidump
    // rejects a nil GUID even with a nonzero age. GUID fields are little-endian;
    // SYNTHETIC_DEBUG_ID is their Breakpad representation with the age appended.
    let cv = Section::with_endian(e)
        .append_bytes(b"RSDS")
        .D32(0x00112233)
        .D16(0x4455)
        .D16(0x6677)
        .append_bytes(&[0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff])
        .D32(1)
        .append_bytes(module_name.as_bytes())
        .D8(0);
    let module = Module::new(e, 0x400000, 0x10000, &name, 0, 0, None).cv_record(&cv);
    let context = x86_context(e, 0x401234, 0x800000);
    let stack = Memory::with_section(Section::with_endian(e).append_repeated(0, 128), 0x800000);
    let thread = Thread::new(e, 7, &stack, &context);
    let mut exception = Exception::new(e);
    exception.thread_id = 7;
    exception.exception_record.exception_code = 0xc0000005;
    exception.exception_record.exception_address = 0x401234;
    exception.exception_record.number_parameters = 2;
    exception.exception_record.exception_information[0] = 0;
    exception.exception_record.exception_information[1] = 0xdeadbeef;
    // Let the thread context drive the stack: an absent exception context is
    // supported by rust-minidump and avoids manual RVA fixups in the fixture.
    exception.thread_context = (0, 0);
    let bytes = SynthMinidump::with_endian(e)
        .add(name)
        .add(cv)
        .add(context)
        .add_module(module)
        .add_thread(thread)
        .add_memory(stack)
        .add_system_info(SystemInfo::new(e).set_platform_id(2))
        .add_exception(exception)
        .finish()
        .unwrap();
    // Validate the shared fixture for every caller before exercising the walk.
    let dump = Minidump::read(bytes.as_slice()).unwrap();
    let modules = dump.get_stream::<MinidumpModuleList>().unwrap();
    let module = modules.module_at_address(0x401234).unwrap();
    assert_eq!(
        module.debug_identifier().unwrap().breakpad().to_string(),
        SYNTHETIC_DEBUG_ID
    );
    let lookup = breakpad_symbols::breakpad_sym_lookup(module).unwrap();
    assert_eq!(
        lookup.cache_rel,
        format!("{module_name}/{SYNTHETIC_DEBUG_ID}/{module_name}.sym")
    );
    bytes
}

#[tokio::test]
async fn synthetic_crash_symbolicated_and_module_offset_events() {
    let temp = Temp::new();
    let symbols = temp.0.join("symbols");
    std::fs::create_dir(&symbols).unwrap();
    let no = worker()
        .walk(
            synthetic("SSFIV.exe"),
            symbols.clone(),
            Duration::from_secs(2),
        )
        .await;
    assert_eq!(
        no.symbolication(),
        format!("partial: symbols missing for SSFIV.exe/{SYNTHETIC_DEBUG_ID}/SSFIV.exe.sym")
    );
    assert_eq!(no.crash_frames[0].module, "SSFIV.exe");
    assert_eq!(no.crash_frames[0].offset, 0x1234);
    assert!(no.crash_frames[0].function.is_none());
    let id = id(&"a".repeat(32));
    let event: Value = serde_json::from_slice(&event::build(&id, &report(), &no, None)).unwrap();
    assert_eq!(
        event["fingerprint"],
        json!(["{{ default }}", "SSFIV.exe+0x1234"])
    );
    assert_eq!(
        event["exception"]["values"][0]["type"],
        "EXCEPTION_ACCESS_VIOLATION_READ"
    );
    assert_eq!(
        event["exception"]["values"][0]["stacktrace"]["frames"][0]["instruction_addr"],
        "0x401234"
    );
    assert_eq!(
        event["exception"]["values"][0]["stacktrace"]["frames"][0]["in_app"],
        false
    );
    assert!(
        event["exception"]["values"][0]["value"]
            .as_str()
            .unwrap()
            .contains("0xdeadbeef")
    );
    let debug_id = SYNTHETIC_DEBUG_ID;
    let directory = symbols.join("Launcher.exe").join(debug_id);
    std::fs::create_dir_all(&directory).unwrap();
    std::fs::write(directory.join("Launcher.exe.sym"), format!("MODULE windows x86 {debug_id} Launcher.exe\nFILE 0 launcher.cpp\nFUNC 1200 100 0 report_crash\n1200 100 42 0\n")).unwrap();
    let yes = worker()
        .walk(synthetic("Launcher.exe"), symbols, Duration::from_secs(2))
        .await;
    assert_eq!(yes.symbolication(), "ok");
    assert_eq!(
        yes.crash_frames[0].function.as_deref(),
        Some("report_crash")
    );
    let event: Value = serde_json::from_slice(&event::build(
        &id,
        &report(),
        &yes,
        Some("/state/dumps/a.dmp"),
    ))
    .unwrap();
    assert!(event.get("fingerprint").is_none());
    let f = &event["exception"]["values"][0]["stacktrace"]["frames"][0];
    assert_eq!(f["function"], "report_crash");
    assert_eq!(f["filename"], "launcher.cpp");
    assert_eq!(f["lineno"], 42);
    assert_eq!(f["package"], "Launcher.exe");
    assert_eq!(f["in_app"], true);
    assert_eq!(event["extra"]["minidump_path"], "/state/dumps/a.dmp");
    assert_eq!(event["platform"], "native");
    assert_eq!(event["level"], "error");
    assert_eq!(event["release"], "0.8.0");
    assert_eq!(event["environment"], "beta");
    assert_eq!(event["tags"]["build_id"], "a".repeat(12));
}

#[cfg(target_os = "linux")]
#[tokio::test]
async fn unreadable_symbols_report_partial_and_log_only_store_relative_name() {
    use std::{
        io::Write,
        os::unix::{fs::PermissionsExt, process::CommandExt},
        process::{Command, Stdio},
    };
    let temp = Temp::new();
    // A root-run test drops only the worker's credentials, so chmod really
    // causes EACCES rather than being bypassed by root's DAC privileges.
    std::fs::set_permissions(&temp.0, std::fs::Permissions::from_mode(0o755)).unwrap();
    let relative = format!("Launcher.exe/{SYNTHETIC_DEBUG_ID}/Launcher.exe.sym");
    let path = temp.0.join(&relative);
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    for directory in [
        path.parent().unwrap(),
        path.parent().unwrap().parent().unwrap(),
    ] {
        std::fs::set_permissions(directory, std::fs::Permissions::from_mode(0o755)).unwrap();
    }
    std::fs::write(
        &path,
        format!(
            "MODULE windows x86 {SYNTHETIC_DEBUG_ID} Launcher.exe\nFUNC 1200 100 0 report_crash\n"
        ),
    )
    .unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o000)).unwrap();
    let root = unsafe { libc::geteuid() } == 0;
    let executable = if root {
        // A root-owned checkout may be under /root, which nobody cannot
        // traverse. Put only the worker binary in the accessible fixture.
        let executable = temp.0.join("symbolicate-worker");
        std::fs::copy(env!("CARGO_BIN_EXE_ember-reports"), &executable).unwrap();
        std::fs::set_permissions(&executable, std::fs::Permissions::from_mode(0o755)).unwrap();
        executable
    } else {
        PathBuf::from(env!("CARGO_BIN_EXE_ember-reports"))
    };
    let mut command = Command::new(executable);
    command
        .arg("symbolicate")
        .arg(&temp.0)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    if root {
        command.uid(65534).gid(65534);
    }
    let mut child = command.spawn().unwrap();
    child
        .stdin
        .take()
        .unwrap()
        .write_all(&synthetic("Launcher.exe"))
        .unwrap();
    let output = child.wait_with_output().unwrap();
    assert!(output.status.success());
    // An older parent reads this status string directly.
    let raw: Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(
        raw["status"],
        format!("partial: symbols unreadable for {relative}")
    );
    let walk: Walk = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(
        walk.symbolication(),
        format!("partial: symbols unreadable for {relative}")
    );
    assert_eq!(walk.crash_frames[0].module, "Launcher.exe");
    assert_eq!(walk.crash_frames[0].offset, 0x1234);
    assert!(walk.crash_frames[0].function.is_none());
    assert_eq!(
        String::from_utf8(output.stderr).unwrap(),
        format!("ember-reports: symbols unreadable: {relative}\n")
    );
    let event: Value =
        serde_json::from_slice(&event::build(&id(&"a".repeat(32)), &report(), &walk, None))
            .unwrap();
    assert_eq!(event["extra"]["symbolication"], walk.symbolication());
    assert!(!walk.symbolication().contains(temp.0.to_str().unwrap()));
}

#[tokio::test]
async fn failed_walk_preserves_raw_facts_and_worker_slot() {
    let temp = Temp::new();
    let mut bytes = synthetic("SSFIV.exe");
    // Remove only the SystemInfo directory entry by changing its stream type.
    let count = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let directory = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
    for i in 0..count {
        let p = directory + i * 12;
        if u32::from_le_bytes(bytes[p..p + 4].try_into().unwrap()) == 7 {
            bytes[p..p + 4].copy_from_slice(&0xffff_u32.to_le_bytes());
        }
    }
    let gate = Gate::new(1, 0);
    let ticket = gate
        .start(gate.reserve().unwrap(), Duration::from_secs(1))
        .await
        .unwrap();
    let walk = worker()
        .walk(bytes, temp.0.clone(), Duration::from_secs(1))
        .await;
    assert_eq!(walk.symbolication(), "walk_failed");
    assert_eq!(walk.code, Some(0xc0000005));
    assert_eq!(walk.address, Some(0xdeadbeef));
    assert_eq!(walk.crash_frames[0].offset, 0x1234);
    assert!(gate.reserve().is_err());
    drop(ticket);
    assert!(gate.reserve().is_ok());
}

#[tokio::test]
async fn input_budget_preserves_exception_without_walking_excess_threads() {
    let temp = Temp::new();
    let mut bytes = synthetic("SSFIV.exe");
    let count = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let directory = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
    for i in 0..count {
        let p = directory + i * 12;
        if u32::from_le_bytes(bytes[p..p + 4].try_into().unwrap()) == 3 {
            let offset = u32::from_le_bytes(bytes[p + 8..p + 12].try_into().unwrap()) as usize;
            bytes[offset..offset + 4].copy_from_slice(&129_u32.to_le_bytes());
        }
    }
    let walk = worker()
        .walk(bytes, temp.0.clone(), Duration::from_secs(1))
        .await;
    assert_eq!(walk.symbolication(), "input_budget");
    assert_eq!(walk.code, Some(0xc0000005));
    assert_eq!(walk.address, Some(0xdeadbeef));
}

#[test]
fn event_order_thread_truncation_problem_and_size_cap() {
    let mut report = report();
    let make_frame = |i| Frame {
        instruction: i,
        module: "Sidecar.dll".into(),
        offset: i,
        function: Some(format!("frame_{i}")),
        filename: Some("source.cpp".into()),
        line: Some(10),
    };
    let walk = Walk {
        crash_frames: (0..80).map(make_frame).collect(),
        threads: vec![Thread {
            id: 2,
            frames: (100..180).map(make_frame).collect(),
        }],
        status: WalkStatus::Ok,
        ..Walk::default()
    };
    for name in intake::LOG_NAMES {
        report
            .logs
            .insert(name.into(), "\0\u{1}\"\\界".repeat(20000));
    }
    let bytes = event::build(&id(&"b".repeat(32)), &report, &walk, None);
    assert!(bytes.len() < EVENT_BYTES);
    let value: Value = serde_json::from_slice(&bytes).unwrap();
    let frames = value["exception"]["values"][0]["stacktrace"]["frames"]
        .as_array()
        .unwrap();
    assert_eq!(frames.len(), 64);
    assert_eq!(frames[0]["function"], "frame_63");
    assert_eq!(frames[63]["function"], "frame_0");
    assert_eq!(
        value["threads"]["values"][0]["stacktrace"]["frames"]
            .as_array()
            .unwrap()
            .len(),
        64
    );
    for name in intake::LOG_NAMES {
        let tail = value["extra"]["logs"][name].as_str().unwrap();
        assert!(report.logs[name].ends_with(tail));
        assert!(tail.len() < report.logs[name].len());
    }
    report.meta.kind = Kind::Problem;
    let value: Value = serde_json::from_slice(&event::build(
        &id(&"b".repeat(32)),
        &report,
        &Walk::default(),
        None,
    ))
    .unwrap();
    assert_eq!(value["message"], report.meta.comment.unwrap());
    assert_eq!(value["level"], "warning");
    assert!(value.get("exception").is_none());
}

#[test]
fn auxiliary_threads_fill_the_budget_exactly_in_order() {
    let frames: Vec<Frame> = (0..64)
        .map(|i| Frame {
            instruction: 0x401000 + i,
            module: "SSFIV.exe".into(),
            offset: 0x1000 + i,
            function: Some(format!("{:0>256}", i)),
            filename: Some("source.cpp".into()),
            line: Some(10),
        })
        .collect();
    let walk = Walk {
        threads: (100..228)
            .map(|id| Thread {
                id,
                frames: frames.clone(),
            })
            .collect(),
        status: WalkStatus::Ok,
        ..Walk::default()
    };
    let bytes = event::build(&id(&"d".repeat(32)), &report(), &walk, None);
    let value: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(value["extra"]["threads_truncated"], true);
    let kept = value["threads"]["values"].as_array().unwrap();
    assert!(!kept.is_empty() && kept.len() < walk.threads.len());
    for (thread, id) in kept.iter().zip(100..) {
        assert_eq!(thread["id"], id);
    }
    // The 64 KiB reserve is measured before the truncation marker is added.
    // Every thread serializes to the same size, so the next one must not fit.
    let budget = EVENT_BYTES - 64 * 1024;
    let measured = bytes.len() - r#","threads_truncated":true"#.len();
    let thread = serde_json::to_vec(&kept[0]).unwrap().len();
    assert!(measured < budget);
    assert!(measured + 1 + thread >= budget);
}
