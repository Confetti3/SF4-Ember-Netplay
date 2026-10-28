// sf4e::Game::Battle::System: the native match result timeline and its
// confirmed publication. Split from sf4e__Game__Battle__System.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"

// Native result state is captured in GGPO saves, including resimulation. The
// history is rewound to each restored state; the emitted latch is deliberately
// match state and survives a rollback so a
// result cannot be submitted twice after re-simulation.
sf4e::native_result::Timeline s_nativeResultTimeline;
bool s_nativeResultEmitted = false;

void ResetNativeResultMatch() {
    s_nativeResultTimeline.Reset();
    s_nativeResultEmitted = false;
}

// The outcome is settled: this fighter confirmed it natively, or the room
// committed the match end (the only way a spectator learns it). Losing the
// stream or the peer after that ends nothing the player needs to hear about.
bool MatchResultKnown() { return s_nativeResultEmitted || sf4e::NetplayFacade::IsRuntimeMatchEndCommitted(); }

void CaptureNativeMatchResult(rSystem* system, int stateFrame) {
    if (!system || !rSystem::staticVars.CurrentBattleFlow) {
        s_nativeResultTimeline.Capture(stateFrame, sf4e::native_result::Flow::Other, -1);
        return;
    }

    const DWORD flowValue = *rSystem::staticVars.CurrentBattleFlow;
    sf4e::native_result::Flow flow = sf4e::native_result::Flow::Other;
    int winnerIndex = -1;
    if (flowValue == rSystem::BF__MATCH_RESULT) {
        flow = sf4e::native_result::Flow::MatchResult;
        GameManager* manager = (system->*rSystem::publicMethods.GetGameManager)();
        if (!manager || !GameManager::publicMethods.GetNativeResultIndex) {
            s_nativeResultTimeline.Capture(stateFrame, sf4e::native_result::Flow::Other, -1);
            return;
        }

        // The native result flow (SSFIV.exe absolute VA 0x005DD93A, RVA
        // 0x001DD93A; GameManager vtable +0x30) uses -1 for BF_DRAW_RESULT and
        // a player index for BF_MATCH_RESULT.  This is the match-level field
        // written by the native round-score finalizer; the separate +0x4C field is the
        // current round winner used by the result HUD and is not used here.
        winnerIndex = (manager->*GameManager::publicMethods.GetNativeResultIndex)();
    }
    else if (flowValue == rSystem::BF__DRAW_RESULT) {
        flow = sf4e::native_result::Flow::DrawResult;
    }

    s_nativeResultTimeline.Capture(stateFrame, flow, winnerIndex);
}

void PublishConfirmedNativeMatchResult() {
    if (s_nativeResultEmitted || !fSystem::ggpo ||
        !fSystem::LocalFighter()) return;
    int confirmed = -1;
    if (!GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(fSystem::ggpo, &confirmed))) return;
    const auto sample = s_nativeResultTimeline.ConfirmedSample(confirmed);
    const auto result = sample.result;
    if (result == sf4e::native_result::Result::None) return;

    s_nativeResultEmitted = true;
    spdlog::info("Match result: confirmed native outcome={} input_frame={} save_frame={}", static_cast<int>(result), confirmed, sample.frame);
    const auto captureFrame = static_cast<std::uint64_t>(sample.frame);
    const auto confirmedFrame = static_cast<std::uint64_t>(confirmed < 0 ? 0 : confirmed);
    switch (result) {
    case sf4e::native_result::Result::P1Win:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::P1Win, captureFrame, confirmedFrame);
        break;
    case sf4e::native_result::Result::P2Win:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::P2Win, captureFrame, confirmedFrame);
        break;
    case sf4e::native_result::Result::Draw:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::Draw, captureFrame, confirmedFrame);
        break;
    case sf4e::native_result::Result::None:
        break;
    }
}

// The frame loop only publishes after a successful advance. Once the fight
// has stopped simulating, confirmation of the result frames can still arrive
// through a plain GGPO poll; without this entry that outcome was never sent.
void fSystem::PollNativeMatchResult() {
    PublishConfirmedNativeMatchResult();
}
