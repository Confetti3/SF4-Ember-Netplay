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

#include "../common/CrashDump.hxx"
#include "../common/CrashReport.hxx"
#include "../common/EnvFlag.hxx"

// sf4e__Platform.hxx, declared here so the crash record builds without the
// game's headers (HeapCorruptionCaptureTest links it on its own).
namespace sf4e { namespace Platform { unsigned long long AsyncLogDropped(); } }

namespace {

using Ring = sf4e::crash::LogRing<64, 256>;

// The last log lines, filled by the logger's single worker thread.
Ring s_ring;
wchar_t s_recordPath[MAX_PATH] = {};
wchar_t s_logsDirectory[MAX_PATH] = {};
wchar_t s_dumpPath[sf4e::crash::DumpPathSize] = {};
LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
sf4e::crash::DumpClient s_dumpClient;
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
	// With the heap corrupt, anything that allocates may fault again and end
	// the process before the dump, so ask the launcher for it first; the
	// request allocates nothing.
	const bool heapCorrupt = sf4e::crash::IsHeapCorruption(pointers);
	const bool dumped = heapCorrupt && s_dumpClient.Request(pointers);
	const sf4e::crash::CrashFacts facts = FactsFor(kind, pointers, message);
	const size_t headerLength = sf4e::crash::FormatCrashHeader(s_header, sizeof(s_header), facts);
	HANDLE file = CreateFileW(s_recordPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) WriteText(file, s_header, headerLength);
	// The asynchronous logger may still hold the last lines: give its worker
	// a moment, then take the ring, which now also carries this line. After
	// heap corruption the worker may be stuck on the heap, and a flush could
	// wait for it forever, so the ring is taken as it stands.
	if (!heapCorrupt) {
		spdlog::critical("Crash: {:.{}}", s_header, headerLength ? headerLength - 1 : 0); // without the newline
		spdlog::default_logger()->flush();
		Sleep(250);
	}
	if (file != INVALID_HANDLE_VALUE) {
		WriteText(file, "last log lines:\n", 16);
		s_ring.ForEach([&](const char* line) {
			WriteText(file, line, strnlen(line, Ring::LineWidth));
			WriteText(file, "\n", 1);
		});
		CloseHandle(file);
	}
	if (!pointers || dumped) return;
	if (!heapCorrupt && s_dumpClient.Request(pointers)) return;
	sf4e::crash::WriteDump(GetCurrentProcess(), GetCurrentProcessId(), s_logsDirectory, GetCurrentThreadId(), pointers, false, s_dumpPath);
}

// Heap corruption ends the process without reaching the unhandled-exception
// filter; ntdll raises it once first, and only a vectored handler sees that.
LONG CALLBACK OnVectoredException(EXCEPTION_POINTERS* pointers) {
	if (sf4e::crash::IsHeapCorruption(pointers)) WriteRecord("heap_corruption", pointers, nullptr);
	return EXCEPTION_CONTINUE_SEARCH;
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

struct HeapCheck {
	unsigned interval = 0;
	unsigned count = 0;
	bool failed = false;
	const char* lastOperation = "start";
	int lastFrame = -1;
};

HeapCheck& HeapCheckState() {
	static HeapCheck check = [] {
		HeapCheck value;
		char text[16] = {};
		const DWORD length = GetEnvironmentVariableA("SF4E_HEAP_CHECK", text, sizeof(text));
		if (length && length < sizeof(text)) value.interval = strtoul(text, nullptr, 10);
		if (value.interval) spdlog::info("HeapCheck: validating every process heap after every {} save-state operations", value.interval);
		return value;
	}();
	return check;
}

// The first heap that does not validate, or nullptr.
HANDLE FirstInvalidHeap() {
	HANDLE heaps[256];
	const DWORD count = GetProcessHeaps(256, heaps);
	for (DWORD i = 0; i < count && i < 256; ++i)
		if (!HeapValidate(heaps[i], 0, nullptr)) return heaps[i];
	return nullptr;
}

void CheckHeaps(const char* operation, int frame, bool always) {
	HeapCheck& check = HeapCheckState();
	if (!check.interval || check.failed) return;
	if (!always && ++check.count % check.interval) return;
	const HANDLE invalid = FirstInvalidHeap();
	if (!invalid) {
		check.lastOperation = operation;
		check.lastFrame = frame;
		return;
	}
	check.failed = true;
	spdlog::error("HeapCheck: heap {} no longer validates after {} frame={}; it last passed after {} frame={}",
		(void*)invalid, operation, frame, check.lastOperation, check.lastFrame);
}

} // namespace

namespace sf4e {
namespace crash {

spdlog::sink_ptr RingSink() {
	return std::make_shared<LastLinesSink>();
}

void Install(const wchar_t* logsDirectory) {
	PathCombineW(s_recordPath, logsDirectory, L"sf4e-crash.log");
	wcsncpy_s(s_logsDirectory, logsDirectory, _TRUNCATE);
	// Room to write the record from a stack overflow on this thread.
	ULONG reserve = 16 * 1024;
	SetThreadStackGuarantee(&reserve);
	s_previousFilter = SetUnhandledExceptionFilter(OnUnhandledException);
	AddVectoredExceptionHandler(1, OnVectoredException);
	std::set_terminate(OnTerminate);
	_set_purecall_handler(OnPureCall);
	_set_invalid_parameter_handler(OnInvalidParameter);
	// A GGPO assertion exits the process; it used to show only a message box.
	ggpo_set_assert_handler(OnGgpoAssertion);
	spdlog::info("Crash record: sf4e-crash.log and sf4e-crash-*.dmp beside sf4e.log");
}

void ConfigureDumpChannel(HANDLE request, HANDLE done, HANDLE mailbox) {
	s_dumpClient.Configure(request, done, mailbox);
}

void OnGgpoAssertion(const char* message) {
	WriteRecord("ggpo_assertion", nullptr, message);
}

void HeapCheckpoint(const char* operation, int frame) {
	CheckHeaps(operation, frame, false);
}

void NoteMatchBoundary(const char* label) {
	CheckHeaps(label, -1, true);
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
