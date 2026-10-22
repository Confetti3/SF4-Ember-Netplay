#include "sf4e__OverlayPrefs.hxx"
#include "../common/ButtonLayoutJson.hxx"
#include "sf4e__CrashDiagnostics.hxx"
#include "sf4e__CustomContent.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../netplay/SettingsWriter.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../netplay/BoolPreferenceJson.hxx"
#include "../netplay/ButtonPromptPreference.hxx"
#include "../netplay/CountryPreferenceJson.hxx"

#include <algorithm>
#include <fstream>
#include <climits>
#include <mutex>
#include <shlobj.h>
#include <pathcch.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/PadKind.hxx"
#include "../common/StageValue.hxx"

namespace sf4e {
namespace OverlayPrefs {

	namespace {

		namespace rBattle = Dimps::Game::Battle;
		// Explicit Main teardown owns the worker; no DLL static destructor joins it.
		netplay::SettingsWriter* writer = nullptr;
		std::mutex cacheMutex;
		Data cached;
		bool haveCached = false;
		std::string startupError;
		constexpr netplay::BoolPreferenceMember<Data> boolPreferences[] = {{"controllerLayer", &Data::controllerLayer}};

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

		// Picks of custom content the setup program has removed since: the original costume (color kept unless it's a
		// custom one), color 1 of the costume, and the default stage. The selectors only offer what is installed (scanned once per game start).
		void DropRemovedCustom(Data& data) {
			const auto installed = custom::Installed();
			auto costume = [&](CharaPick& c) {
				if (selection::IsCustomCostume(c.costume) && !installed->HasCostume(c.charaID, c.costume)) c.costume = 0;
				if (selection::IsCustomColor(c.color) && !installed->HasColor(c.charaID, c.costume, c.color)) c.color = 0;
			};
			costume(data.lobby);
			for (auto& fighter : data.fighters) costume(fighter);
			if (selection::IsCustomStage(data.stageID) && !installed->HasStage(data.stageID))
				data.stageID = Data{}.stageID;
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
        DropRemovedCustom(data);
        ClampChara(data.lobby, data.lobbyEditionSelect);
        for (int id = 0; id < selection::FighterCount; ++id) {
            data.fighters[id].charaID = static_cast<uint8_t>(id);
            // Keep a remembered edition whatever the current rule; restoring a
            // fighter normalizes it against the room's rule at that time.
            ClampChara(data.fighters[id], true);
        }
        if (data.lobby.charaID < data.fighters.size()) data.fighters[data.lobby.charaID] = data.lobby;
        data.stageID = selection::NormalizeStageChoice(data.stageID);
        data.randomPool = selection::RandomPool(std::vector<std::int64_t>(data.randomPool.Excluded().begin(), data.randomPool.Excluded().end()));
        if (data.lobbyRoundCountIdx < 0 || data.lobbyRoundCountIdx >= ROUND_COUNT_OPTIONS) data.lobbyRoundCountIdx = 1;
        if (data.lobbyRoundTimeIdx < 0 || data.lobbyRoundTimeIdx >= ROUND_TIME_OPTIONS) data.lobbyRoundTimeIdx = 2;
    }

	void FromJson(const nlohmann::json& j, Data& out) {
		netplay::ReadBoolPreferences(j, out, boolPreferences);
		out.deviceIdentity.reset();
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
		if (j.contains("randomStageExcluded") && j["randomStageExcluded"].is_array()) {
			std::vector<std::int64_t> excluded;
			for (const auto& id : j["randomStageExcluded"]) if (id.is_number_integer()) excluded.push_back(id.get<std::int64_t>());
			out.randomPool = selection::RandomPool(excluded);
		}

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
			// Migration: a missing identity object means no saved controller.
			if (dev.contains("identity") && dev["identity"].is_object()) {
				const auto& identity = dev["identity"];
				out.deviceIdentity = gamepads::Identity{identity.value("guid", std::string{}),
					identity.value("serial", std::string{}), identity.value("path", std::string{}),
					identity.value("name", std::string{})};
			}
		}
	}

	nlohmann::json ToJson(const Data& data) {
		nlohmann::json j;
		netplay::WriteBoolPreferences(data, j, boolPreferences);
		CharaToJson(j["lobby"], data.lobby);
		j["fighters"] = nlohmann::json::array();
		for (const auto& fighter : data.fighters) {
			nlohmann::json row;
			CharaToJson(row, fighter);
			j["fighters"].push_back(std::move(row));
		}
		j["stageID"] = data.stageID;
		j["randomStageExcluded"] = nlohmann::json::array();
		for (int id : data.randomPool.Excluded()) j["randomStageExcluded"].push_back(id);

		j["lobbySettings"] = {
			{"roundCountIdx", data.lobbyRoundCountIdx},
			{"roundTimeIdx", data.lobbyRoundTimeIdx},
			{"editionSelect", data.lobbyEditionSelect},
		};

		j["device"] = {
			{"idx", data.deviceIdx},
			{"type", data.deviceType},
		};
        if (data.deviceIdentity) {
            const auto& identity = *data.deviceIdentity;
            j["device"]["identity"] = {{"guid", identity.guid}, {"serial", identity.serial},
                {"path", identity.path}, {"name", identity.name}};
        }
        return j;
    }

    bool Load(Data& out) {
        std::lock_guard<std::mutex> lock(cacheMutex);
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
			if (j.contains("diagnostics") && j["diagnostics"].is_object()) {
				const auto& diagnostics = j["diagnostics"];
				if (diagnostics.contains("heapCheckInterval")) {
					const auto& interval = diagnostics["heapCheckInterval"];
					if (interval.is_number_integer() && interval >= 0 && interval <= UINT_MAX)
						crash::ConfigureHeapCheck(interval.get<unsigned>());
					else spdlog::warn("HeapCheck: diagnostics.heapCheckInterval must be an unsigned integer");
				}
			}
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
        nlohmann::json values={{"displayName", preferences.displayName}, {"mainFighter",preferences.mainFighter}, {"inputDelay", preferences.inputDelay}, {"autoInputDelay", preferences.autoInputDelay},
			{"editionSelect", preferences.lobby.editionSelect ? 1 : 0}, {"roundCount", preferences.lobby.roundCount},
			{"roundTimeIntegral", preferences.lobby.roundTime},
            {"matchHudSize",preferences.matchHudSize},{"matchHudAnchor",preferences.matchHudAnchor},{"pictureSpot",preferences.pictureSpot},{"roomColumn",preferences.roomColumn},{"roomSplit",preferences.roomSplit},{"matchHudLayout",preferences.matchHudLayout},{"matchHudNameOffset",preferences.matchHudNameOffset},{"readySoundVolume",preferences.readySoundVolume},
            {"interfaceScale", preferences.interfaceScale}, {"roomDefaults", netplay::RoomPreferences(preferences)}};
        netplay::WriteBoolPreferences(preferences, values);
        netplay::WriteButtonPrompts(preferences, values);
        netplay::WriteCountryPreference(preferences, values);
        values["controllerMappings"] = preferences.controllerMappings;
        values["buttonLayouts"] = gamepads::LayoutsToJson(preferences.buttonLayouts);
        if(preferences.record.available)values["onlineRecord"]=netplay::ProfileRecordJson(preferences.record);
        return writer->QueueLauncher(std::move(values));
	}

    namespace {
        bool QueueOverlayLocked(Data data) {
            Clamp(data);
            if (!writer || !writer->QueueOverlay(ToJson(data))) return false;
            cached = std::move(data);
            haveCached = true;
            return true;
        }
    }

    bool Save(const Data& in) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        Data data = in;
        // The runtime owns device choice; a stale render snapshot cannot replace it.
        if (haveCached) {
            data.deviceType = cached.deviceType;
            data.deviceIdx = cached.deviceIdx;
            data.deviceIdentity = cached.deviceIdentity;
        }
        return QueueOverlayLocked(std::move(data));
    }

    bool SaveGameplayDevice(int type, int index, const std::optional<gamepads::Identity>& identity) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (cached.deviceType == type && cached.deviceIdx == index && cached.deviceIdentity == identity) return true;
        Data data = cached;
        data.deviceType = static_cast<uint8_t>(type);
        data.deviceIdx = static_cast<uint8_t>(index);
        data.deviceIdentity = identity;
        return QueueOverlayLocked(std::move(data));
    }

} // namespace OverlayPrefs
} // namespace sf4e
