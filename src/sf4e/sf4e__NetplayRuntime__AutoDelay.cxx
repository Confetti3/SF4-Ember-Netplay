#include "sf4e__NetplayRuntime.hxx"

// Auto input delay: following the seated opponent, their connection checks and
// the games just played, and the delay a Ready sends.
namespace sf4e { namespace NetplayFacade {
namespace internal {
// The seated opponent a connection check is made against, with the table's
// revision and index; empty when this PC has no opponent there.
std::string CurrentProbePeer(std::uint64_t& revision, int* tableIndex) {
    revision=0;
    if (tableIndex) *tableIndex=-1;
    if (!runtime || !runtime->attached || !RuntimeSession::netplay || !RuntimeSession::server) return {};
    const auto& snapshot=RuntimeSession::netplay->client.GetRoomSnapshot();
    const auto local=std::find_if(snapshot.members.begin(),snapshot.members.end(),[&](const room::Member& member){return member.id==snapshot.localMember;});
    if (local==snapshot.members.end() || local->table<0 || local->table>=room::TableCount || local->seat<0 || local->seat>1) return {};
    const auto& table=snapshot.tables[local->table];
    const auto opponent=local->seat==0 ? table.p2 : table.p1;
    const auto peer=RuntimeSession::server->roomPeerIdentities.find(opponent);
    if (peer==RuntimeSession::server->roomPeerIdentities.end()) return {};
    revision=table.revision;
    if (tableIndex) *tableIndex=local->table;
    return peer->second;
}

bool AutoDelayMeasured() {
    std::uint64_t revision=0;
    return runtime->autoDelayCheck.Measured(CurrentProbePeer(revision));
}

int ReadyDelay() {
    if(!runtime->preferences.autoInputDelay) return runtime->selectedDelay;
    std::uint64_t revision=0;
    return runtime->autoDelayCheck.Delay(CurrentProbePeer(revision),static_cast<AutoDelayStyle>(runtime->preferences.autoDelayStyle));
}

// A Ready sent mid-check would start the match's own link beside the
// check's, so it holds until the check ends, and for a check still to come,
// but never past the check's hold (AutoDelayCheck::HoldMs).
bool AutoDelayMeasuring() {
    if(!runtime->preferences.autoInputDelay || !runtime->room) return false;
    return runtime->autoDelayCheck.Holding(GetTickCount64());
}

// Whether a check against `peer` can be reserved now. The helper refuses a
// reservation unless its own copy of the room seats this pair at exactly the
// table revision the check names, and it ends a check whose opponent already
// has a game link to this PC: a queued opponent who watched the last game
// keeps one until that game's spectators are released.
static bool AutoCheckClear(const std::string& peer, int tableIndex) {
    // No mapping reads as Closed, as the copying Game() does.
    const auto* game=runtime->room->FindGame(peer);
    if(game && game->state!=session::IrohRoom::GameState::Closed) return false;
    const auto* committed=RuntimeSession::server->RoomSnapshot();
    if(!committed || tableIndex<0 || tableIndex>=static_cast<int>(room::TableCount)) return true;
    return room::ProbePairCommitted(RuntimeSession::netplay->client.GetRoomSnapshot().tables[tableIndex],committed->tables[tableIndex]);
}

// Asks for Auto's check. It takes the controller's check of a press of Check
// connection, but a refusal stays in the log: the player did not press it.
static bool RequestAutoCheck(const std::string& peer, std::uint64_t revision) {
    const netplay::Command command{netplay::CommandKind::CheckConnection,runtime->controller.GetSnapshot().generation,{}};
    if(!runtime->controller.Execute(command).accepted) return false;
    const auto request=runtime->nextProbeRequest++;
    const bool sent=runtime->room->RequestProbe(peer,request,revision,false);
    spdlog::info("Auto delay: connection check {} request={} revision={}",sent?"requested":"refused",request,revision);
    return true;
}

// The game that just ended, taken once and logged once per game: old -> new
// delay Auto readies with. It stays local to this PC, like the check's: the
// room commits the higher of the two seats' Ready delays. The game names its
// own opponent, so it cannot be credited to whoever sits down after it.
static void TakeFinishedGame() {
    if(!runtime->finishedGame) return;
    const auto game=*runtime->finishedGame;
    runtime->finishedGame.reset();
    auto& check=runtime->autoDelayCheck;
    const int before=check.Delay(game.opponent);
    if(check.Played(runtime->preferences.autoInputDelay,game))
        spdlog::info("Auto delay: game {} ping median={} ms p95={} ms samples={}: delay {} -> {}",
            game.generation,game.ping.medianMs,game.ping.p95Ms,game.ping.samples,before,check.Delay(game.opponent));
    else
        spdlog::info("Auto delay: game {} ping median={} ms p95={} ms samples={}: delay kept",
            game.generation,game.ping.medianMs,game.ping.p95Ms,game.ping.samples);
}

// Follows the seated opponent and their check results. With Auto on, a due
// check is asked for once the table has settled and this PC holds the pair;
// one that ends without a recommendation is asked for once more.
void TickAutoDelay(bool helperReady) {
    std::uint64_t revision=0; int tableIndex=-1;
    // A room without coordination cannot reserve a check, so Auto readies at
    // once with two frames there.
    const bool coordinated=runtime->room && runtime->room->Coordination().active;
    const auto peer=coordinated ? CurrentProbePeer(revision,&tableIndex) : std::string();
    const auto now=GetTickCount64();
    auto& check=runtime->autoDelayCheck;
    TakeFinishedGame();
    check.Seat(peer,revision,runtime->nextProbeRequest,now);
    if(peer.empty()) return;
    const auto& probe=runtime->room->Probe();
    check.Observe(probe.peer,probe.request,probe.status,probe.recommended,now);
    if(!helperReady || !runtime->preferences.autoInputDelay || !check.Due(now) || probe.status=="checking" ||
        !CurrentGates().canProbe || !AutoCheckClear(peer,tableIndex)) return;
    const auto request=runtime->nextProbeRequest;
    if(RequestAutoCheck(peer,revision)) check.Asked(request);
}

} // namespace internal
} } // namespace sf4e::NetplayFacade
