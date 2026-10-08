#pragma once

#include "ReplayInputs.hxx"
#include "ReplayFileSafety.hxx"
#include <memory>
#include <utility>

namespace sf4e { namespace replayinputs {

struct Detail {
 std::string file;
 Match match;
 Summary summary;
 std::vector<std::string> logs;
};
enum class DetailState { Pending, Ready, Unreadable, Failed };
struct DetailCompletion {
 std::uint64_t revision = 0;
 DetailState state = DetailState::Pending;
 std::shared_ptr<const Detail> value;
};

// Accessed under the lister mutex. A late completion may never overwrite
// the explicit failure/success of a newer entry, including enqueue failure.
class DetailRequests {
public:
 struct Request { std::string file; std::uint64_t revision = 0; };
 bool Want(const std::string& file, std::uint64_t revision) noexcept {
  if (file.empty() || revision == requested_) return false;
  requested_ = revision;
  try { wanted_ = {file, revision}; return true; }
  catch (...) { Fail(revision); return false; }
 }
 bool Pending() const { return !wanted_.file.empty(); }
 Request Take() noexcept { Request request = std::move(wanted_); wanted_.file.clear(); return request; }
 void Complete(DetailCompletion completion) noexcept {
  if (completion.revision == requested_) latest_ = std::move(completion);
 }
 void Fail(std::uint64_t revision) noexcept {
  if (revision != requested_) return;
  wanted_.file.clear(); latest_ = {revision, DetailState::Failed, {}};
 }
 DetailCompletion Latest() const { return latest_; }
private:
 std::uint64_t requested_ = 0;
 Request wanted_;
 DetailCompletion latest_;
};

// Worker-owned, bounded to one successful detail. File identity is the exact
// bounded file contents, including wrapper metadata; size/mtime/path alone
// cannot detect replacement. Every request rereads; only parsing is cached.
// Failures are never cached, so repair and retry need no unrelated selection.
class DetailCache {
public:
 DetailCompletion Read(const std::string& file, std::uint64_t revision) noexcept {
  DetailCompletion result{revision, DetailState::Failed, {}};
  try {
   auto bytes = replayfiles::ReadFile(std::filesystem::u8path(file));
   if (!bytes) { result.state = DetailState::Unreadable; return result; }
   if (kept_ && kept_->file == file && *bytes == identity_) {
    result.state = DetailState::Ready; result.value = kept_; return result;
   }
   auto detail = std::make_shared<Detail>();
   detail->file = file;
   if (!Parse(bytes->data(), bytes->size(), detail->match)) { result.state = DetailState::Unreadable; return result; }
   detail->summary = Summarize(detail->match);
   for (const auto& round : detail->match.rounds) detail->logs.push_back(Log(round));
   identity_ = std::move(*bytes); kept_ = std::move(detail);
   result.state = DetailState::Ready; result.value = kept_;
  } catch (...) {} // scalar completion survives even the initial allocation
  return result;
 }
private:
 replayslots::Bytes identity_;
 std::shared_ptr<const Detail> kept_;
};

} }
