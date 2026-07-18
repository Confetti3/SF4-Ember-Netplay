#include "sf4e__OverlayPrefs.hxx"

#include <fstream>
#include <shlobj.h>
#include <pathcch.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"

namespace sf4e {
namespace OverlayPrefs {

	namespace {

		namespace rBattle = Dimps::Game::Battle;

		void CharaToJson(nlohmann::json& j, const CharaPick& c) {
			j["charaID"] = c.charaID;
			j["costume"] = c.costume;
			j["color"] = c.color;
			j["personalAction"] = c.personalAction;
			j["winQuote"] = c.winQuote;
			j["ultraCombo"] = c.ultraCombo;
			j["handicap"] = c.handicap;
			j["unc_edition"] = c.unc_edition;
		}

		void CharaFromJson(const nlohmann::json& j, CharaPick& c) {
			if (!j.is_object()) {
				return;
			}
			c.charaID = (uint8_t)j.value("charaID", (int)c.charaID);
			c.costume = (uint8_t)j.value("costume", (int)c.costume);
			c.color = (uint8_t)j.value("color", (int)c.color);
			c.personalAction = (uint8_t)j.value("personalAction", (int)c.personalAction);
			c.winQuote = (uint8_t)j.value("winQuote", (int)c.winQuote);
			c.ultraCombo = (uint8_t)j.value("ultraCombo", (int)c.ultraCombo);
			c.handicap = (uint8_t)j.value("handicap", (int)c.handicap);
			c.unc_edition = (uint8_t)j.value("unc_edition", (int)c.unc_edition);
		}

		void ClampChara(CharaPick& c) {
			if (c.charaID >= 0x2c) {
				c.charaID = 0;
			}
			bool editionOk = false;
			for (int i = 0; i < NUM_VALID_EDITIONS; i++) {
				if (rBattle::validEditionsPerChara
					&& rBattle::validEditionsPerChara[c.charaID].valid[i]
					&& rBattle::orderedEditions[i] == (int)c.unc_edition) {
					editionOk = true;
					break;
				}
			}
			if (!editionOk) {
				c.unc_edition = (uint8_t)rBattle::ED_USF4;
				if (rBattle::validEditionsPerChara) {
					for (int i = 0; i < NUM_VALID_EDITIONS; i++) {
						if (rBattle::validEditionsPerChara[c.charaID].valid[i]) {
							c.unc_edition = (uint8_t)rBattle::orderedEditions[i];
							break;
						}
					}
				}
			}
		}

		void CopyCString(char* dest, size_t destChars, const std::string& src) {
			if (destChars == 0) {
				return;
			}
			strncpy_s(dest, destChars, src.c_str(), _TRUNCATE);
		}

	} // namespace

	void FromConfirmed(
		CharaPick& out,
		const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& in
	) {
		out.charaID = in.charaID;
		out.costume = in.costume;
		out.color = in.color;
		out.personalAction = in.personalAction;
		out.winQuote = in.winQuote;
		out.ultraCombo = in.ultraCombo;
		out.handicap = in.handicap;
		out.unc_edition = in.unc_edition;
	}

	void ToConfirmed(
		Dimps::GameEvents::VsMode::ConfirmedCharaConditions& out,
		const CharaPick& in
	) {
		out.charaID = in.charaID;
		out.costume = in.costume;
		out.color = in.color;
		out._unused = 0;
		out.personalAction = in.personalAction;
		out.winQuote = in.winQuote;
		out.ultraCombo = in.ultraCombo;
		out.handicap = in.handicap;
		out.unc_edition = in.unc_edition;
	}

	void Clamp(Data& data) {
		ClampChara(data.lobby);
		ClampChara(data.mainMenuP1);
		ClampChara(data.mainMenuP2);
		if (data.stageID < 0 || data.stageID >= 30) {
			data.stageID = 0;
		}
		if (data.mainMenuJumpStageID < 0 || data.mainMenuJumpStageID >= 30) {
			data.mainMenuJumpStageID = 0;
		}
		if (data.hostRoundCountIdx < 0 || data.hostRoundCountIdx > 3) {
			data.hostRoundCountIdx = 1;
		}
		if (data.hostRoundTimeIdx < 0 || data.hostRoundTimeIdx > 2) {
			data.hostRoundTimeIdx = 2;
		}
		if (data.mainMenuRoundCountIdx < 0 || data.mainMenuRoundCountIdx > 3) {
			data.mainMenuRoundCountIdx = 1;
		}
		if (data.mainMenuRoundTimeIdx < 0 || data.mainMenuRoundTimeIdx > 2) {
			data.mainMenuRoundTimeIdx = 2;
		}
		if (data.extraFramesToSimulate < 1) {
			data.extraFramesToSimulate = 1;
		}
	}

	bool GetFilePath(wchar_t* outPath, int outPathChars) {
		wchar_t appData[MAX_PATH] = { 0 };
		if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appData))) {
			return false;
		}
		if (FAILED(PathCchCombine(outPath, outPathChars, appData, L"sf4e"))) {
			return false;
		}
		CreateDirectoryW(outPath, NULL);
		if (FAILED(PathCchCombine(outPath, outPathChars, outPath, L"overlay_prefs.json"))) {
			return false;
		}
		return true;
	}

	bool Load(Data& out) {
		wchar_t path[MAX_PATH] = { 0 };
		if (!GetFilePath(path, MAX_PATH)) {
			return false;
		}

		std::ifstream f(path);
		if (!f.is_open()) {
			return false;
		}

		try {
			nlohmann::json j;
			f >> j;
			if (!j.is_object()) {
				return false;
			}

			if (j.contains("lobby")) {
				CharaFromJson(j["lobby"], out.lobby);
			}
			out.stageID = j.value("stageID", out.stageID);

			if (j.contains("host") && j["host"].is_object()) {
				const auto& h = j["host"];
				out.hostDelay = (uint8_t)h.value("delay", (int)out.hostDelay);
				CopyCString(out.hostName, sizeof(out.hostName), h.value("name", std::string(out.hostName)));
				out.hostPort = (uint16_t)h.value("hostPort", (int)out.hostPort);
				out.hostGgpoPort = (uint16_t)h.value("ggpoPort", (int)out.hostGgpoPort);
				out.hostRoundCountIdx = h.value("roundCountIdx", out.hostRoundCountIdx);
				out.hostRoundTimeIdx = h.value("roundTimeIdx", out.hostRoundTimeIdx);
				out.hostEditionSelect = h.value("editionSelect", out.hostEditionSelect);
			}

			if (j.contains("join") && j["join"].is_object()) {
				const auto& join = j["join"];
				out.joinDelay = (uint8_t)join.value("delay", (int)out.joinDelay);
				out.joinGgpoPort = (uint16_t)join.value("ggpoPort", (int)out.joinGgpoPort);
				CopyCString(out.joinName, sizeof(out.joinName), join.value("name", std::string(out.joinName)));
				CopyCString(out.joinAddr, sizeof(out.joinAddr), join.value("joinAddr", std::string(out.joinAddr)));
			}

			if (j.contains("mainMenu") && j["mainMenu"].is_object()) {
				const auto& m = j["mainMenu"];
				out.mainMenuShouldJump = m.value("shouldJump", out.mainMenuShouldJump);
				out.mainMenuJumpStageID = m.value("stageID", out.mainMenuJumpStageID);
				out.mainMenuRoundCountIdx = m.value("roundCountIdx", out.mainMenuRoundCountIdx);
				out.mainMenuRoundTimeIdx = m.value("roundTimeIdx", out.mainMenuRoundTimeIdx);
				out.mainMenuEditionSelect = m.value("editionSelect", out.mainMenuEditionSelect);
				if (m.contains("p1")) {
					CharaFromJson(m["p1"], out.mainMenuP1);
				}
				if (m.contains("p2")) {
					CharaFromJson(m["p2"], out.mainMenuP2);
				}
			}

			if (j.contains("windows") && j["windows"].is_object()) {
				const auto& w = j["windows"];
				out.show_chara_window = w.value("chara", out.show_chara_window);
				out.show_command_window = w.value("command", out.show_command_window);
				out.show_demo_window = w.value("demo", out.show_demo_window);
				out.show_event_window = w.value("event", out.show_event_window);
				out.show_gfxapp_window = w.value("gfxapp", out.show_gfxapp_window);
				out.show_help_window = w.value("help", out.show_help_window);
				out.show_hud_window = w.value("hud", out.show_hud_window);
				out.show_log_window = w.value("log", out.show_log_window);
				out.show_main_menu_window = w.value("mainMenu", out.show_main_menu_window);
				out.show_memento_window = w.value("memento", out.show_memento_window);
				out.show_network_window = w.value("network", out.show_network_window);
				out.show_pad_window = w.value("pad", out.show_pad_window);
				out.show_sound_window = w.value("sound", out.show_sound_window);
				out.show_system_window = w.value("system", out.show_system_window);
				out.show_task_window = w.value("task", out.show_task_window);
				out.show_vfx_window = w.value("vfx", out.show_vfx_window);
				out.show_vsbattle_window = w.value("vsbattle", out.show_vsbattle_window);
				out.show_vscharaselect_window = w.value("vscharaselect", out.show_vscharaselect_window);
				out.show_vsstageselect_window = w.value("vsstageselect", out.show_vsstageselect_window);
			}

			if (j.contains("debug") && j["debug"].is_object()) {
				const auto& d = j["debug"];
				out.verboseLogging = d.value("verboseLogging", out.verboseLogging);
				out.networkDebug = d.value("networkDebug", out.networkDebug);
				out.trackSoundRequests = d.value("trackSoundRequests", out.trackSoundRequests);
				out.usePureSounds = d.value("usePureSounds", out.usePureSounds);
				out.soundShowDetails = d.value("soundShowDetails", out.soundShowDetails);
				out.blockBattleInit = d.value("blockBattleInit", out.blockBattleInit);
				out.blockBattleTermination = d.value("blockBattleTermination", out.blockBattleTermination);
				out.forceNextMatchOnline = d.value("forceNextMatchOnline", out.forceNextMatchOnline);
				out.terminateOnNextLeftBattle = d.value("terminateOnNextLeftBattle", out.terminateOnNextLeftBattle);
				out.overrideNextRandomSeed = d.value("overrideNextRandomSeed", out.overrideNextRandomSeed);
				out.nextMatchRandomSeed = (uint32_t)d.value("nextMatchRandomSeed", (uint32_t)out.nextMatchRandomSeed);
				out.forceTimerOnNextStageSelect = d.value("forceTimerOnNextStageSelect", out.forceTimerOnNextStageSelect);
				out.randomizeLocalInputsEveryXFrames = d.value(
					"randomizeLocalInputsEveryXFrames",
					out.randomizeLocalInputsEveryXFrames
				);
				out.extraFramesToSimulate = d.value("extraFramesToSimulate", out.extraFramesToSimulate);
			}

			if (j.contains("device") && j["device"].is_object()) {
				const auto& dev = j["device"];
				out.deviceIdx = (uint8_t)dev.value("idx", (int)out.deviceIdx);
				out.deviceType = (uint8_t)dev.value("type", (int)out.deviceType);
			}

			Clamp(out);
			return true;
		}
		catch (...) {
			spdlog::warn("Could not parse overlay_prefs.json");
			return false;
		}
	}

	bool Save(const Data& in) {
		wchar_t path[MAX_PATH] = { 0 };
		if (!GetFilePath(path, MAX_PATH)) {
			return false;
		}

		Data clamped = in;
		Clamp(clamped);

		nlohmann::json j;
		CharaToJson(j["lobby"], clamped.lobby);
		j["stageID"] = clamped.stageID;

		j["host"] = {
			{"delay", clamped.hostDelay},
			{"name", clamped.hostName},
			{"hostPort", clamped.hostPort},
			{"ggpoPort", clamped.hostGgpoPort},
			{"roundCountIdx", clamped.hostRoundCountIdx},
			{"roundTimeIdx", clamped.hostRoundTimeIdx},
			{"editionSelect", clamped.hostEditionSelect},
		};

		j["join"] = {
			{"delay", clamped.joinDelay},
			{"ggpoPort", clamped.joinGgpoPort},
			{"name", clamped.joinName},
			{"joinAddr", clamped.joinAddr},
		};

		nlohmann::json mainMenu;
		mainMenu["shouldJump"] = clamped.mainMenuShouldJump;
		mainMenu["stageID"] = clamped.mainMenuJumpStageID;
		mainMenu["roundCountIdx"] = clamped.mainMenuRoundCountIdx;
		mainMenu["roundTimeIdx"] = clamped.mainMenuRoundTimeIdx;
		mainMenu["editionSelect"] = clamped.mainMenuEditionSelect;
		CharaToJson(mainMenu["p1"], clamped.mainMenuP1);
		CharaToJson(mainMenu["p2"], clamped.mainMenuP2);
		j["mainMenu"] = mainMenu;

		j["windows"] = {
			{"chara", clamped.show_chara_window},
			{"command", clamped.show_command_window},
			{"demo", clamped.show_demo_window},
			{"event", clamped.show_event_window},
			{"gfxapp", clamped.show_gfxapp_window},
			{"help", clamped.show_help_window},
			{"hud", clamped.show_hud_window},
			{"log", clamped.show_log_window},
			{"mainMenu", clamped.show_main_menu_window},
			{"memento", clamped.show_memento_window},
			{"network", clamped.show_network_window},
			{"pad", clamped.show_pad_window},
			{"sound", clamped.show_sound_window},
			{"system", clamped.show_system_window},
			{"task", clamped.show_task_window},
			{"vfx", clamped.show_vfx_window},
			{"vsbattle", clamped.show_vsbattle_window},
			{"vscharaselect", clamped.show_vscharaselect_window},
			{"vsstageselect", clamped.show_vsstageselect_window},
		};

		j["debug"] = {
			{"verboseLogging", clamped.verboseLogging},
			{"networkDebug", clamped.networkDebug},
			{"trackSoundRequests", clamped.trackSoundRequests},
			{"usePureSounds", clamped.usePureSounds},
			{"soundShowDetails", clamped.soundShowDetails},
			{"blockBattleInit", clamped.blockBattleInit},
			{"blockBattleTermination", clamped.blockBattleTermination},
			{"forceNextMatchOnline", clamped.forceNextMatchOnline},
			{"terminateOnNextLeftBattle", clamped.terminateOnNextLeftBattle},
			{"overrideNextRandomSeed", clamped.overrideNextRandomSeed},
			{"nextMatchRandomSeed", clamped.nextMatchRandomSeed},
			{"forceTimerOnNextStageSelect", clamped.forceTimerOnNextStageSelect},
			{"randomizeLocalInputsEveryXFrames", clamped.randomizeLocalInputsEveryXFrames},
			{"extraFramesToSimulate", clamped.extraFramesToSimulate},
		};

		j["device"] = {
			{"idx", clamped.deviceIdx},
			{"type", clamped.deviceType},
		};

		std::ofstream f(path);
		if (!f.is_open()) {
			return false;
		}
		f << j.dump(2);
		return true;
	}

} // namespace OverlayPrefs
} // namespace sf4e
