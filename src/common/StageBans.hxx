#pragma once
// The stage P1's Ready sends under a room's Banned stages rule
// (room::Snapshot::bannedStages), as one pure function so every game and the
// tests decide it the same way.
//
// `resolve` is the game's own choice of stage (GameCatalog::resolveStage with
// the roll, pool and installed content already bound): given the stages that
// may not be rolled or sent for the opponent's sake it returns the stage for
// the choice, or `randomStage` itself for a Random with none left.
// - No bans: exactly `resolve(unavailable)`.
// - A specific choice that is banned: the game's Training stage.
// - Random: rolled by `resolve` over the stages that are neither unavailable
//   nor banned; with none left, the Training stage.
// `training` is GameState::TrainingStage, -1 where the game has none, which
// leaves the result as `resolve` made it. The stage sent decides the game on
// every copy, so the copies cannot disagree. Stages the opponent lacks keep
// their handling (the Ready check refuses a result they lack).
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <vector>

namespace sf4e {

inline int StageUnderBans(int choice, int randomStage, std::int64_t training, const std::vector<std::int64_t>& unavailable,
	const std::vector<std::int64_t>& banned, const std::function<int(const std::vector<std::int64_t>&)>& resolve) {
	if (banned.empty()) return resolve(unavailable);
	if (choice != randomStage) {
		const int chosen = resolve(unavailable);
		const bool isBanned = std::binary_search(banned.begin(), banned.end(), static_cast<std::int64_t>(chosen));
		return isBanned && training >= 0 ? static_cast<int>(training) : chosen;
	}
	std::vector<std::int64_t> excluded;
	std::set_union(unavailable.begin(), unavailable.end(), banned.begin(), banned.end(), std::back_inserter(excluded));
	const int rolled = resolve(excluded);
	return rolled == randomStage && training >= 0 ? static_cast<int>(training) : rolled;
}

}
