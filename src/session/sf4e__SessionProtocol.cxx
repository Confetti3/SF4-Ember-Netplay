#include "../Dimps/Dimps__GameEvents.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	namespace SessionProtocol {
		bool ConnectionID::operator==(const ConnectionID& rhs) const {
			return this->host == rhs.host && this->user == rhs.user;
		}

		bool LobbyID::operator==(const LobbyID& rhs) {
			if (this->host == "" || this->key == "") {
				return rhs.host == "" || rhs.key == "";
			}

			return this->host == rhs.host && this->key == rhs.key;
		}

		const LobbyID LobbyID::NULL_LOBBY_ID = { "", "" };
		const LobbyData LobbyData::NULL_LOBBY = {
			LobbyID::NULL_LOBBY_ID,
			false,
			0,
			{0, 0},
			false,
			{}
		};

		MatchData::MatchData()
		{
			Clear();
		}

		void MatchData::Clear() {
			readyMessageNum[0] = -1;
			readyMessageNum[1] = -1;
			stageID = -1;
			rngSeed = 0xffffffff;
			memset(chara, 0, sizeof(Dimps::GameEvents::VsMode::ConfirmedCharaConditions) * 2);
		}

		void MatchData::ClearReady() {
			readyMessageNum[0] = -1;
			readyMessageNum[1] = -1;
		}

		bool MatchData::IsAllReady() {
			return (
				readyMessageNum[0] != -1 &&
				readyMessageNum[1] != -1
			);
		}
	}
}
