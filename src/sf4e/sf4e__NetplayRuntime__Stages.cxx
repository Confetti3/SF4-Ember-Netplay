// Stages a seated player lacks (docs/design: SFV Ember's EMBER_LIBRARY.md
// section 38). A game whose installs can differ says which stages this copy
// cannot load (GameState::MissingStages); each member carries them into the
// room, and the session owner's start gate refuses a game on a stage either
// fighter lacks (SessionServer::StartReadyTables). This file is the player's
// side of it: the one set of stages this player may not choose now, the Ready
// check and Random that read it, and what the room's StageNotShared tells
// each member. Where no copy lacks a stage, as for SF4, the set is empty and
// nothing here refuses anything.
#include "sf4e__NetplayRuntime.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {

std::vector<std::int64_t> UnavailableStagesNow() {
	std::vector<std::int64_t> own = Game().state.MissingStages();
	if (!runtime->attached || !RuntimeSession::netplay) return own;
	return room::UnavailableStagesFor(RuntimeSession::netplay->client.GetRoomSnapshot(), own);
}

int StageToSend(const commands::Room& command, std::uint32_t roll) {
	return ember::Catalog().resolveStage(command.stage, roll, command.randomPool, InstalledContent().get(), UnavailableStagesNow());
}

const char* StageReadyRefusal(const commands::Room& command) {
	const int sent = StageToSend(command, 0);
	if (!ember::Catalog().validStage(sent)) return loc::T("runtime.ready.stage_unavailable");
	const auto unavailable = UnavailableStagesNow();
	if (std::find(unavailable.begin(), unavailable.end(), sent) != unavailable.end()) return loc::T("runtime.ready.stage_not_shared");
	return nullptr;
}

void ObserveStageNotShared(const room::Event& event) {
	if (!runtime->attached || !RuntimeSession::netplay) return;
	const auto& snapshot = RuntimeSession::netplay->client.GetRoomSnapshot();
	const auto local = snapshot.localMember;
	const auto& table = snapshot.tables[event.table];
	spdlog::info("Room: stage not shared table={} generation={} member={}", event.table, event.matchGeneration, event.member);
	if (event.matchGeneration == 0) {
		// The start took P1's Ready back: P1 chooses again, P2 waits for it.
		if (event.member == local) FailReady(loc::T("runtime.ready.stage_not_shared"));
		else if (local && local == table.p2) PushAlert(loc::T("room.stage_not_shared_wait"), NoticeSeverity::Info);
	} else if (event.member == local) {
		PushAlert(loc::T("runtime.spectator_stage_missing"), NoticeSeverity::Info);
	}
}

const char* MatchEntryRefusal() {
	const auto missing = Game().state.MissingStages();
	const std::int64_t stage = RuntimeSession::netplay ? RuntimeSession::netplay->client._matchData.stageID : -1;
	if (std::find(missing.begin(), missing.end(), stage) == missing.end()) return loc::T("runtime.unsupported_match");
	return loc::T(LocalIsSpectator() ? "runtime.spectator_stage_missing" : "runtime.stage_not_installed");
}

} // namespace internal
} } // namespace sf4e::NetplayFacade
