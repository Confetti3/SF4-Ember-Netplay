// Random on the fighter grid (docs/design/RANDOM_FIGHTER.md): the overlay
// rolls the pick and notes it here every frame; the runtime shows the room
// host each roll as soon as the host takes it, and keeps a Random player's
// rematch out of the battle in place, which would replay the same fighters.
#include "sf4e__NetplayRuntime.hxx"
#include "../session/RandomFighterWatch.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
// How long a shown roll the room does not show yet waits before it goes again
// (the host drops it while the table's game is still being torn down).
constexpr std::uint64_t ShowAgainMs = 1000;

struct Noted {
	bool random = false;
	Dimps::GameEvents::VsMode::ConfirmedCharaConditions rolled = {};
};
// Written by the drawing thread, read by the game thread.
std::mutex notedMutex;
Noted noted;
std::uint64_t shownAtMs = 0;

Noted Take() {
	std::lock_guard<std::mutex> lock(notedMutex);
	return noted;
}
}

void NoteRandomFighter(bool random, const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& rolled) {
	std::lock_guard<std::mutex> lock(notedMutex);
	noted.random = random;
	noted.rolled = rolled;
}

namespace internal {
bool RandomFighterChosen() { return Take().random; }

void ShowRandomFighter() {
	if (!runtime || !runtime->attached || !UserApp::netplay) return;
	const auto pick = Take();
	auto& client = UserApp::netplay->client;
	// A Ready on its way carries the pick itself.
	if (!pick.random || runtime->readyIntent.Active() || client._outstandingReadyRequestNumber != -1) return;
	if (!room::RandomRollToShow(client.GetRoomSnapshot(), pick.rolled.charaID)) return;
	const auto now = GetTickCount64();
	if (shownAtMs && now - shownAtMs < ShowAgainMs) return;
	shownAtMs = now;
	if (client.PreBattle_SetChara(pick.rolled) == session::SendResult::Queued)
		spdlog::info("Room: shows the Random roll, fighter {}", pick.rolled.charaID);
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
