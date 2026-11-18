#pragma once
// A full terminal ledger, the only state in which a Leave (or a Kick's) is
// refused. Reaching it by play takes 64 unacknowledged games, so suites write it
// into a room checkpoint instead.
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

// Fills `checkpoint` (RoomAuthority::Checkpoint) with the maximum number of
// unacknowledged receipts on tables 1 to 3, each owed by the recipients of
// table 0's live game, whose fighters are p1 and p2.
inline void FillTerminalLedger(nlohmann::json& checkpoint, std::uint64_t p1, std::uint64_t p2) {
	auto recipients = checkpoint.at("active_match_recipients").at(0);
	for (auto& row : recipients) row["acknowledged"] = false;
	auto receipts = nlohmann::json::array();
	std::uint64_t generation = 1;
	std::uint8_t table = 1;
	while (receipts.size() < 64) {
		receipts.push_back({{"table", table}, {"generation", generation}, {"result", 0},
			{"fighters", {p1, p2}}, {"recipients", recipients}, {"acknowledged", false}});
		if (++table == 4) { table = 1; ++generation; }
	}
	for (std::size_t i = 1; i < 4; ++i) checkpoint["snapshot"]["tables"][i]["match_generation"] = generation;
	checkpoint["next_match"] = (std::max)(checkpoint.at("next_match").get<std::uint64_t>(), generation + 10);
	checkpoint["terminal_receipts"] = receipts;
}
