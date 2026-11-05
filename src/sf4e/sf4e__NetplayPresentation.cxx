// What the player is shown about netplay: the current notice and the status
// sample the overlay draws. Both are owned by the game thread; the drawing
// thread reads only the PresentationSnapshot published at the end of each
// outer tick (sf4e__RuntimeBridge).
#include "sf4e__NetplayFacade.hxx"

#include <cstring>

#include <spdlog/spdlog.h>

#include "../common/Localization.hxx"
#include "../common/MatchNotice.hxx"
#include "sf4e__GameAdapter.hxx"
#include "sf4e__RuntimeBridge.hxx"
#include "sf4e__RuntimeSession.hxx"

using ember::RuntimeSession;

namespace sf4e {

	static MatchNoticeState s_notice;

	void NetplayFacade::PushAlert(const char* msg, NoticeSeverity severity) {
		if (!msg || !msg[0]) return;
		s_notice.Push(msg, severity, GetTickCount64());
		spdlog::info("Netplay notice ({}): {}", (int)severity, s_notice.Text());
	}

	void NetplayFacade::PushDesyncNotice(bool spectatorOnly) {
		PushAlert(loc::T(spectatorOnly ? "runtime.desync_spectator" : "runtime.desync_match_ended"),
			spectatorOnly ? NoticeSeverity::Warning : NoticeSeverity::Error);
	}

	void NetplayFacade::SetLastError(const char* msg) { PushAlert(msg, NoticeSeverity::Error); }

	void NetplayFacade::PushAlert(const char* msg) { PushAlert(msg, NoticeSeverity::Error); }

	void NetplayFacade::ClearMatchNotice() { s_notice.Clear(); }

	void NetplayFacade::ClearTransientMatchNotice() { s_notice.ClearTransient(); }

	NetplayStatus NetplayFacade::GetStatus() {
		NetplayStatus st;
		if (!s_notice.Empty()) {
			strncpy_s(st.lastError, s_notice.Text(), _TRUNCATE);
			st.lastErrorSeverity = s_notice.Severity();
		}
		const auto& battle = ember::game::Game().battle;
		const bool live = battle.Live();
		const auto link = live ? battle.Sample() : ember::game::LinkSample{};
		st.connectionWarning = link.connectionWarning;
		st.predictionStalled = link.predictionStalled;
		st.disconnectCountdownMs = link.disconnectCountdownMs;
		st.active = RuntimeSession::netplay != nullptr || RuntimeSession::server != nullptr;
		if (RuntimeSession::netplay) {
			st.connected = RuntimeSession::netplay->client.IsConnected();
			st.inLobby = st.connected && !live;
			st.inMatch = live;
			st.inputDelay = RuntimeSession::netplay->delay;
			for (int side = 0; side < 2; ++side) st.matchSides[side] = RuntimeSession::netplay->matchSides[side];
			// Spectators carry their table too, so everyone watching sees the same count.
			const auto& room = RuntimeSession::netplay->client.GetRoomSnapshot();
			std::uint32_t liveScore[2] = { 0, 0 };
			bool liveScoreKnown = false;
			for (const auto& m : room.members)
				if (m.id == room.localMember && m.table >= 0 && m.table < static_cast<int>(room.tables.size())) {
					liveScoreKnown = true;
					// The game that ended a set leaves the table at 0-0 for the next
					// one; its final score stays on screen until the next game begins.
					const auto& table = room.tables[m.table];
					st.spectators = static_cast<int>(table.spectators.size());
					const bool decided = table.lastSet.generation != 0 && table.lastSet.generation == table.matchGeneration;
					for (int side = 0; side < 2; ++side) liveScore[side] = decided ? table.lastSet.score[side] : table.score[side];
				}
			const auto place = room::PlaceOf(room, room.localMember);
			if (place.kind == room::Place::Kind::Queue) {
				st.queuePosition = place.position;
				st.queueLength = static_cast<int>(room.tables[place.table].queue.size());
			}
			st.hasMatchScore = HudScore(live && RuntimeSession::netplay->spectating, RuntimeSession::netplay->startScoreKnown,
				RuntimeSession::netplay->startScore, liveScoreKnown, liveScore, st.matchScore);
			st.rollbackFrames = battle.RecentRollbackFrames();

			for (const auto& m : RuntimeSession::netplay->client._lobbyData.members) {
				if (m.name != RuntimeSession::netplay->client._name) {
					strncpy_s(st.opponentName, m.name.c_str(), _TRUNCATE);
					break;
				}
			}

			st.pingMs = link.pingMs;
			st.appliedDelay = link.appliedDelay;
			st.ownDelays = link.ownDelays;
			st.spectator = link.spectator;
			st.spectatorBacklog = link.spectatorBacklog;
		}
		return st;
	}

	void NetplayFacade::PublishPresentationFrame() {
		static std::uint64_t sequence = 0;
		s_notice.Expire(GetTickCount64());
		auto next = std::make_shared<PresentationSnapshot>();
		next->runtime = bridge::LatestRuntime();
		next->netplay = GetStatus();
		const auto& battle = ember::game::Game().battle;
		next->ggpoSessionActive = battle.Live();
		next->localFighter = next->ggpoSessionActive && battle.Sample().localFighter;
		next->sequence = ++sequence;
		bridge::PublishPresentation(std::move(next));
	}

	std::shared_ptr<const NetplayFacade::PresentationSnapshot> NetplayFacade::GetPresentationSnapshotShared() {
		return bridge::LatestPresentation();
	}

} // namespace sf4e
