#pragma once

#include <stdint.h>
#include <cstdint>
#include <array>
#include <string>
#include <optional>
#include <tuple>
#include "../common/GamepadIdentity.hxx"
#include <nlohmann/json.hpp>
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"
#include "../netplay/PlayerPreferences.hxx"

#include "../Dimps/Dimps__GameEvents.hxx"

namespace sf4e {
namespace OverlayPrefs {

	// Lengths of roundCountList / roundTimeList in sf4e__Overlay.cxx. Kept
	// here so Clamp can bound the saved indices against the same values the
	// combos are built from.
	const int ROUND_COUNT_OPTIONS = 6;
	const int ROUND_TIME_OPTIONS = 5;

	struct CharaPick {
		uint8_t charaID = 0;
		uint8_t costume = 0;
		uint8_t color = 0;
		uint8_t personalAction = 0;
		uint8_t winQuote = 0;
		uint8_t ultraCombo = 0;
		uint8_t handicap = 0;
		uint8_t unc_edition = 14; // ED_USF4
		bool operator==(const CharaPick& other) const {
            return std::tie(charaID, costume, color, personalAction, winQuote, ultraCombo, handicap, unc_edition) ==
                std::tie(other.charaID, other.costume, other.color, other.personalAction, other.winQuote,
                    other.ultraCombo, other.handicap, other.unc_edition);
        }
	};

	struct Data {
		Data() { for (int id = 0; id < selection::FighterCount; ++id) fighters[id].charaID = static_cast<uint8_t>(id); }

		// Lobby: the current selection, which every consumer reads.
		CharaPick lobby;
		// What each fighter last used, indexed by charaID. Switching fighter
		// restores that fighter's entry; fighters[lobby.charaID] equals lobby.
		std::array<CharaPick, selection::FighterCount> fighters;
		// Training's other side (Training select).
		CharaPick trainingDummy;
		int stageID = 0;
		// The stages Random may land on (saved as the list of those taken out).
		selection::RandomPool randomPool;

		// Lobby match settings (host-editable in the network panel)
		int lobbyRoundCountIdx = 1;
		int lobbyRoundTimeIdx = 2;
		bool lobbyEditionSelect = true;

		// Device capture
		uint8_t deviceIdx = 0xff;
		uint8_t deviceType = 0xff;
		std::optional<gamepads::Identity> deviceIdentity;
		bool controllerLayer = true;
		bool operator==(const Data& other) const {
            return std::tie(lobby, fighters, trainingDummy, stageID, randomPool, lobbyRoundCountIdx, lobbyRoundTimeIdx,
                lobbyEditionSelect, deviceIdx, deviceType, deviceIdentity, controllerLayer) ==
                std::tie(other.lobby, other.fighters, other.trainingDummy, other.stageID, other.randomPool,
                    other.lobbyRoundCountIdx, other.lobbyRoundTimeIdx, other.lobbyEditionSelect,
                    other.deviceIdx, other.deviceType, other.deviceIdentity, other.controllerLayer);
        }
        bool operator!=(const Data& other) const { return !(*this == other); }
	};

	void FromConfirmed(
		CharaPick& out,
		const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& in
	);
	void ToConfirmed(
		Dimps::GameEvents::VsMode::ConfirmedCharaConditions& out,
		const CharaPick& in
	);
	void Clamp(Data& data);
	// The settings file's "overlay" section. FromJson accepts a section written
	// before per-fighter picks existed and remembers its one pick for that
	// fighter. Neither clamps; Load and Save do.
	void FromJson(const nlohmann::json& section, Data& out);
	nlohmann::json ToJson(const Data& data);

	// A one-shot read: it reads the disk whenever nothing is cached, even
	// right after a failed read.
	bool Load(Data& out);
	// Load for a caller that may run every tick: after a failed read it skips
	// the disk for a few seconds and returns false.
	bool LoadEachTick(Data& out);
	// Only the saved gameplay device's identity, read as Load would, without
	// copying the rest of the profile. Empty when none is saved or Load fails.
	// Read every tick, so after a failed read it skips the disk for a few
	// seconds and returns empty.
	std::optional<gamepads::Identity> DeviceIdentity();
	// Tests only: loads read this settings folder instead of the user's, and
	// the cached profile and failed-read memory are cleared. A writer started
	// after this (StartPersistence) saves there too; one already running is unchanged.
	void UseDirectoryForTest(const std::wstring& directory);
	// Start outside DllMain; keep the writer alive across D3D device resets.
	void StartPersistence();
	void StopPersistence();
	std::string PersistenceError();
	bool PersistencePending();
	// Queues a complete profile snapshot. True means accepted, not written.
	bool SavePlayerPreferences(const netplay::PlayerPreferences& preferences);
	// As above, returning the accepted revision (0 when rejected). Pass it to
	// PlayerPreferencesSaved when the caller must know the write completed.
	std::uint64_t QueuePlayerPreferences(const netplay::PlayerPreferences& preferences);
	bool PlayerPreferencesSaved(std::uint64_t revision);
	// Accepts a copied snapshot; completion/errors belong to the settings worker.
	bool Save(const Data& in);
    // Game-thread choice, merged with overlay edits through the same settings writer.
    bool SaveGameplayDevice(int type, int index, const std::optional<gamepads::Identity>& identity);


} // namespace OverlayPrefs
} // namespace sf4e
