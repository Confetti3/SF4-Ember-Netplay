#include "sf4e__CrashDiagnostics.hxx"
#include "sf4e__ImportPatch.hxx"

#include <atomic>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <Shlwapi.h>
#include <dbghelp.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <ggponet.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include "../common/AddressSpaceReport.hxx"
#include "../common/CrashDump.hxx"
#include "../common/CrashReport.hxx"
#include "../common/CrashUnloaded.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/SpeechCrash.hxx"

// sf4e__Platform.hxx, declared here so the crash record builds without the
// game's headers (HeapCorruptionCaptureTest links it on its own).
namespace sf4e { namespace Platform { unsigned long long AsyncLogDropped(); } }

namespace {

using Ring = sf4e::crash::LogRing<64, 256>;

// The last log lines, filled by the logger's single worker thread.
Ring s_ring;
wchar_t s_recordPath[MAX_PATH] = {};
wchar_t s_logsDirectory[MAX_PATH] = {};
wchar_t s_speechMarkerPath[MAX_PATH] = {};
wchar_t s_dumpPath[sf4e::crash::DumpPathSize] = {};
LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
sf4e::crash::DumpClient s_dumpClient;
// One record per process: a fault inside the record must not recurse.
std::atomic<bool> s_recording(false);
// Static so the record needs no stack in a stack overflow.
char s_header[1024];
char s_module[MAX_PATH];
char s_detail[256];
char s_callers[640];
char s_registers[256];
char s_code[128];
// What a fault in no loaded module adds. The stack words come last and stay
// out of the log, so they cannot crowd the ring.
char s_unmapped[2560];
// One line and its newline, so they go out in a single write.
char s_line[704];

// The game's and Sidecar's images, found at Install so the crash path only
// compares numbers. The callers line and the stack words read the same table.
sf4e::crash::ImageTable s_loadedImages;

// ntdll's own readers, resolved at Install so the crash path calls no loader
// function. A null pointer leaves its part out of the record.
using QueryThreadFn = LONG (NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using UnloadTraceFn = void (NTAPI*)(ULONG**, ULONG**, PVOID*);
struct NtdllReaders {
	QueryThreadFn queryThread = nullptr;
	UnloadTraceFn unloadTrace = nullptr;
};
NtdllReaders s_ntdll;
// The THREADINFOCLASS value ThreadQuerySetWin32StartAddress.
constexpr int Win32StartAddressClass = 9;
// A copy of ntdll's unload array, read through the kernel.
uint8_t s_unloadTrace[sf4e::crash::UnloadTraceMaxBytes];
constexpr size_t StackWordsKept = 32;

using sf4e::crash::UnloadedImages;

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

// Names the module that contains `address` into `name`; 0 when unknown.
uintptr_t ModuleBaseOf(uintptr_t address, char (&name)[MAX_PATH]) {
	name[0] = '\0';
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)address, &module) || !module) return 0;
	char path[MAX_PATH];
	if (GetModuleFileNameA(module, path, MAX_PATH)) {
		const char* file = strrchr(path, '\\');
		snprintf(name, MAX_PATH, "%s", file ? file + 1 : path);
	}
	return (uintptr_t)module;
}

// A fault inside Windows' speech stack leaves a marker beside the record, so the
// next start can turn menu speech off instead of crashing the same way again.
// Plain file calls and a stack buffer only: this runs inside the crash handler.
void NoteSpeechCrash(const char* module) {
	char text[160];
	const size_t length = sf4e::crash::FormatSpeechCrashMarker(text, sizeof(text), module);
	if (!length || !s_speechMarkerPath[0]) return;
	const HANDLE file = CreateFileW(s_speechMarkerPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	WriteText(file, text, length);
	CloseHandle(file);
}

sf4e::crash::CrashFacts FactsFor(const char* kind, EXCEPTION_POINTERS* pointers, const char* message) {
	sf4e::crash::CrashFacts facts = { kind, 0, 0, "", 0, GetCurrentThreadId(), message ? message : "" };
	if (!pointers || !pointers->ExceptionRecord) return facts;
	const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
	facts.code = record.ExceptionCode;
	facts.address = (uintptr_t)record.ExceptionAddress;
	const uintptr_t base = ModuleBaseOf(facts.address, s_module);
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

void NoteImage(HMODULE module) {
	if (!module) return;
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
	char path[MAX_PATH];
	if (!GetModuleFileNameA(module, path, MAX_PATH)) return;
	const char* name = strrchr(path, '\\');
	s_loadedImages.Add(name ? name + 1 : path, reinterpret_cast<uintptr_t>(module), nt->OptionalHeader.SizeOfImage, false);
}

// A line and its newline in one write, so a line another thread appends to the
// record cannot land between them.
void WriteLine(HANDLE file, const char* text, size_t length) {
	if (length > sizeof(s_line) - 1) length = sizeof(s_line) - 1;
	memcpy(s_line, text, length);
	s_line[length] = '\n';
	WriteText(file, s_line, length + 1);
}

// Return addresses on the faulting thread's stack, so a report sent without
// a dump still names the calls that led to the fault. It scans rather than
// unwinds: every stack word that points just past a call in the game or
// Sidecar is listed, so a stale frame can appear among the live ones. Reads
// only this thread's own committed stack and the two modules' images. A
// nonzero scanFrom starts the scan higher than the context's Esp.
const char* FormatCallers(const EXCEPTION_POINTERS* pointers, uintptr_t scanFrom) {
	s_callers[0] = '\0';
	if (!pointers || !pointers->ContextRecord) return s_callers;
	const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase);
	uintptr_t sp = pointers->ContextRecord->Esp;
	if (scanFrom > sp) sp = scanFrom;
	// A fault on another thread's stack, or a corrupt Esp, is not scanned.
	if (sp < reinterpret_cast<uintptr_t>(tib->StackLimit) || sp >= top) return s_callers;
	size_t length = (size_t)snprintf(s_callers, sizeof(s_callers), "callers:");
	unsigned found = 0;
	for (unsigned words = 0; words < 2048 && sp + sizeof(uintptr_t) <= top && found < 16; ++words, sp += sizeof(uintptr_t)) {
		const uintptr_t value = *reinterpret_cast<const uintptr_t*>(sp);
		const sf4e::crash::ImageRange* range = s_loadedImages.Find(value);
		if (!range || value < range->base + 0x1000 + 7) continue;
		if (!sf4e::crash::EndsInCall(reinterpret_cast<const uint8_t*>(value - 7))) continue;
		if (!sf4e::crash::AppendCaller(s_callers, sizeof(s_callers), length, range->name, value - range->base)) return s_callers;
		++found;
	}
	if (!found) s_callers[0] = '\0';
	return s_callers;
}

sf4e::crash::CrashRegisters RegistersOf(const CONTEXT& context) {
	return { context.Eax, context.Ebx, context.Ecx, context.Edx, context.Esi, context.Edi, context.Ebp, context.Esp, context.Eip };
}

// The registers at the fault as one line in s_registers; empty without a
// context. Returns the line's length.
size_t FormatRegistersLine(const EXCEPTION_POINTERS* pointers) {
	s_registers[0] = '\0';
	if (!pointers || !pointers->ContextRecord) return 0;
	return sf4e::crash::FormatRegisters(s_registers, sizeof(s_registers), RegistersOf(*pointers->ContextRecord));
}

// Reads this process's own memory through the kernel: an address that is not
// mapped gives fewer bytes instead of a second fault, and nothing is allocated.
// Returns the bytes read.
SIZE_T ReadOwnMemory(const void* from, void* to, size_t bytes) {
	SIZE_T read = 0;
	ReadProcessMemory(GetCurrentProcess(), from, to, bytes, &read);
	return read > bytes ? 0 : read;
}

// The 16 bytes at the faulting instruction as one line in s_code; empty
// without an exception record. Returns the line's length.
size_t FormatCodeLine(const EXCEPTION_POINTERS* pointers) {
	s_code[0] = '\0';
	if (!pointers || !pointers->ExceptionRecord) return 0;
	const uintptr_t address = reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
	uint8_t bytes[16] = {};
	const SIZE_T read = ReadOwnMemory(reinterpret_cast<const void*>(address), bytes, sizeof(bytes));
	return sf4e::crash::FormatCodeBytes(s_code, sizeof(s_code), address, bytes, read);
}

// The modules ntdll last unloaded, newest first. The getter's last value is
// the address of ntdll's variable that holds the array pointer, which stays
// null until a module has been unloaded.
UnloadedImages ReadUnloadedImages() {
	UnloadedImages none;
	if (!s_ntdll.unloadTrace) return none;
	ULONG* sizeAt = nullptr;
	ULONG* countAt = nullptr;
	PVOID holder = nullptr;
	s_ntdll.unloadTrace(&sizeAt, &countAt, &holder);
	ULONG elementSize = 0, elementCount = 0;
	uintptr_t array = 0;
	if (ReadOwnMemory(sizeAt, &elementSize, sizeof(elementSize)) != sizeof(elementSize) ||
		ReadOwnMemory(countAt, &elementCount, sizeof(elementCount)) != sizeof(elementCount) ||
		ReadOwnMemory(holder, &array, sizeof(array)) != sizeof(array) || !array) return none;
	const size_t bytes = sf4e::crash::UnloadTraceBytes(elementSize, elementCount);
	if (!bytes || ReadOwnMemory(reinterpret_cast<const void*>(array), s_unloadTrace, bytes) != bytes) return none;
	return sf4e::crash::ParseUnloadTrace(s_unloadTrace, elementSize, elementCount);
}

sf4e::crash::AddressRegion RegionOf(const MEMORY_BASIC_INFORMATION& memory) {
	using sf4e::crash::RegionState;
	using sf4e::crash::RegionType;
	sf4e::crash::AddressRegion region;
	region.state = memory.State == MEM_FREE ? RegionState::Free : memory.State == MEM_RESERVE ? RegionState::Reserved :
		memory.State == MEM_COMMIT ? RegionState::Committed : RegionState::Unknown;
	region.type = memory.Type == MEM_IMAGE ? RegionType::Image : memory.Type == MEM_MAPPED ? RegionType::Mapped :
		memory.Type == MEM_PRIVATE ? RegionType::Private : RegionType::None;
	region.protect = memory.Protect;
	region.allocationBase = reinterpret_cast<uintptr_t>(memory.AllocationBase);
	region.regionBase = reinterpret_cast<uintptr_t>(memory.BaseAddress);
	region.regionSize = memory.RegionSize;
	return region;
}

struct UnmappedLines {
	size_t logged = 0; // the bytes in s_unmapped that also go to the log
	size_t total = 0;  // all of them, the stack words included
};

// What a fault in no loaded module leaves to name its cause: the region the
// address is in now, the images ntdll recently unloaded and whether the fault
// or a register points into one, the thread's Win32 start address, and the
// first stack words that point into a known image. Addresses are annotated by
// comparing numbers against the table; the only loader lookup is the start
// address, and only when no table range holds it.
UnmappedLines FormatUnmappedLines(const EXCEPTION_POINTERS* pointers) {
	UnmappedLines lines;
	s_unmapped[0] = '\0';
	if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord) return lines;
	const CONTEXT& context = *pointers->ContextRecord;
	const uintptr_t address = reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
	const UnloadedImages unloaded = ReadUnloadedImages();
	sf4e::crash::ImageTable images = s_loadedImages;
	images.AddUnloaded(unloaded);
	MEMORY_BASIC_INFORMATION memory = {};
	sf4e::crash::AddressRegion region;
	if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == sizeof(memory)) region = RegionOf(memory);
	size_t& length = lines.logged;
	length += sf4e::crash::FormatAddressRegion(s_unmapped + length, sizeof(s_unmapped) - length, address, region);
	length += sf4e::crash::FormatUnloadedHits(s_unmapped + length, sizeof(s_unmapped) - length, unloaded, address, RegistersOf(context));
	length += sf4e::crash::FormatUnloadedList(s_unmapped + length, sizeof(s_unmapped) - length, unloaded);
	uintptr_t start = 0;
	if (s_ntdll.queryThread && s_ntdll.queryThread(GetCurrentThread(), Win32StartAddressClass, &start, sizeof(start), nullptr) >= 0 && start) {
		char description[96];
		if (!sf4e::crash::DescribeImage(description, sizeof(description), start, images)) {
			char name[MAX_PATH];
			const uintptr_t base = ModuleBaseOf(start, name);
			if (base) sf4e::crash::FormatImageOffset(description, sizeof(description), name, start - base, false);
		}
		length += sf4e::crash::FormatLocatedAddress(s_unmapped + length, sizeof(s_unmapped) - length, "start address", start, description);
	}
	lines.total = length;
	// Only this thread's committed stack, between the limits its TIB gives.
	const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	const uintptr_t esp = context.Esp;
	if (esp >= reinterpret_cast<uintptr_t>(tib->StackLimit) && esp < reinterpret_cast<uintptr_t>(tib->StackBase)) {
		uint32_t words[StackWordsKept];
		const size_t room = (reinterpret_cast<uintptr_t>(tib->StackBase) - esp) / sizeof(uint32_t);
		const size_t read = ReadOwnMemory(reinterpret_cast<const void*>(esp), words, (room < StackWordsKept ? room : StackWordsKept) * sizeof(uint32_t));
		lines.total += sf4e::crash::FormatStackWords(s_unmapped + length, sizeof(s_unmapped) - length, static_cast<uint32_t>(esp), words,
			read / sizeof(uint32_t), images);
	}
	return lines;
}

// Each line of the first `length` bytes of `text` as a Crash log line.
void LogRecordLines(const char* text, size_t length) {
	for (const char* line = text; line < text + length;) {
		const char* end = static_cast<const char*>(memchr(line, '\n', (size_t)(text + length - line)));
		const size_t lineLength = end ? (size_t)(end - line) : (size_t)(text + length - line);
		spdlog::critical("Crash {:.{}}", line, lineLength);
		line += lineLength + 1;
	}
}

// Adds one whole line to the record from any thread. It opens its own append
// handle, so it neither shares the recording thread's handle nor lands inside
// one of that thread's lines, which are written whole.
void AppendRecordLine(const char* text, size_t length) {
	const HANDLE file = CreateFileW(s_recordPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	WriteText(file, text, length);
	CloseHandle(file);
}

// Whether this process wrote a dump after the launcher did not.
enum class InProcessDump { NotTried, Wrote, Failed };

// Says in the record how the dump went, so a missing dump is explained by the
// record that was sent without it.
void NoteDumpOutcome(sf4e::crash::DumpAnswer answer, InProcessDump inProcess) {
	char line[192];
	const char* named = sf4e::crash::DumpAnswerName(answer);
	const int printed = inProcess == InProcessDump::NotTried ? snprintf(line, sizeof(line), "dump: %s\n", named) :
		snprintf(line, sizeof(line), "dump: %s; this process %s\n", named, inProcess == InProcessDump::Wrote ? "wrote one" : "could not write one");
	if (printed > 0) AppendRecordLine(line, (size_t)printed < sizeof(line) ? (size_t)printed : sizeof(line) - 1);
}

// The game left through ExitProcess while a crash record was being written or
// already had been. That exit leaves no record of its own, and on another
// thread it can end the process before the dump was asked for.
void NoteExitAfterRecord(UINT code, const void* caller) {
	char label[96], description[96], line[256];
	snprintf(label, sizeof(label), "ExitProcess(%u) on thread %lu after the crash record began, called from", code, GetCurrentThreadId());
	sf4e::crash::DescribeImage(description, sizeof(description), reinterpret_cast<uintptr_t>(caller), s_loadedImages);
	const size_t length = sf4e::crash::FormatLocatedAddress(line, sizeof(line), label, reinterpret_cast<uintptr_t>(caller), description);
	if (length) AppendRecordLine(line, length);
}

// dumpInProcess=false never falls back to MiniDumpWriteDump in this process,
// which can deadlock on the loader lock; the launcher's bounded request is
// the only dump then. False when a record was already begun, which this call
// then leaves alone.
bool WriteRecord(const char* kind, EXCEPTION_POINTERS* pointers, const char* message, bool dumpInProcess = true, uintptr_t scanFrom = 0) {
	if (s_recording.exchange(true)) return false;
	// With the heap corrupt, anything that allocates may fault again and end
	// the process before the dump, so ask the launcher for it first; the
	// request allocates nothing.
	const bool heapCorrupt = sf4e::crash::IsHeapCorruption(pointers);
	sf4e::crash::DumpAnswer answer = heapCorrupt ? s_dumpClient.Request(pointers) : sf4e::crash::DumpAnswer::NotAsked;
	const sf4e::crash::CrashFacts facts = FactsFor(kind, pointers, message);
	// Before the record, which does more and could fault itself.
	if (pointers && sf4e::crash::IsSpeechEngineModule(facts.module)) NoteSpeechCrash(facts.module);
	const size_t headerLength = sf4e::crash::FormatCrashHeader(s_header, sizeof(s_header), facts);
	// The record holds this crash only: a crash report sends it, and one that
	// carried earlier crashes would be grouped with them. It is emptied first
	// and then written through an append handle, so a line another thread adds
	// later goes after ours instead of being written over.
	const HANDLE emptied = CreateFileW(s_recordPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (emptied != INVALID_HANDLE_VALUE) CloseHandle(emptied);
	HANDLE file = CreateFileW(s_recordPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) WriteText(file, s_header, headerLength);
	const size_t registersLength = FormatRegistersLine(pointers);
	const size_t codeLength = FormatCodeLine(pointers);
	if (file != INVALID_HANDLE_VALUE) {
		if (registersLength) WriteText(file, s_registers, registersLength);
		if (codeLength) WriteText(file, s_code, codeLength);
	}
	const char* callers = FormatCallers(pointers, scanFrom);
	if (file != INVALID_HANDLE_VALUE && callers[0]) {
		WriteLine(file, callers, strlen(callers));
	}
	// A fault in no loaded module is code that is gone: what the address is
	// now and which unloaded image it was in say what took it away.
	const UnmappedLines unmapped = facts.module[0] ? UnmappedLines() : FormatUnmappedLines(pointers);
	if (file != INVALID_HANDLE_VALUE) {
		if (unmapped.total) WriteText(file, s_unmapped, unmapped.total);
	}
	// The asynchronous logger may still hold the last lines: give its worker
	// a moment, then take the ring, which now also carries this line. After
	// heap corruption the worker may be stuck on the heap, and a flush could
	// wait for it forever, so the ring is taken as it stands.
	if (!heapCorrupt) {
		spdlog::critical("Crash: {:.{}}", s_header, headerLength ? headerLength - 1 : 0); // without the newline
		if (registersLength) spdlog::critical("Crash {:.{}}", s_registers, registersLength - 1);
		if (codeLength) spdlog::critical("Crash {:.{}}", s_code, codeLength - 1);
		if (callers[0]) spdlog::critical("Crash {}", callers);
		if (unmapped.logged) LogRecordLines(s_unmapped, unmapped.logged);
		spdlog::default_logger()->flush();
		Sleep(250);
	}
	if (file != INVALID_HANDLE_VALUE) {
		WriteText(file, "last log lines:\n", 16);
		s_ring.ForEach([&](const char* line) {
			WriteLine(file, line, strnlen(line, Ring::LineWidth));
		});
		CloseHandle(file);
	}
	if (!pointers) return true;
	if (!heapCorrupt) answer = s_dumpClient.Request(pointers);
	InProcessDump inProcess = InProcessDump::NotTried;
	if (answer != sf4e::crash::DumpAnswer::Written && dumpInProcess)
		inProcess = sf4e::crash::WriteDump(GetCurrentProcess(), GetCurrentProcessId(), s_logsDirectory, GetCurrentThreadId(), pointers, false, s_dumpPath) ?
			InProcessDump::Wrote : InProcessDump::Failed;
	NoteDumpOutcome(answer, inProcess);
	return true;
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

// The game's own ExitProcess import. Its C runtime ends a fatal runtime
// error with _exit(255), which raises no exception, so the handlers above
// never see it and the player is left with an exit code and nothing else.
// Set before the hook is published, so the hook never finds it empty.
std::atomic<void*> s_exitProcess(nullptr);

__declspec(noinline) void WINAPI OnExitProcess(UINT code) {
	if (code != 0) {
		// A record and a dump with this thread's stack, which shows the call
		// that chose to exit. The record lives on this stack; the launcher
		// reads it while this thread waits.
		CONTEXT context = {};
		RtlCaptureContext(&context);
		EXCEPTION_RECORD record = {};
		record.ExceptionCode = code;
		record.ExceptionAddress = _ReturnAddress();
		EXCEPTION_POINTERS pointers = { &record, &context };
		char message[32];
		snprintf(message, sizeof(message), "ExitProcess(%u)", code);
		// The captured Esp is this function's own, whose locals hold a return
		// address into Sidecar: the callers start above the game's call instead.
		if (!WriteRecord("exit", &pointers, message, false, reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) + sizeof(void*)))
			NoteExitAfterRecord(code, record.ExceptionAddress);
	}
	void* original = s_exitProcess.load();
	if (!original) original = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess");
	reinterpret_cast<decltype(&ExitProcess)>(original)(code);
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

// The one VirtualQuery walk over the address space: `visit` is called with
// each region in address order.
template <typename Visit>
void ForEachRegion(Visit visit) {
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	MEMORY_BASIC_INFORMATION region;
	const char* cursor = (const char*)system.lpMinimumApplicationAddress;
	while (cursor < (const char*)system.lpMaximumApplicationAddress &&
		VirtualQuery(cursor, &region, sizeof(region)) == sizeof(region)) {
		visit(region);
		cursor = (const char*)region.BaseAddress + region.RegionSize;
	}
}

sf4e::crash::AddressSpaceSummary WalkAddressSpace() {
	sf4e::crash::AddressSpaceSummary summary;
	ForEachRegion([&](const MEMORY_BASIC_INFORMATION& region) {
		if (region.State == MEM_FREE) summary.AddFreeRegion(region.RegionSize);
	});
	return summary;
}

sf4e::crash::Region ToRegion(const MEMORY_BASIC_INFORMATION& region) {
	using sf4e::crash::RegionState;
	using sf4e::crash::RegionType;
	sf4e::crash::Region out;
	out.base = reinterpret_cast<uintptr_t>(region.BaseAddress);
	out.size = region.RegionSize;
	out.state = region.State == MEM_FREE ? RegionState::Free : region.State == MEM_RESERVE ? RegionState::Reserved : RegionState::Committed;
	if (region.State != MEM_FREE) {
		out.type = region.Type == MEM_IMAGE ? RegionType::Image : region.Type == MEM_MAPPED ? RegionType::Mapped : RegionType::Private;
		out.allocationBase = reinterpret_cast<uintptr_t>(region.AllocationBase);
	}
	return out;
}

// The base file name of the module loaded at `allocationBase`, or empty.
std::string ModuleFileName(std::uint64_t allocationBase) {
	char path[MAX_PATH] = {};
	HMODULE module = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(allocationBase));
	const DWORD length = GetModuleFileNameA(module, path, sizeof(path));
	if (!length || length >= sizeof(path)) return std::string();
	const char* name = strrchr(path, '\\');
	return name ? name + 1 : path;
}

struct HeapCheck {
	unsigned interval = 0;
	bool environmentSet = false;
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
		value.environmentSet = length != 0;
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
	PathCombineW(s_speechMarkerPath, logsDirectory, sf4e::crash::SpeechCrashMarkerFile);
	NoteImage(GetModuleHandleW(nullptr));
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	s_ntdll.queryThread = reinterpret_cast<QueryThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
	s_ntdll.unloadTrace = reinterpret_cast<UnloadTraceFn>(GetProcAddress(ntdll, "RtlGetUnloadEventTraceEx"));
	if (!s_ntdll.unloadTrace) spdlog::warn("Crash record: ntdll has no RtlGetUnloadEventTraceEx; a fault in unloaded code will not name the module");
	HMODULE sidecar = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&Install), &sidecar) && sidecar != GetModuleHandleW(nullptr))
		NoteImage(sidecar);
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

void WatchGameExit() {
	if (s_exitProcess.load()) return;
	if (!imports::PatchImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "ExitProcess", (void*)&OnExitProcess, s_exitProcess))
		spdlog::warn("Crash record: the game's ExitProcess import was not found; a non-zero exit is not recorded");
}

void ConfigureDumpChannel(HANDLE request, HANDLE done, HANDLE mailbox) {
	s_dumpClient.Configure(request, done, mailbox);
}

void OnGgpoAssertion(const char* message) {
	WriteRecord("ggpo_assertion", nullptr, message);
}

bool HeapCheckEnabled() {
	return HeapCheckState().interval != 0;
}

void ConfigureHeapCheck(unsigned interval) {
	HeapCheck& check = HeapCheckState();
	if (check.environmentSet) return;
	check.interval = interval;
	if (interval) spdlog::info("HeapCheck: validating every process heap after every {} save-state operations (settings.json)", interval);
}

void HeapCheckpoint(const char* operation, int frame) {
	CheckHeaps(operation, frame, false);
}

unsigned long long LargestFreeRange() {
	return WalkAddressSpace().largestFree;
}

void NoteAddressSpace() {
	std::vector<Region> regions;
	ForEachRegion([&](const MEMORY_BASIC_INFORMATION& region) { regions.push_back(ToRegion(region)); });
	AddressSpaceReport report = SummarizeAddressSpace(regions);
	for (Allocation& allocation : report.largestAllocations)
		if (allocation.type == RegionType::Image) allocation.image = ModuleFileName(allocation.base);
	for (const std::string& line : FormatAddressSpace(report)) spdlog::info("{}", line);
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
	// The system's commit limit and what is left of it, so a stall from commit
	// exhaustion reads differently from one with the address space still free.
	MEMORYSTATUSEX system = {};
	system.dwLength = sizeof(system);
	const bool haveCommit = GlobalMemoryStatusEx(&system) != FALSE;
	const double mb = 1024.0 * 1024.0;
	// Growth since the last boundary of the same kind, so a leak across
	// matches reads from one line per match. The load watch's reports can
	// come from another thread than the game's, so the table has a lock.
	static std::map<std::string, SIZE_T> previous;
	static std::mutex previousLock;
	double growth = 0.0;
	{
		std::lock_guard<std::mutex> guard(previousLock);
		auto& last = previous[label];
		growth = last ? (static_cast<double>(memory.PrivateUsage) - static_cast<double>(last)) / mb : 0.0;
		last = memory.PrivateUsage;
	}
	spdlog::info("Process [{}]: private={:.0f}MB ({:+.0f}MB since the last) working={:.0f}MB largest_free={:.0f}MB free={:.0f}MB "
		"free_regions={} handles={} threads={} commit_limit={:.0f}MB commit_free={:.0f}MB log_dropped={} crash_filter={}",
		label, memory.PrivateUsage / mb, growth, memory.WorkingSetSize / mb, space.largestFree / mb, space.totalFree / mb,
		space.freeRegions, handles, CountThreads(), haveCommit ? system.ullTotalPageFile / mb : -1.0,
		haveCommit ? system.ullAvailPageFile / mb : -1.0, sf4e::Platform::AsyncLogDropped(), restored ? "restored" : "ours");
}

} // namespace crash
} // namespace sf4e
