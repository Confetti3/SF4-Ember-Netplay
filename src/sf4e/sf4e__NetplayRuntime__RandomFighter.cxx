// Random on the fighter grid (docs/design/RANDOM_FIGHTER.md). The runtime owns
// the next game's fighter (room::RandomFighterPick): it rolls on the game
// thread, every Ready takes its fighter from it as it is dispatched, the room
// host is shown each roll as soon as it takes it, and a Random player's
// rematch leaves the battle, which would replay the same fighters in place.
// The overlay notes the player's pick and shows the rolled fighter.
#include "sf4e__NetplayRuntime.hxx"
#include "sf4e__CustomContent.hxx"
#include "../session/RandomFighterPick.hxx"
#include <random>

namespace sf4e { namespace NetplayFacade {
namespace {
using Chara = Dimps::GameEvents::VsMode::ConfirmedCharaConditions;
// How long a shown roll the room does not show yet waits before it goes again
// (the host drops it while the table's game is still being torn down).
constexpr std::uint64_t ShowAgainMs = 1000;

struct Noted {
	bool random = false, played = false;
	Chara lobby = {};
	std::vector<Chara> remembered = std::vector<Chara>(ember::Catalog().fighterCount);
};
// Written by the drawing thread, read by the game thread.
std::mutex notedMutex;
Noted noted;
// The game thread's own.
room::RandomFighterPick pick;
std::mt19937 rolls{std::random_device{}()};
std::uint64_t shownAtMs = 0;
std::function<int()> rollForTest;

Noted Take() {
	std::lock_guard<std::mutex> lock(notedMutex);
	Noted taken = noted;
	noted.played = false;
	return taken;
}

// The pick a Ready made with `pressed` sends: the rolled fighter, in the look
// pressed with or else remembered for it.
Chara Sent(const Noted& from, const Chara& pressed) {
	const int fighter = pick.Fighter();
	if (fighter < 0) return pressed;
	const auto availability = Dimps::Selection::ReadAvailability(fighter);
	Chara sent = pressed;
	selection::ToNative(pick.Sent(selection::FromNative(pressed), selection::FromNative(from.remembered[fighter]),
		CurrentGates().editionSelect, &availability, custom::Installed().get()), sent);
	return sent;
}
}

void NoteRandomFighter(bool random, const Chara& lobby, const std::vector<Chara>& remembered) {
	std::lock_guard<std::mutex> lock(notedMutex);
	noted.random = random; noted.lobby = lobby; noted.remembered = remembered;
}

void NoteRandomFighterPlayed() {
	std::lock_guard<std::mutex> lock(notedMutex);
	noted.played = true;
}

namespace internal {
bool RandomFighterChosen() {
	std::lock_guard<std::mutex> lock(notedMutex);
	return noted.random;
}

RandomFighterRoll CurrentRandomFighter() { return {pick.Fighter(), pick.Serial()}; }

void TickRandomFighter() {
	if (!runtime) return;
	const auto from = Take();
	if (from.played) pick.Played();
	static const room::Snapshot noRoom;
	const bool inRoom = runtime->attached && UserApp::netplay;
	const auto& room = inRoom ? UserApp::netplay->client.GetRoomSnapshot() : noRoom;
	pick.Update(from.random, room, [] {
		if (rollForTest) return rollForTest();
		const bool editionSelect = CurrentGates().editionSelect;
		return selection::DrawFighter(selection::RandomCandidates(
			[](int id) { return Dimps::Selection::ReadAvailability(id); }, editionSelect),
			[] { return static_cast<std::uint32_t>(rolls()); });
	});
	if (!inRoom || !from.random) return;
	// A Ready on its way carries the roll itself.
	auto& client = UserApp::netplay->client;
	if (runtime->readyIntent.Active() || client._outstandingReadyRequestNumber != -1 ||
		!room::RandomRollToShow(room, pick.Fighter())) return;
	const auto now = GetTickCount64();
	if (shownAtMs && now - shownAtMs < ShowAgainMs) return;
	shownAtMs = now;
	if (client.PreBattle_SetChara(Sent(from, from.lobby)) == session::SendResult::Queued)
		spdlog::info("Room: shows the Random roll, fighter {}", pick.Fighter());
}

void SetRandomFighterRollForTest(std::function<int()> roll) { rollForTest = std::move(roll); }

TypedCommand& WithRandomFighter(TypedCommand& command) {
	auto* ready = std::get_if<commands::Room>(&command);
	if (!ready || (ready->command.kind != netplay::CommandKind::Ready && ready->command.kind != netplay::CommandKind::Rematch))
		return command;
	Noted from;
	{ std::lock_guard<std::mutex> lock(notedMutex); from = noted; }
	if (from.random) ready->character = Sent(from, ready->character);
	return command;
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
