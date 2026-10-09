#pragma once
#include <windows.h>
#include <filesystem>

namespace sf4e { namespace platform { namespace replays {
// One-shot guard: armed before reads, never rearmed for a prepared transaction.
class SaveFolderFreshness {
public:
 ~SaveFolderFreshness() { if (change_ != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change_); }
 SaveFolderFreshness() = default;
 SaveFolderFreshness(const SaveFolderFreshness&) = delete;
 SaveFolderFreshness& operator=(const SaveFolderFreshness&) = delete;
 bool Arm(const std::filesystem::path& folder) {
  if (change_ != INVALID_HANDLE_VALUE) return false;
  change_ = FindFirstChangeNotificationW(folder.c_str(), FALSE,
   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
  return change_ != INVALID_HANDLE_VALUE;
 }
 bool Fresh() const { return change_ != INVALID_HANDLE_VALUE && WaitForSingleObject(change_, 0) == WAIT_TIMEOUT; }
private:
 HANDLE change_ = INVALID_HANDLE_VALUE;
};
} } }
