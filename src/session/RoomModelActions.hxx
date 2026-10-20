#pragma once
#include <cstdint>

namespace sf4e { namespace room {

enum class ActionKind : std::uint8_t {
	Join = 0,
	Leave,
	Queue,
	Unqueue,
	Watch,
	Unwatch,
	Ready,
	Unready,
	SetRules,
	SetCapacity,
	Lock,
	Kick,
	Close,
	Chat,
	Rename,
	TransferHost,
	RecordResult,
	MatchFinished,
	CancelResult,
	AbortMatch,
	AcknowledgeTerminal,
	// A spectator's lock-in, on (Action::locked) or off. Appended so older
	// authorities reject it as an unknown kind.
	LockSpectating,
	// A bound table's fighter has the bridge's permit for the reserved game
	// (Action::matchGeneration), named by Action::text.
	PermitReady,
	// A member's own word that they are in the game's Training mode, on
	// (Action::inTraining) or off. It changes what the others are shown and
	// nothing else. Appended so older authorities reject it as an unknown
	// kind.
	SetTraining,
	// Opt-in private matches: report a numbered game without ending its live
	// transport, then close that transport after both fighters request it.
	RecordContinuousResult,
	FinishContinuousMatch,
};
// The last kind, the wire's upper bound: a kind appended after it moves this too.
constexpr ActionKind LastActionKind = ActionKind::FinishContinuousMatch;

// Both normal and continuous games use the same two-fighter result quorum.
enum class ReportAgreement { First, Agreed, Disputed, Duplicate };

} }
