#pragma once
#include "../common/ReplayRecordingAttribution.hxx"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace sf4e { namespace replayfiles {
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
