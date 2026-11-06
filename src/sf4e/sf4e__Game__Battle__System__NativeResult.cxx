// sf4e::Game::Battle::System: the native match result timeline and its
// confirmed publication. Split from sf4e__Game__Battle__System.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"

#include <optional>

// Native result state is captured in GGPO saves, including resimulation. The
// history is rewound to each restored state; the emitted latch is deliberately
// match state and survives a rollback so a
// result cannot be submitted twice after re-simulation.
sf4e::native_result::Timeline s_nativeResultTimeline;
bool s_nativeResultEmitted = false;

// A result an in-place rematch may follow waits over the WIN: the room hears
// it as a game of the running match once the next game's fight is confirmed,
// or as the match's end when the battle closes instead.
struct HeldResult { sf4e::room::MatchResult result; std::uint64_t captureFrame, confirmedFrame; };
static std::optional<HeldResult> s_heldResult;
static int s_resultFrame = -1;  // save frame of the last result published
static unsigned s_gamesReported = 0;  // games of this battle the room heard
// The games of this battle after its first, which the game does not save.
sf4e::replay::InPlaceReplays s_inPlaceReplays;
static sf4e::room::MatchResult s_gameResult = sf4e::room::MatchResult::Abort;  // the WIN's (GameResult)
// A spectator saves no states (it plays confirmed frames only), so the timeline
// stays empty there: the result its stream last showed is read frame by frame.
static sf4e::native_result::Result s_watchedResult = sf4e::native_result::Result::None;

void ResetNativeResultMatch() {
    s_nativeResultTimeline.Reset();
    s_nativeResultEmitted = false;
    s_heldResult.reset();
    s_resultFrame = -1;
    s_gamesReported = 0;
    s_inPlaceReplays.Reset();
    s_gameResult = sf4e::room::MatchResult::Abort;
    s_watchedResult = sf4e::native_result::Result::None;
}

bool fSystem::ResultHeld() { return s_heldResult.has_value(); }
unsigned fSystem::GamesPlayedInPlace() { return s_gamesReported; }
sf4e::room::MatchResult fSystem::GameResult() { return s_gameResult; }

static void ReportNativeResult(const HeldResult& held, bool continued) {
    sf4e::NetplayFacade::NotifyRuntimeMatchResult(held.result, held.captureFrame, held.confirmedFrame, s_gamesReported, continued);
    if (continued) ++s_gamesReported;
}

void FlushHeldNativeResult() {
    if (!s_heldResult) return;
    ReportNativeResult(*s_heldResult, false);
    s_heldResult.reset();
}

// The outcome is settled: this fighter confirmed it natively, or the room
// committed the match end (the only way a spectator learns it). Losing the
// stream or the peer after that ends nothing the player needs to hear about.
bool MatchResultKnown() { return s_nativeResultEmitted || sf4e::NetplayFacade::IsRuntimeMatchEndCommitted(); }

struct NativeFlow { sf4e::native_result::Flow flow = sf4e::native_result::Flow::Other; int winnerIndex = -1; };

// The battle flow the game is in now and, over a WIN, the side that won.
static NativeFlow ReadNativeFlow(rSystem* system) {
    if (!system || !rSystem::staticVars.CurrentBattleFlow) return {};
    const DWORD flowValue = *rSystem::staticVars.CurrentBattleFlow;
    NativeFlow read;
    if (flowValue == rSystem::BF__MATCH_RESULT) {
        GameManager* manager = (system->*rSystem::publicMethods.GetGameManager)();
        if (!manager || !GameManager::publicMethods.GetNativeResultIndex) return {};
        read.flow = sf4e::native_result::Flow::MatchResult;

        // The native result flow (SSFIV.exe absolute VA 0x005DD93A, RVA
        // 0x001DD93A; GameManager vtable +0x30) uses -1 for BF_DRAW_RESULT and
        // a player index for BF_MATCH_RESULT.  This is the match-level field
        // written by the native round-score finalizer; the separate +0x4C field is the
        // current round winner used by the result HUD and is not used here.
        read.winnerIndex = (manager->*GameManager::publicMethods.GetNativeResultIndex)();
    }
    else if (flowValue == rSystem::BF__DRAW_RESULT) {
        read.flow = sf4e::native_result::Flow::DrawResult;
    }
    else if (flowValue == rSystem::BF__READY || flowValue == rSystem::BF__FIGHT) {
        read.flow = sf4e::native_result::Flow::Fight;
    }
    return read;
}

void CaptureNativeMatchResult(rSystem* system, int stateFrame) {
    const NativeFlow read = ReadNativeFlow(system);
    s_nativeResultTimeline.Capture(stateFrame, read.flow, read.winnerIndex);
}

// A spectator's frames are final, so the WIN it is showing is the result.
static void NoteWatchedResult() {
    const NativeFlow read = ReadNativeFlow(rSystem::staticMethods.GetSingleton());
    const auto result = sf4e::native_result::Decode(read.flow, read.winnerIndex);
    if (result != sf4e::native_result::Result::None) s_watchedResult = result;
}

void CountWatchedInPlaceGame() {
    using sf4e::native_result::Result;
    if (s_watchedResult == Result::None) return;
    sf4e::NetplayFacade::NotifyRuntimeWatchedGame(
        s_watchedResult == Result::P1Win ? sf4e::room::MatchResult::P1Win :
        s_watchedResult == Result::P2Win ? sf4e::room::MatchResult::P2Win : sf4e::room::MatchResult::Draw);
    s_watchedResult = Result::None;
}

void PublishConfirmedNativeMatchResult() {
    if (!fSystem::ggpo) return;
    if (!fSystem::LocalFighter()) { NoteWatchedResult(); return; }
    int confirmed = -1;
    if (!GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(fSystem::ggpo, &confirmed))) return;
    if (s_heldResult) {
        const int fight = s_nativeResultTimeline.ConfirmedFightAfter(s_resultFrame, confirmed);
        if (fight < 0) return;
        spdlog::info("Match result: game {} went on to a rematch in this battle (fight confirmed at frame {})", s_gamesReported + 1, fight);
        ReportNativeResult(*s_heldResult, true);
        HandOverInPlaceReplay();
        s_heldResult.reset();
        s_gameResult = sf4e::room::MatchResult::Abort;
        s_nativeResultEmitted = false;
        s_resultFrame = fight;
    }
    if (s_nativeResultEmitted) return;
    const auto sample = s_nativeResultTimeline.ConfirmedSample(confirmed);
    if (sample.result == sf4e::native_result::Result::None || sample.frame <= s_resultFrame) return;

    s_nativeResultEmitted = true;
    s_resultFrame = sample.frame;
    spdlog::info("Match result: confirmed native outcome={} input_frame={} save_frame={}", static_cast<int>(sample.result), confirmed, sample.frame);
    const HeldResult held{
        sample.result == sf4e::native_result::Result::P1Win ? sf4e::room::MatchResult::P1Win :
        sample.result == sf4e::native_result::Result::P2Win ? sf4e::room::MatchResult::P2Win : sf4e::room::MatchResult::Draw,
        static_cast<std::uint64_t>(sample.frame), static_cast<std::uint64_t>(confirmed < 0 ? 0 : confirmed)};
    s_gameResult = held.result;
    if (sf4e::NetplayFacade::RematchInPlaceOffered(held.result)) s_heldResult = held;
    else ReportNativeResult(held, false);
}

// The frame loop only publishes after a successful advance. Once the fight
// has stopped simulating, confirmation of the result frames can still arrive
// through a plain GGPO poll; without this entry that outcome was never sent.
void fSystem::PollNativeMatchResult() {
    PublishConfirmedNativeMatchResult();
}
