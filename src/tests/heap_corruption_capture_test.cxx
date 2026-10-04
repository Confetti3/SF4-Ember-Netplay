// Heap corruption (0xC0000374) skips the unhandled-exception filter, so a
// game that died of it left no crash record. This runs the production path
// across two processes: a child installs Sidecar's crash handler
// (sf4e__CrashDiagnostics.cxx) with the launcher's dump channel and corrupts
// a heap, and this process waits on it with the launcher's loop
// (DumpChannel::ServeUntilExit), writing the dump it asks for.

#include "../common/CrashDump.hxx"
#include "../sf4e/sf4e__CrashDiagnostics.hxx"

#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdlib.h>
#include <string>

#include <spdlog/async.h>

#include "test_support.hxx"

using namespace sf4e::crash;

// The one platform call the crash record makes.
namespace sf4e { namespace Platform { unsigned long long AsyncLogDropped() { return 0; } } }

namespace {

// Bytes no module holds: built at run time into a heap block nothing points
// at, so only a dump that keeps heap memory can contain them.
constexpr size_t MarkerSize = 256;
void FillMarker(unsigned char* out) {
	for (size_t i = 0; i < MarkerSize; ++i) out[i] = static_cast<unsigned char>((i * 131 + 17) ^ 0xA5);
}

// On a thread that then exits, so no stack, live or stale, still holds the
// block's address for a dump to follow.
DWORD WINAPI MarkerThread(void*) {
	HANDLE heap = HeapCreate(0, 0, 0);
	auto* block = static_cast<unsigned char*>(heap ? HeapAlloc(heap, 0, MarkerSize) : nullptr);
	if (!block) return 1;
	FillMarker(block);
	return 0;
}

void LeaveUnreferencedMarker() {
	HANDLE thread = CreateThread(nullptr, 0, MarkerThread, nullptr, 0, nullptr);
	CHECK(thread);
	WaitForSingleObject(thread, INFINITE);
	DWORD code = 1;
	GetExitCodeThread(thread, &code);
	CloseHandle(thread);
	CHECK(code == 0);
}

bool FileContainsMarker(const std::wstring& path) {
	std::ifstream in(path.c_str(), std::ios::binary);
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	unsigned char marker[MarkerSize];
	FillMarker(marker);
	return bytes.find(std::string(reinterpret_cast<const char*>(marker), MarkerSize)) != std::string::npos;
}

HANDLE HandleArgument(const char* text) {
	return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(strtoull(text, nullptr, 16)));
}

// child <heap> <logs directory> <request> <done> <mailbox>. "private"
// corrupts a heap of its own; "process" corrupts the process heap, the CRT
// heap the game shares, where anything that allocates afterwards may fault.
int RunChild(char** argv) {
	std::vector<spdlog::sink_ptr> sinks{RingSink()};
	spdlog::init_thread_pool(8192, 1);
	spdlog::set_default_logger(std::make_shared<spdlog::async_logger>("sf4e", sinks.begin(), sinks.end(),
		spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest));
	spdlog::info("the last line before the corruption");
	wchar_t logs[MAX_PATH] = {};
	MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, logs, MAX_PATH);
	Install(logs);
	ConfigureDumpChannel(HandleArgument(argv[4]), HandleArgument(argv[5]), HandleArgument(argv[6]));
	Sleep(300); // the logger's worker puts the line in the ring
	if (std::string(argv[2]) == "exit") {
		// The game's C runtime ends a fatal runtime error this way.
		WatchGameExit();
		ExitProcess(255);
	}
	LeaveUnreferencedMarker();
	HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
	HANDLE heap = std::string(argv[2]) == "process" ? GetProcessHeap() : HeapCreate(0, 0, 0);
	if (!heap) return 71;
	// A double free is the usual shape of the game's crash. If this heap
	// tolerates it, smash the next block's header as well.
	char* block = static_cast<char*>(HeapAlloc(heap, 0, 64));
	char* next = static_cast<char*>(HeapAlloc(heap, 0, 64));
	HeapFree(heap, 0, block);
	HeapFree(heap, 0, block);
	memset(next - 16, 0x41, 16);
	HeapFree(heap, 0, next);
	HeapValidate(heap, 0, nullptr);
	return 77; // the heap never noticed
}

std::string Narrow(const std::wstring& text) {
	char out[MAX_PATH * 3] = {};
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out, sizeof(out), nullptr, nullptr);
	return out;
}

std::wstring TempDirectory() {
	wchar_t base[MAX_PATH] = {}, path[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, base);
	GetTempFileNameW(base, L"sf4", 0, path);
	DeleteFileW(path);
	CreateDirectoryW(path, nullptr);
	return path;
}

// The exception code the dump itself records.
DWORD DumpedExceptionCode(const std::wstring& path, ULONGLONG& size) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) return 0;
	LARGE_INTEGER length = {};
	GetFileSizeEx(file, &length);
	size = static_cast<ULONGLONG>(length.QuadPart);
	HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
	void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
	DWORD code = 0;
	MINIDUMP_DIRECTORY* entry = nullptr;
	void* stream = nullptr;
	ULONG streamSize = 0;
	if (view && MiniDumpReadDumpStream(view, ExceptionStream, &entry, &stream, &streamSize) && stream)
		code = static_cast<MINIDUMP_EXCEPTION_STREAM*>(stream)->ExceptionRecord.ExceptionCode;
	if (view) UnmapViewOfFile(view);
	if (mapping) CloseHandle(mapping);
	CloseHandle(file);
	return code;
}

// Finished dumps only: names that end in ".dmp".
size_t CountDumps(const std::wstring& directory) {
	size_t count = 0;
	for (const auto& name : DumpFiles(directory.c_str(), L"*"))
		if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".dmp") == 0) ++count;
	return count;
}

size_t Count(const std::string& text, const std::string& part) {
	size_t count = 0;
	for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
	return count;
}

void TestHandlerRecordsAndLauncherDumps(const wchar_t* heap) {
	DumpChannel channel;
	CHECK(channel.Create(true));
	const std::wstring logs = TempDirectory();
	wchar_t self[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	wchar_t command[2048] = {};
	swprintf_s(command, L"\"%s\" child %s \"%s\" %p %p %p", self, heap, logs.c_str(), channel.request, channel.done, channel.mailbox);
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process = {};
	// No Windows Error Reporting dialog for the child's deliberate crash.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	CHECK(CreateProcessW(self, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process));

	std::wstring dump;
	int served = 0, written = 0;
	channel.ServeUntilExit(process.hProcess, logs.c_str(), [&](bool ok) {
		++served; written += ok ? 1 : 0;
		if (ok) dump = channel.written;
	});
	DWORD exitCode = 0;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	channel.Close();

	if (exitCode != HeapCorruptionCode) std::fprintf(stderr, "child exit code 0x%08lX\n", exitCode);
	CHECK(exitCode == HeapCorruptionCode);
	// One request, served once.
	CHECK(served == 1 && written == 1);
	ULONGLONG size = 0;
	CHECK(dump.find(logs + L"\\sf4e-crash-") == 0);
	CHECK(IsHeapDumpName(dump.c_str()));
	CHECK(DumpedExceptionCode(dump, size) == HeapCorruptionCode);
	// The heap itself is in the dump, not only what the stacks point at.
	CHECK(FileContainsMarker(dump));
	CHECK(CountDumps(logs) == 1);
	std::printf("heap_corruption_capture_test: %ls heap dump is %llu KB\n", heap, size / 1024);
	// MiniDumpNormal gave about 64 KB, too little to follow a corruption.
	CHECK(size > 64 * 1024);
	// One record, naming the fault, with the lines logged before it.
	std::ifstream in(Narrow(logs + L"\\sf4e-crash.log"), std::ios::binary);
	std::stringstream record;
	record << in.rdbuf();
	in.close();
	CHECK(Count(record.str(), "kind=heap_corruption code=0xC0000374") == 1);
	CHECK(record.str().find("the last line before the corruption") != std::string::npos);
	DeleteFileW(dump.c_str());
	DeleteFileW((logs + L"\\sf4e-crash.log").c_str());
	RemoveDirectoryW(logs.c_str());
}

// A non-zero ExitProcess through the module's own import leaves a record
// naming the exit and a dump of the exiting thread, then exits as asked.
void TestNonZeroExitIsRecorded() {
	DumpChannel channel;
	CHECK(channel.Create(true));
	const std::wstring logs = TempDirectory();
	wchar_t self[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	wchar_t command[2048] = {};
	swprintf_s(command, L"\"%s\" child exit \"%s\" %p %p %p", self, logs.c_str(), channel.request, channel.done, channel.mailbox);
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process = {};
	CHECK(CreateProcessW(self, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process));
	std::wstring dump;
	int written = 0;
	channel.ServeUntilExit(process.hProcess, logs.c_str(), [&](bool ok) { written += ok ? 1 : 0; if (ok) dump = channel.written; });
	DWORD exitCode = 0;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	channel.Close();
	CHECK(exitCode == 255);
	CHECK(written == 1);
	ULONGLONG size = 0;
	CHECK(DumpedExceptionCode(dump, size) == 255);
	std::ifstream in(Narrow(logs + L"\\sf4e-crash.log"), std::ios::binary);
	std::stringstream record;
	record << in.rdbuf();
	in.close();
	// The header leads the record; the last lines repeat it once logged.
	CHECK(record.str().find("kind=exit code=0x000000FF") == 0);
	CHECK(record.str().find("ExitProcess(255)") != std::string::npos);
	CHECK(record.str().find("the last line before the corruption") != std::string::npos);
	DeleteFileW(dump.c_str());
	DeleteFileW((logs + L"\\sf4e-crash.log").c_str());
	RemoveDirectoryW(logs.c_str());
}

// Without the launcher's channel the exit still records and ends promptly,
// and writes no dump of its own from inside the exiting process.
void TestNonZeroExitWithoutChannel() {
	const std::wstring logs = TempDirectory();
	wchar_t self[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	wchar_t command[2048] = {};
	swprintf_s(command, L"\"%s\" child exit \"%s\" 0 0 0", self, logs.c_str());
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process = {};
	CHECK(CreateProcessW(self, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
	CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
	DWORD exitCode = 0;
	GetExitCodeProcess(process.hProcess, &exitCode);
	if (exitCode == STILL_ACTIVE) TerminateProcess(process.hProcess, 1);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CHECK(exitCode == 255);
	CHECK(CountDumps(logs) == 0);
	std::ifstream in(Narrow(logs + L"\\sf4e-crash.log"), std::ios::binary);
	std::stringstream record;
	record << in.rdbuf();
	in.close();
	CHECK(record.str().find("kind=exit code=0x000000FF") == 0);
	DeleteFileW((logs + L"\\sf4e-crash.log").c_str());
	RemoveDirectoryW(logs.c_str());
}

DWORD WINAPI ReturnAtOnce(void*) { return 0; }

// Every crash keeps a dump of its own; a failed write leaves nothing behind,
// and a bad request still lets the game go on.
void TestEveryDumpIsKeptAndBadRequestsRefused() {
	const std::wstring logs = TempDirectory();
	wchar_t paths[3][DumpPathSize] = {};
	for (auto& path : paths) {
		const bool ok = WriteDump(GetCurrentProcess(), GetCurrentProcessId(), logs.c_str(), GetCurrentThreadId(), nullptr, false, path);
		if (!ok) std::fprintf(stderr, "WriteDump failed: 0x%08lX\n", GetLastError());
		CHECK(ok);
		CHECK(!IsHeapDumpName(path));
	}
	// Other crashes keep the smaller dump: no heap block nothing points at.
	LeaveUnreferencedMarker();
	{
		wchar_t plain[DumpPathSize] = {};
		CHECK(WriteDump(GetCurrentProcess(), GetCurrentProcessId(), logs.c_str(), GetCurrentThreadId(), nullptr, false, plain));
		CHECK(!FileContainsMarker(plain));
		DeleteFileW(plain);
	}
	CHECK(std::wstring(paths[0]) != paths[1] && std::wstring(paths[1]) != paths[2] && std::wstring(paths[0]) != paths[2]);
	CHECK(CountDumps(logs) == 3);
	// The launcher keeps the newest, which sort last.
	PruneDumps(logs.c_str(), 2);
	CHECK(CountDumps(logs) == 2 && GetFileAttributesW(paths[2]) != INVALID_FILE_ATTRIBUTES);
	// A dump that cannot be written leaves no file.
	wchar_t failed[DumpPathSize] = {};
	CHECK(!WriteDump(GetCurrentProcess(), GetCurrentProcessId(), (logs + L"\\missing-folder").c_str(), 0, nullptr, false, failed));
	// No such process: the dump fails after its file exists.
	CHECK(!WriteDump(nullptr, 0xFFFFFFF0u, logs.c_str(), 0, nullptr, false, failed));
	CHECK(CountDumps(logs) == 2);
	// A write cut off by a dying launcher leaves only a ".partial", which is
	// never counted as a dump, even one newer than every finished dump, and
	// is cleared with the old dumps.
	{
		const std::wstring cut = logs + L"\\sf4e-crash-99991231-235959-1.dmp.partial";
		HANDLE file = CreateFileW(cut.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
		CHECK(file != INVALID_HANDLE_VALUE);
		CloseHandle(file);
		PruneDumps(logs.c_str(), 2);
		CHECK(CountDumps(logs) == 2 && GetFileAttributesW(paths[2]) != INVALID_FILE_ATTRIBUTES);
		CHECK(GetFileAttributesW(cut.c_str()) == INVALID_FILE_ATTRIBUTES);
	}
	CHECK(DumpFiles(logs.c_str(), L"*.partial").empty());

	DumpChannel channel;
	CHECK(channel.Create());
	channel.view->magic = 0;
	CHECK(!channel.Serve(GetCurrentProcess(), logs.c_str()));
	CHECK(WaitForSingleObject(channel.done, 0) == WAIT_OBJECT_0 && channel.view->written == 0 && !channel.written[0]);
	CHECK(CountDumps(logs) == 2);
	channel.Close();
	PruneDumps(logs.c_str(), 0);
	CHECK(CountDumps(logs) == 0);
	RemoveDirectoryW(logs.c_str());

	// Without a channel the launcher only waits for the game to exit.
	DumpChannel none;
	HANDLE game = CreateThread(nullptr, 0, ReturnAtOnce, nullptr, 0, nullptr);
	CHECK(game);
	int served = 0;
	none.ServeUntilExit(game, logs.c_str(), [&](bool) { ++served; });
	CloseHandle(game);
	CHECK(served == 0);
}

// Heap dumps are large, so fewer of them are kept, apart from the others.
void TestHeapDumpsAreKeptApart() {
	const std::wstring logs = TempDirectory();
	const wchar_t* names[] = {
		L"sf4e-crash-20260101-000000-000-1-heap.dmp", L"sf4e-crash-20260102-000000-000-1-heap.dmp",
		L"sf4e-crash-20260103-000000-000-1-heap.dmp", L"sf4e-crash-20260101-000000-000-2.dmp",
		L"sf4e-crash-20260102-000000-000-2.dmp", L"sf4e-crash-20260103-000000-000-2.dmp",
	};
	for (const wchar_t* name : names) {
		HANDLE file = CreateFileW((logs + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
		CHECK(file != INVALID_HANDLE_VALUE);
		CloseHandle(file);
	}
	PruneDumps(logs.c_str(), 5);
	CHECK(CountDumps(logs) == 3 + HeapDumpsKept);
	CHECK(GetFileAttributesW((logs + L"\\" + names[0]).c_str()) == INVALID_FILE_ATTRIBUTES);
	CHECK(GetFileAttributesW((logs + L"\\" + names[2]).c_str()) != INVALID_FILE_ATTRIBUTES);
	CHECK(GetFileAttributesW((logs + L"\\" + names[3]).c_str()) != INVALID_FILE_ATTRIBUTES);
	PruneDumps(logs.c_str(), 0);
	CHECK(CountDumps(logs) == HeapDumpsKept);
	for (const auto& name : DumpFiles(logs.c_str(), L"*")) DeleteFileW((logs + L"\\" + name).c_str());
	RemoveDirectoryW(logs.c_str());
}

void TestClientWithoutChannelDeclines() {
	DumpClient client;
	CHECK(!client.Available());
	CHECK(!client.Configure(nullptr, nullptr, nullptr));
	EXCEPTION_POINTERS pointers = {};
	CHECK(!client.Request(&pointers));
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 7 && std::string(argv[1]) == "child") return RunChild(argv);
	if (argc == 2 && std::string(argv[1]) == "heap-check-settings") {
		CHECK(SetEnvironmentVariableA("SF4E_HEAP_CHECK", nullptr));
		CHECK(!HeapCheckEnabled());
		ConfigureHeapCheck(1);
		CHECK(HeapCheckEnabled());
		HeapCheckpoint("settings-test", 1);
		ConfigureHeapCheck(0);
		CHECK(!HeapCheckEnabled());
		return 0;
	}
	if (argc == 2 && std::string(argv[1]) == "heap-check-environment") {
		CHECK(SetEnvironmentVariableA("SF4E_HEAP_CHECK", "0"));
		ConfigureHeapCheck(1);
		CHECK(!HeapCheckEnabled());
		return 0;
	}
	TestClientWithoutChannelDeclines();
	TestEveryDumpIsKeptAndBadRequestsRefused();
	TestHeapDumpsAreKeptApart();
	TestHandlerRecordsAndLauncherDumps(L"private");
	TestHandlerRecordsAndLauncherDumps(L"process");
	TestNonZeroExitIsRecorded();
	TestNonZeroExitWithoutChannel();
	std::printf("heap_corruption_capture_test: all tests passed\n");
	return 0;
}
