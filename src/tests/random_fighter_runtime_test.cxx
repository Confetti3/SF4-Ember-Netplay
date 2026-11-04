// A Random pick's Rematch through the netplay runtime's real dispatch: the
// Rematch parks with the fighter just played, the room's settled result comes
// in, the game's teardown and terminal acknowledgement complete, the parked
// command is retried, and the character it sends is the roll made once the
// result was in (docs/design/RANDOM_FIGHTER.md).
#include "runtime_room_fixture.hxx"
#include "../session/RoomModel.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include <iostream>
#include <vector>

using namespace sf4e;
using nlohmann::json;
namespace facade = sf4e::NetplayFacade;
namespace internal = sf4e::NetplayFacade::internal;
using Chara = Dimps::GameEvents::VsMode::ConfirmedCharaConditions;
using test::RuntimeRoom;

namespace {
// Every fighter owned, with every costume, color and personal action.
selection::Availability Everything(int fighter) {
    selection::Availability availability; availability.ready = true; availability.personalActions = 0x3ff;
    for (int costume = 0; costume < selection::CostumeCount(fighter); ++costume) {
        availability.costumes |= 1u << costume;
        availability.colors[costume] = (1u << selection::ColorCount(fighter, costume)) - 1;
    }
    return availability;
}
Chara Pressed(int fighter, int costume) {
    selection::Pick pick; pick.fighter = fighter; pick.costume = costume;
    Chara chara{}; selection::ToNative(pick, chara);
    return chara;
}

// This PC (member 2) in table 0's second seat of an Arcade room whose winners
// keep their fighters, first to 2, member 1 opposite. The room shows this PC
// on `shown`.
room::Snapshot Room(std::uint64_t revision, int shown) {
    room::Snapshot snapshot;
    snapshot.roomEpoch = 5; snapshot.revision = revision; snapshot.localMember = 2; snapshot.arcade = room::ArcadeRoom;
    snapshot.name = "Random room";
    for (std::size_t i = 0; i < snapshot.tables.size(); ++i) snapshot.tables[i].id = static_cast<std::uint8_t>(i);
    room::Member opponent, local;
    opponent.id = 1; opponent.name = "Opponent"; opponent.host = true; opponent.table = 0; opponent.seat = 0; opponent.fighter = 9;
    local.id = 2; local.name = "Random"; local.table = 0; local.seat = 1; local.fighter = shown;
    snapshot.members = {opponent, local};
    auto& table = snapshot.tables[0];
    table.p1 = 1; table.p2 = 2; table.phase = room::TablePhase::Waiting;
    table.rules.winnerKeepsFighter = true; table.rules.format = room::SetFormat::Ft2;
    return snapshot;
}

// The fighter each pre-battle character message this PC sent names, in order.
std::vector<int> SentFighters(const RuntimeRoom& r) {
    std::vector<int> fighters;
    for (const auto& message : r.transport->sent)
        if (message.value("type", json()) == json(SessionProtocol::MT_PREBATTLE_SETCHARA))
            fighters.push_back(message.at("chara").get<Chara>().charaID);
    return fighters;
}

// One Rematch: game 31 at 1-0 to the opponent, its result settled as
// `score`/`keeper`. Returns the fighter the retried Rematch sent.
int RematchSends(const char* name, std::uint32_t mine, std::uint32_t theirs, room::MemberId keeper) {
    RuntimeRoom r;
    auto& runtime = *r.runtime;
    std::vector<int> rolls{5, 12};
    std::size_t made = 0;
    internal::SetRandomFighterRollForTest([&] { return rolls[made++ % rolls.size()]; });
    std::vector<Chara> remembered(ember::Catalog().fighterCount);
    for (std::size_t id = 0; id < remembered.size(); ++id) remembered[id] = Pressed(static_cast<int>(id), 2);
    // The overlay's pick: Random, showing the first roll in costume 1.
    facade::NoteRandomFighter(true, Pressed(5, 1), remembered);
    runtime.input.Adopt([] { input::Device pad; pad.type = 0; pad.index = 0; pad.connected = true; return pad; }());
    r.Projection(30, 1);
    std::uint64_t revision = 1;
    r.Snapshot(Room(revision++, 5));
    internal::TickRandomFighter();
    CHECK(made == 1);
    // Game 31, the opponent a game up.
    auto playing = Room(revision++, 5);
    playing.tables[0].phase = room::TablePhase::Playing; playing.tables[0].matchGeneration = 31; playing.tables[0].score[0] = 1;
    r.Snapshot(playing);
    internal::TickRandomFighter();
    // Rematch over the held WIN: parked with the fighter just played.
    internal::commands::Room rematch;
    rematch.command = {netplay::CommandKind::Rematch, runtime.controller.GetSnapshot().generation, {}};
    rematch.character = Pressed(5, 1);
    runtime.readyIntent.Defer(internal::TypedCommand(rematch), rematch.command.generation, GetTickCount64(), internal::Intent::Budget::Timed);
    CHECK(runtime.readyIntent.Parked());
    // The room's settled result, its teardown not yet acknowledged.
    auto settled = Room(revision++, 5);
    settled.tables[0].matchGeneration = 31; settled.tables[0].score[0] = theirs; settled.tables[0].score[1] = mine;
    settled.tables[0].keeper = keeper; settled.tables[0].keptFighter = keeper == 2 ? 5 : keeper ? 9 : -1;
    settled.tables[0].keptEdition = keeper ? selection::UltraEdition : -1;
    settled.localTerminalPending = true; settled.terminalPending[0] = true;
    r.Snapshot(settled);
    internal::TickRandomFighter();
    internal::SetAtMainMenuForTest(true);
    internal::ResolvePendingIntents(true);
    CHECK_MSG(SentFighters(r).empty(), "%s: sent before the game's teardown was acknowledged", name);
    // Teardown and the terminal acknowledgement done: the parked Rematch goes.
    settled.revision = revision++; settled.localTerminalPending = false; settled.terminalPending[0] = false;
    r.Snapshot(settled);
    internal::TickRandomFighter();
    internal::ResolvePendingIntents(true);
    internal::SetAtMainMenuForTest(std::nullopt);
    internal::SetRandomFighterRollForTest({});
    facade::NoteRandomFighter(false, Pressed(0, 0), remembered);
    internal::TickRandomFighter();
    const auto sent = SentFighters(r);
    CHECK_MSG(sent.size() == 1, "%s: %zu characters sent", name, sent.size());
    std::cout << name << ": the retried Rematch sent fighter " << sent.back() << "\n";
    return sent.back();
}
}

int main() {
    Dimps::Selection::SetAvailabilityForTest(Everything);
    // 1-0 down, this player wins: 1-1, nobody keeps a fighter: the new roll.
    CHECK(RematchSends("trailing win", 1, 1, 0) == 12);
    // A draw with the opponent at set point: still 1-0, nobody keeps: the new roll.
    CHECK(RematchSends("draw at set point", 0, 1, 0) == 12);
    // This player wins the set and keeps the fighter they won with.
    CHECK(RematchSends("set won, fighter kept", 0, 0, 2) == 5);
    Dimps::Selection::SetAvailabilityForTest({});
    std::cout << "Random fighter runtime tests passed\n";
    return 0;
}
