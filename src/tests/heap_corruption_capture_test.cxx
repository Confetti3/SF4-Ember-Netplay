// Heap corruption (0xC0000374) skips the unhandled-exception filter, so a
// game that died of it left no crash record. This runs the real capture path
// across two processes: a child corrupts a heap and asks for its dump the way
// Sidecar's vectored handler does, and this process serves the request the
// way the launcher does (common/CrashDump.hxx).

#include "../common/CrashDump.hxx"

#include <stdlib.h>
#include <string>

#include "test_support.hxx"

using namespace sf4e::crash;

namespace {

DumpClient s_client;
volatile LONG s_seen = 0;

LONG CALLBACK OnVectoredException(EXCEPTION_POINTERS* pointers) {
	if (IsHeapCorruption(pointers) && InterlockedExchange(&s_seen, 1) == 0) s_client.Request(pointers);
	return EXCEPTION_CONTINUE_SEARCH;
}

HANDLE HandleArgument(const char* text) {
	return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(strtoull(text, nullptr, 16)));
}

// "private" corrupts a heap of its own; "process" corrupts the process heap,
// the CRT heap the game shares, where anything that allocates after the
// corruption may fault again.
int RunChild(char** argv) {
	if (!s_client.Configure(HandleArgument(argv[3]), HandleArgument(argv[4]), HandleArgument(argv[5]))) return 70;
	AddVectoredExceptionHandler(1, OnVectoredException);
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

std::wstring TempDumpPath() {
	wchar_t directory[MAX_PATH] = {}, path[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, directory);
	GetTempFileNameW(directory, L"sf4", 0, path);
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

void TestLauncherWritesTheDumpOfAHeapCorruption(const wchar_t* heap) {
	DumpChannel channel;
	CHECK(channel.Create(true));
	wchar_t self[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	wchar_t command[1024] = {};
	swprintf_s(command, L"\"%s\" child %s %p %p %p", self, heap, channel.request, channel.done, channel.mailbox);
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process = {};
	// No Windows Error Reporting dialog for the child's deliberate crash.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	CHECK(CreateProcessW(self, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process));

	const std::wstring path = TempDumpPath();
	int served = 0;
	for (;;) {
		const HANDLE waits[] = { process.hProcess, channel.request };
		const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, 60000);
		CHECK(woke == WAIT_OBJECT_0 || woke == WAIT_OBJECT_0 + 1);
		if (woke == WAIT_OBJECT_0) break;
		CHECK(channel.Serve(process.hProcess, path.c_str()));
		++served;
	}
	DWORD exitCode = 0;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	channel.Close();

	if (exitCode != HeapCorruptionCode) std::fprintf(stderr, "child exit code 0x%08lX\n", exitCode);
	CHECK(exitCode == HeapCorruptionCode);
	CHECK(served == 1);
	ULONGLONG size = 0;
	CHECK(DumpedExceptionCode(path, size) == HeapCorruptionCode);
	std::printf("heap_corruption_capture_test: %ls heap dump is %llu KB\n", heap, size / 1024);
	// MiniDumpNormal gave about 64 KB, too little to follow a corruption.
	CHECK(size > 64 * 1024);
	DeleteFileW(path.c_str());
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
	if (argc == 6 && std::string(argv[1]) == "child") return RunChild(argv);
	TestClientWithoutChannelDeclines();
	TestLauncherWritesTheDumpOfAHeapCorruption(L"private");
	TestLauncherWritesTheDumpOfAHeapCorruption(L"process");
	std::printf("heap_corruption_capture_test: all tests passed\n");
	return 0;
}
