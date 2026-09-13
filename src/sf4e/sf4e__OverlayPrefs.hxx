#pragma once

#include <stdint.h>
#include <string>
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
	};

	struct Data {
		// Lobby
		CharaPick lobby;
		int stageID = 0;

		// Lobby match settings (host-editable in the network panel)
		int lobbyRoundCountIdx = 1;
		int lobbyRoundTimeIdx = 2;
		bool lobbyEditionSelect = true;

		// Device capture
		uint8_t deviceIdx = 0xff;
		uint8_t deviceType = 0xff;
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

	bool Load(Data& out);
	// Start outside DllMain; keep the writer alive across D3D device resets.
	void StartPersistence();
	void StopPersistence();
	std::string PersistenceError();
	bool PersistencePending();
	bool SavePlayerPreferences(const netplay::PlayerPreferences& preferences);
	// Accepts a copied snapshot; completion/errors belong to the settings worker.
	bool Save(const Data& in);


} // namespace OverlayPrefs
} // namespace sf4e
