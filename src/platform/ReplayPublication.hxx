#pragma once
#include "../common/ReplaySlots.hxx"
#include <windows.h>
#include <atomic>
#include <filesystem>

namespace sf4e { namespace platform { namespace replays {
// Durable create-only publication. A destination that appears during the
// write wins; neither unreadable nor confirmed damaged files are replaced.
inline bool PublishFile(const std::filesystem::path& path, const replayslots::Bytes& contents) {
 static std::atomic<unsigned> serial{0};
 const std::filesystem::path partial = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial) + L".tmp";
 HANDLE handle = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
 if (handle == INVALID_HANDLE_VALUE) return false;
 DWORD written = 0;
 const bool ok = contents.size() <= MAXDWORD && WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
  written == contents.size() && FlushFileBuffers(handle);
 const bool closed = CloseHandle(handle) != 0;
 if (ok && closed && MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
 DeleteFileW(partial.c_str());
 return false;
}
} } }
