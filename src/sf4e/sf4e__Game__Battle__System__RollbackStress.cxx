// Development-only rollback stress harness for sf4e::Game::Battle::System.
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "../common/RollbackAudit.hxx"

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
//
// SF4E_ROLLBACK_STRESS_PREDICT=<1|2|3> (P1, P2 or both) makes the first pass
// mispredict those sides the way GGPO predicts a remote player: each frame of
// a window runs with the side's last input before the window, and the
// rollback then replays the real inputs. A move the player starts inside a
// window, such as an install super, exists only in the corrected timeline, so
// every rollback restores across a timeline that did not have it. Each window
// is replayed twice from the same save, and the two replays are compared:
// state the load does not restore carries over from the discarded timeline
// and makes them differ.
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
    int predictMask = 0; // bit 0 = P1, bit 1 = P2
    int auditMode = 0; // AuditMode
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
    char predict[8] = {};
    const DWORD predictLength = GetEnvironmentVariableA("SF4E_ROLLBACK_STRESS_PREDICT", predict, sizeof(predict));
    if (predictLength == 1 && predict[0] >= '1' && predict[0] <= '3') {
        stress.predictMask = predict[0] - '0';
    } else if (predictLength != 0) {
        spdlog::warn(
            "RollbackStress: SF4E_ROLLBACK_STRESS_PREDICT={} ignored; use 1 (P1), 2 (P2) or 3 (both)",
            predictLength < sizeof(predict) ? predict : "(too long)"
        );
    }
    char audit[8] = {};
    const DWORD auditLength = GetEnvironmentVariableA("SF4E_ROLLBACK_STRESS_AUDIT", audit, sizeof(audit));
    if (auditLength == 1 && audit[0] >= '0' && audit[0] <= '2') {
        stress.auditMode = audit[0] - '0';
    } else if (auditLength != 0) {
        spdlog::warn(
            "RollbackStress: SF4E_ROLLBACK_STRESS_AUDIT={} ignored; use 0, 1 (audit) or 2 (audit and probes)",
            auditLength < sizeof(audit) ? audit : "(too long)"
        );
    }
    spdlog::warn("RollbackStress: enabled for offline battles, distance={} frames predict={} audit={}", distance, stress.predictMask, stress.auditMode);
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

// SF4E_ROLLBACK_STRESS_AUDIT=1|2 (diagnostic). Each stress save also snapshots,
// per fighter: the actor (raw, diagnostic only), both Chara::Afterimage objects
// (actor +0x6EEC and +0x6EF0, built by the actor setup at 0x53ED50), each
// afterimage's Action::Engine (*(afterimage+0xB0)) and its five collision-box
// lists (list objects at afterimage +0x130). After each load the live state is
// compared with the loaded slot's snapshot, only over the fields the native
// mementos restore, and counted per category; `battle closed` prints the
// tally. Mode 2 also flips one engine byte and one box-node byte through the
// same capture and compare, and requires the comparator to report that byte.
namespace audit = sf4e::audit;

enum AuditMode { AUDIT_OFF = 0, AUDIT_ON = 1, AUDIT_PROBE = 2 };

constexpr size_t kActorBytes = 0x703C;
constexpr size_t kAfterimageOffsets[2] = { 0x6EEC, 0x6EF0 };
constexpr size_t kAfterimageBytes = 0x6B10; // up to its GameMementoKey
constexpr size_t kAfterimageEngine = 0xB0;
constexpr size_t kAfterimageBoxLists = 0x130;
constexpr int kBoxLists = 5;
constexpr size_t kBoxListHead = 32; // list[8]
constexpr size_t kBoxNodeNext = 168;
constexpr size_t kBoxNodeData = 160;
constexpr size_t kEngineBytes = 0xFC; // up to its GameMementoKey
constexpr size_t kProbeEngineOffset = 0xE8;

const std::vector<audit::Range> kActorRanges = { { 0, kActorBytes } };
// Action::Actor's memento fields (0x52B7C0, relative to its IMementoable at
// +0x60), without the engine pointer (+0xB0) and list heads (+0x130..+0x144),
// then Afterimage's own memcpy (0x5620A0) up to its key.
const std::vector<audit::Range> kAfterimageRanges = {
    { 0x70, 0xB0 }, { 0xB4, 0xDC }, { 0xE0, 0x130 }, { 0x148, 0x298 }, { 676, kAfterimageBytes },
};
// Action::Engine's record (0x52F2F0) copies 0xF4 bytes from its IMementoable at +4.
const std::vector<audit::Range> kEngineRanges = { { 8, kEngineBytes } };
const std::vector<audit::Range> kWholeRange = { { 0, SIZE_MAX } };

struct AuditFighter {
    std::vector<uint8_t> actor;
    std::vector<uint8_t> afterimage[2];
    std::vector<uint8_t> engine[2];
    std::vector<uint8_t> boxes[2];
};
// All audit state for one battle, reset together when a battle's stress
// history starts (StressStep's first prime) and when it closes.
struct AuditSession {
    AuditFighter snapshots[RollbackStress::kRing][2];
    // Last reported diff per object, so a persistent difference is logged
    // once per battle. Index: 0 actor, 1+k afterimage k, 3+k engine k, 5+k boxes k.
    std::string last[2][7];
    audit::Tally tally;
};

AuditSession& Audit() {
    static AuditSession session;
    return session;
}

void AuditReset() {
    Audit() = AuditSession();
}

const uint8_t* ReadPointer(const uint8_t* base, size_t offset) {
    return *reinterpret_cast<const uint8_t* const*>(base + offset);
}

const uint8_t* BoxNext(const uint8_t* node) { return ReadPointer(node, kBoxNodeNext); }
const uint8_t* BoxData(const uint8_t* node) { return ReadPointer(node, kBoxNodeData); }

const uint8_t* AuditActor(int side) {
    rSystem* system = rSystem::staticMethods.GetSingleton();
    CharaUnit* unit = system ? (system->*rSystem::publicMethods.GetCharaUnit)() : nullptr;
    return unit ? reinterpret_cast<const uint8_t*>((unit->*CharaUnit::publicMethods.GetActorByIndex)(side)) : nullptr;
}

// Serializes the five lists; false when a list object is missing or a list
// was not fully serialized.
bool SerializeBoxes(const uint8_t* afterimage, std::vector<uint8_t>& out, int& nodes) {
    out.clear();
    nodes = 0;
    bool complete = true;
    for (int i = 0; i < kBoxLists; i++) {
        const uint8_t* list = ReadPointer(afterimage, kAfterimageBoxLists + 4 * i);
        if (!list) {
            complete = false;
            continue;
        }
        const audit::BoxListResult result = audit::SerializeBoxList(out, ReadPointer(list, kBoxListHead), BoxNext, BoxData);
        nodes += result.nodes;
        complete = complete && result.complete;
    }
    return complete;
}

void AuditCapture(int index) {
    for (int side = 0; side < 2; side++) {
        AuditFighter& snapshot = Audit().snapshots[index][side];
        const uint8_t* actor = AuditActor(side);
        snapshot.actor.clear();
        for (int k = 0; k < 2; k++) {
            snapshot.afterimage[k].clear();
            snapshot.engine[k].clear();
            snapshot.boxes[k].clear();
        }
        if (!actor) continue;
        snapshot.actor.assign(actor, actor + kActorBytes);
        for (int k = 0; k < 2; k++) {
            const uint8_t* afterimage = ReadPointer(actor, kAfterimageOffsets[k]);
            if (!afterimage) continue;
            snapshot.afterimage[k].assign(afterimage, afterimage + kAfterimageBytes);
            if (const uint8_t* engine = ReadPointer(afterimage, kAfterimageEngine)) {
                snapshot.engine[k].assign(engine, engine + kEngineBytes);
            }
            int nodes = 0;
            if (!SerializeBoxes(afterimage, snapshot.boxes[k], nodes)) snapshot.boxes[k].clear();
        }
    }
}

// Counts one comparison and logs its diff when it changed since the last.
void AuditRecord(audit::Counts& counts, int side, int slot, const char* name, const std::vector<audit::Range>& diff, int frame) {
    counts.checked++;
    if (!diff.empty()) counts.failed++;
    std::string described = audit::Describe(diff);
    std::string& last = Audit().last[side][slot];
    if (described == last) return;
    last = described;
    if (diff.empty()) {
        spdlog::info("RollbackStress: audit p{} {} restores fully again (frame {})", side + 1, name, frame);
    } else {
        spdlog::warn("RollbackStress: audit p{} {} not restored after load of frame {}: {} ranges{}",
            side + 1, name, frame, diff.size(), described);
    }
}

// Flips one live byte, compares, restores it. Detected only when the byte
// was clean before and the comparator reports exactly it afterwards.
template <class Compare>
audit::Probe ProbeByte(uint8_t* live, size_t position, Compare compare) {
    const std::vector<audit::Range> before = compare();
    if (audit::Contains(before, position)) return audit::Probe::Pending;
    live[0] ^= 0xFF;
    const std::vector<audit::Range> after = compare();
    live[0] ^= 0xFF;
    return audit::Contains(after, position) ? audit::Probe::Detected : audit::Probe::Missed;
}

void AuditProbeEngine(const std::vector<uint8_t>& snapshot, uint8_t* engine) {
    audit::Tally& tally = Audit().tally;
    for (size_t offset = kProbeEngineOffset; offset < kEngineBytes && tally.probeEngine == audit::Probe::Pending; offset++) {
        tally.probeEngine = ProbeByte(engine + offset, offset, [&]() {
            return audit::Diff(snapshot, std::vector<uint8_t>(engine, engine + kEngineBytes), kEngineRanges);
        });
    }
    if (tally.probeEngine != audit::Probe::Pending) {
        spdlog::info("RollbackStress: audit probe engine={}", audit::ProbeName(tally.probeEngine));
    }
}

void AuditProbeBoxes(const std::vector<uint8_t>& snapshot, const uint8_t* afterimage) {
    audit::Tally& tally = Audit().tally;
    for (int i = 0; i < kBoxLists; i++) {
        const uint8_t* list = ReadPointer(afterimage, kAfterimageBoxLists + 4 * i);
        uint8_t* node = list ? const_cast<uint8_t*>(ReadPointer(list, kBoxListHead)) : nullptr;
        if (!node) continue;
        // The node's first byte is at this list's first node in the stream.
        std::vector<uint8_t> live;
        int nodes = 0;
        size_t position = 0;
        for (int j = 0; j < i; j++) {
            const uint8_t* earlier = ReadPointer(afterimage, kAfterimageBoxLists + 4 * j);
            std::vector<uint8_t> part;
            if (earlier) audit::SerializeBoxList(part, ReadPointer(earlier, kBoxListHead), BoxNext, BoxData);
            position += part.size();
        }
        position += 4; // this list's count
        for (size_t byte = 0; byte < audit::kBoxNodeFieldBytes && tally.probeBoxes == audit::Probe::Pending; byte++) {
            tally.probeBoxes = ProbeByte(node + byte, position + byte, [&]() {
                SerializeBoxes(afterimage, live, nodes);
                return audit::Diff(snapshot, live, kWholeRange);
            });
        }
        break;
    }
    if (tally.probeBoxes != audit::Probe::Pending) {
        spdlog::info("RollbackStress: audit probe boxes={}", audit::ProbeName(tally.probeBoxes));
    }
}

void AuditCompare(int index, int frame, int mode) {
    audit::Tally& tally = Audit().tally;
    for (int side = 0; side < 2; side++) {
        const AuditFighter& snapshot = Audit().snapshots[index][side];
        const uint8_t* actor = AuditActor(side);
        if (!actor) {
            // A fighter the save captured but the load did not bring back:
            // its actor and both expected afterimages, with their engines
            // and boxes, are missing.
            if (!snapshot.actor.empty()) {
                tally.actor.missing++;
                tally.afterimage.missing += 2;
                tally.engine.missing += 2;
                tally.boxes.missing += 2;
            }
            continue;
        }
        if (snapshot.actor.empty()) {
            tally.actor.missing++;
        } else {
            AuditRecord(tally.actor, side, 0, "actor", audit::Diff(snapshot.actor,
                std::vector<uint8_t>(actor, actor + kActorBytes), kActorRanges), frame);
        }
        for (int k = 0; k < 2; k++) {
            const char* afterimageName = k ? "afterimage1" : "afterimage0";
            const uint8_t* afterimage = ReadPointer(actor, kAfterimageOffsets[k]);
            if (!afterimage || snapshot.afterimage[k].empty()) {
                tally.afterimage.missing++;
                tally.engine.missing++;
                tally.boxes.missing++;
                continue;
            }
            AuditRecord(tally.afterimage, side, 1 + k, afterimageName, audit::Diff(snapshot.afterimage[k],
                std::vector<uint8_t>(afterimage, afterimage + kAfterimageBytes), kAfterimageRanges), frame);

            uint8_t* engine = const_cast<uint8_t*>(ReadPointer(afterimage, kAfterimageEngine));
            if (!engine || snapshot.engine[k].empty()) {
                tally.engine.missing++;
            } else {
                AuditRecord(tally.engine, side, 3 + k, k ? "afterimage1 engine" : "afterimage0 engine",
                    audit::Diff(snapshot.engine[k], std::vector<uint8_t>(engine, engine + kEngineBytes), kEngineRanges), frame);
                if (mode == AUDIT_PROBE && tally.probeEngine == audit::Probe::Pending) {
                    AuditProbeEngine(snapshot.engine[k], engine);
                }
            }

            std::vector<uint8_t> boxes;
            int nodes = 0;
            if (!SerializeBoxes(afterimage, boxes, nodes) || snapshot.boxes[k].empty()) {
                tally.boxes.missing++;
            } else {
                tally.boxNodesChecked += nodes;
                AuditRecord(tally.boxes, side, 5 + k, k ? "afterimage1 boxes" : "afterimage0 boxes",
                    audit::Diff(snapshot.boxes[k], boxes, kWholeRange), frame);
                if (mode == AUDIT_PROBE && nodes > 0 && tally.probeBoxes == audit::Probe::Pending) {
                    AuditProbeBoxes(snapshot.boxes[k], afterimage);
                }
            }
        }
    }
}

// GGPO frees the ring slot, then saves, before every simulated frame.
void StressSaveBefore(int stressFrame) {
    auto& stress = Stress();
    const int index = stressFrame % RollbackStress::kRing;
    if (stress.states[index].used) {
        fSystem::SaveState::Free(&stress.states[index]);
    }
    // A failed save leaves the slot unused, and the rollback below skips it.
    if (!fSystem::SaveState::Save(&stress.states[index])) {
        spdlog::error("RollbackStress: frame {} cannot be saved; its rollback is skipped", stressFrame);
    }
    stress.stateFrame[index] = stressFrame;
    if (stress.auditMode) {
        AuditCapture(index);
    }
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
    fPadSystem::Inputs simulated[2] = { stress.inputs[index][0], stress.inputs[index][1] };
    if (stress.predictMask) {
        // GGPO repeats the last input it has for a remote side until the
        // real one arrives; here that is the input just before this window.
        const int windowStart = current - current % stress.distance;
        for (int side = 0; side < 2; side++) {
            if (stress.predictMask & (1 << side)) {
                simulated[side] = windowStart > 0
                    ? stress.inputs[(windowStart - 1) % RollbackStress::kRing][side]
                    : fPadSystem::Inputs{ 0, 0 };
            }
        }
    }
    StressSaveBefore(current);
    StressSimulate(system, simulated);
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
    const auto targetSaved = [&]() {
        return stress.states[targetIndex].used && stress.stateFrame[targetIndex] == target;
    };
    if (!targetSaved()) {
        return true;
    }
    // Loads the window's first state and re-simulates it with the real inputs.
    // With `compare`, each frame is checked against the stored hashes; without
    // it (the first replay after a mispredicted pass), the replay becomes the
    // reference. Returns false when there was nothing sound to replay from.
    const auto replayWindow = [&](bool compare) {
        if (!targetSaved()) {
            // The previous replay re-saved this slot and the save failed.
            spdlog::error("RollbackStress: frame {} lost its save during the first replay; second replay skipped", target);
            return false;
        }
        if (!fSystem::SaveState::Load(&stress.states[targetIndex])) {
            // Replaying from a partly restored engine proves nothing; count it
            // as a divergence and skip the replay.
            stress.divergences++;
            spdlog::error("RollbackStress: frame {} did not fully restore; replay skipped", target);
            return false;
        }
        if (stress.auditMode) {
            AuditCompare(targetIndex, target, stress.auditMode);
        }
        for (int replayed = target; replayed < stress.frame; replayed++) {
            diag::ScopedTimer _cb(diag::OP_ROLLBACK_CALLBACK);
            if (diag::Enabled()) {
                diag::G().OnRollbackCallback(diag::NowMs());
            }
            const int replayIndex = replayed % RollbackStress::kRing;
            StressSaveBefore(replayed);
            StressSimulate(system, stress.inputs[replayIndex]);
            const auto replay = fSystem::ComputeSemanticHashes(system);
            if (!compare) {
                stress.hashes[replayIndex] = replay;
                stress.fields[replayIndex] = CaptureCharaFields(system);
                continue;
            }
            const auto& original = stress.hashes[replayIndex];
            if (replay.overall == original.overall) {
                continue;
            }
            stress.divergences++;
            const CharaFields replayFields = CaptureCharaFields(system);
            const CharaFields& originalFields = stress.fields[replayIndex];
            spdlog::error(
                "RollbackStress: replay diverged stress_frame={} engine_frame={} distance={} predict={} free_path={} flow={} p1={} p2={} inputs={:08x}/{:08x} {:08x}/{:08x} actions={}/{}{}",
                replayed,
                StressEngineFrame(system),
                stress.distance,
                stress.predictMask,
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
        return true;
    };
    // A mispredicted pass has nothing to compare with: its first replay is
    // the corrected timeline, and a second replay from the same save must
    // reproduce it exactly. A rollback counts only once its checked replay ran.
    const bool replayed = (!stress.predictMask || replayWindow(false)) && replayWindow(true);
    if (!replayed) {
        return true;
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

// The battle-start flow's entry (System::OnBattleFlow_BattleStart, 0x5DD350)
// runs once as each battle begins, including one whose predecessor never
// reached StressCloseBattle. Records such a battle left point into freed
// engine objects, so they are dropped without engine calls, and the
// per-battle counters and audit session start clean.
void StressOpenBattle() {
    auto& stress = Stress();
    StressReclaimAll("battle_start");
    stress.gameMode = -1;
    stress.rollbacks = stress.divergences = stress.resets = 0;
    AuditReset();
}

// Every battle the engine closes passes System::CloseBattle, which calls
// this to report the battle and release its history.
void StressCloseBattle() {
    auto& stress = Stress();
    // Disabled, or a CPU-driven battle that was never stressed: nothing to report.
    if (stress.distance && stress.gameMode >= 0) {
        spdlog::info(
            "RollbackStress: battle closed mode={} distance={} free_path={} rollbacks={} divergences={} resets={}",
            stress.gameMode, stress.distance, SaveStateFreePathName(), stress.rollbacks, stress.divergences, stress.resets
        );
        if (stress.auditMode) {
            spdlog::info("RollbackStress: {}", Audit().tally.Summary());
        }
        EmitRollbackDiagSummary("rollback_stress_close");
        StressReset();
    }
    stress.gameMode = -1;
    stress.rollbacks = stress.divergences = stress.resets = 0;
    AuditReset();
}
