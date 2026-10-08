#include "VideoServe.hxx"
#include "VideoEncoder.hxx"
#include "VideoShared.hxx"

#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <cstring>

namespace sf4e { namespace platform { namespace videolink {

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

// The folder takes a file of that name: asked first, so a folder that cannot
// be written is not taken for a Windows without an encoder.
bool Writable(const wchar_t* file) {
	const HANDLE made = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (made == INVALID_HANDLE_VALUE) return false;
	CloseHandle(made);
	return DeleteFileW(file) != 0;
}

Result Closing(Shared* shared, Result result) {
	strncpy_s(shared->closedAs, video::Summary().c_str(), _TRUNCATE);
	spdlog::info("Video: {}", result == Written ? "the file has its name" : "no file");
	return result;
}
}

int Serve(const std::wstring& link) {
	const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, link.c_str());
	const HANDLE wake = OpenEventW(SYNCHRONIZE, FALSE, (link + L"-wake").c_str());
	Shared* const shared = mapping ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	if (!shared || !wake) return NoLink;
	const HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, shared->game);
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	LogToFile();
	shared->file[1023] = shared->final[1023] = 0;
	const bool writable = Writable(shared->file);
	const bool opened = writable && video::Begin(shared->file, shared->width, shared->height, shared->game);
	strncpy_s(shared->openedAs, writable ? video::Summary().c_str() : "the file could not be made in its folder", _TRUNCATE);
	InterlockedExchange(&shared->opened, opened ? 1 : -1);
	if (!opened) return writable ? NotOpened : NotWritable;
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
	// Only a file that holds a video, and is wanted, takes the export's name, over an earlier one.
	const bool closed = video::End() && shared->stop != 2;
	if (closed && MoveFileExW(shared->file, shared->final, MOVEFILE_REPLACE_EXISTING)) return Closing(shared, Written);
	DeleteFileW(shared->file);
	return Closing(shared, closed ? NotNamed : NoVideo);
}

} } }
