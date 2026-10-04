#pragma once

// The Win32 half of the crash record (F-015). A process that dies with an
// unhandled exception, heap corruption, a CRT fault or a GGPO assertion
// leaves sf4e/logs/sf4e-crash.log (the fault, the module and offset, the
// last log lines), written synchronously from the failing thread before the
// asynchronous logger gets a chance to lose them, and a sf4e-crash-*.dmp, which
// the launcher writes when it started the game (common/CrashDump.hxx). The
// portable model lives in common/CrashReport.hxx.

#include <windows.h>
#include <spdlog/spdlog.h>

namespace sf4e {
namespace crash {

// A logger sink that keeps the last lines in memory for the record.
spdlog::sink_ptr RingSink();

// Installs the handlers and sets where the record and the dump go. Call
// once, after the logger exists, on the game thread.
void Install(const wchar_t* logsDirectory);

// Records a non-zero ExitProcess from the game's own code, such as its C
// runtime's _exit(255) after a fatal runtime error, as kind=exit with a dump
// the launcher writes (never one written in-process on this path).
// Call once after Install.
void WatchGameExit();

// The launcher's dump channel from the payload. Without it the game writes
// the dump itself.
void ConfigureDumpChannel(HANDLE request, HANDLE done, HANDLE mailbox);

// SF4E_HEAP_CHECK=<n> validates every process heap after every n-th
// save-state operation, and at each match boundary, and logs the first one
// after which a heap no longer validates. A pass can take milliseconds, so
// it is for reproducing a corruption, not for play. Game thread only.
void HeapCheckpoint(const char* operation, int frame);
// Startup preferences supply an interval when SF4E_HEAP_CHECK is absent.
void ConfigureHeapCheck(unsigned interval);
// True when heap checking is enabled, so a caller can skip gathering what
// HeapCheckpoint would log.
bool HeapCheckEnabled();

// GGPO's assertion handler: records the message, then GGPO exits.
void OnGgpoAssertion(const char* message);

// One log line about the process at a match boundary (memory, the largest
// free region of the 32-bit address space, handles, threads, dropped log
// lines), and puts the crash filter back if something replaced it.
void NoteMatchBoundary(const char* label);

} // namespace crash
} // namespace sf4e
