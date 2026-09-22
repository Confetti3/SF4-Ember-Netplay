// Development-only rollback stress harness for sf4e::Game::Battle::System.
#include "sf4e__Game__Battle__System__Internal.hxx"

// ---------------------------------------------------------------------------
// Local rollback stress (development only).
//
// SF4E_ROLLBACK_STRESS=<1..8> makes an offline battle (Versus or Training;
// never Arcade or the title demo, where the CPU drives a side)
// drive save states the way a GGPO session with that rollback distance does:
//
//   * every frame frees the oldest ring slot and saves before simulating;
//   * every <distance> frames the state from <distance> frames ago is loaded
//     and those frames are re-simulated with their recorded inputs, freeing
//     and saving again on each one, exactly as Sync::AdjustSimulation does.
//
// After each re-simulated frame the semantic hash is compared with the
// original pass and any divergence is logged with its subsystem. Inputs pass
// through the same playback path as netplay, and Training Lab playback is
// honoured, so a recorded sequence can be replayed under rollback on one PC.
// With SF4E_ROLLBACK_DIAGNOSTICS=1 the usual timing summary is logged every
// 600 frames, which makes per-character save/free/restore cost comparable
// without a network. GGPO's own synctest backend is not used: it breaks into
// the debugger on a mismatch and writes a log file for every frame.
// ---------------------------------------------------------------------------
namespace {
// Both characters' hashed values, so a divergence names the field that changed.
struct CharaFields {
    fSystem::CharaSemantics side[2];
};

CharaFields CaptureCharaFields(rSystem* system) {
    return { { fSystem::CaptureCharaSemantics(system, 0), fSystem::CaptureCharaSemantics(system, 1) } };
}

// " p1.action 000001be=>000001bf p1.rootX ..." for every field that differs.
std::string DescribeFieldDiff(const CharaFields& original, const CharaFields& replay) {
    std::string out;
    for (int i = 0; i < 2; i++) {
        for (int f = 0; f < fSystem::CharaSemantics::kFields; f++) {
            const uint32_t before = original.side[i].v[f];
            const uint32_t after = replay.side[i].v[f];
            if (before != after) {
                out += fmt::format(" p{}.{} {:08x}=>{:08x}", i + 1, fSystem::CharaSemantics::kFieldNames[f], before, after);
            }
        }
    }
    return out;
}

struct RollbackStress {
    static constexpr int kRing = NUM_SAVE_STATES;
    int distance = 0; // 0 disables
    bool configured = false;
    bool primed = false;
    int frame = 0;
    int16_t lastEngineFrame = 0;
    fSystem::SaveState states[kRing];
    int stateFrame[kRing] = {};
    fPadSystem::Inputs inputs[kRing][2] = {};
    // hashes[g % kRing] is the state after simulating stress frame g.
    fSystem::SemanticHashes hashes[kRing];
    CharaFields fields[kRing];
    uint64_t rollbacks = 0;
    uint64_t divergences = 0;
    uint64_t resets = 0;
    int gameMode = -1; // of the last stressed battle
};

RollbackStress& Stress() {
    static RollbackStress stress;
    return stress;
}

int16_t StressEngineFrame(rSystem* system) {
    return rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
}

void StressConfigure() {
    auto& stress = Stress();
    if (stress.configured) {
        return;
    }
    stress.configured = true;
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("SF4E_ROLLBACK_STRESS", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) {
        return;
    }
    const int distance = atoi(value);
    if (distance < 1 || distance > GGPO_MAX_PREDICTION_FRAMES) {
        spdlog::warn("RollbackStress: SF4E_ROLLBACK_STRESS={} ignored; use 1..{}", value, GGPO_MAX_PREDICTION_FRAMES);
        return;
    }
    stress.distance = distance;
    spdlog::warn("RollbackStress: enabled for offline battles, distance={} frames", distance);
}

void StressReset() {
    auto& stress = Stress();
    for (int i = 0; i < RollbackStress::kRing; i++) {
        if (stress.states[i].used) {
            fSystem::SaveState::Free(&stress.states[i]);
        }
        stress.stateFrame[i] = -1;
    }
    stress.frame = 0;
    stress.primed = false;
}

// Drops any records left over from a battle that ended without reaching
// StressCloseBattle. Their keys point into destroyed objects, so this must
// not call the engine (same rule as fSystem::StartGGPO's sweep).
void StressReclaimAll(const char* reason) {
    auto& stress = Stress();
    for (int i = 0; i < RollbackStress::kRing; i++) {
        fSystem::SaveState::Reclaim(&stress.states[i], reason, i);
        stress.stateFrame[i] = -1;
    }
    stress.frame = 0;
    stress.primed = false;
}

fPadSystem::Inputs StressReadInput(rPadSystem* pad, int side) {
    sf4e::training::Input practice;
    if (sf4e::training::ReadOverride(side, practice)) {
        return { practice.mapped, practice.raw };
    }
    if (sf4e::Overlay::CapturesMenuInput()) {
        return { 0, 0 };
    }
    return {
        (pad->*rPadSystem::publicMethods.GetButtons_MappedOn)(side),
        (pad->*rPadSystem::publicMethods.GetButtons_RawOn)(side),
    };
}

void StressSimulate(rSystem* system, const fPadSystem::Inputs* inputs) {
    PlaybackFrameScopeGuard playbackGuard;
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = inputs[0];
    fPadSystem::playbackData[0][1] = inputs[1];
    diag::ScopedTimer _t(diag::OP_ENGINE_BATTLE_UPDATE);
    (system->*rSystem::publicMethods.BattleUpdate)();
}

// GGPO frees the ring slot, then saves, before every simulated frame.
void StressSaveBefore(int stressFrame) {
    auto& stress = Stress();
    const int index = stressFrame % RollbackStress::kRing;
    if (stress.states[index].used) {
        fSystem::SaveState::Free(&stress.states[index]);
    }
    fSystem::SaveState::Save(&stress.states[index]);
    stress.stateFrame[index] = stressFrame;
}

const char* SubsystemState(uint64_t replay, uint64_t original) {
    return replay == original ? "match" : "DIFF";
}

}

// Runs this outer frame's update under the stress schedule. Returns false when
// stress is disabled so the caller runs the plain update.
bool StressStep(rSystem* system) {
    StressConfigure();
    auto& stress = Stress();
    if (!stress.distance) {
        return false;
    }
    // The CPU opponent's decisions (Battle::Com) are not in the save state,
    // so a CPU-driven side diverges on every replay. Online play has no CPU.
    // Versus against the CPU and a CPU training dummy are not filtered here;
    // the battle-closed line names the mode.
    const int mode = (system->*rSystem::publicMethods.GetGameMode)();
    if (mode == Dimps::Game::Battle::GAMEMODE_ARCADE || mode == Dimps::Game::Battle::GAMEMODE_BENCHMARK_DEMO) {
        return false;
    }
    stress.gameMode = mode;
    // The engine's chara record (0x5633a0) dereferences both actors_0x18
    // entries, which stay null for the first frames of a battle.
    CharaUnit* chara = (system->*rSystem::publicMethods.GetCharaUnit)();
    if (!chara || !chara->actors_0x18[0] || !chara->actors_0x18[1]) {
        return false;
    }
    const int16_t before = StressEngineFrame(system);
    if (stress.primed && before != stress.lastEngineFrame) {
        // A training restore or other jump outside this loop: the recorded
        // history no longer describes the live timeline.
        StressReset();
        stress.resets++;
    }
    if (!stress.primed) {
        // A previous battle that never reached StressCloseBattle leaves
        // slots pointing into freed engine objects. Sweep before the first
        // save of this battle.
        StressReclaimAll("stress_prime");
        diag::InitFromEnvironment();
        if (diag::Enabled() && stress.rollbacks == 0) {
            diag::G().ResetForMatch(diag::NowMs());
        }
        stress.primed = true;
    }

    rPadSystem* pad = rPadSystem::staticMethods.GetSingleton();
    const int current = stress.frame;
    const int index = current % RollbackStress::kRing;
    stress.inputs[index][0] = StressReadInput(pad, 0);
    stress.inputs[index][1] = StressReadInput(pad, 1);
    StressSaveBefore(current);
    StressSimulate(system, stress.inputs[index]);
    stress.lastEngineFrame = StressEngineFrame(system);
    if (static_cast<int16_t>(stress.lastEngineFrame - before) != 1) {
        // Paused, or the native flow held the simulation: not a replayable
        // frame. Start the history again from the next one.
        StressReset();
        stress.primed = true;
        return true;
    }
    stress.hashes[index] = fSystem::ComputeSemanticHashes(system);
    stress.fields[index] = CaptureCharaFields(system);
    stress.frame++;
    if (diag::Enabled()) {
        diag::G().OnFrameAdvanced(diag::NowMs());
    }

    if (stress.frame < stress.distance || stress.frame % stress.distance != 0) {
        return true;
    }
    const int target = stress.frame - stress.distance;
    const int targetIndex = target % RollbackStress::kRing;
    if (!stress.states[targetIndex].used || stress.stateFrame[targetIndex] != target) {
        return true;
    }
    fSystem::SaveState::Load(&stress.states[targetIndex]);
    for (int replayed = target; replayed < stress.frame; replayed++) {
        diag::ScopedTimer _cb(diag::OP_ROLLBACK_CALLBACK);
        if (diag::Enabled()) {
            diag::G().OnRollbackCallback(diag::NowMs());
        }
        const int replayIndex = replayed % RollbackStress::kRing;
        StressSaveBefore(replayed);
        StressSimulate(system, stress.inputs[replayIndex]);
        const auto replay = fSystem::ComputeSemanticHashes(system);
        const auto& original = stress.hashes[replayIndex];
        if (replay.overall != original.overall) {
            stress.divergences++;
            const CharaFields replayFields = CaptureCharaFields(system);
            const CharaFields& originalFields = stress.fields[replayIndex];
            spdlog::error(
                "RollbackStress: replay diverged stress_frame={} engine_frame={} distance={} free_path={} flow={} p1={} p2={} inputs={:08x}/{:08x} {:08x}/{:08x} actions={}/{}{}",
                replayed,
                StressEngineFrame(system),
                stress.distance,
                SaveStateFreePathName(),
                SubsystemState(replay.flow, original.flow),
                SubsystemState(replay.chara[0], original.chara[0]),
                SubsystemState(replay.chara[1], original.chara[1]),
                stress.inputs[replayIndex][0].mappedOn,
                stress.inputs[replayIndex][0].rawOn,
                stress.inputs[replayIndex][1].mappedOn,
                stress.inputs[replayIndex][1].rawOn,
                originalFields.side[0].v[fSystem::CharaSemantics::kAction],
                originalFields.side[1].v[fSystem::CharaSemantics::kAction],
                DescribeFieldDiff(originalFields, replayFields)
            );
            stress.fields[replayIndex] = replayFields;
            // Continue from the replayed timeline so one divergence is not
            // reported again on every later rollback.
            stress.hashes[replayIndex] = replay;
        }
    }
    stress.rollbacks++;
    stress.lastEngineFrame = StressEngineFrame(system);

    if (stress.rollbacks % (600 / stress.distance) == 0) {
        spdlog::info(
            "RollbackStress: distance={} free_path={} rollbacks={} divergences={} resets={}",
            stress.distance, SaveStateFreePathName(), stress.rollbacks, stress.divergences, stress.resets
        );
        EmitRollbackDiagSummary("rollback_stress");
    }
    return true;
}

void StressCloseBattle() {
    auto& stress = Stress();
    if (!stress.distance || stress.gameMode < 0) {
        return; // disabled, or a CPU-driven battle that was never stressed
    }
    spdlog::info(
        "RollbackStress: battle closed mode={} distance={} free_path={} rollbacks={} divergences={} resets={}",
        stress.gameMode, stress.distance, SaveStateFreePathName(), stress.rollbacks, stress.divergences, stress.resets
    );
    stress.gameMode = -1;
    EmitRollbackDiagSummary("rollback_stress_close");
    StressReset();
    stress.rollbacks = stress.divergences = stress.resets = 0;
}
