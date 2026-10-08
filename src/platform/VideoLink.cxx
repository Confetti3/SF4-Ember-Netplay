#include "VideoLink.hxx"

#include <windows.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace {
using namespace sf4e::platform::videolink;

HANDLE s_mapping = nullptr, s_wake = nullptr, s_process = nullptr;
Shared* s_shared = nullptr;
LONG s_dropped = 0;

void Close() {
	if (s_shared) UnmapViewOfFile(s_shared);
	for (HANDLE* handle : {&s_mapping, &s_wake, &s_process}) { if (*handle) CloseHandle(*handle); *handle = nullptr; }
	s_shared = nullptr;
}

void CopyRows(BYTE* to, const void* from, int pitch, unsigned width, unsigned rows) {
	for (unsigned row = 0; row < rows; row++) memcpy(to + row * width, static_cast<const BYTE*>(from) + row * pitch, width);
}
}

namespace sf4e { namespace platform { namespace videolink {

bool Start(const std::wstring& file, unsigned width, unsigned height, const std::wstring& encoder) {
	const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
	const std::wstring part = file + L"." + unique + L".tmp";
	if (s_shared || part.size() >= 1024) return false;
	const std::wstring name = L"Local\\sf4e-video-" + unique;
	const ULONGLONG size = sizeof(Shared) + static_cast<ULONGLONG>(width) * height * 3 / 2 * kSlots;
	s_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), name.c_str());
	s_wake = CreateEventW(nullptr, FALSE, FALSE, (name + L"-wake").c_str());
	s_shared = s_mapping ? static_cast<Shared*>(MapViewOfFile(s_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	if (!s_shared || !s_wake) { spdlog::warn("Video: no shared memory for {}x{} ({})", width, height, GetLastError()); Close(); return false; }
	s_shared->width = width; s_shared->height = height; s_shared->game = GetCurrentProcessId();
	wcscpy_s(s_shared->file, part.c_str()); wcscpy_s(s_shared->final, file.c_str());
	s_dropped = 0;

	std::wstring exe = encoder;
	if (exe.empty()) {
		// Beside the module this code is in: Sidecar.dll, in Ember's folder.
		HMODULE self = nullptr; wchar_t path[MAX_PATH] = {0};
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Start), &self);
		GetModuleFileNameW(self, path, MAX_PATH);
		exe = path; exe.resize(exe.find_last_of(L'\\') + 1); exe += L"Launcher.exe";
	}
	std::wstring command = L"\"" + exe + L"\" --encode-video " + name;
	STARTUPINFOW startup = {sizeof startup}; PROCESS_INFORMATION process = {};
	if (!CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
		spdlog::warn("Video: the encoder's process did not start ({})", GetLastError()); Close(); return false;
	}
	CloseHandle(process.hThread);
	s_process = process.hProcess;
	return true;
}

void Send(const void* luma, int lumaPitch, const void* chroma, int chromaPitch) {
	if (!s_shared) return;
	if (s_shared->opened != 1) return;
	const LONG sent = s_shared->sent;
	if (sent - s_shared->taken >= static_cast<LONG>(kSlots)) { s_dropped++; return; }
	BYTE* const slot = Slot(s_shared, sent);
	CopyRows(slot, luma, lumaPitch, s_shared->width, s_shared->height);
	CopyRows(slot + s_shared->width * s_shared->height, chroma, chromaPitch, s_shared->width, s_shared->height / 2);
	s_shared->times[sent % kSlots] = Clock();
	InterlockedIncrement(&s_shared->sent);
	SetEvent(s_wake);
}

bool Lost() { return s_shared && !s_shared->stop && WaitForSingleObject(s_process, 0) != WAIT_TIMEOUT; }

void Stop(bool keep) {
	if (!s_shared) return;
	InterlockedExchange(&s_shared->stop, keep ? 1 : 2);
	SetEvent(s_wake);
}

bool Closed(Result& result) {
	result = NoLink;
	if (!s_shared) return true;
	if (WaitForSingleObject(s_process, 0) == WAIT_TIMEOUT) return false;
	DWORD code = NoLink;
	GetExitCodeProcess(s_process, &code);
	result = static_cast<Result>(code);
	s_shared->openedAs[255] = s_shared->closedAs[255] = 0; s_shared->file[1023] = 0;
	spdlog::info("Video: {}", s_shared->openedAs[0] ? s_shared->openedAs : "the encoder's process said nothing");
	spdlog::info("Video: {} (exit {}); {} of {} pictures not sent with all {} slots full", s_shared->closedAs, code, s_dropped, s_shared->sent + s_dropped, kSlots);
	// The process removes its own file when it gives up; one that was ended could not.
	if (result != Written && DeleteFileW(s_shared->file)) spdlog::info("Video: removed the unfinished file");
	Close();
	return true;
}

Abandoned Abandon() {
	Abandoned left;
	if (!s_shared) return left;
	s_shared->file[1023] = 0;
	left.file = s_shared->file;
	left.process = s_process; s_process = nullptr;
	Close();
	return left;
}

void End(Abandoned& left) {
	if (!left.process) return;
	spdlog::warn("Video: the encoder's process did not finish; ending it");
	TerminateProcess(left.process, NoVideo);
	WaitForSingleObject(left.process, 1000);
	CloseHandle(left.process); left.process = nullptr;
	// What it leaves is a file no player opens: gone with it.
	if (DeleteFileW(left.file.c_str())) spdlog::info("Video: removed the unfinished file");
}

} } }
