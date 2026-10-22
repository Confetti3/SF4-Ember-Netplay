#pragma once
#include <cstdint>

namespace sf4e { namespace replay {
// Observed only on the outer game tick. A confirmed match token increases
// across games and GGPO sessions; predicted results never authorize a save.
struct MatchBoundary {
 bool continuous = false;
 std::uint64_t token = 0;
 bool firstGame = false, decided = false;
};
enum class SaveRoute { Check, Idle, Wait };
struct RecordingEvents { bool start = false, end = false; };

class MatchRecording {
public:
 RecordingEvents Observe(const MatchBoundary& match) {
  RecordingEvents events;
  if (!match.continuous || !match.token || match.token < started_) return events;
  if (match.token > started_) {
   started_ = match.token;
   // The session entry already posted the first match's start and names.
   events.start = !match.firstGame;
  }
  if (match.decided && match.token > ended_) {
   ended_ = match.token;
   events.end = true;
  }
  return events;
 }
 SaveRoute Route(const MatchBoundary& match, bool idle) const {
  if (!match.continuous) return idle ? SaveRoute::Idle : SaveRoute::Check;
  if (!match.token || !match.decided || match.token <= saved_)
   return idle ? SaveRoute::Idle : SaveRoute::Wait;
  // The native task becomes idle after saving the first game. Reuse its
  // original check for each subsequent confirmed result, without rebuilding
  // the task or touching the recorder's rollback-owned buffers.
  return SaveRoute::Check;
 }
 void Submitted(const MatchBoundary& match) {
  if (match.continuous && match.decided && match.token > saved_) saved_ = match.token;
 }
private:
 std::uint64_t started_ = 0, ended_ = 0, saved_ = 0;
};
} }
