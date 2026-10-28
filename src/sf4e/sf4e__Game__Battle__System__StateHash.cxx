// sf4e::Game::Battle::System: semantic state hashing, the hash checkpoint ring
// and the v1 snapshot capture. Split from
// sf4e__Game__Battle__System__SaveState.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"

std::map<int, std::pair<StateSnapshot, fSystem::StateSnapshotMeta>> fSystem::snapshotMap;
fSystem::HashCheckpoint fSystem::hashCheckpoints[fSystem::NUM_HASH_CHECKPOINTS];

const char* const fSystem::CharaSemantics::kFieldNames[kFields] = {
    "status", "side", "rootX", "rootY", "rootZ", "rootW",
    "vitality", "vitalityMax", "revenge", "revengeMax", "recoverable", "recoverableMax",
    "super", "superMax", "scTime", "scTimeMax", "ucTime", "ucTimeMax", "comboDamage", "damage",
    // Action timing (v0.8.6): the running move, how far into it the
    // character is, its posture, and the side's time scale (hitstop and
    // slowdown). A replay that keeps positions and health but lands a move
    // on a different frame differs here.
    "action", "actionFrame", "posture", "timeScale",
};

// Hashing these words with Hasher::U32 is byte-for-byte what F32, I32 and
// Fixed emit for the same values, so the v2 hash is unchanged.
fSystem::CharaSemantics fSystem::CaptureCharaSemantics(rSystem* src, int side) {
    CharaActor::__publicMethods& m = CharaActor::publicMethods;
    CharaUnit* unit = (src->*rSystem::publicMethods.GetCharaUnit)();
    CharaActor* a = (unit->*CharaUnit::publicMethods.GetActorByIndex)(side);
    CharaSemantics out;
    uint32_t* v = out.v;
    const auto raw = [](const void* value) {
        uint32_t word;
        memcpy(&word, value, sizeof(word));
        return word;
    };
    FixedPoint fp;
    const auto fixed = [&](auto getter) {
        (a->*getter)(&fp);
        return raw(&fp);
    };
    *v++ = (a->*m.GetStatus)();
    *v++ = (a->*m.GetCurrentSide)();
    const float* rootPos = (a->*m.GetCurrentRootPosition)();
    for (int c = 0; c < 4; c++) {
        *v++ = raw(&rootPos[c]);
    }
    *v++ = fixed(m.GetVitalityAmt_FixedPoint);
    *v++ = fixed(m.GetVitalityMax_FixedPoint);
    *v++ = fixed(m.GetRevengeAmt_FixedPoint);
    *v++ = fixed(m.GetRevengeMax_FixedPoint);
    *v++ = fixed(m.GetRecoverableVitalityAmt_FixedPoint);
    *v++ = fixed(m.GetRecoverableVitalityMax_FixedPoint);
    *v++ = fixed(m.GetSuperComboAmt_FixedPoint);
    *v++ = fixed(m.GetSuperComboMax_FixedPoint);
    *v++ = fixed(m.GetSCTimeAmt_FixedPoint);
    *v++ = fixed(m.GetSCTimeMax_FixedPoint);
    *v++ = fixed(m.GetUCTimeAmt_FixedPoint);
    *v++ = fixed(m.GetUCTimeMax_FixedPoint);
    *v++ = fixed(m.GetComboDamage);
    *v++ = fixed(m.GetDamage);
    *v++ = (a->*m.GetActionID)();
    *v++ = fixed(m.GetActionFrame);
    *v++ = (a->*m.GetActionPosture)();
    (src->*rSystem::publicMethods.GetUnitTimeScale_Fixed)(&fp, side);
    *v++ = raw(&fp);
    assert(v == out.v + CharaSemantics::kFields);
    return out;
}

// Computes the v2 semantic hashes for the current frame. Coverage is
// deliberately conservative: the frame counter, battle-flow numeric state,
// and per-character semantic values read through engine getters (the same
// values the legacy snapshot exchanges, which are known deterministic
// across peers, plus action id, action frame, posture and time scale).
// Explicitly EXCLUDED: the battle-flow function pointers,
// the raw GameManager block (shallow pointer fields), GameMementoKey bytes,
// sound maps (process-local pointer keys), the battle RNG (System+0x54, not
// saved in any memento and read only by presentation, see DESYNC_V2.md),
// and all presentation/log/overlay state.
fSystem::SemanticHashes fSystem::ComputeSemanticHashes(rSystem* src) {
    const CharaSemantics chara[2] = { CaptureCharaSemantics(src, 0), CaptureCharaSemantics(src, 1) };
    return ComputeSemanticHashes(src, chara);
}

fSystem::SemanticHashes fSystem::ComputeSemanticHashes(rSystem* src, const CharaSemantics chara[2]) {
    using sf4e::statehash::Hasher;
    SemanticHashes out;

    FixedPoint* numFrames = rSystem::GetNumFramesSimulated_FixedPoint(src);

    Hasher flow;
    flow.Fixed(numFrames->fractional, numFrames->integral);
    flow.U32(*rSystem::staticVars.CurrentBattleFlow);
    flow.U32(*rSystem::staticVars.PreviousBattleFlow);
    flow.U32(*rSystem::staticVars.CurrentBattleFlowSubstate);
    flow.U32(*rSystem::staticVars.PreviousBattleFlowSubstate);
    flow.Fixed(
        rSystem::staticVars.CurrentBattleFlowFrame->fractional,
        rSystem::staticVars.CurrentBattleFlowFrame->integral
    );
    flow.Fixed(
        rSystem::staticVars.CurrentBattleFlowSubstateFrame->fractional,
        rSystem::staticVars.CurrentBattleFlowSubstateFrame->integral
    );
    flow.Fixed(
        rSystem::staticVars.PreviousBattleFlowFrame->fractional,
        rSystem::staticVars.PreviousBattleFlowFrame->integral
    );
    flow.Fixed(
        rSystem::staticVars.PreviousBattleFlowSubstateFrame->fractional,
        rSystem::staticVars.PreviousBattleFlowSubstateFrame->integral
    );
    out.flow = flow.Value();

    for (int i = 0; i < 2; i++) {
        Hasher ch;
        for (uint32_t word : chara[i].v) {
            ch.U32(word);
        }
        out.chara[i] = ch.Value();
    }

    Hasher overall;
    overall.U64(out.flow);
    overall.U64(out.chara[0]);
    overall.U64(out.chara[1]);
    out.overall = overall.Value();
    return out;
}

void fSystem::CaptureMeterSample(rSystem* src, bool show) {
    int confirmed = -1;
    if (!LocalFighter()) { sf4e::training::ObserveMatch(src, 0, -1); return; }
    if (show && !(ggpo && GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(ggpo, &confirmed)))) confirmed = -1;
    sf4e::training::ObserveMatch(src, ggpoTimeline.Held(), confirmed);
}

// The GGPO state frame the frame just simulated reaches. The engine counter is
// a signed 16-bit field and wraps during a long match; this frame identity
// does not. A spectator has no save callback: the input it simulated is the
// confirmed one it just consumed.
static int SimulatedStateFrame() {
    if (fSystem::LocalFighter()) {
        return fSystem::ggpoTimeline.Next();
    }
    int confirmed = -1;
    return fSystem::ggpo && GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(fSystem::ggpo, &confirmed))
        ? sf4e::statehash::StateFrameAfterInput(confirmed) : -1;
}

static void CaptureHashCheckpoint(rSystem* src, int stateFrame, const fSystem::CharaSemantics chara[2]) {
    const int ringIndex = sf4e::statehash::CheckpointRingIndex(stateFrame);
    if (ringIndex < 0) {
        return;
    }
    fSystem::HashCheckpoint& slot = fSystem::hashCheckpoints[ringIndex];
    // Rollback resimulation legitimately re-captures a frame with corrected
    // values. The GGPO input-confirmation boundary gates exchange, rather
    // than elapsed engine frames (which have a different frame origin).
    sf4e::statehash::PrepareCheckpointIdentity(slot.frameIdx,slot.ggpoStateFrame,slot.sent,stateFrame);
    slot.engineFrame = rSystem::GetNumFramesSimulated_FixedPoint(src)->integral;
    slot.flow = *rSystem::staticVars.CurrentBattleFlow;
    slot.substate = *rSystem::staticVars.CurrentBattleFlowSubstate;
    {
        diag::ScopedTimer _hashTimer(diag::OP_SEMANTIC_HASH);
        slot.hashes = fSystem::ComputeSemanticHashes(src, chara);
    }
    slot.valid = true;
}

void fSystem::CaptureSimulatedFrame(rSystem* src, const fPadSystem::Inputs inputs[2]) {
    const int stateFrame = SimulatedStateFrame();
    CharaUnit* unit = (src->*rSystem::publicMethods.GetCharaUnit)();
    if (stateFrame <= 0 || !unit || !unit->playerActors[0] || !unit->playerActors[1]) {
        return;
    }
    const CharaSemantics chara[2] = { CaptureCharaSemantics(src, 0), CaptureCharaSemantics(src, 1) };
    TraceFrame(src, stateFrame, inputs, chara);
    CaptureHashCheckpoint(src, stateFrame, chara);
}

fSystem::HashCheckpoint* fSystem::FindHashCheckpoint(int frameIdx) {
    const int ringIndex = sf4e::statehash::CheckpointRingIndex(frameIdx);
    if (ringIndex < 0) {
        return nullptr;
    }
    HashCheckpoint& slot =
        hashCheckpoints[ringIndex];
    return (slot.valid && slot.frameIdx == frameIdx) ? &slot : nullptr;
}

void fSystem::ClearHashCheckpoints() {
    for (int i = 0; i < NUM_HASH_CHECKPOINTS; i++) {
        hashCheckpoints[i] = HashCheckpoint();
    }
    ClearFrameTrace();
}

void fSystem::CaptureSnapshot(rSystem* src) {
    int frameIdx = rSystem::GetNumFramesSimulated_FixedPoint(src)->integral;

    // Only capture snapshots every second.
    if (frameIdx % 60 != 0) {
        return;
    }

    auto iter = snapshotMap.find(frameIdx);
    if (iter != snapshotMap.end()) {
        snapshotMap.erase(iter);
    }
    // Entries are normally retired by SessionClient::Step once sent and
    // confirmed. When the control plane is lost that never happens, and the
    // signed 16-bit engine counter wraps in a long match, so keep the map
    // bounded to the last ten seconds of checkpoints.
    for (auto old = snapshotMap.begin(); old != snapshotMap.end();) {
        if (old->first > frameIdx || frameIdx - old->first > 600) {
            old = snapshotMap.erase(old);
        }
        else {
            ++old;
        }
    }

    StateSnapshot snapshot;
    snapshot.frameIdx = frameIdx;

    CharaActor::__publicMethods& methods = CharaActor::publicMethods;
    CharaUnit* lpCharaUnit = (src->*rSystem::publicMethods.GetCharaUnit)();
    for (int i = 0; i < 2; i++) {
        CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);
        memcpy_s(
            snapshot.chara[i].rootPos,
            sizeof(float) * 4,
            (a->*methods.GetCurrentRootPosition)(),
            sizeof(float) * 4
        );
        snapshot.chara[i].status = (a->*methods.GetStatus)();
        snapshot.chara[i].side = (a->*methods.GetCurrentSide)();

        (a->*methods.GetVitalityAmt_FixedPoint)(&snapshot.chara[i].vit);
        (a->*methods.GetVitalityMax_FixedPoint)(&snapshot.chara[i].vitmax);
        (a->*methods.GetRevengeAmt_FixedPoint)(&snapshot.chara[i].revenge);
        (a->*methods.GetRevengeMax_FixedPoint)(&snapshot.chara[i].revengemax);
        (a->*methods.GetRecoverableVitalityAmt_FixedPoint)(&snapshot.chara[i].recoverable);
        (a->*methods.GetRecoverableVitalityMax_FixedPoint)(&snapshot.chara[i].recoverablemax);
        (a->*methods.GetSuperComboAmt_FixedPoint)(&snapshot.chara[i].super);
        (a->*methods.GetSuperComboMax_FixedPoint)(&snapshot.chara[i].supermax);
        (a->*methods.GetSCTimeAmt_FixedPoint)(&snapshot.chara[i].sctimeamt);
        (a->*methods.GetSCTimeMax_FixedPoint)(&snapshot.chara[i].sctimemax);
        (a->*methods.GetUCTimeAmt_FixedPoint)(&snapshot.chara[i].uctime);
        (a->*methods.GetUCTimeMax_FixedPoint)(&snapshot.chara[i].uctimemax);
        (a->*methods.GetComboDamage)(&snapshot.chara[i].combodamage);
        (a->*methods.GetDamage)(&snapshot.chara[i].damage);
    }
    StateSnapshotMeta meta{ false, false };
    snapshotMap.emplace(frameIdx, std::make_pair(std::move(snapshot), meta));
}
