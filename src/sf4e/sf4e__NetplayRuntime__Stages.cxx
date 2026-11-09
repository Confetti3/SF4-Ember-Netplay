// Stages a seated player lacks (docs/design: SFV Ember's EMBER_LIBRARY.md
// section 38). A game whose installs can differ says which stages this copy
// cannot load (GameState::MissingStages); each member carries them into the
// room, and the session owner's start refuses a game on a stage either
// fighter lacks (RoomAuthority::BeginMatch). This file is the player's side
// of it: the one set of stages this player may not choose now, the Ready
// check and Random that read it, what the room's StageNotShared and
// WatcherLacksStage tell each member, and the notice for a match the game
// refuses at entry. Where no copy lacks a stage, as for SF4, the set is empty
// and nothing here refuses anything.
#include "sf4e__NetplayRuntime.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {

// UnavailableStagesNow from this copy's own missing stages.
static std::vector<std::int64_t> UnavailableWith(const std::vector<std::int64_t>& own) {
	if (!runtime->attached || !RuntimeSession::netplay) return own;
	return room::UnavailableStagesFor(RuntimeSession::netplay->client.GetRoomSnapshot(), own);
}

// The catalog's stage for the command's choice, Random rolled with `roll`
// over the stages `unavailable` leaves.
static int ResolveChoice(const commands::Room& command, std::uint32_t roll, const std::vector<std::int64_t>& unavailable) {
	return ember::Catalog().resolveStage(command.stage, roll, command.randomPool, InstalledContent().get(), unavailable);
}

std::vector<std::int64_t> UnavailableStagesNow() {
	return UnavailableWith(Game().state.MissingStages());
}

int StageToSend(const commands::Room& command, std::uint32_t roll) {
	return ResolveChoice(command, roll, UnavailableStagesNow());
}

const char* StageReadyRefusal(const commands::Room& command) {
	const auto own = Game().state.MissingStages();
	const auto unavailable = UnavailableWith(own);
	const int sent = ResolveChoice(command, 0, unavailable);
	if (!ember::Catalog().validStage(sent) || std::binary_search(own.begin(), own.end(), sent))
		return loc::T("runtime.ready.stage_unavailable");
	if (std::binary_search(unavailable.begin(), unavailable.end(), sent)) return loc::T("runtime.ready.stage_not_shared");
	return nullptr;
}

void ObserveStageNotShared(const room::Event& event) {
	if (!runtime->attached || !RuntimeSession::netplay) return;
	const auto& snapshot = RuntimeSession::netplay->client.GetRoomSnapshot();
	const auto local = snapshot.localMember;
	spdlog::info("Room: stage refused at the start table={} p1={}", event.table, event.member);
	// P1 chooses again; P2 waits for it.
	if (local && event.member == local) FailReady(loc::T("runtime.ready.stage_not_shared"));
	else if (local && local == snapshot.tables[event.table].p2) PushAlert(loc::T("room.stage_not_shared_wait"), NoticeSeverity::Info);
}

void ObserveWatcherLacksStage(const room::Event& event) {
	if (!runtime->attached || !RuntimeSession::netplay) return;
	if (!event.member || event.member != RuntimeSession::netplay->client.GetRoomSnapshot().localMember) return;
	spdlog::info("Room: sitting out generation {} at table {}: its stage is not installed here", event.matchGeneration, event.table);
	PushAlert(loc::T("runtime.spectator_stage_missing"), NoticeSeverity::Info);
}

} // namespace internal

const char* EntryRefusalNotice(game::EntryRefusal refusal) {
	switch (refusal) {
	case game::EntryRefusal::UnsupportedStage: return loc::T("runtime.unsupported_stage");
	case game::EntryRefusal::StageNotInstalled:
		return loc::T(LocalIsSpectator() ? "runtime.spectator_stage_missing" : "runtime.stage_not_installed");
	case game::EntryRefusal::UnsupportedMatch: break;
	}
	return loc::T("runtime.unsupported_match");
}

} } // namespace sf4e::NetplayFacade
