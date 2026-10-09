#pragma once
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sf4e { namespace platform { namespace replays {
// Kernel notifications wake the provenance mailbox. No timeout, filesystem
// polling or slot scanning when a closed recording is waiting for its save.
class RecordingChanges {
public:
 explicit RecordingChanges(std::function<void()> changed) : changed_(std::move(changed)) {
  control_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!control_) throw std::runtime_error("recording notification event unavailable");
  try { thread_ = std::thread([this] { Run(); }); }
  catch (...) { CloseHandle(control_); throw; }
 }
 ~RecordingChanges() { Stop(); CloseHandle(control_); }
 bool Watch(const std::filesystem::path& folder) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stop_) return false;
  if (folder == folder_ && acknowledged_ == revision_) return active_;
  folder_ = folder; const auto revision = ++revision_;
  SetEvent(control_);
  configured_.wait(lock, [&] { return stop_ || acknowledged_ >= revision; });
  return !stop_ && active_;
 }
 void Stop() {
  { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; SetEvent(control_); configured_.notify_all(); }
  if (thread_.joinable()) thread_.join();
 }
private:
 void Run() noexcept {
  HANDLE files = INVALID_HANDLE_VALUE;
  HANDLE account = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HKEY key = nullptr;
  bool accountArmed = account && RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", 0, KEY_NOTIFY, &key) == ERROR_SUCCESS &&
   RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, account, TRUE) == ERROR_SUCCESS;
  for (;;) {
   HANDLE handles[3] = {control_, files, account};
   DWORD count = files == INVALID_HANDLE_VALUE ? 1 : 2;
   if (count == 2 && accountArmed) count = 3;
   const auto result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
   if (result == WAIT_OBJECT_0) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) break;
    if (files != INVALID_HANDLE_VALUE) FindCloseChangeNotification(files);
    files = folder_.empty() ? INVALID_HANDLE_VALUE : FindFirstChangeNotificationW(folder_.c_str(), FALSE,
     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    active_ = files != INVALID_HANDLE_VALUE && accountArmed;
    acknowledged_ = revision_; configured_.notify_all();
   } else if (result == WAIT_OBJECT_0 + 1) {
    // Rearm before waking the reader, so a change during its scan is retained.
    if (!FindNextChangeNotification(files)) {
     FindCloseChangeNotification(files); files = INVALID_HANDLE_VALUE;
     std::lock_guard<std::mutex> lock(mutex_); active_ = false;
    }
    try { changed_(); } catch (...) {}
   } else if (result == WAIT_OBJECT_0 + 2) {
    ResetEvent(account);
    accountArmed = RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, account, TRUE) == ERROR_SUCCESS;
    try { changed_(); } catch (...) {}
   } else {
    std::lock_guard<std::mutex> lock(mutex_); stop_ = true; configured_.notify_all(); break;
   }
  }
  if (files != INVALID_HANDLE_VALUE) FindCloseChangeNotification(files);
  if (key) RegCloseKey(key);
  if (account) CloseHandle(account);
 }
 std::function<void()> changed_;
 std::mutex mutex_;
 std::condition_variable configured_;
 std::filesystem::path folder_;
 std::uint64_t revision_ = 0, acknowledged_ = 0;
 bool stop_ = false, active_ = false;
 HANDLE control_ = nullptr;
 std::thread thread_;
};
} } }
