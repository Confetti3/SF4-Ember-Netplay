#pragma once

// What the game's own Versus and Training events hold for the battle about to
// load, as BattleLoadFacts. Ember's loads write their picks into the same
// events, so one reader serves every load. Plain reads of event memory, apart
// from sf4e__BattleLoad.cxx so the test can read a fake event.

#include <cstdint>
#include <optional>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../common/BattleLoadFacts.hxx"

namespace sf4e {
namespace battleload {

inline loadwatch::LoadSide SideOf(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& c) {
	return { c.charaID, c.costume, c.color, c.ultraCombo, c.unc_edition };
}

// Empty when a side is not set up (the load then says not read).
inline std::optional<loadwatch::BattleLoadFacts> ReadVersus(Dimps::GameEvents::VsMode* mode) {
	using Player = Dimps::GameEvents::VsMode::ConfirmedPlayerConditions;
	Player* players = Dimps::GameEvents::VsMode::GetConfirmedPlayerConditions(mode);
	loadwatch::BattleLoadFacts facts;
	facts.mode = loadwatch::LoadMode::Versus;
	for (int i = 0; i < 2; ++i) {
		if (!*Player::GetSideActive(&players[i])) return std::nullopt;
		facts.sides[i] = SideOf(*Player::GetCharaConditions(&players[i]));
	}
	facts.stage = *Dimps::GameEvents::VsMode::GetStageCode(mode);
	return facts;
}

// The stage the push at `stagePush` holds now (TrainingBattle::stagePush);
// empty for bytes that are not that push.
inline std::optional<int> TrainingStageOf(const std::uint8_t* stagePush) {
	if (stagePush[0] != 0x6A || stagePush[1] > 0x7F) return std::nullopt;
	return stagePush[1];
}

inline std::optional<loadwatch::BattleLoadFacts> ReadTraining(Dimps::GameEvents::TrainingMode* mode, const std::uint8_t* stagePush) {
	Dimps::GameEvents::VsMode::ConfirmedCharaConditions chosen[2];
	if (!Dimps::GameEvents::TrainingMode::GetChosenSides(mode, chosen)) return std::nullopt;
	loadwatch::BattleLoadFacts facts;
	facts.mode = loadwatch::LoadMode::Training;
	for (int i = 0; i < 2; ++i) facts.sides[i] = SideOf(chosen[i]);
	facts.stage = TrainingStageOf(stagePush);
	return facts;
}

} // namespace battleload
} // namespace sf4e
