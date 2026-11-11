#include "VideoLink.hxx"
#include "VideoLinkProtocol.hxx"
#include "VideoTemporary.hxx"
#include "../common/install_paths.hxx"

#include <windows.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
using sf4e::platform::videolink::kSlots;
using sf4e::platform::videolink::Shared;
using sf4e::platform::videolink::Slot;

HANDLE s_mapping = nullptr, s_wake = nullptr, s_process = nullptr;
Shared* s_shared = nullptr;
LONG s_dropped = 0;
std::wstring s_partial;
// When the game first asked the encoder to stop (Stop or Fail); 0 before.
ULONGLONG s_stoppedAt = 0;
// The encoder's process may take this long to close its file before it is ended.
constexpr ULONGLONG kClosePatienceMs = 30000;
// The encoder's process was ended at that deadline: the export failed,
// whatever the process exits with.
bool s_ended = false;
// Temporary files that could not be removed yet, tried again on the next Start.
std::vector<std::wstring> s_leftovers;
// Fast: the sound for the next picture (SetSound).
std::vector<float> s_pendingSound;

// The one place a temporary file goes, once no process writes it: the encoder
// gives a file that holds a video its name first, so what is left is a
// failed export's. One that cannot go yet is kept for the next try. True
// when it was removed here.
bool Remove(const std::wstring& path, bool again = false) {
	if (path.empty()) return false;
	if (DeleteFileW(path.c_str())) return true;
	const DWORD error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
	if (!again) spdlog::warn("Video: the unfinished file could not be removed yet ({})", error);
	s_leftovers.push_back(path);
	return false;
}

// Lets go of the link once its process has ended, or never started. True
// when the temporary file was removed.
bool Close() {
	const bool removed = Remove(s_partial);
	s_partial.clear();
	if (s_shared) UnmapViewOfFile(s_shared);
	for (HANDLE* handle : {&s_mapping, &s_wake, &s_process}) { if (*handle) CloseHandle(*handle); *handle = nullptr; }
	s_shared = nullptr;
	s_stoppedAt = 0;
	s_ended = false;
	return removed;
}

void CopyRows(BYTE* to, const void* from, int pitch, unsigned width, unsigned rows) {
	for (unsigned row = 0; row < rows; row++) memcpy(to + row * width, static_cast<const BYTE*>(from) + row * pitch, width);
}
}

namespace sf4e { namespace platform { namespace videolink {

bool Start(const std::wstring& file, unsigned width, unsigned height, const std::wstring& encoder, bool fast) {
	if (s_shared || file.size() >= 1024) return false;
	std::vector<std::wstring> leftovers;
	leftovers.swap(s_leftovers);
	for (const std::wstring& path : leftovers) Remove(path, true);
	std::wstring exe = encoder;
	if (exe.empty()) {
		// Launcher.exe in Ember's install, wherever the package layout puts it.
		wchar_t path[MAX_PATH] = {0};
		if (!install::ResolveInstallFile(L"Launcher.exe", path, MAX_PATH)) { spdlog::warn("Video: no Launcher.exe in the install for the encoder"); return false; }
		exe = path;
	}
	const std::wstring name = L"Local\\sf4e-video-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount());
	if (!ReserveTemporary(file, s_partial)) { s_partial.clear(); return false; }
	const ULONGLONG size = sizeof(Shared) + static_cast<ULONGLONG>(width) * height * 3 / 2 * kSlots;
	s_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), name.c_str());
	s_wake = CreateEventW(nullptr, FALSE, FALSE, WakeName(name).c_str());
	s_shared = s_mapping ? static_cast<Shared*>(MapViewOfFile(s_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	if (!s_shared || !s_wake) { spdlog::warn("Video: no shared memory for {}x{} ({})", width, height, GetLastError()); Close(); return false; }
	s_shared->width = width; s_shared->height = height; s_shared->game = GetCurrentProcessId(); s_shared->fast = fast ? 1 : 0;
	s_pendingSound.clear();
	wcscpy_s(s_shared->file, s_partial.c_str()); wcscpy_s(s_shared->final, file.c_str());
	s_dropped = 0;

	std::wstring command = L"\"" + exe + L"\" --encode-video " + name;
	STARTUPINFOW startup = {sizeof startup}; PROCESS_INFORMATION process = {};
	if (!CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
		spdlog::warn("Video: the encoder's process did not start ({})", GetLastError()); Close(); return false;
	}
	CloseHandle(process.hThread);
	s_process = process.hProcess;
	// Fast: the encoder opens before the game goes on, at most 15 seconds, so
	// the first picture and its sound start together (sf4e__ExportSound.hxx).
	for (int waited = 0; fast && s_shared->opened == 0 && waited < 1500; waited++)
		if (WaitForSingleObject(s_process, 10) != WAIT_TIMEOUT) break;
	return true;
}

void SetSound(const float* stereo, unsigned frames) {
	s_pendingSound.assign(stereo, stereo + static_cast<size_t>((std::min)(frames, kSoundPerSlot)) * 2);
}

void Send(const void* luma, int lumaPitch, const void* chroma, int chromaPitch) {
	if (!s_shared) return;
	// Fast: the game waits for the encoder to open and for a free slot, while
	// its process lives and the export has not been stopped.
	for (unsigned turn = 0; s_shared->fast && !s_shared->stop && s_shared->opened >= 0 &&
		(s_shared->opened != 1 || s_shared->sent - s_shared->taken >= static_cast<LONG>(kSlots)); turn++) {
		if (WaitForSingleObject(s_process, 0) != WAIT_TIMEOUT) break;
		Sleep(turn < 64 ? 0 : 1);
	}
	if (s_shared->opened != 1) return;
	const LONG sent = s_shared->sent;
	if (sent - s_shared->taken >= static_cast<LONG>(kSlots)) { s_dropped++; return; }
	BYTE* const slot = Slot(s_shared, sent);
	if (s_shared->fast) {
		const UINT32 frames = static_cast<UINT32>(s_pendingSound.size() / 2);
		memcpy(s_shared->sound[sent % kSlots], s_pendingSound.data(), s_pendingSound.size() * sizeof(float));
		s_shared->soundFrames[sent % kSlots] = frames;
		s_pendingSound.clear();
	}
	CopyRows(slot, luma, lumaPitch, s_shared->width, s_shared->height);
	CopyRows(slot + s_shared->width * s_shared->height, chroma, chromaPitch, s_shared->width, s_shared->height / 2);
	// Fast: stamped by its number, from the first (VideoEncoder.hxx: Begin).
	s_shared->times[sent % kSlots] = s_shared->fast ? static_cast<LONGLONG>(sent) * 10000000 / 60 : video::Clock();
	InterlockedIncrement(&s_shared->sent);
	SetEvent(s_wake);
}

void Stop() {
	if (!s_shared) return;
	InterlockedCompareExchange(&s_shared->stop, 1, 0);
	if (!s_stoppedAt) s_stoppedAt = GetTickCount64();
	SetEvent(s_wake);
}

void Fail() {
	if (!s_shared) return;
	InterlockedExchange(&s_shared->stop, -1);
	if (!s_stoppedAt) s_stoppedAt = GetTickCount64();
	SetEvent(s_wake);
}

Result Poll() {
	if (!s_shared) return Result::Failed;
	const DWORD waited = WaitForSingleObject(s_process, 0);
	if (waited == WAIT_TIMEOUT) {
		if (!s_stoppedAt || GetTickCount64() - s_stoppedAt <= kClosePatienceMs) return Result::Pending;
		// Asked again on each Poll until it has ended, without waiting for it
		// here: what it leaves is a file no player opens, removed once it is gone.
		if (!TerminateProcess(s_process, 1) && !s_ended) spdlog::warn("Video: the encoder's process did not finish and could not be ended ({})", GetLastError());
		else if (!s_ended) spdlog::warn("Video: the encoder's process did not finish; ending it");
		s_ended = true;
		return Result::Pending;
	}
	if (waited != WAIT_OBJECT_0) {
		// Its end can never be seen: the export failed, and its file is left
		// for the next Start, when nothing may hold it any more.
		spdlog::warn("Video: the encoder's process cannot be waited for ({})", GetLastError());
		s_leftovers.push_back(s_partial);
		s_partial.clear();
		Close();
		return Result::Failed;
	}
	DWORD code = 1;
	if (!GetExitCodeProcess(s_process, &code)) code = 1;
	s_shared->openedAs[255] = s_shared->closedAs[255] = 0;
	spdlog::info("Video: {}", s_shared->openedAs[0] ? s_shared->openedAs : "the encoder's process said nothing");
	spdlog::info("Video: {}; {} of {} pictures not sent with all {} slots full", s_shared->closedAs, s_dropped, s_shared->sent + s_dropped, kSlots);
	const bool done = code == 0 && !s_ended, ended = s_ended;
	if (Close() && ended) spdlog::info("Video: removed the unfinished file");
	return done ? Result::Done : Result::Failed;
}

} } }
