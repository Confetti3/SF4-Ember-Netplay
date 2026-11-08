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
#include "ProductIdentity.hxx"

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
constexpr ULONGLONG ReportDumpLimit = 4 * 1024 * 1024;
constexpr MINIDUMP_TYPE ReportDumpType = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

inline std::wstring ReportDumpPath(const std::wstring& full) {
	return full.size() >= 4 ? full.substr(0, full.size() - 4) + L"-report.dmp" : std::wstring();
}

inline bool IsReportDumpName(const std::wstring& name) {
	return name.size() >= 11 && _wcsicmp(name.c_str() + name.size() - 11, L"-report.dmp") == 0;
}

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

// Both artifacts use the same staging/publication primitive. The dump type,
// fallback list, byte limit and deadline are explicit policy, not path rules.
using DumpWriter = decltype(&MiniDumpWriteDump);
struct DumpPolicy {
    const MINIDUMP_TYPE* types;
    size_t count;
    ULONGLONG maxBytes = 0;
    ULONGLONG deadline = 0;
    DumpWriter writer = MiniDumpWriteDump;
};
inline BOOL CALLBACK DumpDeadline(void* parameter, MINIDUMP_CALLBACK_INPUT* input, MINIDUMP_CALLBACK_OUTPUT* output) {
    if (input->CallbackType == CancelCallback) {
        const auto deadline = *static_cast<const ULONGLONG*>(parameter);
        output->CheckCancel = TRUE;
        output->Cancel = deadline && GetTickCount64() >= deadline;
    }
    return TRUE;
}
inline HANDLE CreateDumpFile(const wchar_t* path) {
    // Report names add seven characters to a full dump's bounded path.
    wchar_t partial[DumpPathSize + 16] = {};
    if (swprintf_s(partial, L"%s.partial", path) < 0) return INVALID_HANDLE_VALUE;
    return CreateFileW(partial, GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
}
// Reserves the full artifact's staging name before the report is written.
// Leaves a proposed path on a creation failure so report capture can still
// succeed independently (for example, with a full-file-only failure).
inline HANDLE ReserveDump(HANDLE process, DWORD processId, const wchar_t* directory,
    EXCEPTION_POINTERS* pointers, bool clientPointers, wchar_t (&path)[DumpPathSize]) {
	SYSTEMTIME now;
	GetLocalTime(&now);
	const bool heap = ExceptionCodeForDump(process, pointers, clientPointers) == HeapCorruptionCode;
	const wchar_t* suffix = heap ? L"-heap" : L"";
	HANDLE file = INVALID_HANDLE_VALUE;
	// Two dumps of one process in one millisecond take the next free number.
	for (int attempt = 0; attempt < 100 && file == INVALID_HANDLE_VALUE; ++attempt) {
		const int printed = attempt == 0 ?
			swprintf_s(path, L"%s\\%s%04u%02u%02u-%02u%02u%02u-%03u-%lu%s.dmp", directory, ember::Product().crashDumpPrefix, now.wYear, now.wMonth,
				now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, processId, suffix) :
			swprintf_s(path, L"%s\\%s%04u%02u%02u-%02u%02u%02u-%03u-%lu-%02d%s.dmp", directory, ember::Product().crashDumpPrefix, now.wYear, now.wMonth,
				now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, processId, attempt, suffix);
		if (printed < 0) { path[0] = 0; return INVALID_HANDLE_VALUE; }
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) continue;
		// Nobody else may open the dump until it is published: a reader could
		// otherwise block the rename and the cleanup.
		file = CreateDumpFile(path);
		if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return file;
	}
    if (file == INVALID_HANDLE_VALUE) path[0] = 0;
    return file;
}
// Publishes through the still-held handle, never replacing an existing file.
// Failure marks the staging file for deletion before releasing that handle.
inline bool PublishDump(HANDLE file, const wchar_t* path, HANDLE process, DWORD processId,
    DWORD threadId, EXCEPTION_POINTERS* pointers, bool clientPointers, const DumpPolicy& policy) {
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION exception = { threadId, pointers, clientPointers ? TRUE : FALSE };
    MINIDUMP_CALLBACK_INFORMATION callback = { DumpDeadline, const_cast<ULONGLONG*>(&policy.deadline) };
	BOOL written = FALSE;
	for (size_t index = 0; index < policy.count; ++index) {
        if (policy.deadline && GetTickCount64() >= policy.deadline) break;
		LARGE_INTEGER start = {};
		if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) break;
		written = policy.writer(process, processId, file, policy.types[index], pointers ? &exception : nullptr, nullptr,
            policy.deadline ? &callback : nullptr);
		if (written) break;
	}
	// Publish through the handle still held, so no other process can come
	// between the write and the rename. It never replaces a file of that name.
	bool published = false;
	LARGE_INTEGER size = {};
	if (written && GetFileSizeEx(file, &size) && (!policy.maxBytes || static_cast<ULONGLONG>(size.QuadPart) <= policy.maxBytes) && FlushFileBuffers(file)) {
		alignas(FILE_RENAME_INFO) unsigned char buffer[sizeof(FILE_RENAME_INFO) + (DumpPathSize + 8) * sizeof(wchar_t)] = {};
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
	}
	CloseHandle(file);
	return published;
}

// Ordinary/heap dumps retain the canonical fallback sequence. `writer` is
// DbgHelp's own unless a caller that hooks MiniDumpWriteDump passes the
// original, so its own dump does not come back through its hook.
inline bool WriteDump(HANDLE process, DWORD processId, const wchar_t* directory, DWORD threadId,
    EXCEPTION_POINTERS* pointers, bool clientPointers, wchar_t (&path)[DumpPathSize], DumpWriter writer = MiniDumpWriteDump) {
    const bool heap = ExceptionCodeForDump(process, pointers, clientPointers) == HeapCorruptionCode;
    const MINIDUMP_TYPE types[] = { heap ? HeapDumpType : DumpType, DumpType, MiniDumpNormal };
    const HANDLE file = ReserveDump(process, processId, directory, pointers, clientPointers, path);
    const bool saved = PublishDump(file, path, process, processId, threadId, pointers, clientPointers, { types, 3, 0, 0, writer });
    if (!saved) path[0] = 0;
    return saved;
}
inline bool FileAbsent(const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
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
inline void PruneDumps(const wchar_t* directory, size_t keep, const wchar_t* protectedReport = nullptr) {
	for (const auto& name : DumpFiles(directory, (std::wstring(ember::Product().crashDumpPrefix) + L"*.dmp.partial").c_str()))
		DeleteFileW((std::wstring(directory) + L"\\" + name).c_str());
	std::vector<std::wstring> ordinary, heap;
	for (const auto& name : DumpFiles(directory, (std::wstring(ember::Product().crashDumpPrefix) + L"*.dmp").c_str())) {
		// "*.dmp" also matches "*.dmp.partial" under 8.3 names; count only whole dumps.
		if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".dmp") != 0) continue;
		if (IsReportDumpName(name)) {
            // A just-captured small dump remains useful even if its full
            // artifact failed. Keep it until its immutable preview is read.
            if (protectedReport && std::wstring(directory) + L"\\" + name == protectedReport) continue;
			const auto full = name.substr(0, name.size() - 11) + L".dmp";
			if (FileAbsent((std::wstring(directory) + L"\\" + full).c_str()))
				DeleteFileW((std::wstring(directory) + L"\\" + name).c_str());
			continue;
		}
		(IsHeapDumpName(name.c_str()) ? heap : ordinary).push_back(name);
	}
	const auto prune = [directory](const std::vector<std::wstring>& names, size_t kept) {
		for (size_t i = 0; i + kept < names.size(); ++i) {
			const auto full = std::wstring(directory) + L"\\" + names[i];
			if (DeleteFileW(full.c_str()) || FileAbsent(full.c_str())) DeleteFileW(ReportDumpPath(full).c_str());
		}
	};
	prune(ordinary, keep);
	prune(heap, HeapDumpsKept);
}

// A game that stops answering writes no dump of its own. Windows calls its
// window hung after 5 seconds without a message read (WindowNudge.hxx); the
// launcher dumps it FreezeDumpAfterMs later, about 15 seconds stuck, within
// FreezeDumpWaitMs (half a second on a 32 MB dump), so a crash request that
// comes meanwhile still finds the launcher before the game gives up on it.
// The limit is cooperative: ServeUntilExit dumps on its own wait loop, and the
// deadline is checked between attempts and by DbgHelp's cancel callback, so a
// write blocked inside the file system is not cut off, and the FlushFileBuffers
// that publishes the dump (PublishDump) runs outside it. On slow or stalled
// storage the launcher can be busy longer than this, as it can for a crash dump.
constexpr ULONGLONG FreezeDumpAfterMs = 10000;
constexpr DWORD FreezeDumpWaitMs = 5000;

class FreezeWatch {
public:
	enum class Window { Unknown, Answering, Hung };
	enum class Step { None, Dump, Answered };
	// What to do at nowMs (a monotonic clock): Dump once per freeze, and
	// Answered when a window dumped for it reads messages again. A window
	// that cannot be seen leaves the watch as it was.
	Step Next(Window window, ULONGLONG nowMs) {
		if (window == Window::Unknown) return Step::None;
		if (window == Window::Answering) {
			const bool answered = dumped_;
			hung_ = dumped_ = false;
			return answered ? Step::Answered : Step::None;
		}
		if (!hung_) { hung_ = true; since_ = nowMs; }
		if (dumped_ || nowMs - since_ < FreezeDumpAfterMs) return Step::None;
		dumped_ = true;
		return Step::Dump;
	}
private:
	bool hung_ = false;
	bool dumped_ = false;
	ULONGLONG since_ = 0;
};
inline FreezeWatch::Window UnseenWindow() { return FreezeWatch::Window::Unknown; }

// The launcher's end: two events and the page that carries the request.
struct DumpChannel {
	HANDLE request = nullptr;
	HANDLE done = nullptr;
	HANDLE mailbox = nullptr;
	DumpRequest* view = nullptr;
	// The dump the last request wrote.
	wchar_t written[DumpPathSize] = {};
	std::wstring reportWritten;
	bool reportEnabled = false;
	DWORD exceptionCode = 0;
	uint64_t crashAddress = 0;

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
	// Writes the report dump, when enabled, and the full dump, both within
	// `deadline`. `pointers` are in launcher memory, or null for a game that
	// only stopped: then every thread is written as it stands.
	bool Capture(HANDLE process, const wchar_t* directory, DWORD threadId, EXCEPTION_POINTERS* pointers,
		DumpWriter writer, ULONGLONG deadline) {
		reportWritten.clear();
		const DWORD processId = GetProcessId(process);
		HANDLE fullFile = ReserveDump(process, processId, directory, pointers, false, written);
		if (reportEnabled && written[0]) {
			const auto path = ReportDumpPath(written);
			const MINIDUMP_TYPE type = ReportDumpType;
			if (PublishDump(CreateDumpFile(path.c_str()), path.c_str(), process, processId, threadId, pointers, false,
				{ &type, 1, ReportDumpLimit, deadline, writer })) reportWritten = path;
		}
		const MINIDUMP_TYPE types[] = { exceptionCode == HeapCorruptionCode ? HeapDumpType : DumpType, DumpType, MiniDumpNormal };
		const bool ok = PublishDump(fullFile, written, process, processId, threadId, pointers, false, { types, 3, 0, deadline, writer });
		if (!ok) written[0] = L'\0';
		return ok;
	}
	// The dump of a game that stopped answering (FreezeWatch), in place of
	// any earlier one, so the report of the exit that follows carries it.
	// True when either file was written, as for a request.
	bool ServeFreeze(HANDLE process, const wchar_t* directory) {
		exceptionCode = 0; crashAddress = 0;
		return Capture(process, directory, 0, nullptr, MiniDumpWriteDump, GetTickCount64() + FreezeDumpWaitMs) || !reportWritten.empty();
	}
	// A game that answered after its freeze dump was only slow: the dump
	// goes, so it neither reaches a later report nor takes a crash dump's place.
	void ForgetFreeze() {
		if (written[0]) DeleteFileW(written);
		if (!reportWritten.empty()) DeleteFileW(reportWritten.c_str());
		written[0] = L'\0';
		reportWritten.clear();
	}
	// Writes the dump the game asked for into `directory`, then lets the game
	// go on dying.
    // Optional writer/deadline parameters are a bounded fault-injection seam;
    // production uses DbgHelp and the game's existing 30-second wait budget.
    bool Serve(HANDLE process, const wchar_t* directory, DumpWriter writer = MiniDumpWriteDump,
        DWORD budgetMs = DumpRequestWaitMs) {
        const DumpRequest asked = *view;
        view->magic = 0;
        bool ok = false;
        written[0] = L'\0';
        reportWritten.clear(); exceptionCode = 0; crashAddress = 0;
        if (asked.magic == DumpRequestMagic) {
            const auto deadline = GetTickCount64() + budgetMs;
            auto* remotePointers = reinterpret_cast<EXCEPTION_POINTERS*>(static_cast<uintptr_t>(asked.exceptionPointers));
            // Keep exception structures in launcher memory before any slow write.
            // A game whose wait expires can destroy its pointers during a heap
            // dump; DbgHelp must never dereference those addresses afterwards.
            EXCEPTION_POINTERS remote = {}, local = {};
            EXCEPTION_RECORD record = {}; CONTEXT context = {}; SIZE_T read = 0;
            EXCEPTION_POINTERS* pointers = nullptr;
            if (remotePointers && ReadProcessMemory(process, remotePointers, &remote, sizeof(remote), &read) && read == sizeof(remote) &&
                remote.ExceptionRecord && ReadProcessMemory(process, remote.ExceptionRecord, &record, sizeof(record), &read) && read == sizeof(record)) {
                exceptionCode = record.ExceptionCode; crashAddress = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
                record.ExceptionRecord = nullptr;
                if (remote.ContextRecord && ReadProcessMemory(process, remote.ContextRecord, &context, sizeof(context), &read) && read == sizeof(context)) {
                    local = { &record, &context }; pointers = &local;
                }
            }
            ok = Capture(process, directory, asked.threadId, pointers, writer, deadline);
        }
        // Either artifact means the request captured evidence. Track the paths
        // independently; a full failure does not discard a completed report.
        view->written = ok || !reportWritten.empty() ? 1 : 0;
        SetEvent(done);
        return ok;
    }
	// Waits for `process` to exit, writing each dump it asks for on the way;
	// `served(ok, freeze)` hears of each, with the file in `written`. About
	// once a second `window()` tells how the game's window answers, for the
	// freeze dumps (FreezeWatch). Without a channel it only waits.
	template <typename Served, typename WindowState = FreezeWatch::Window (*)()>
	void ServeUntilExit(HANDLE process, const wchar_t* directory, Served served, WindowState window = UnseenWindow) {
		FreezeWatch freeze;
		for (;;) {
			const HANDLE waits[] = { process, request };
			const DWORD woke = WaitForMultipleObjects(view ? 2 : 1, waits, FALSE, view ? 1000 : INFINITE);
			if (woke != WAIT_TIMEOUT && woke != WAIT_OBJECT_0 + 1) return;
			// A game that asks for a dump is answering too.
			const FreezeWatch::Step step = freeze.Next(woke == WAIT_TIMEOUT ? window() : FreezeWatch::Window::Answering, GetTickCount64());
			if (step == FreezeWatch::Step::Answered) ForgetFreeze();
			if (step == FreezeWatch::Step::Dump) served(ServeFreeze(process, directory), true);
			if (woke == WAIT_OBJECT_0 + 1) served(Serve(process, directory), false);
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
	bool Request(EXCEPTION_POINTERS* pointers) const { return Request(pointers, GetCurrentThreadId()); }
	// The same for a fault on another thread: `threadId` is the faulting
	// one, as a crash reporter that runs on its own thread names it.
	bool Request(EXCEPTION_POINTERS* pointers, DWORD threadId) const {
		if (!view_ || !pointers) return false;
		view_->written = 0;
		view_->threadId = threadId;
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
