#include "VideoLinkServe.hxx"
#include "VideoLinkProtocol.hxx"
#include "VideoEncoder.hxx"
#include "../netplay/SettingsStore.hxx"

#include <windows.h>
#include <objbase.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <cstring>

namespace {
// The encoder's process has no log of its own (the launcher's rotates on a
// launcher start, which this is not). One file, written anew each export,
// beside the others: what was opened, a line every ten seconds, how it closed.
void LogToFile() {
	const std::wstring root = sf4e::netplay::SettingsStore::DefaultDirectory();
	if (root.empty()) return;
	const std::wstring folder = root + L"\\logs";
	if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) return;
	try {
		auto logger = spdlog::basic_logger_mt("video-encoder", folder + L"\\video-encoder.log", true);
		logger->flush_on(spdlog::level::info);
		spdlog::set_default_logger(logger);
	}
	catch (const std::exception&) {}
}

// The encoder's file until it takes the export's name. Only a file that
// holds a video does, over an earlier one; any other goes when this does, on
// every way out of Serve, since the game that would remove it may be gone.
// Made before the encoder, so it goes after the encoder has let go of the file.
class Temporary {
public:
	explicit Temporary(const wchar_t* path) : path_(path) {}
	~Temporary() {
		if (published_ || DeleteFileW(path_.c_str())) return;
		const DWORD error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) spdlog::warn("Video: the unfinished file could not be removed ({})", error);
	}
	Temporary(const Temporary&) = delete;
	Temporary& operator=(const Temporary&) = delete;

	// False when the name could not be taken, such as while a player has the
	// earlier export open; that one is then left as it was.
	bool Publish(const wchar_t* final) {
		published_ = MoveFileExW(path_.c_str(), final, MOVEFILE_REPLACE_EXISTING) != 0;
		if (!published_) spdlog::warn("Video: the file could not take the export's name ({})", GetLastError());
		return published_;
	}

private:
	std::wstring path_;
	bool published_ = false;
};
}

namespace sf4e { namespace platform { namespace videolink {

int Serve(const std::wstring& link) {
	const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, link.c_str());
	Shared* const shared = mapping ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	if (!shared) return 2;
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	LogToFile();
	// From here the temporary file is this process's to remove, whatever else
	// is missing: the game that reserved it may already be gone.
	shared->file[1023] = 0; shared->final[1023] = 0;
	Temporary file(shared->file);
	// The game sets wake to stop the export, and its end stops it too; without
	// either, nothing would.
	const HANDLE wake = OpenEventW(SYNCHRONIZE, FALSE, WakeName(link).c_str());
	if (!wake) { spdlog::warn("Video: the game's link has no wake event ({})", GetLastError()); return 2; }
	const HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, shared->game);
	if (!game) { spdlog::warn("Video: the game is gone ({})", GetLastError()); return 2; }
	video::Encoder encoder;
	const bool opened = encoder.Begin(shared->file, shared->width, shared->height, shared->game);
	strncpy_s(shared->openedAs, encoder.Summary().c_str(), _TRUNCATE);
	InterlockedExchange(&shared->opened, opened ? 1 : -1);
	if (!opened) return 3;
	const HANDLE either[2] = {wake, game};
	ULONGLONG noted = GetTickCount64();
	for (bool gone = false; !gone;) {
		gone = WaitForMultipleObjects(2, either, FALSE, 200) == WAIT_OBJECT_0 + 1 || shared->stop;
		if (GetTickCount64() - noted >= 10000) { noted = GetTickCount64(); spdlog::info("Video: {} pictures taken of {} sent", shared->taken, shared->sent); }
		for (; shared->taken != shared->sent; InterlockedIncrement(&shared->taken)) {
			const BYTE* const slot = Slot(shared, shared->taken);
			encoder.Frame(slot, shared->width, slot + shared->width * shared->height, shared->width, shared->times[shared->taken % kSlots]);
		}
	}
	bool closed = encoder.End();
	if (InterlockedCompareExchange(&shared->stop, 0, 0) < 0) closed = false;
	strncpy_s(shared->closedAs, encoder.Summary().c_str(), _TRUNCATE);
	closed = closed && file.Publish(shared->final);
	spdlog::info("Video: {}", closed ? "the file has its name" : "no file");
	return closed ? 0 : 1;
}

} } }
