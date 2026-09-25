#include "sf4e__CrashDiagnostics.hxx"

#include <atomic>
#include <exception>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <Shlwapi.h>
#include <dbghelp.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <tlhelp32.h>

#include <ggponet.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include "../common/CrashReport.hxx"
#include "sf4e__Platform.hxx"

namespace {

using Ring = sf4e::crash::LogRing<64, 256>;

// The last log lines, filled by the logger's single worker thread.
Ring s_ring;
wchar_t s_recordPath[MAX_PATH] = {};
wchar_t s_dumpPath[MAX_PATH] = {};
LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
// One record per process: a fault inside the record must not recurse.
std::atomic<bool> s_recording(false);
// Static so the record needs no stack in a stack overflow.
char s_header[1024];
char s_module[MAX_PATH];
char s_detail[256];

// spdlog's thread pool has exactly one worker (sf4e__Platform.cxx), so the
// sink needs no lock of its own.
class LastLinesSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
protected:
	void sink_it_(const spdlog::details::log_msg& msg) override {
		spdlog::memory_buf_t formatted;
		formatter_->format(msg, formatted);
		size_t length = formatted.size();
		while (length && (formatted[length - 1] == '\n' || formatted[length - 1] == '\r')) --length;
		s_ring.Push(formatted.data(), length);
	}
	void flush_() override {}
};

void WriteText(HANDLE file, const char* text, size_t length) {
	DWORD written = 0;
	WriteFile(file, text, (DWORD)length, &written, nullptr);
}

// Names the module that contains `address` into s_module; 0 when unknown.
uintptr_t ModuleBaseOf(uintptr_t address) {
	s_module[0] = '\0';
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)address, &module) || !module) return 0;
	char path[MAX_PATH];
	if (GetModuleFileNameA(module, path, MAX_PATH)) {
		const char* name = strrchr(path, '\\');
		snprintf(s_module, sizeof(s_module), "%s", name ? name + 1 : path);
	}
	return (uintptr_t)module;
}

sf4e::crash::CrashFacts FactsFor(const char* kind, EXCEPTION_POINTERS* pointers, const char* message) {
	sf4e::crash::CrashFacts facts = { kind, 0, 0, "", 0, GetCurrentThreadId(), message ? message : "" };
	if (!pointers || !pointers->ExceptionRecord) return facts;
	const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
	facts.code = record.ExceptionCode;
	facts.address = (uintptr_t)record.ExceptionAddress;
	const uintptr_t base = ModuleBaseOf(facts.address);
	facts.module = s_module;
	facts.moduleOffset = base ? facts.address - base : 0;
	if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
		record.NumberParameters >= 2) {
		const char* access = record.ExceptionInformation[0] == 0 ? "read" : record.ExceptionInformation[0] == 1 ? "write" : "execute";
		snprintf(s_detail, sizeof(s_detail), "%s of 0x%08llX", access, (unsigned long long)record.ExceptionInformation[1]);
		facts.message = s_detail;
	}
	return facts;
}

void WriteRecord(const char* kind, EXCEPTION_POINTERS* pointers, const char* message) {
	if (s_recording.exchange(true)) return;
	const sf4e::crash::CrashFacts facts = FactsFor(kind, pointers, message);
	const size_t headerLength = sf4e::crash::FormatCrashHeader(s_header, sizeof(s_header), facts);
	HANDLE file = CreateFileW(s_recordPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) WriteText(file, s_header, headerLength);
	// The asynchronous logger may still hold the last lines: give its worker
	// a moment, then take the ring, which now also carries this line.
	spdlog::critical("Crash: {:.{}}", s_header, headerLength ? headerLength - 1 : 0); // without the newline
	spdlog::default_logger()->flush();
	Sleep(250);
	if (file != INVALID_HANDLE_VALUE) {
		WriteText(file, "last log lines:\n", 16);
		s_ring.ForEach([&](const char* line) {
			WriteText(file, line, strnlen(line, Ring::LineWidth));
			WriteText(file, "\n", 1);
		});
		CloseHandle(file);
	}
	if (!pointers) return;
	HANDLE dump = CreateFileW(s_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (dump == INVALID_HANDLE_VALUE) return;
	MINIDUMP_EXCEPTION_INFORMATION exception = { GetCurrentThreadId(), pointers, FALSE };
	MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal, &exception, nullptr, nullptr);
	CloseHandle(dump);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
	WriteRecord("unhandled_exception", pointers, nullptr);
	return s_previousFilter ? s_previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate() {
	WriteRecord("terminate", nullptr, "std::terminate");
	abort();
}

void OnPureCall() {
	WriteRecord("purecall", nullptr, "pure virtual call");
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
	WriteRecord("invalid_parameter", nullptr, "invalid CRT parameter");
}

unsigned CountThreads() {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;
	unsigned count = 0;
	const DWORD process = GetCurrentProcessId();
	THREADENTRY32 entry;
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID == process) ++count;
	}
	CloseHandle(snapshot);
	return count;
}

sf4e::crash::AddressSpaceSummary WalkAddressSpace() {
	sf4e::crash::AddressSpaceSummary summary;
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	MEMORY_BASIC_INFORMATION region;
	const char* cursor = (const char*)system.lpMinimumApplicationAddress;
	while (cursor < (const char*)system.lpMaximumApplicationAddress &&
		VirtualQuery(cursor, &region, sizeof(region)) == sizeof(region)) {
		if (region.State == MEM_FREE) summary.AddFreeRegion(region.RegionSize);
		cursor = (const char*)region.BaseAddress + region.RegionSize;
	}
	return summary;
}

} // namespace

namespace sf4e {
namespace crash {

spdlog::sink_ptr RingSink() {
	return std::make_shared<LastLinesSink>();
}

void Install(const wchar_t* logsDirectory) {
	PathCombineW(s_recordPath, logsDirectory, L"sf4e-crash.log");
	PathCombineW(s_dumpPath, logsDirectory, L"sf4e-crash.dmp");
	// Room to write the record from a stack overflow on this thread.
	ULONG reserve = 16 * 1024;
	SetThreadStackGuarantee(&reserve);
	s_previousFilter = SetUnhandledExceptionFilter(OnUnhandledException);
	std::set_terminate(OnTerminate);
	_set_purecall_handler(OnPureCall);
	_set_invalid_parameter_handler(OnInvalidParameter);
	// A GGPO assertion exits the process; it used to show only a message box.
	ggpo_set_assert_handler(OnGgpoAssertion);
	spdlog::info("Crash record: sf4e-crash.log and sf4e-crash.dmp beside sf4e.log");
}

void OnGgpoAssertion(const char* message) {
	WriteRecord("ggpo_assertion", nullptr, message);
}

void NoteMatchBoundary(const char* label) {
	PROCESS_MEMORY_COUNTERS_EX memory = {};
	memory.cb = sizeof(memory);
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&memory, sizeof(memory));
	const AddressSpaceSummary space = WalkAddressSpace();
	DWORD handles = 0;
	GetProcessHandleCount(GetCurrentProcess(), &handles);
	// Something else may have taken the filter since Install; take it back
	// and chain to it, so the record is written either way.
	const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(OnUnhandledException);
	const bool restored = current != OnUnhandledException;
	if (restored && current) s_previousFilter = current;
	const double mb = 1024.0 * 1024.0;
	spdlog::info("Process [{}]: private={:.0f}MB working={:.0f}MB largest_free={:.0f}MB free={:.0f}MB free_regions={} "
		"handles={} threads={} log_dropped={} crash_filter={}",
		label, memory.PrivateUsage / mb, memory.WorkingSetSize / mb, space.largestFree / mb, space.totalFree / mb,
		space.freeRegions, handles, CountThreads(), sf4e::Platform::AsyncLogDropped(), restored ? "restored" : "ours");
}

} // namespace crash
} // namespace sf4e
