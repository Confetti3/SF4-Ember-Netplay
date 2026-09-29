#pragma once

// The crash dump is written by the launcher, from outside the game. A heap the
// game has already corrupted can stop MiniDumpWriteDump inside the game, and
// that is exactly the crash (0xC0000374) that used to leave no record. The
// game's handler fills a DumpRequest, signals `request` and waits on `done`;
// the launcher writes the dump with its own, untouched heap. Without a
// launcher (tests, a developer start) the game writes the dump itself.

#include <stdint.h>
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

// Enough heap and module data to follow a corruption back to its source,
// without the whole 400 MB of a running match.
constexpr MINIDUMP_TYPE DumpType = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
	MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

// `path` ending in ".dmp" with ".dmp" replaced by `suffix`, into `out`.
inline bool DumpSibling(const wchar_t* path, const wchar_t* suffix, wchar_t* out, size_t size) {
	const size_t length = wcslen(path);
	if (length < 4 || _wcsicmp(path + length - 4, L".dmp") != 0) return false;
	if (wcsncpy_s(out, size, path, length - 4) != 0) return false;
	return wcscat_s(out, size, suffix) == 0;
}

// Writes `path` (ending in ".dmp") in full or not at all: the dump goes to a
// ".partial.dmp" first and replaces `path` only once written, and the dump it
// replaces is kept as ".previous.dmp", so a failed or later crash never costs
// the one already there. `clientPointers` is true when `pointers` belongs to
// `process` rather than to the caller. Allocates nothing itself.
inline bool WriteDump(HANDLE process, DWORD processId, const wchar_t* path, DWORD threadId,
	EXCEPTION_POINTERS* pointers, bool clientPointers) {
	wchar_t partial[MAX_PATH + 32] = {}, previous[MAX_PATH + 32] = {};
	if (!DumpSibling(path, L".partial.dmp", partial, MAX_PATH + 32) ||
		!DumpSibling(path, L".previous.dmp", previous, MAX_PATH + 32)) return false;
	HANDLE file = CreateFileW(partial, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	MINIDUMP_EXCEPTION_INFORMATION exception = { threadId, pointers, clientPointers ? TRUE : FALSE };
	const BOOL written = MiniDumpWriteDump(process, processId, file, DumpType, pointers ? &exception : nullptr, nullptr, nullptr);
	CloseHandle(file);
	if (!written) { DeleteFileW(partial); return false; }
	if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING);
	return MoveFileExW(partial, path, MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// The launcher's end: two events and the page that carries the request.
struct DumpChannel {
	HANDLE request = nullptr;
	HANDLE done = nullptr;
	HANDLE mailbox = nullptr;
	DumpRequest* view = nullptr;

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
	// Writes the dump the game asked for, then lets the game go on dying.
	bool Serve(HANDLE process, const wchar_t* path) {
		const DumpRequest asked = *view;
		view->magic = 0;
		bool written = false;
		if (asked.magic == DumpRequestMagic)
			written = WriteDump(process, GetProcessId(process), path, asked.threadId,
				reinterpret_cast<EXCEPTION_POINTERS*>(static_cast<uintptr_t>(asked.exceptionPointers)), true);
		view->written = written ? 1 : 0;
		SetEvent(done);
		return written;
	}
	// Waits for `process` to exit, writing each dump it asks for on the way;
	// `served(written)` hears of each request. Without a channel it only waits.
	template <typename Served> void ServeUntilExit(HANDLE process, const wchar_t* path, Served served) {
		for (;;) {
			const HANDLE waits[] = { process, request };
			const DWORD woke = WaitForMultipleObjects(view ? 2 : 1, waits, FALSE, INFINITE);
			if (woke != WAIT_OBJECT_0 + 1) return;
			served(Serve(process, path));
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
