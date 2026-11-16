#pragma once

// Watches one battle load from its start to its first frame, so a load that
// never finishes leaves a line in the log. The game thread keeps drawing frames
// through such a stall, so the freeze watch (CrashDump.hxx) never fires.
// Time is passed in (milliseconds on a monotonic clock): no clock, no
// Windows, nothing to mock. The Win32 side is sf4e__BattleLoad.cxx.

#include <cstdint>
#include <optional>
#include <variant>

namespace sf4e {
namespace loadwatch {

// How long a load runs before each report. The reports are once per load.
constexpr std::uint64_t FirstStallMs = 20000;
constexpr std::uint64_t SecondStallMs = 60000;

enum class ReportKind { StalledAt20s, StillStalledAt60s, Recovered };

struct Report {
	ReportKind kind;
	// Milliseconds since the load started.
	std::uint64_t elapsedMs;
};

class LoadWatch {
public:
	// The engine began a battle load. A load still being watched is abandoned.
	void LoadStarted(std::uint64_t nowMs) { state_ = Loading{ nowMs }; }

	// The battle's first frame. Recovered when a stall had been reported for
	// this load; a healthy load says nothing.
	std::optional<Report> FirstFrame(std::uint64_t nowMs) {
		std::optional<Report> report;
		if (const auto* reported = std::get_if<Reported>(&state_))
			report = Report{ ReportKind::Recovered, Since(reported->since, nowMs) };
		state_ = Idle{};
		return report;
	}

	// The battle closed, so a load that never produced a frame is over.
	void BattleClosed(std::uint64_t) { state_ = Idle{}; }

	// Called every frame: the next report that is due, if any.
	std::optional<Report> Tick(std::uint64_t nowMs) {
		if (const auto* loading = std::get_if<Loading>(&state_)) {
			const std::uint64_t since = loading->since;
			const std::uint64_t elapsed = Since(since, nowMs);
			if (elapsed < FirstStallMs) return std::nullopt;
			state_ = Reported{ since, Stage::At20s };
			return Report{ ReportKind::StalledAt20s, elapsed };
		}
		if (const auto* reported = std::get_if<Reported>(&state_)) {
			const std::uint64_t since = reported->since;
			const std::uint64_t elapsed = Since(since, nowMs);
			if (reported->stage != Stage::At20s || elapsed < SecondStallMs) return std::nullopt;
			state_ = Reported{ since, Stage::At60s };
			return Report{ ReportKind::StillStalledAt60s, elapsed };
		}
		return std::nullopt;
	}

private:
	enum class Stage { At20s, At60s };
	struct Idle {};
	struct Loading { std::uint64_t since; };
	struct Reported { std::uint64_t since; Stage stage; };

	static std::uint64_t Since(std::uint64_t since, std::uint64_t nowMs) { return nowMs > since ? nowMs - since : 0; }

	std::variant<Idle, Loading, Reported> state_;
};

} // namespace loadwatch
} // namespace sf4e
