// sf4e::Game::Battle::System: the per-frame trace a v2 desync report logs.
//
// Checkpoints are 30 frames apart, so a mismatch only says the PCs parted
// somewhere in those frames. Each PC keeps every frame's played inputs and both
// fighters' hashed values (CharaSemantics), and logs the frames up to the first
// mismatched checkpoint. Comparing the two PCs' lines names the first frame and
// field that differ, the inputs that frame played, and whether a rollback
// re-simulated it.
//
// Two native values ride along: which fighter the engine simulates first
// (System+0xD74, rotated by 0x5E19D0 and saved in the System's additional
// memento) and which fighter leads the chara unit's update order (Unit+0x10,
// rebuilt from it each frame by 0x565D50, reordered by 0x565DD0). Neither is
// expected to differ first; if one does, the order the fighters were resolved
// in is what parted the PCs.
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "../common/FrameTrace.hxx"

namespace {

struct TraceRow {
    int engineFrame = -1;
    fPadSystem::Inputs inputs[2] = {};
    int firstChara = -1;
    int unitLeader = -1;
    fSystem::CharaSemantics chara[2];
};

// About four seconds: a checkpoint is compared only once both PCs confirmed it
// and the remote hash arrived, well inside that even at 400 ms ping.
using Trace = sf4e::statehash::FrameTrace<TraceRow, 256>;

Trace& FramesTraced() {
    static Trace trace;
    return trace;
}

int UnitLeader(const CharaUnit* unit) {
    for (int side = 0; side < 2; side++) {
        if (unit->actors_0x10[0] && unit->actors_0x10[0] == unit->playerActors[side]) return side;
    }
    return -1;
}

std::string Words(const fSystem::CharaSemantics& chara) {
    std::string out;
    for (uint32_t word : chara.v) out += fmt::format(" {:08x}", word);
    return out;
}

}

void TraceFrame(rSystem* system, int stateFrame, const fPadSystem::Inputs inputs[2], const fSystem::CharaSemantics chara[2]) {
    TraceRow& row = FramesTraced().Begin(stateFrame).row;
    row.engineFrame = rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
    row.inputs[0] = inputs[0];
    row.inputs[1] = inputs[1];
    row.firstChara = *rSystem::GetFirstCharaToSimulate(system);
    row.unitLeader = UnitLeader((system->*rSystem::publicMethods.GetCharaUnit)());
    row.chara[0] = chara[0];
    row.chara[1] = chara[1];
}

void ClearFrameTrace() {
    FramesTraced().Clear();
}

void fSystem::LogFrameTrace(int frameIdx) {
    if (!FramesTraced().TakeDump()) return;
    std::string names;
    for (const char* name : CharaSemantics::kFieldNames) names += fmt::format(" {}", name);
    spdlog::error("Desync v2: frames up to {}, as this PC played them; per fighter:{}", frameIdx, names);
    for (int frame = frameIdx - HASH_CHECKPOINT_INTERVAL; frame <= frameIdx; frame++) {
        const Trace::Entry* entry = FramesTraced().Find(frame);
        if (!entry) {
            spdlog::error("Desync v2: frame {} not traced", frame);
            continue;
        }
        const TraceRow& row = entry->row;
        spdlog::error(
            "Desync v2: frame {} engine={} passes={} inputs={:08x}/{:08x} {:08x}/{:08x} first={} leader={} p1{} p2{}",
            frame, row.engineFrame, entry->passes,
            row.inputs[0].mappedOn, row.inputs[0].rawOn, row.inputs[1].mappedOn, row.inputs[1].rawOn,
            row.firstChara, row.unitLeader, Words(row.chara[0]), Words(row.chara[1]));
    }
}
