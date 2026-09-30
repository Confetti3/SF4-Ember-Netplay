#pragma once

// The crash dump is written by the launcher, from outside the game. A heap the
// game has already corrupted can stop MiniDumpWriteDump inside the game, and
// that is exactly the crash (0xC0000374) that used to leave no record. The
// game's handler fills a DumpRequest, signals `request` and waits on `done`;
// the launcher writes the dump with its own, untouched heap. Without a
// launcher (tests, a developer start) the game writes the dump itself.

#include <stdint.h>
#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>
#include <dbghelp.h>

namespace sf4e {
namespace crash {

// STATUS_HEAP_CORRUPTION: ntdll raises it once, so a vectored handler sees it
// even though the unhandled-exception filter never does.
constexpr DWORD HeapCorruptionCode = 0xC0000374u;

inline bool IsHeapCorruption(const EXCEPTION_POINTERS* pointers) {
	return pointers && pointers->ExceptionRecord && pointers->ExceptionRecord->ExceptionCode == HeapCorruptionCode;
}

constexpr uint32_t DumpRequestMagic = 0x504D5544; // "DUMP"
// Long enough for a large dump on a slow disk; the game is dying anyway.
constexpr DWORD DumpRequestWaitMs = 30000;

struct DumpRequest {
	uint32_t magic;
	uint32_t threadId;
	// Set by the launcher before it signals `done`.
	uint32_t written;
	// EXCEPTION_POINTERS* in the game's address space.
	uint64_t exceptionPointers;
};

// Stacks, module data and the memory they point at: enough to follow most
// crashes, a few MB.
constexpr MINIDUMP_TYPE DumpType = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
	MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
// Heap corruption is found long after the write that caused it, at a site
// that says little, so its dump also keeps the process's private memory, the
// heaps included: several hundred MB during a match, and it can hold names,
// chat and invitations. Named "...-heap.dmp", and fewer are kept.
constexpr MINIDUMP_TYPE HeapDumpType = MINIDUMP_TYPE(DumpType | MiniDumpWithPrivateReadWriteMemory);
constexpr size_t HeapDumpsKept = 2;

// The exception code behind `pointers`, read from `process` when they are its
// (the launcher serving the game), or 0 when it cannot be read. Allocates nothing.
inline DWORD ExceptionCodeForDump(HANDLE process, const EXCEPTION_POINTERS* pointers, bool clientPointers) {
	if (!pointers) return 0;
	if (!clientPointers) return pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0;
	EXCEPTION_POINTERS remote = {};
	EXCEPTION_RECORD record = {};
	SIZE_T read = 0;
	if (!ReadProcessMemory(process, pointers, &remote, sizeof(remote), &read) || read != sizeof(remote) || !remote.ExceptionRecord) return 0;
	if (!ReadProcessMemory(process, remote.ExceptionRecord, &record, sizeof(record), &read) || read != sizeof(record)) return 0;
	return record.ExceptionCode;
}

inline bool IsHeapDumpName(const wchar_t* name) {
	const size_t length = wcslen(name);
	return length >= 9 && _wcsicmp(name + length - 9, L"-heap.dmp") == 0;
}

// Each crash gets a dump of its own, "sf4e-crash-<date>-<time>-<ms>-<pid>.dmp" in
// the logs folder ("...-heap.dmp" for heap corruption), so a later or failed
// crash never costs an earlier one.
constexpr size_t DumpPathSize = MAX_PATH + 64;

// Writes the dump into `directory` and names the finished file in `path`.
// The dump is written as "<name>.dmp.partial" and renamed to "<name>.dmp"
// only once complete, so a ".dmp" is always a whole dump: an interrupted or
// failed write leaves at most a ".partial", which PruneDumps clears and never
// counts. No existing file is ever opened or replaced. `clientPointers` is
// true when `pointers` belongs to `process` rather than to the caller.
// Allocates nothing itself.
inline bool WriteDump(HANDLE process, DWORD processId, const wchar_t* directory, DWORD threadId,
	EXCEPTION_POINTERS* pointers, bool clientPointers, wchar_t (&path)[DumpPathSize]) {
	SYSTEMTIME now;
	GetLocalTime(&now);
	const bool heap = ExceptionCodeForDump(process, pointers, clientPointers) == HeapCorruptionCode;
	const wchar_t* suffix = heap ? L"-heap" : L"";
	wchar_t partial[DumpPathSize + 8] = {};
	HANDLE file = INVALID_HANDLE_VALUE;
	// Two dumps of one process in one millisecond take the next free number.
	for (int attempt = 0; attempt < 100 && file == INVALID_HANDLE_VALUE; ++attempt) {
		const int printed = attempt == 0 ?
			swprintf_s(path, L"%s\\sf4e-crash-%04u%02u%02u-%02u%02u%02u-%03u-%lu%s.dmp", directory, now.wYear, now.wMonth,
				now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, processId, suffix) :
			swprintf_s(path, L"%s\\sf4e-crash-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%02d%s.dmp", directory, now.wYear, now.wMonth,
				now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, processId, attempt, suffix);
		if (printed < 0 || swprintf_s(partial, L"%s.partial", path) < 0) return false;
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) continue;
		// Nobody else may open the dump until it is published: a reader could
		// otherwise block the rename and the cleanup.
		file = CreateFileW(partial, GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return false;
	}
	if (file == INVALID_HANDLE_VALUE) return false;
	MINIDUMP_EXCEPTION_INFORMATION exception = { threadId, pointers, clientPointers ? TRUE : FALSE };
	// Memory that changes while it is read (a dump of a running process, and
	// always a dump of oneself) can fail the write with ERROR_PARTIAL_COPY.
	// Try again (a heap dump with the ordinary type), then with the plain dump,
	// rather than end up with none.
	const MINIDUMP_TYPE types[] = { heap ? HeapDumpType : DumpType, DumpType, MiniDumpNormal };
	BOOL written = FALSE;
	for (const MINIDUMP_TYPE type : types) {
		LARGE_INTEGER start = {};
		if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) break;
		written = MiniDumpWriteDump(process, processId, file, type, pointers ? &exception : nullptr, nullptr, nullptr);
		if (written) break;
	}
	// Publish through the handle still held, so no other process can come
	// between the write and the rename. It never replaces a file of that name.
	bool published = false;
	if (written && FlushFileBuffers(file)) {
		alignas(FILE_RENAME_INFO) unsigned char buffer[sizeof(FILE_RENAME_INFO) + DumpPathSize * sizeof(wchar_t)] = {};
		auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer);
		rename->ReplaceIfExists = FALSE;
		rename->FileNameLength = static_cast<DWORD>(wcslen(path) * sizeof(wchar_t));
		memcpy(rename->FileName, path, rename->FileNameLength);
		published = SetFileInformationByHandle(file, FileRenameInfo, rename, sizeof(buffer)) != FALSE;
	}
	if (!published) {
		// Gone once closed, whoever else wanted it.
		FILE_DISPOSITION_INFO remove = { TRUE };
		SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
		path[0] = L'\0';
	}
	CloseHandle(file);
	return published;
}

// The files in `directory` matching `pattern`, sorted by name.
inline std::vector<std::wstring> DumpFiles(const wchar_t* directory, const wchar_t* pattern) {
	std::vector<std::wstring> names;
	WIN32_FIND_DATAW found;
	HANDLE search = FindFirstFileW((std::wstring(directory) + L"\\" + pattern).c_str(), &found);
	if (search == INVALID_HANDLE_VALUE) return names;
	do if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(found.cFileName);
	while (FindNextFileW(search, &found));
	FindClose(search);
	std::sort(names.begin(), names.end());
	return names;
}

// Keeps the newest `keep` ordinary dumps and the newest HeapDumpsKept heap
// dumps in `directory` (the names sort by time) and clears any unfinished
// ".partial" left by an interrupted write. Call it once the game has exited.
// Launcher side only: it allocates.
inline void PruneDumps(const wchar_t* directory, size_t keep) {
	for (const auto& name : DumpFiles(directory, L"sf4e-crash-*.dmp.partial"))
		DeleteFileW((std::wstring(directory) + L"\\" + name).c_str());
	std::vector<std::wstring> ordinary, heap;
	for (const auto& name : DumpFiles(directory, L"sf4e-crash-*.dmp")) {
		// "*.dmp" also matches "*.dmp.partial" under 8.3 names; count only whole dumps.
		if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".dmp") != 0) continue;
		(IsHeapDumpName(name.c_str()) ? heap : ordinary).push_back(name);
	}
	const auto prune = [directory](const std::vector<std::wstring>& names, size_t kept) {
		for (size_t i = 0; i + kept < names.size(); ++i) DeleteFileW((std::wstring(directory) + L"\\" + names[i]).c_str());
	};
	prune(ordinary, keep);
	prune(heap, HeapDumpsKept);
}

// The launcher's end: two events and the page that carries the request.
struct DumpChannel {
	HANDLE request = nullptr;
	HANDLE done = nullptr;
	HANDLE mailbox = nullptr;
	DumpRequest* view = nullptr;
	// The dump the last request wrote.
	wchar_t written[DumpPathSize] = {};

	bool Create(bool inheritable = false) {
		SECURITY_ATTRIBUTES attributes = { sizeof(attributes), nullptr, inheritable ? TRUE : FALSE };
		request = CreateEventW(&attributes, FALSE, FALSE, nullptr);
		done = CreateEventW(&attributes, FALSE, FALSE, nullptr);
		mailbox = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(DumpRequest), nullptr);
		if (mailbox) view = static_cast<DumpRequest*>(MapViewOfFile(mailbox, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(DumpRequest)));
		if (request && done && view) return true;
		Close();
		return false;
	}
	void Close() {
		if (view) UnmapViewOfFile(view);
		HANDLE* handles[] = { &request, &done, &mailbox };
		for (HANDLE* handle : handles) {
			if (*handle) CloseHandle(*handle);
			*handle = nullptr;
		}
		view = nullptr;
	}
	// Writes the dump the game asked for into `directory`, then lets the game
	// go on dying.
	bool Serve(HANDLE process, const wchar_t* directory) {
		const DumpRequest asked = *view;
		view->magic = 0;
		bool ok = false;
		written[0] = L'\0';
		if (asked.magic == DumpRequestMagic)
			ok = WriteDump(process, GetProcessId(process), directory, asked.threadId,
				reinterpret_cast<EXCEPTION_POINTERS*>(static_cast<uintptr_t>(asked.exceptionPointers)), true, written);
		if (!ok) written[0] = L'\0';
		view->written = ok ? 1 : 0;
		SetEvent(done);
		return ok;
	}
	// Waits for `process` to exit, writing each dump it asks for on the way;
	// `served(ok)` hears of each request, with the file in `written`.
	// Without a channel it only waits.
	template <typename Served> void ServeUntilExit(HANDLE process, const wchar_t* directory, Served served) {
		for (;;) {
			const HANDLE waits[] = { process, request };
			const DWORD woke = WaitForMultipleObjects(view ? 2 : 1, waits, FALSE, INFINITE);
			if (woke != WAIT_OBJECT_0 + 1) return;
			served(Serve(process, directory));
		}
	}
};

// The game's end, set from the handles the launcher duplicated into it.
class DumpClient {
public:
	bool Configure(HANDLE request, HANDLE done, HANDLE mailbox) {
		if (!request || !done || !mailbox) return false;
		void* view = MapViewOfFile(mailbox, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(DumpRequest));
		if (!view) return false;
		request_ = request;
		done_ = done;
		view_ = static_cast<DumpRequest*>(view);
		return true;
	}
	bool Available() const { return view_ != nullptr; }
	// True once the launcher has written the dump. Allocates nothing.
	bool Request(EXCEPTION_POINTERS* pointers) const {
		if (!view_ || !pointers) return false;
		view_->written = 0;
		view_->threadId = GetCurrentThreadId();
		view_->exceptionPointers = reinterpret_cast<uintptr_t>(pointers);
		MemoryBarrier();
		view_->magic = DumpRequestMagic;
		return SetEvent(request_) && WaitForSingleObject(done_, DumpRequestWaitMs) == WAIT_OBJECT_0 && view_->written;
	}
private:
	HANDLE request_ = nullptr;
	HANDLE done_ = nullptr;
	DumpRequest* view_ = nullptr;
};

} // namespace crash
} // namespace sf4e
