#pragma once
#include "ReplayFiles.hxx"
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace sf4e { namespace platform { namespace replays {
// The one bounded import preparation/recovery mailbox. The game owner only
// moves immutable transactions under this lock; disk work and destruction
// stay outside.
class ImportWorker {
public:
 using Prepare = std::function<ImportTransaction(const std::filesystem::path&)>;
 using Recover = std::function<void(const ImportTransaction&)>;
 using Complete = std::function<void(const ImportTransaction&)>;
 ImportWorker(Prepare prepare, Recover recover, Complete complete = {}) : prepare_(std::move(prepare)), recover_(std::move(recover)), complete_(std::move(complete)), thread_([this] { Run(); }) {}
 ~ImportWorker() { Stop(); }
 bool Request(const std::filesystem::path& file) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_ || busy_ || failed_) return false;
  request_ = file; busy_ = true; wake_.notify_one(); return true;
 }
 // An empty value with ready=true is a failed preparation (including OOM).
 bool Take(ImportTransaction& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) return false;
  out = std::move(completed_); ready_ = false; busy_ = false; return true;
 }
 void RetainRecovery(ImportTransaction value) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  failed_ = std::move(value); recoveryWanted_ = true; wake_.notify_one();
 }
 void CompleteLater(ImportTransaction value) {
  std::lock_guard<std::mutex> lock(mutex_);
  finished_ = std::move(value); wake_.notify_one();
 }
 bool Failed() const { std::lock_guard<std::mutex> lock(mutex_); return bool(failed_); }
 void Stop() {
  { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; wake_.notify_all(); }
  if (thread_.joinable()) thread_.join();
 }
private:
 void Run() noexcept {
  for (;;) {
   std::optional<std::filesystem::path> file;
   ImportTransaction recovery, finished;
   {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return stop_ || request_ || recoveryWanted_ || finished_; });
    // Retained recovery receives exactly one persistence attempt, even at shutdown.
    if (recoveryWanted_) { recovery = failed_; recoveryWanted_ = false; }
    else if (finished_) finished = std::move(finished_);
    else if (stop_) return;
    else { file = std::move(request_); request_.reset(); }
   }
   if (recovery) { try { recover_(recovery); } catch (...) {} continue; }
   if (finished) { try { if (complete_) complete_(finished); } catch (...) {} continue; }
   ImportTransaction value;
   try { value = prepare_(*file); } catch (...) {}
   { std::lock_guard<std::mutex> lock(mutex_); completed_ = std::move(value); ready_ = true; }
  }
 }
 Prepare prepare_;
 Recover recover_;
 Complete complete_;
 mutable std::mutex mutex_;
 std::condition_variable wake_;
 std::optional<std::filesystem::path> request_;
 ImportTransaction completed_, failed_, finished_;
 bool stop_ = false, busy_ = false, ready_ = false, recoveryWanted_ = false;
 std::thread thread_;
};
} } }
