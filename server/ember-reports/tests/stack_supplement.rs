mod support;
use ember_reports::symbolicate::{Walk, WalkStatus};
use minidump_synth::{
    DumpString, Exception, Memory, Module, SimpleStream, SynthMinidump, SystemInfo, Thread,
    x86_context,
};
use std::time::Duration;
use support::{Temp, worker};
use test_assembler::{Endian, Section};

const STACK_SUPPLEMENT: u32 = 0x5346_3401;

// A thread 7 with return addresses 0x401500 (in the supplement, at its offset
// 12) and 0x401600 (in the stored stack, 8 bytes in). The stored stack starts
// at `stored`; esp is `esp`; the supplement, when given, is the 24 bytes ending
// at `ends_at`.
fn dump(supplement_ends_at: Option<u32>, stored: u32, esp: u32) -> Vec<u8> {
    let e = Endian::Little;
    let name = DumpString::new("Launcher.exe", e);
    let cv = Section::with_endian(e)
        .append_bytes(b"RSDS")
        .D32(0x00112233)
        .D16(0x4455)
        .D16(0x6677)
        .append_bytes(&[0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff])
        .D32(1)
        .append_bytes(b"Launcher.exe")
        .D8(0);
    let module = Module::new(e, 0x400000, 0x10000, &name, 0, 0, None).cv_record(&cv);
    let context = x86_context(e, 0x401234, esp);
    let stack = Memory::with_section(
        Section::with_endian(e)
            .D32(0)
            .D32(0)
            .D32(0x401600)
            .append_repeated(0, 64),
        u64::from(stored),
    );
    let thread = Thread::new(e, 7, &stack, &context);
    let mut exception = Exception::new(e);
    exception.thread_id = 7;
    exception.exception_record.exception_code = 0x8000_0101;
    exception.exception_record.exception_address = 0x401234;
    exception.thread_context = (0, 0);
    let mut synth = SynthMinidump::with_endian(e)
        .add(name)
        .add(cv)
        .add(context)
        .add_module(module)
        .add_thread(thread)
        .add_memory(stack)
        .add_system_info(SystemInfo::new(e).set_platform_id(2))
        .add_exception(exception);
    if let Some(ends_at) = supplement_ends_at {
        let section = Section::with_endian(e)
            .D32(1)
            .D32(1)
            .D32(7)
            .D32(ends_at - 24)
            .D32(24)
            .D32(0)
            .D32(0)
            .D32(0)
            .D32(0x401500)
            .D32(0)
            .D32(0);
        synth = synth.add_stream(SimpleStream {
            stream_type: STACK_SUPPLEMENT,
            section,
        });
    }
    synth.finish().unwrap()
}

async fn walked(bytes: Vec<u8>) -> Walk {
    let temp = Temp::new();
    let symbols = temp.0.join("symbols");
    std::fs::create_dir(&symbols).unwrap();
    worker().walk(&bytes, symbols, Duration::from_secs(5)).await
}

async fn frames(bytes: Vec<u8>) -> Vec<u64> {
    let walk = walked(bytes).await;
    walk.crash_frames.iter().map(|f| f.instruction).collect()
}

#[tokio::test]
async fn the_walk_starts_at_the_stack_pointer_once_the_supplement_is_joined() {
    // Without the stream the stored stack starts above esp: nothing to walk.
    assert_eq!(frames(dump(None, 0x800000, 0x7ffff0)).await, [0x401234]);
    // Adjacent: the frame in the supplement and the one in the stored part
    // (return addresses show as address - 1).
    let all = [0x401234, 0x4014ff, 0x4015ff];
    assert_eq!(frames(dump(Some(0x800000), 0x800000, 0x7ffff0)).await, all);
    // Overlapping: the range runs 4 bytes past the stored start; the stored
    // stack is joined at its own start, so nothing is lost.
    assert_eq!(frames(dump(Some(0x800004), 0x800000, 0x7ffff0)).await, all);
    // A gap with esp in the range: the range stands alone.
    assert_eq!(
        frames(dump(Some(0x7ffffc), 0x800000, 0x7fffe8)).await,
        [0x401234, 0x4014ff]
    );
    // A gap with esp outside the range (stale bytes): the record stays.
    assert_eq!(
        frames(dump(Some(0x7fffe8), 0x800000, 0x7ffff0)).await,
        [0x401234]
    );
    // Covered: DbgHelp stored from esp already; the stored stack is kept.
    assert_eq!(
        frames(dump(Some(0x800008), 0x7ffff0, 0x7ffff0)).await,
        [0x401234, 0x4015ff]
    );
}

#[tokio::test]
async fn a_malformed_supplement_changes_nothing() {
    let mut bytes = dump(Some(0x800000), 0x800000, 0x7ffff0);
    // Break the version word of the stream (the first 0x01 0 0 0 before the count).
    let at = bytes
        .windows(12)
        .position(|w| w == [1, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0])
        .unwrap();
    bytes[at] = 9;
    assert_eq!(frames(bytes).await, [0x401234]);
}

// 128 thread records sharing one 3.9 MB stack, and a supplement of 128 ranges
// that each end at its start. Honest dumps never need more than one copy of a
// stack per thread; this must be refused, not copied 128 times.
fn shared_stack_dump(range_size: u32) -> Vec<u8> {
    let e = Endian::Little;
    let name = DumpString::new("Launcher.exe", e);
    let module = Module::new(e, 0x400000, 0x10000, &name, 0, 0, None);
    let context = x86_context(e, 0x401234, 0x7ffff0);
    let stack = Memory::with_section(
        Section::with_endian(e).append_repeated(0, 3_900_000),
        0x800000,
    );
    let threads: Vec<_> = (7..7 + 128)
        .map(|id| Thread::new(e, id, &stack, &context))
        .collect();
    let mut synth = SynthMinidump::with_endian(e).add(name).add(context);
    for thread in threads {
        synth = synth.add_thread(thread);
    }
    let mut exception = Exception::new(e);
    exception.thread_id = 7;
    exception.exception_record.exception_code = 0x8000_0101;
    exception.exception_record.exception_address = 0x401234;
    exception.thread_context = (0, 0);
    let mut section = Section::with_endian(e).D32(1).D32(128);
    for id in 7..7 + 128 {
        section = section.D32(id).D32(0x800000 - range_size).D32(range_size);
    }
    for _ in 0..128 * range_size {
        section = section.D8(0);
    }
    synth
        .add_module(module)
        .add_memory(stack)
        .add_system_info(SystemInfo::new(e).set_platform_id(2))
        .add_exception(exception)
        .add_stream(SimpleStream {
            stream_type: STACK_SUPPLEMENT,
            section,
        })
        .finish()
        .unwrap()
}

#[tokio::test]
async fn a_supplement_cannot_make_the_server_copy_one_stack_many_times() {
    // Empty ranges are refused; one-byte ranges would copy 3.9 MB each.
    // The walk must still complete: a worker that ran out of memory reports
    // only the raw exception frame, with another status (or none at all).
    for size in [0, 1] {
        let walk = walked(shared_stack_dump(size)).await;
        assert_eq!(walk.status, WalkStatus::Ok, "size {size}");
        assert_eq!(walk.crash_frames.len(), 1, "size {size}");
    }
}
