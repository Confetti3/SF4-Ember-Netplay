#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <thread>

#include "ReplayArchivePass.hxx"

namespace sf4e { namespace replayfiles {
// Runs archive passes (ReplayArchivePass.hxx) on a thread of its own and
// decides when the next one runs, so a match's replay reaches the archive as
// the game saves it. Three schedules run beside asked passes:
//   after a match   MatchEnded opens a window of `window` from then: the game
//                   saves the replay after the battle closes, which a
//                   watcher may not report. Passes run on the schedule
//                   until the window closes, the last one as it closes; a
//                   later MatchEnded opens a new window.
//   retries         a pass that asks for a retry (a slot that a later pass may
//                   find whole) starts `attempts` more passes on the schedule,
//                   unless they are running already; when they run out, the
//                   next asked pass starts them again.
//   outbox          while the outbox holds entries still to be settled, a
//                   pass every `steady`, for as long as that lasts, or after
//                   `first` when a pass left some for the next one.
// The first two space their passes from `first`, doubling up to `most`.
// Want asks for a pass now (the save folder changed, a match started); it
// never moves a schedule. Asks made during a pass are kept for one more.
// Passes never overlap, and Stop waits for one in progress.
class ArchiveWorker {
public:
 using Clock = std::chrono::steady_clock;
 using Pass = std::function<ArchivePass()>;
 struct Schedule { Clock::duration first, most; int attempts; Clock::duration window, steady; };
 ArchiveWorker(Pass pass, Schedule schedule) : pass_(std::move(pass)), schedule_(schedule), thread_([this] { Run(); }) {}
 ~ArchiveWorker() { Stop(); }
 ArchiveWorker(const ArchiveWorker&) = delete;
 ArchiveWorker& operator=(const ArchiveWorker&) = delete;
 void Want() { Ask(false); }
 void MatchEnded() { Ask(true); }
 void Stop() {
  { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; wake_.notify_all(); }
  if (thread_.joinable()) thread_.join();
 }
private:
 void Ask(bool matchEnded) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_) return;
  wanted_ = true; matchEnded_ = matchEnded_ || matchEnded;
  wake_.notify_one();
 }
 Clock::duration Delay(int& ran) const { return (std::min)(schedule_.first * (1 << (std::min)(ran++, 16)), schedule_.most); }
 void Run() noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  // The match window's end and next pass and the next retry, each with how
  // many of its passes have run, and the next pass for the outbox.
  std::optional<Clock::time_point> windowEnd, poll, retry, outbox;
  int polled = 0, retried = 0;
  for (;;) {
   const auto woken = [&] { return stop_ || wanted_; };
   std::optional<Clock::time_point> due;
   for (const auto& next : {poll, retry, outbox}) if (next && (!due || *next < *due)) due = next;
   if (due) wake_.wait_until(lock, *due, woken);
   else wake_.wait(lock, woken);
   if (stop_) return;
   const auto now = Clock::now();
   const bool asked = wanted_, ended = matchEnded_;
   const bool pollDue = poll && now >= *poll, retryDue = retry && now >= *retry;
   wanted_ = matchEnded_ = false;
   lock.unlock();
   ArchivePass pass;
   try { pass = pass_(); } catch (...) { pass.retry = true; }
   const auto after = Clock::now();
   if (ended) { windowEnd = now + schedule_.window; polled = 0; }
   if (ended || pollDue) {
    if (windowEnd && now < *windowEnd) poll = (std::min)(after + Delay(polled), *windowEnd);
    else { windowEnd.reset(); poll.reset(); }
   }
   if (!pass.retry) { retry.reset(); retried = 0; }
   else if (retryDue) { if (retried < schedule_.attempts) retry = after + Delay(retried); else { retry.reset(); retried = 0; } }
   else if (asked && !retry) { retried = 0; retry = after + Delay(retried); }
   outbox.reset();
   if (pass.waiting) outbox = after + (pass.more ? schedule_.first : schedule_.steady);
   lock.lock();
  }
 }
 Pass pass_;
 const Schedule schedule_;
 std::mutex mutex_;
 std::condition_variable wake_;
 bool wanted_ = false, matchEnded_ = false, stop_ = false;
 std::thread thread_;
};
} }
