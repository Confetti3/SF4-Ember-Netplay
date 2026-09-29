// Heap corruption (0xC0000374) skips the unhandled-exception filter, so a
// game that died of it left no crash record. This runs the production path
// across two processes: a child installs Sidecar's crash handler
// (sf4e__CrashDiagnostics.cxx) with the launcher's dump channel and corrupts
// a heap, and this process waits on it with the launcher's loop
// (DumpChannel::ServeUntilExit), writing the dump it asks for.

#include "../common/CrashDump.hxx"
#include "../sf4e/sf4e__CrashDiagnostics.hxx"

#include <fstream>
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

	const std::wstring dump = logs + L"\\sf4e-crash.dmp";
	int served = 0, written = 0;
	channel.ServeUntilExit(process.hProcess, dump.c_str(), [&](bool ok) { ++served; written += ok ? 1 : 0; });
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
	CHECK(DumpedExceptionCode(dump, size) == HeapCorruptionCode);
	CHECK(GetFileAttributesW((logs + L"\\sf4e-crash.partial.dmp").c_str()) == INVALID_FILE_ATTRIBUTES);
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

DWORD WINAPI ReturnAtOnce(void*) { return 0; }

// A later dump never costs the earlier one, and a bad request still lets
// the game go on.
void TestDumpsKeepThePreviousAndRefuseBadRequests() {
	const std::wstring logs = TempDirectory();
	const std::wstring dump = logs + L"\\sf4e-crash.dmp", previous = logs + L"\\sf4e-crash.previous.dmp";
	CHECK(WriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump.c_str(), GetCurrentThreadId(), nullptr, false));
	CHECK(WriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump.c_str(), GetCurrentThreadId(), nullptr, false));
	CHECK(GetFileAttributesW(dump.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(previous.c_str()) != INVALID_FILE_ATTRIBUTES);
	CHECK(!WriteDump(GetCurrentProcess(), GetCurrentProcessId(), (logs + L"\\not-a-dump.txt").c_str(), 0, nullptr, false));

	DumpChannel channel;
	CHECK(channel.Create());
	channel.view->magic = 0;
	CHECK(!channel.Serve(GetCurrentProcess(), dump.c_str()));
	CHECK(WaitForSingleObject(channel.done, 0) == WAIT_OBJECT_0 && channel.view->written == 0);
	channel.Close();
	DeleteFileW(dump.c_str());
	DeleteFileW(previous.c_str());
	RemoveDirectoryW(logs.c_str());

	// Without a channel the launcher only waits for the game to exit.
	DumpChannel none;
	HANDLE game = CreateThread(nullptr, 0, ReturnAtOnce, nullptr, 0, nullptr);
	CHECK(game);
	int served = 0;
	none.ServeUntilExit(game, dump.c_str(), [&](bool) { ++served; });
	CloseHandle(game);
	CHECK(served == 0);
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
	TestClientWithoutChannelDeclines();
	TestDumpsKeepThePreviousAndRefuseBadRequests();
	TestHandlerRecordsAndLauncherDumps(L"private");
	TestHandlerRecordsAndLauncherDumps(L"process");
	std::printf("heap_corruption_capture_test: all tests passed\n");
	return 0;
}
