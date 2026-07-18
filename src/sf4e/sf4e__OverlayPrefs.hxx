#pragma once

#include <stdint.h>

#include "../Dimps/Dimps__GameEvents.hxx"

namespace sf4e {
namespace OverlayPrefs {

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

		// Dev host panel
		uint8_t hostDelay = 1;
		char hostName[32] = { 0 };
		uint16_t hostPort = 23456;
		uint16_t hostGgpoPort = 23457;
		int hostRoundCountIdx = 1;
		int hostRoundTimeIdx = 2;
		bool hostEditionSelect = true;

		// Dev join panel
		uint8_t joinDelay = 1;
		uint16_t joinGgpoPort = 23456;
		char joinName[32] = { 0 };
		char joinAddr[64] = { 0 };

		// Main-menu skip-select
		bool mainMenuShouldJump = false;
		int mainMenuJumpStageID = 0;
		CharaPick mainMenuP1;
		CharaPick mainMenuP2;
		int mainMenuRoundCountIdx = 1;
		int mainMenuRoundTimeIdx = 2;
		bool mainMenuEditionSelect = true;

		// Window visibility
		bool show_chara_window = false;
		bool show_command_window = false;
		bool show_demo_window = false;
		bool show_event_window = false;
		bool show_gfxapp_window = false;
		bool show_help_window = false;
		bool show_hud_window = false;
		bool show_log_window = false;
		bool show_main_menu_window = false;
		bool show_memento_window = false;
		bool show_network_window = false;
		bool show_pad_window = false;
		bool show_sound_window = false;
		bool show_system_window = false;
		bool show_task_window = false;
		bool show_vfx_window = false;
		bool show_vsbattle_window = false;
		bool show_vscharaselect_window = false;
		bool show_vsstageselect_window = false;

		// Debug / utility
		bool verboseLogging = false;
		bool networkDebug = false;
		bool trackSoundRequests = false;
		bool usePureSounds = true;
		bool soundShowDetails = false;
		bool blockBattleInit = false;
		bool blockBattleTermination = false;
		bool forceNextMatchOnline = false;
		bool terminateOnNextLeftBattle = false;
		bool overrideNextRandomSeed = false;
		uint32_t nextMatchRandomSeed = 0;
		bool forceTimerOnNextStageSelect = false;
		int randomizeLocalInputsEveryXFrames = 0;
		int extraFramesToSimulate = 1;

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
	bool Save(const Data& in);
	bool GetFilePath(wchar_t* outPath, int outPathChars);

} // namespace OverlayPrefs
} // namespace sf4e
