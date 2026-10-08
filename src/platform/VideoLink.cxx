#include "VideoLink.hxx"
#include "VideoEncoder.hxx"

#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <cstring>

namespace {
using sf4e::platform::videolink::kSlots;

// The start of the shared memory; the slots follow. One side writes each
// counter: the game sent and stop, the encoder taken, opened and the notes.
struct Shared {
	volatile LONG sent, taken, stop;
	// 0 while the encoder opens, then 1, or -1 when it could not.
	volatile LONG opened;
	UINT32 width, height;
	DWORD game;
	LONGLONG times[kSlots];
	// The encoder writes `file` and, once it holds a video, renames it to `final`.
	wchar_t file[1024], final[1024];
	char openedAs[256], closedAs[256];
};

HANDLE s_mapping = nullptr, s_wake = nullptr, s_process = nullptr;
Shared* s_shared = nullptr;
LONG s_dropped = 0;

size_t FrameBytes(const Shared* shared) { return static_cast<size_t>(shared->width) * shared->height * 3 / 2; }
BYTE* Slot(Shared* shared, LONG index) { return reinterpret_cast<BYTE*>(shared + 1) + (index % kSlots) * FrameBytes(shared); }

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
	// Written beside itself as "<name>.part.mp4", so a failed export never
	// costs the file an earlier one made (the container goes by the extension).
	const size_t dot = file.find_last_of(L'.');
	const std::wstring part = (dot == std::wstring::npos ? file : file.substr(0, dot)) + L".part.mp4";
	if (s_shared || part.size() >= 1024) return false;
	const std::wstring name = L"Local\\sf4e-video-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount());
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
	s_shared->times[sent % kSlots] = video::Clock();
	InterlockedIncrement(&s_shared->sent);
	SetEvent(s_wake);
}

void Stop() {
	if (!s_shared) return;
	InterlockedExchange(&s_shared->stop, 1);
	SetEvent(s_wake);
}

bool Closed(bool& ok) {
	ok = false;
	if (!s_shared) return true;
	if (WaitForSingleObject(s_process, 0) == WAIT_TIMEOUT) return false;
	DWORD code = 1;
	GetExitCodeProcess(s_process, &code);
	s_shared->openedAs[255] = s_shared->closedAs[255] = 0;
	spdlog::info("Video: {}", s_shared->openedAs[0] ? s_shared->openedAs : "the encoder's process said nothing");
	spdlog::info("Video: {}; {} of {} pictures not sent with all {} slots full", s_shared->closedAs, s_dropped, s_shared->sent + s_dropped, kSlots);
	Close();
	ok = code == 0;
	return true;
}

void Abort() {
	if (!s_shared) return;
	spdlog::warn("Video: the encoder's process did not finish; ending it");
	// What it leaves is a file no player opens: gone with it.
	s_shared->file[1023] = 0;
	const std::wstring part = s_shared->file;
	TerminateProcess(s_process, 1);
	WaitForSingleObject(s_process, 1000);
	bool ok = false;
	Closed(ok);
	Close();
	if (DeleteFileW(part.c_str())) spdlog::info("Video: removed the unfinished file");
}

bool Finish() {
	if (!s_shared) return false;
	Stop();
	// The writer closing its file; a second or so.
	if (WaitForSingleObject(s_process, 30000) != WAIT_OBJECT_0) { Abort(); return false; }
	bool ok = false;
	Closed(ok);
	return ok;
}

namespace {
// The encoder's process has no log of its own (the launcher's rotates on a
// launcher start, which this is not). One file, written anew each export,
// beside the others: what was opened, a line every ten seconds, how it closed.
void LogToFile() {
	PWSTR appData = nullptr;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData) != S_OK) return;
	const std::wstring folder = std::wstring(appData) + L"\\sf4e\\logs";
	CoTaskMemFree(appData);
	if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) return;
	try {
		auto logger = spdlog::basic_logger_mt("video-encoder", folder + L"\\video-encoder.log", true);
		logger->flush_on(spdlog::level::info);
		spdlog::set_default_logger(logger);
	}
	catch (const std::exception&) {}
}
}

int Serve(const std::wstring& link) {
	const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, link.c_str());
	const HANDLE wake = OpenEventW(SYNCHRONIZE, FALSE, (link + L"-wake").c_str());
	Shared* const shared = mapping ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	if (!shared || !wake) return 2;
	const HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, shared->game);
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	LogToFile();
	shared->file[1023] = 0;
	const bool opened = video::Begin(shared->file, shared->width, shared->height, shared->game);
	strncpy_s(shared->openedAs, video::Summary().c_str(), _TRUNCATE);
	InterlockedExchange(&shared->opened, opened ? 1 : -1);
	if (!opened) return 3;
	const HANDLE either[2] = {wake, game};
	ULONGLONG noted = GetTickCount64();
	for (bool gone = false; !gone;) {
		gone = WaitForMultipleObjects(game ? 2 : 1, either, FALSE, 200) == WAIT_OBJECT_0 + 1 || shared->stop;
		if (GetTickCount64() - noted >= 10000) { noted = GetTickCount64(); spdlog::info("Video: {} pictures taken of {} sent", shared->taken, shared->sent); }
		for (; shared->taken != shared->sent; InterlockedIncrement(&shared->taken)) {
			const BYTE* const slot = Slot(shared, shared->taken);
			video::Frame(slot, shared->width, slot + shared->width * shared->height, shared->width, shared->times[shared->taken % kSlots]);
		}
	}
	bool closed = video::End();
	shared->final[1023] = 0;
	// Only a file that holds a video takes the export's name, over an earlier one.
	if (closed) closed = MoveFileExW(shared->file, shared->final, MOVEFILE_REPLACE_EXISTING) != 0;
	else DeleteFileW(shared->file);
	strncpy_s(shared->closedAs, video::Summary().c_str(), _TRUNCATE);
	spdlog::info("Video: {}", closed ? "the file has its name" : "no file");
	return closed ? 0 : 1;
}

} } }
