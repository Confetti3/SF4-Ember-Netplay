// The fighters and stage a battle load reads from the Versus and Training
// events, whoever filled them, on fake event memory laid out as the game's is.

#include "../sf4e/sf4e__BattleLoadRead.hxx"

#include <cstdint>
#include <cstring>

#include "test_support.hxx"

using namespace sf4e;
using namespace Dimps::GameEvents;

static void SetSide(std::uint8_t* player, int active, int fighter, int costume, int color, int ultra, int edition) {
	const VsMode::ConfirmedCharaConditions chara = { (BYTE)fighter, (BYTE)costume, (BYTE)color, 0, 0, 0, (BYTE)ultra, 0, (BYTE)edition };
	std::memcpy(player + 0x128, &chara, sizeof(chara));
	std::memcpy(player + 0x138, &active, sizeof(active));
}

static void TestVersusIsReadFromTheEvent() {
	alignas(8) std::uint8_t event[0x400] = {};
	auto* mode = reinterpret_cast<VsMode*>(event);
	auto* players = event + 0x58;
	SetSide(players, 1, 3, 1, 2, 1, 14);
	SetSide(players + sizeof(VsMode::ConfirmedPlayerConditions), 1, 7, 0, 5, 2, 14);
	const int stage = 18;
	std::memcpy(event + 0x2ec, &stage, sizeof(stage));
	const auto facts = battleload::ReadVersus(mode);
	CHECK(facts.has_value());
	CHECK(loadwatch::FormatBattleLoad(*facts) ==
		"mode=native versus P1 fighter=3 costume=1 color=2 ultra=1 edition=14, P2 fighter=7 costume=0 color=5 ultra=2 edition=14, stage=18");
	// A side that is not set up is not guessed.
	SetSide(players + sizeof(VsMode::ConfirmedPlayerConditions), 0, 7, 0, 5, 2, 14);
	CHECK(!battleload::ReadVersus(mode));
}

static void TestTrainingIsReadFromTheSelections() {
	alignas(8) std::uint8_t event[0x100] = {};
	auto* mode = reinterpret_cast<TrainingMode*>(event);
	const VsMode::ConfirmedCharaConditions p1 = { 0, 2, 1, 0, 0, 0, 1, 0, 14 };
	const VsMode::ConfirmedCharaConditions p2 = { 12, 0, 3, 0, 0, 0, 2, 0, 14 };
	TrainingMode::GetSelection(mode, 0)->conditions = p1;
	TrainingMode::GetSelection(mode, 0)->chosen = TRUE;
	TrainingMode::GetSelection(mode, 1)->conditions = p2;
	TrainingMode::GetSelection(mode, 1)->chosen = TRUE;
	const std::uint8_t push[2] = { 0x6A, 4 };
	const auto facts = battleload::ReadTraining(mode, push);
	CHECK(facts.has_value());
	CHECK(loadwatch::FormatBattleLoad(*facts) ==
		"mode=native training P1 fighter=0 costume=2 color=1 ultra=1 edition=14, P2 fighter=12 costume=0 color=3 ultra=2 edition=14, stage=4");
	// Bytes that are not Training's stage push leave the stage unread, not zero.
	const std::uint8_t other[2] = { 0x90, 4 };
	const auto unread = battleload::ReadTraining(mode, other);
	CHECK(unread.has_value());
	CHECK(loadwatch::FormatBattleLoad(*unread).find("stage=not read") != std::string::npos);
	TrainingMode::GetSelection(mode, 1)->chosen = FALSE;
	CHECK(!battleload::ReadTraining(mode, push));
}

int main() {
	TestVersusIsReadFromTheEvent();
	TestTrainingIsReadFromTheSelections();
	return 0;
}
