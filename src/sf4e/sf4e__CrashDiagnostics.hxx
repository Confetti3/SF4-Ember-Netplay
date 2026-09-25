#pragma once

// The Win32 half of the crash record (F-015). A process that dies with an
// unhandled exception, a CRT fault or a GGPO assertion leaves
// sf4e/logs/sf4e-crash.log (the fault, the module and offset, the last log
// lines) and sf4e-crash.dmp, written synchronously from the failing thread
// before the asynchronous logger gets a chance to lose them. The portable
// model lives in common/CrashReport.hxx.

#include <spdlog/spdlog.h>

namespace sf4e {
namespace crash {

// A logger sink that keeps the last lines in memory for the record.
spdlog::sink_ptr RingSink();

// Installs the handlers and sets where the record and the dump go. Call
// once, after the logger exists, on the game thread.
void Install(const wchar_t* logsDirectory);

// GGPO's assertion handler: records the message, then GGPO exits.
void OnGgpoAssertion(const char* message);

// One log line about the process at a match boundary (memory, the largest
// free region of the 32-bit address space, handles, threads, dropped log
// lines), and puts the crash filter back if something replaced it.
void NoteMatchBoundary(const char* label);

} // namespace crash
} // namespace sf4e
