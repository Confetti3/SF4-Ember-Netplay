#pragma once
#include "ReplayProvenance.hxx"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace sf4e { namespace replayfiles {
struct RecordingSnapshot {
 std::filesystem::path saves, archive;
 std::vector<RecordedSlot> slots;
};
struct RecordingBoundary {
 enum class Kind { Start, End } kind = Kind::End;
 std::uint64_t time = 0, sequence = 0;
 bool recordingEnabled = false;
 ReplayNames names;
 int fighters[2] = {-1, -1};
};

// Only the worker calls this state machine. Closed recordings survive a
// rematch and failed publications, but ownership and memory stay bounded.
class RecordingAttribution {
public:
 static constexpr std::size_t kPendingLimit = 4;
 using Log = std::function<void(const char*)>;
 explicit RecordingAttribution(Log log = {}) : log_(std::move(log)) {}
 void Begin(const RecordingBoundary& event, std::shared_ptr<const RecordingSnapshot> before) {
  open_ = {};
  // Native times have only one-second precision. An unobserved old save at
  // or after the next start cannot be distinguished from that rematch.
  for (auto at = pending_.begin(); at != pending_.end();) {
   if (at->body.empty() && event.time <= at->ended) {
    if (event.time <= at->start.time) { LogDrop("ambiguous rematch save boundary"); at = pending_.erase(at); continue; }
    at->ended = event.time - 1;
   }
   ++at;
  }
  if (!event.recordingEnabled || !before || before->saves.empty() || before->archive.empty()) return;
  open_.before = std::move(before); open_.start = event;
  // Fence the new recording too: a delayed old save in the shared boundary
  // second must not receive the next match's names either.
  if (previousEnd_ && *previousEnd_ >= event.time) {
   if (*previousEnd_ == (std::numeric_limits<std::uint64_t>::max)()) { open_ = {}; return; }
   open_.start.time = *previousEnd_ + 1;
  }
 }
 void End(std::uint64_t time) {
  previousEnd_ = time;
  if (!open_.before) return;
  open_.ended = time;
  if (pending_.size() == kPendingLimit) { LogDrop("waiting save evicted at recording limit"); pending_.pop_front(); }
  pending_.push_back(std::move(open_)); open_ = {};
 }
 void Reset() { open_ = {}; pending_.clear(); previousEnd_.reset(); }
 bool Pending() const { return !pending_.empty(); }
 bool NeedsSnapshot() const {
  for (const auto& job : pending_) if (job.body.empty()) return true;
  return false;
 }
 void ObserveSave(const RecordingSnapshot& after) {
  for (auto at = pending_.begin(); at != pending_.end();) {
   if (at->before->saves != after.saves || at->before->archive != after.archive) {
    LogDrop("recording account/archive changed"); at = pending_.erase(at); continue;
   }
   if (at->body.empty()) {
    const auto* body = RecordedBody(at->before->slots, after.slots, at->start.time, at->ended, at->start.fighters);
    if (body) at->body = *body;
   }
   ++at;
  }
 }
 template<class Publish> void PublishIdentified(const Publish& publish) {
  for (auto at = pending_.begin(); at != pending_.end();) {
   if (at->body.empty()) { ++at; continue; }
   NamePublication result = NamePublication::Retryable;
   ++at->attempts;
   try { result = publish(at->before->archive, at->body, at->start.names); } catch (...) {}
   if (result == NamePublication::Published) at = pending_.erase(at);
   else if (result == NamePublication::Refused || at->attempts >= 3) {
    LogDrop(result == NamePublication::Refused ? "names target refused; manual repair required" : "names publication retry budget exhausted");
    at = pending_.erase(at);
   } else ++at;
  }
 }
 void GiveUp(const char* reason = "unresolved recording names discarded at shutdown") { if (Pending()) LogDrop(reason); Reset(); }

private:
 struct PendingRecording {
  RecordingBoundary start;
  std::uint64_t ended = 0;
  std::shared_ptr<const RecordingSnapshot> before;
  replayslots::Bytes body;
  unsigned attempts = 0;
 };
 void LogDrop(const char* reason) { if (log_) log_(reason); }
 Log log_;
 PendingRecording open_;
 std::deque<PendingRecording> pending_;
 std::optional<std::uint64_t> previousEnd_;
};

// Hooks enqueue only bounded facts. Neither snapshotting nor publishing
// holds the mailbox mutex. If a boundary arrives before/during a baseline
// read, that baseline is discarded: it cannot prove a before observation.
class RecordingWorker {
public:
 using Read = std::function<RecordingSnapshot()>;
 using Publish = std::function<NamePublication(const std::filesystem::path&, const replayslots::Bytes&, const ReplayNames&)>;
 // Optional observer synchronizes lifecycle fixtures after a fact has been
 // consumed, so tests need no sleeps or filesystem timing assumptions.
 using Observe = std::function<void(const RecordingBoundary&)>;
 RecordingWorker(Read read, Publish publish, Observe observe = {}, RecordingAttribution::Log log = {}) : read_(std::move(read)), publish_(std::move(publish)), observe_(std::move(observe)), log_(std::move(log)), thread_([this] { Run(); }) {}
 ~RecordingWorker() { Stop(); }
 RecordingWorker(const RecordingWorker&) = delete;
 RecordingWorker& operator=(const RecordingWorker&) = delete;
 void Post(RecordingBoundary event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_) return;
  event.sequence = ++sequence_;
  if (events_.size() >= 64) {
   events_.clear(); reset_ = true;
  }
  try { events_.push_back(std::move(event)); }
  catch (...) { events_.clear(); reset_ = true; }
  wake_.notify_one();
 }
 void FilesChanged() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stop_) { changed_ = true; wake_.notify_one(); }
 }
 void Invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++sequence_; events_.clear(); reset_ = true; wake_.notify_one();
 }
 void Stop() {
  { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; wake_.notify_all(); }
  if (thread_.joinable()) thread_.join();
 }
private:
 void Run() noexcept {
  try {
  RecordingAttribution owner(log_);
  std::uint64_t consumed = 0;
  for (;;) {
   std::deque<RecordingBoundary> events;
   bool reset = false, observedSnapshot = false;
   {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return stop_ || reset_ || changed_ || !events_.empty(); });
    if (stop_) { lock.unlock(); owner.GiveUp(); return; }
    changed_ = false;
    events.swap(events_);
    reset = reset_; reset_ = false;
   }
   if (reset) owner.GiveUp("recording ownership invalidated");
   for (const auto& event : events) {
    try {
     if (event.kind == RecordingBoundary::Kind::End) owner.End(event.time);
     else {
      std::shared_ptr<const RecordingSnapshot> before;
      if (event.recordingEnabled && sequence_.load() == event.sequence) {
       before = std::make_shared<const RecordingSnapshot>(read_());
       if (sequence_.load() != event.sequence) before.reset();
      }
      owner.Begin(event, before);
      // Apply the rematch timestamp fence before observing the old save.
      if (before) { owner.ObserveSave(*before); observedSnapshot = true; }
     }
    } catch (...) { owner.Begin(event, {}); }
    consumed = event.sequence;
    try { if (observe_) observe_(event); } catch (...) {}
   }
   if (owner.NeedsSnapshot() && !observedSnapshot) {
    try {
     if (sequence_.load() == consumed) {
      const auto after = read_();
      if (sequence_.load() == consumed) owner.ObserveSave(after);
     }
    }
    catch (...) {} // dormant until a filesystem/lifecycle change
   }
   owner.PublishIdentified(publish_);
  }
  } catch (...) {
   // Allocation failure constructing the owner/mailbox must not terminate
   // the game. Future hooks refuse this stopped owner without touching disk.
   { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; events_.clear(); }
   try { if (log_) log_("recording worker failed; attribution stopped"); } catch (...) {}
  }
 }
 Read read_;
 Publish publish_;
 Observe observe_;
 RecordingAttribution::Log log_;
 std::mutex mutex_;
 std::condition_variable wake_;
 std::deque<RecordingBoundary> events_;
 std::atomic<std::uint64_t> sequence_{0};
 bool stop_ = false, reset_ = false, changed_ = false;
 std::thread thread_;
};
} }
