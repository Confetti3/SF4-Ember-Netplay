#include "sf4e__OverlayPrefs.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../netplay/SettingsWriter.hxx"
#include "../netplay/RoomPreferences.hxx"

#include <fstream>
#include <shlobj.h>
#include <pathcch.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/StageValue.hxx"

namespace sf4e {
namespace OverlayPrefs {

	namespace {

		namespace rBattle = Dimps::Game::Battle;
		// Explicit Main teardown owns the worker; no DLL static destructor joins it.
		netplay::SettingsWriter* writer = nullptr;
		Data cached;
		bool haveCached = false;
		std::string startupError;

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
			using selection::PreferenceByte;
			c.charaID = PreferenceByte(j, "charaID", c.charaID);
			c.costume = PreferenceByte(j, "costume", c.costume);
			c.color = PreferenceByte(j, "color", c.color);
			c.personalAction = PreferenceByte(j, "personalAction", c.personalAction);
			c.winQuote = PreferenceByte(j, "winQuote", c.winQuote);
			c.ultraCombo = PreferenceByte(j, "ultraCombo", c.ultraCombo);
			c.handicap = PreferenceByte(j, "handicap", c.handicap);
			c.unc_edition = PreferenceByte(j, "unc_edition", c.unc_edition);
		}

		void ClampChara(CharaPick& c, bool editionSelect) {
            auto pick = selection::FromNative(c);
            selection::Normalize(pick, editionSelect);
            selection::ToNative(pick, c);
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
        ClampChara(data.lobby, data.lobbyEditionSelect);
        for (int id = 0; id < selection::FighterCount; ++id) {
            data.fighters[id].charaID = static_cast<uint8_t>(id);
            // Keep a remembered edition whatever the current rule; restoring a
            // fighter normalizes it against the room's rule at that time.
            ClampChara(data.fighters[id], true);
        }
        if (data.lobby.charaID < data.fighters.size()) data.fighters[data.lobby.charaID] = data.lobby;
        data.stageID = selection::NormalizeStageChoice(data.stageID);
        if (data.lobbyRoundCountIdx < 0 || data.lobbyRoundCountIdx >= ROUND_COUNT_OPTIONS) data.lobbyRoundCountIdx = 1;
        if (data.lobbyRoundTimeIdx < 0 || data.lobbyRoundTimeIdx >= ROUND_TIME_OPTIONS) data.lobbyRoundTimeIdx = 2;
        if ((data.deviceType != 1 && data.deviceType != 3) || (data.deviceType == 3 && data.deviceIdx > 3)) data.deviceIdx = data.deviceType = 0xff;
    }

	void FromJson(const nlohmann::json& j, Data& out) {
		if (j.contains("lobby")) {
			CharaFromJson(j["lobby"], out.lobby);
		}
		if (j.contains("fighters") && j["fighters"].is_array()) {
			const auto& fighters = j["fighters"];
			for (std::size_t id = 0; id < fighters.size() && id < out.fighters.size(); ++id) {
				CharaFromJson(fighters[id], out.fighters[id]);
				out.fighters[id].charaID = static_cast<uint8_t>(id);
			}
		} else if (out.lobby.charaID < out.fighters.size()) {
			// Written before picks were per fighter: the one shared pick belongs
			// to the fighter it was last used on.
			out.fighters[out.lobby.charaID] = out.lobby;
		}
		out.stageID = selection::PreferenceStage(j, "stageID", out.stageID);

		if (j.contains("lobbySettings") && j["lobbySettings"].is_object()) {
			const auto& ls = j["lobbySettings"];
			out.lobbyRoundCountIdx = ls.value("roundCountIdx", out.lobbyRoundCountIdx);
			out.lobbyRoundTimeIdx = ls.value("roundTimeIdx", out.lobbyRoundTimeIdx);
			out.lobbyEditionSelect = ls.value("editionSelect", out.lobbyEditionSelect);
		}

		if (j.contains("device") && j["device"].is_object()) {
			const auto& dev = j["device"];
			out.deviceIdx = (uint8_t)dev.value("idx", (int)out.deviceIdx);
			out.deviceType = (uint8_t)dev.value("type", (int)out.deviceType);
		}
	}

	nlohmann::json ToJson(const Data& data) {
		nlohmann::json j;
		CharaToJson(j["lobby"], data.lobby);
		j["fighters"] = nlohmann::json::array();
		for (const auto& fighter : data.fighters) {
			nlohmann::json row;
			CharaToJson(row, fighter);
			j["fighters"].push_back(std::move(row));
		}
		j["stageID"] = data.stageID;

		j["lobbySettings"] = {
			{"roundCountIdx", data.lobbyRoundCountIdx},
			{"roundTimeIdx", data.lobbyRoundTimeIdx},
			{"editionSelect", data.lobbyEditionSelect},
		};

		j["device"] = {
			{"idx", data.deviceIdx},
			{"type", data.deviceType},
		};
		return j;
	}

	bool Load(Data& out) {
		// Device recreation must not reload older disk state over queued edits.
		if (haveCached) { out = cached; return true; }
		nlohmann::json j;
		std::string error;
		netplay::SettingsStore store(netplay::SettingsStore::DefaultDirectory());
		if (!store.LoadOverlay(j, error)) {
			spdlog::warn("{}", error);
			return false;
		}

		try {
			if (!j.is_object()) {
				return false;
			}

			FromJson(j, out);
			Clamp(out);
			cached = out;
			haveCached = true;
			return true;
		}
		catch (...) {
			spdlog::warn("Could not parse overlay preferences in settings.json");
			return false;
		}
	}

	void StartPersistence() {
		if (writer) return;
		try {
			writer = new netplay::SettingsWriter(netplay::SettingsStore::DefaultDirectory());
			startupError.clear();
		} catch (...) { startupError = "The preferences worker could not start."; }
	}

	void StopPersistence() {
		if (!writer) return;
		writer->Stop();
		const auto status = writer->GetStatus();
		if (status.pending) spdlog::warn("Preferences could not be saved before exit: {}", status.error);
		delete writer;
		writer = nullptr;
	}

	std::string PersistenceError() { return writer ? writer->GetStatus().error : startupError; }
	bool PersistencePending() { return writer && writer->GetStatus().pending; }
	bool SavePlayerPreferences(const netplay::PlayerPreferences& preferences) {
		return QueuePlayerPreferences(preferences) != 0;
	}
	bool PlayerPreferencesSaved(std::uint64_t revision) {
		return writer && revision && writer->SavedLauncherRevision() >= revision;
	}
	std::uint64_t QueuePlayerPreferences(const netplay::PlayerPreferences& preferences) {
		if (!writer || !preferences.Valid()) return 0;
        nlohmann::json values={{"displayName", preferences.displayName}, {"mainFighter",preferences.mainFighter}, {"inputDelay", preferences.inputDelay},
			{"editionSelect", preferences.lobby.editionSelect ? 1 : 0}, {"roundCount", preferences.lobby.roundCount},
			{"roundTimeIntegral", preferences.lobby.roundTime}, {"showMatchHud", preferences.showMatchHud},
			{"discordPresence", preferences.discordPresence}, {"discordInvites", preferences.discordInvites},
            {"matchHudSize",preferences.matchHudSize},{"matchHudRaised",preferences.matchHudRaised},
            {"interfaceScale", preferences.interfaceScale}, {"roomDefaults", netplay::RoomPreferences(preferences)}};
        if(preferences.record.available)values["onlineRecord"]=netplay::ProfileRecordJson(preferences.record);
        return writer->QueueLauncher(std::move(values));
	}

	bool Save(const Data& in) {
		Data clamped = in;
		Clamp(clamped);

		if (!writer || !writer->QueueOverlay(ToJson(clamped))) return false;
		cached = clamped;
		haveCached = true;
		return true;
	}

} // namespace OverlayPrefs
} // namespace sf4e
