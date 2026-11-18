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

int RecommendedDelay(const session::IrohRoom::ProbeSnapshot& probe) {
    return probe.measured ? RecommendedInputDelay(Game().delayBands,probe.Rtt()) : -1;
}

static AutoDelayStyle PlayerAutoStyle() { return static_cast<AutoDelayStyle>(runtime->preferences.autoDelayStyle); }

int ReadyDelay() {
    if(!runtime->preferences.autoInputDelay) return runtime->selectedDelay;
    std::uint64_t revision=0;
    return runtime->autoDelayCheck.Delay(CurrentProbePeer(revision),PlayerAutoStyle());
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

static const char* AutoStyleName(AutoDelayStyle style) {
    return style==AutoDelayStyle::Responsive ? "Responsive" : style==AutoDelayStyle::Smooth ? "Smooth" : "Balanced";
}

// Why this PC readied with its delay against `opponent`: a chosen delay, or
// Auto's, with what it stood on and the game's bands.
static std::string ReadyDelayReason(const std::string& opponent) {
    if(!runtime->preferences.autoInputDelay) return "a chosen delay, not Auto";
    const auto& check=runtime->autoDelayCheck;
    const auto basis=check.Basis(opponent);
    const auto style=PlayerAutoStyle();
    if(basis.source==netplay::AutoDelayBasis::Source::None)
        return fmt::format("Auto {} {} frames, nothing measured",AutoStyleName(style),check.Delay(opponent,style));
    const auto& bands=Game().delayBands;
    return fmt::format("Auto {} {} frames from {} median={} ms p95={} ms, recommended {} (bands: 1 frame to {} ms, 1 more per {} ms)",
        AutoStyleName(style),check.Delay(opponent,style),
        basis.source==netplay::AutoDelayBasis::Source::Check ? "the connection check's" : "the last game's ping",
        basis.rtt.medianMs,basis.rtt.p95Ms,check.Recommended(opponent),bands.firstBandMs,bands.bandStepMs);
}

// Once per game, as its session starts, for a fighter: the delay the game
// plays at, what each seat readied with, and why this one did, so a triage
// reads the round trip and the choice from the log instead of from the
// rollbacks.
void LogMatchDelay(std::size_t localSlot) {
    if(!runtime || !runtime->match || !RuntimeSession::netplay || localSlot>1) return;
    const auto& seats=RuntimeSession::netplay->client._matchData.inputDelay;
    const bool own=RuntimeSession::netplay->client.GetRoomSnapshot().delayPolicy==room::DelayPolicy::Own;
    const int mine=seats[localSlot], theirs=seats[1-localSlot];
    spdlog::info("Match delay: game {} plays at {} frames ({} delays; this seat readied {}, the other {}); {}",
        runtime->match->Generation(),own ? mine : room::MatchDelay(mine,theirs),own ? "own" : "shared",mine,theirs,
        ReadyDelayReason(runtime->match->OpponentPeer()));
}

// The game that just ended, taken once and logged once per game: its ping, its
// rollbacks, and the delay Auto readies with next, old -> new. It stays local
// to this PC, like the check's: the room commits the higher of the two seats'
// Ready delays. The game names its own opponent, so it cannot be credited to
// whoever sits down after it.
static void TakeFinishedGame() {
    if(!runtime->finishedGame) return;
    const auto game=*runtime->finishedGame;
    runtime->finishedGame.reset();
    auto& check=runtime->autoDelayCheck;
    const auto style=PlayerAutoStyle();
    const int before=check.Delay(game.opponent,style);
    const bool followed=check.Played(runtime->preferences.autoInputDelay,game);
    const auto& rollbacks=game.rollbacks;
    spdlog::info("Match end: game {} ping median={} ms p95={} ms samples={}; rollbacks={} mean depth={:.2f} deepest={} frames; "
        "Auto delay {} -> {} ({})",
        game.generation,game.ping.medianMs,game.ping.p95Ms,game.ping.samples,rollbacks.rollbacks,
        rollbacks.rollbacks ? static_cast<double>(rollbacks.frames)/rollbacks.rollbacks : 0.0,rollbacks.deepest,
        before,check.Delay(game.opponent,style),followed ? "follows this game's ping" : "kept");
}

// A connection check result Auto took, once: the measured path and what it
// recommends in this game's bands.
static void LogCheckTaken(const session::IrohRoom::ProbeSnapshot& probe) {
    const auto rtt=probe.Rtt();
    spdlog::info("Auto delay: connection check request={} route={} median={} ms p95={} ms p99={} ms jitter={} ms replies={}/{}: "
        "recommends {} frames",
        probe.request,probe.route,rtt.medianMs,rtt.p95Ms,RoundTripMs(probe.p99RttUs),RoundTripMs(probe.jitterUs),
        probe.samples,probe.samples+probe.lost,RecommendedDelay(probe));
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
    using Source=netplay::AutoDelayBasis::Source;
    if(check.Observe(probe.peer,probe.request,probe.status,{probe.measured ? Source::Check : Source::None,probe.Rtt()},now))
        LogCheckTaken(probe);
    if(!helperReady || !runtime->preferences.autoInputDelay || !check.Due(now) || probe.status=="checking" ||
        !CurrentGates().canProbe || !AutoCheckClear(peer,tableIndex)) return;
    const auto request=runtime->nextProbeRequest;
    if(RequestAutoCheck(peer,revision)) check.Asked(request);
}

} // namespace internal
} } // namespace sf4e::NetplayFacade
