// What the player is shown about netplay: the current notice and the status
// sample the overlay draws. Both are owned by the game thread; the drawing
// thread reads only the PresentationSnapshot published at the end of each
// outer tick (sf4e__RuntimeBridge).
#include "sf4e__NetplayFacade.hxx"

#include <cstring>

#include <spdlog/spdlog.h>

#include "../common/Localization.hxx"
#include "../common/MatchNotice.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__RuntimeBridge.hxx"
#include "sf4e__UserApp.hxx"

using fSystem = sf4e::Game::Battle::System;
using fUserApp = sf4e::UserApp;

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
		if (fSystem::ggpo) {
			st.connectionWarning = fSystem::simGate.connectionWarningActive;
			st.predictionStalled = fSystem::simGate.predictionStalled;
			st.disconnectCountdownMs = fSystem::DisconnectCountdownMs();
		}
		st.active = fUserApp::netplay != nullptr || fUserApp::server != nullptr;
		if (fUserApp::netplay) {
			st.connected = fUserApp::netplay->client.IsConnected();
			st.inLobby = st.connected && fSystem::ggpo == nullptr;
			st.inMatch = fSystem::ggpo != nullptr;
			st.inputDelay = fUserApp::netplay->delay;
			for (int side = 0; side < 2; ++side) st.matchSides[side] = fUserApp::netplay->matchSides[side];
			// Spectators carry their table too, so everyone watching sees the same count.
			const auto& room = fUserApp::netplay->client.GetRoomSnapshot();
			std::uint32_t liveScore[2] = { 0, 0 };
			bool liveScoreKnown = false;
			for (const auto& m : room.members)
				if (m.id == room.localMember && m.table >= 0 && m.table < static_cast<int>(room.tables.size())) {
					liveScoreKnown = true;
					// The game that ended a set leaves the table at 0-0 for the next
					// one; its fighters keep seeing the final score until it is gone.
					const auto& table = room.tables[m.table];
					const bool decided = table.lastSet.generation != 0 && table.lastSet.generation == fUserApp::netplay->startGeneration;
					for (int side = 0; side < 2; ++side) liveScore[side] = decided ? table.lastSet.score[side] : table.score[side];
				}
			st.hasMatchScore = HudScore(fSystem::ggpo && fUserApp::netplay->spectating, fUserApp::netplay->startScoreKnown,
				fUserApp::netplay->startScore, liveScoreKnown, liveScore, st.matchScore);
			st.rollbackFrames = fSystem::RecentRollbackFrames();

			for (const auto& m : fUserApp::netplay->client._lobbyData.members) {
				if (m.name != fUserApp::netplay->client._name) {
					strncpy_s(st.opponentName, m.name.c_str(), _TRUNCATE);
					break;
				}
			}

			if (fSystem::ggpo) {
				st.pingMs = fSystem::matchTelemetry.Ping(GetTickCount64());
				st.appliedDelay = fSystem::matchTelemetry.appliedDelay;
				st.spectator = fSystem::matchTelemetry.spectator;
			}
		}
		return st;
	}

	void NetplayFacade::PublishPresentationFrame() {
		static std::uint64_t sequence = 0;
		s_notice.Expire(GetTickCount64());
		auto next = std::make_shared<PresentationSnapshot>();
		next->runtime = bridge::LatestRuntime();
		next->netplay = GetStatus();
		next->ggpoSessionActive = fSystem::ggpo != nullptr;
		next->sequence = ++sequence;
		bridge::PublishPresentation(std::move(next));
	}

	std::shared_ptr<const NetplayFacade::PresentationSnapshot> NetplayFacade::GetPresentationSnapshotShared() {
		return bridge::LatestPresentation();
	}

} // namespace sf4e
