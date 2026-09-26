// sf4e::Game::Battle::System: save states, semantic state hashing and snapshot capture.
#include "sf4e__Game__Battle__System__Internal.hxx"

// SaveState operations mutate live engine objects (memento record/restore,
// key clearing) and are only valid on the game main thread — the thread that
// runs BattleUpdate, Steam_PostUpdate, and every GGPO callback (see
// docs/design/GGPO_LIFECYCLE.md). Debug builds assert this; engine-memento work
// must never move to a background thread.
static DWORD s_saveStateThreadId = 0;
static void AssertSaveStateThreadAffinity() {
    DWORD tid = GetCurrentThreadId();
    if (s_saveStateThreadId == 0) {
        s_saveStateThreadId = tid;
        return;
    }
    if (tid != s_saveStateThreadId) {
        // Release builds previously latched the id and said nothing, so a
        // violation was invisible in exactly the builds players run. There is
        // no safe recovery (the engine memento calls are already in flight),
        // but the log line makes it diagnosable instead of silent.
        static bool s_warnedThreadAffinity = false;
        if (!s_warnedThreadAffinity) {
            s_warnedThreadAffinity = true;
            spdlog::error(
                "SaveState: op ran on thread {} but pool is owned by thread {}",
                tid,
                s_saveStateThreadId
            );
        }
        assert(false && "SaveState ops must stay on the game main thread");
    }
}

// Keys reserved per save state. Field logs from v0.9.8 and v0.9.9 show 89 to
// 91 keys in every save state, so 96 leaves headroom and the first save of a
// battle does not reallocate. Save warns once if the count outgrows it.
static constexpr std::size_t kSaveStateKeyReservation = 96;

fSystem::SaveState::SaveState() {
    keys.reserve(kSaveStateKeyReservation);
    // Sound records: clear() keeps criPlayerState's capacity and Reset()
    // keeps every manager record with its pool vectors, so after the first
    // save of a battle these never allocate again.
    criPlayerState.reserve(64);
    managerState.Reserve(8);
}

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
// sound maps (process-local pointer keys), RNG (the evolving RNG state has
// not been located; the match seed alone is not it), and all
// presentation/log/overlay state.
fSystem::SemanticHashes fSystem::ComputeSemanticHashes(rSystem* src) {
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
        for (uint32_t word : CaptureCharaSemantics(src, i).v) {
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

void fSystem::CaptureHashCheckpoint(rSystem* src) {
    // The engine counter is a signed 16-bit field and wraps during a long
    // match. GGPO's save callback uses a monotonic frame identity; spectators
    // have no save callback, so the confirmed input boundary plus one is the
    // equivalent state frame.
    int stateFrame = lastGgpoSaveFrame;
    if (localPlayerHandle == GGPO_INVALID_HANDLE) {
        int confirmed = -1;
        stateFrame = ggpo && GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(ggpo, &confirmed))
            ? sf4e::statehash::SpectatorCheckpointStateFrame(confirmed) : -1;
    }
    const int ringIndex = sf4e::statehash::CheckpointRingIndex(stateFrame);
    if (ringIndex < 0) {
        return;
    }
    HashCheckpoint& slot =
        hashCheckpoints[ringIndex];
    // Rollback resimulation legitimately re-captures a frame with corrected
    // values. The GGPO input-confirmation boundary gates exchange, rather
    // than elapsed engine frames (which have a different frame origin).
    sf4e::statehash::PrepareCheckpointIdentity(slot.frameIdx,slot.ggpoStateFrame,slot.sent,stateFrame);
    {
        diag::ScopedTimer _hashTimer(diag::OP_SEMANTIC_HASH);
        slot.hashes = ComputeSemanticHashes(src);
    }
    slot.valid = true;
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

// Looks up `key` in a flat save record. Records are appended in
// shadowManagerMap order, so the cursor makes the common case O(1); the
// scan covers a changed adapter set. Returns null when the key was not saved.
template <class Entries, class Key>
static auto FindSavedEntry(Entries& entries, Key key, size_t& cursor) -> decltype(&entries[0].second) {
    const size_t count = entries.size();
    for (size_t probe = 0; probe < count; probe++) {
        const size_t index = (cursor + probe) % count;
        if (entries[index].first == key) {
            cursor = index + 1;
            return &entries[index].second;
        }
    }
    return nullptr;
}

void CopyIntoPlace(fSystem::SaveState* src) {
    rSystem* system = rSystem::staticMethods.GetSingleton();

    *rSystem::staticVars.CurrentBattleFlow = src->d.CurrentBattleFlow;
    *rSystem::staticVars.PreviousBattleFlow = src->d.PreviousBattleFlow;
    *rSystem::staticVars.CurrentBattleFlowSubstate = src->d.CurrentBattleFlowSubstate;
    *rSystem::staticVars.PreviousBattleFlowSubstate = src->d.PreviousBattleFlowSubstate;
    *rSystem::staticVars.CurrentBattleFlowFrame = src->d.CurrentBattleFlowFrame;
    *rSystem::staticVars.CurrentBattleFlowSubstateFrame = src->d.CurrentBattleFlowSubstateFrame;
    *rSystem::staticVars.PreviousBattleFlowFrame = src->d.PreviousBattleFlowFrame;
    *rSystem::staticVars.PreviousBattleFlowSubstateFrame = src->d.PreviousBattleFlowSubstateFrame;
    *rSystem::staticVars.BattleFlowSubstateCallable_aa9258 = src->d.BattleFlowSubstateCallable_aa9258;
    *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254 = src->d.BattleFlowCallback_CallEveryFrame_aa9254;
    memcpy_s((system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager), &src->d.gameManager, sizeof(GameManager));

    // Restore only what the state recorded. An adapter or manager that did
    // not exist at save time is left alone; the old map-based lookup
    // inserted and restored a zeroed entry for it.
    {
        size_t adapterCursor = 0;
        size_t managerCursor = 0;
        for (
            auto managerIter = fSoundPlayerManager::shadowManagerMap.begin();
            managerIter != fSoundPlayerManager::shadowManagerMap.end();
            managerIter++) {
            rSoundPlayerManager* stubManager = managerIter->first;
            rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
            for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
                const auto* record = FindSavedEntry(src->criPlayerState, &adapters[i], adapterCursor);
                if (record) {
                    fSoundPlayerManager::adapterToCurrentSound[&adapters[i]] = *record;
                }
            }
            auto* pool = FindSavedEntry(src->managerState, stubManager, managerCursor);
            if (pool) {
                sf4e::Platform::SoundObjectPool<4>::Load(rSoundPlayerManager::GetAdapterPool(stubManager), pool);
            }
        }
    }

    // Place each memento key back into its position.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        *iter->first = iter->second;
    }

    // Force the system to reload from the replaced mementos.
    fSystem::RestoreAllFromInternalMementos(system, &GGPO_MEMENTO_ID);
}

void Clear(fSystem::SaveState* victim) {
    // Only release payloads this state still has a claim on. A state whose
    // ownership was handed back (see SaveState::Free) holds stale copies
    // whose payloads now belong to the live keys; clearing through them
    // would be a double free.
    if (victim->ownsKeys) {
        for (auto iter = victim->keys.begin(); iter != victim->keys.end(); iter++) {
            if (iter->first) {
                (iter->first->*rKey::publicMethods.ClearKey)();
                memset(iter->first, 0, sizeof(rKey));
            }
        }
    }
    victim->keys.clear();
    victim->ownsKeys = true;

    // Restore all non-memento-key state to a sane default. Slot reuse must
    // also reset frame metadata so a stale callback identity can never be
    // attributed to a new frame.
    victim->used = false;
    victim->simulationFrame = -1;
    victim->ggpoFrame = -1;
    victim->d.CurrentBattleFlow = 0;
    victim->d.PreviousBattleFlow = 0;
    victim->d.CurrentBattleFlowSubstate = 0;
    victim->d.PreviousBattleFlowSubstate = 0;
    victim->d.CurrentBattleFlowFrame = { 0, 0 };
    victim->d.CurrentBattleFlowSubstateFrame = { 0, 0 };
    victim->d.PreviousBattleFlowFrame = { 0, 0 };
    victim->d.PreviousBattleFlowSubstateFrame = { 0, 0 };
    victim->d.BattleFlowSubstateCallable_aa9258 = nullptr;
    victim->d.BattleFlowCallback_CallEveryFrame_aa9254 = nullptr;
    victim->criPlayerState.clear();
    victim->managerState.Reset();
}

void fSystem::SaveState::Reclaim(SaveState* victim, const char* reason, int slotIndex) {
    if (!victim->used && victim->keys.empty()) {
        return;
    }
    // Process totals, so a field log can set abandoned payloads against the
    // process memory slope. Payload sizes are engine-owned and not known here
    // (ledger A-010).
    static uint64_t s_reclaimedSlots = 0, s_reclaimedKeys = 0;
    ++s_reclaimedSlots;
    if (victim->ownsKeys) s_reclaimedKeys += victim->keys.size();
    spdlog::warn(
        "SaveState: reclaiming leaked slot {} ({}) used={} keys={} owned={} simFrame={} ggpoFrame={} process_total_slots={} process_total_owned_keys={}",
        slotIndex,
        reason ? reason : "?",
        victim->used,
        victim->keys.size(),
        victim->ownsKeys,
        victim->simulationFrame,
        victim->ggpoFrame,
        s_reclaimedSlots,
        s_reclaimedKeys
    );
    // Drop the records without engine calls. At the points that call this
    // (session start, post-teardown sweep) the battle objects the keys point
    // at are either gone or owned by a fresh battle, so ClearKey through
    // those pointers is exactly what must not happen.
    victim->ownsKeys = false;
    Clear(victim);
}

// Read once per process. SF4E_LEGACY_SAVESTATE_FREE=1 selects the v0.8.5
// round-trip release for A/B comparison; SF4E_SAVESTATE_FREE_VERIFY=1 checks
// that each release leaves the live game state untouched.
struct SaveStateFreePolicy {
    bool legacyRoundTrip;
    bool verify;
};

static const SaveStateFreePolicy& FreePolicy() {
    static const SaveStateFreePolicy policy = {
        sf4e::EnvFlag("SF4E_LEGACY_SAVESTATE_FREE"),
        sf4e::EnvFlag("SF4E_SAVESTATE_FREE_VERIFY"),
    };
    return policy;
}

const char* SaveStateFreePathName() {
    return FreePolicy().legacyRoundTrip ? "legacy_round_trip" : "swap";
}

void LogSaveStateFreePolicy() {
    spdlog::info(
        "SaveState: free path={} verify={}",
        FreePolicy().legacyRoundTrip ? "legacy_round_trip" : "swap",
        FreePolicy().verify
    );
}

// Everything a release must not change: the semantic gameplay hash, the
// battle-flow globals, the GameManager block and, for the swap path, every
// live memento key byte. The legacy round trip hands the temporary save's
// payloads to the live keys by design, so its key bytes always differ.
static uint64_t HashLiveStateForFreeCheck(bool includeKeys) {
    sf4e::statehash::Hasher hasher;
    rSystem* system = rSystem::staticMethods.GetSingleton();
    if (!system) {
        return 0;
    }
    hasher.U64(fSystem::ComputeSemanticHashes(system).overall);
    const auto bytes = [&](const void* data, size_t size) {
        const auto* cursor = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) {
            hasher.U8(cursor[i]);
        }
    };
    bytes(rSystem::staticVars.CurrentBattleFlow, sizeof(DWORD));
    bytes(rSystem::staticVars.CurrentBattleFlowSubstate, sizeof(DWORD));
    bytes(rSystem::staticVars.CurrentBattleFlowFrame, sizeof(FixedPoint));
    bytes((system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager));
    if (includeKeys) {
        for (rKey* key : fKey::trackedKeys) {
            hasher.U32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)));
            bytes(key, sizeof(rKey));
        }
    }
    return hasher.Value();
}

// Default release. The engine's ClearKey (0x52F3D0) uses the live mementoable
// object only to find its vtable, and every memento destructor it reaches
// touches memento-owned memory alone (docs/design/SAVESTATE_FREE.md). So the victim's
// key is installed just long enough to release it, and the live key is put
// back. No temporary save and no memento restore is needed.
static void FreeBySwap(fSystem::SaveState* victim) {
    diag::ScopedTimer _t(diag::OP_FREE_SWAP);
    if (victim->ownsKeys) {
        for (auto& entry : victim->keys) {
            if (!entry.first) {
                continue;
            }
            const rKey live = *entry.first;
            *entry.first = entry.second;
            // The original engine function, not the tracking detour: the
            // address still belongs to a live, tracked key.
            (entry.first->*rKey::publicMethods.ClearKey)();
            *entry.first = live;
        }
    }
    // The payloads were released above; Clear must only drop the records.
    victim->ownsKeys = false;
    Clear(victim);
}

void fSystem::SaveState::Free(SaveState* victim) {
    diag::ScopedTimer _freeTimer(diag::OP_FREE_TOTAL);
    AssertSaveStateThreadAffinity();
    const auto& policy = FreePolicy();
    const uint64_t before = policy.verify ? HashLiveStateForFreeCheck(!policy.legacyRoundTrip) : 0;
    if (policy.legacyRoundTrip) {
        FreeByRoundTrip(victim);
    }
    else {
        FreeBySwap(victim);
    }
    if (policy.verify) {
        const uint64_t after = HashLiveStateForFreeCheck(!policy.legacyRoundTrip);
        if (after != before) {
            spdlog::error(
                "SaveState: releasing a state changed live game state (path={} simFrame={} before={:016x} after={:016x})",
                policy.legacyRoundTrip ? "legacy_round_trip" : "swap",
                rSystem::staticMethods.GetSingleton()
                    ? (int)rSystem::GetNumFramesSimulated_FixedPoint(rSystem::staticMethods.GetSingleton())->integral
                    : -1,
                before,
                after
            );
        }
    }
    if (diag::Enabled()) {
        uint32_t occupied = 0;
        for (int i = 0; i < NUM_SAVE_STATES; i++) {
            if (saveStates[i].used) {
                occupied++;
            }
        }
        diag::G().occupiedSaveSlots.Update(occupied);
    }
}

// v0.8.5 release, kept for SF4E_LEGACY_SAVESTATE_FREE A/B comparison.
void fSystem::SaveState::FreeByRoundTrip(SaveState* victim) {
    SaveState tmp;

    {
        diag::ScopedTimer _t(diag::OP_FREE_TMP_SAVE);
        if (!SaveState::Save(&tmp, true)) {
            // Without a complete copy of the live state there is nothing
            // safe to round-trip back into place; release by swap instead.
            spdlog::warn("SaveState: round-trip release could not save the live state; releasing by swap");
            FreeBySwap(victim);
            return;
        }
    }

    // Calls to clear SF4's mementos delegate those calls to the mementoable
    // object. If the mementoable object pointer isn't valid, the key can't
    // be cleared. This isn't relevant to SF4's training mode, because clearing
    // is only ever done on re-initialization after a save, but manually
    // clearing keys when releasing the state is necessary for GGPO to avoid
    // memory leaks.
    //
    // Copy the victim state into the engine. Once the victim state is copied,
    // the mementoable object pointers in each key are valid, and each key can
    // be safely cleared.
    {
        diag::ScopedTimer _t(diag::OP_FREE_VICTIM_INSTALL);
        CopyIntoPlace(victim);
    }
    {
        diag::ScopedTimer _t(diag::OP_FREE_CLEAR);
        Clear(victim);
    }

    // Restore the state at the start of the function.
    //
    // CopyIntoPlace copies each key struct back into its live slot, which
    // hands ownership of every memento payload back to the engine. `tmp`
    // still holds identical copies pointing at those same payloads, so its
    // ownership record is now stale and must be dropped WITHOUT calling
    // ClearKey — releasing it through the engine would free the payloads the
    // live keys just took back. Being short-lived is not sufficient: `tmp`
    // has no destructor, and CloseBattle calls Free in a loop, so a stale
    // claim here is freed again by the next iteration's Clear().
    {
        diag::ScopedTimer _t(diag::OP_FREE_LIVE_RESTORE);
        sf4e::Game::MementoFailure::restore = false;
        CopyIntoPlace(&tmp);
        tmp.ownsKeys = false;
        tmp.keys.clear();
    }
    if (sf4e::Game::MementoFailure::restore) {
        // The live battle did not come back whole; it cannot be played on
        // (ledger A-001). Only this legacy A/B release restores live state.
        spdlog::error("SaveState: round-trip release could not restore the live battle; leaving it");
        if (rSystem* system = rSystem::staticMethods.GetSingleton())
            *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
    }
}

bool fSystem::SaveState::Load(SaveState* src) {
    diag::ScopedTimer _loadTimer(diag::OP_LOAD_TOTAL);
    AssertSaveStateThreadAffinity();
    // Main-thread scratch (asserted above). Kept across loads so a rollback
    // does not allocate; clear() retains the capacity.
    static std::vector<std::pair<rKey*, rKey>> tmpVec;
    tmpVec.clear();
    tmpVec.reserve(fKey::trackedKeys.size());

    // Loading a state abandons the current timeline, and with it any
    // stop intents queued by frames that are about to be re-simulated
    // (or, for training-mode loads, discarded outright). Re-simulation
    // re-queues every stop that survives in the corrected timeline, and
    // the sync step's "shouldn't be playing" pass covers sounds that die
    // with the abandoned one, so stale entries must not linger here-
    // they'd stop (and audibly restart) sounds that are still supposed
    // to be playing. Note this deliberately does NOT happen in
    // CopyIntoPlace: SaveState::Free round-trips through that with the
    // current timeline still live, and must preserve the queue.
    if (fSoundPlayerManager::bUsePureSounds) {
        for (auto& queue : fSoundPlayerManager::queuedStops) {
            queue.second.clear();
        }
    }

    // Copy and zero all currently tracked keys. It's possible that the
    // initialization detour started tracking keys that were only
    // initialized after the save state was created.
    {
        diag::ScopedTimer _t(diag::OP_LOAD_KEY_BACKUP);
        for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
            tmpVec.push_back(std::make_pair(*iter, **iter));
            memset(*iter, 0, sizeof(rKey));
        }
    }

    sf4e::Game::MementoFailure::restore = false;
    {
        diag::ScopedTimer _t(diag::OP_LOAD_COPY_INTO_PLACE);
        CopyIntoPlace(src);
    }
    // Preserve samples on the retained timeline and discard speculative
    // outcomes after the restored GGPO state. Corrected saves refill them.
    // Free() only round-trips storage and must not rewind this history.
    // Only GGPO saves carry a frame; a training or stress load has none and
    // must not wipe the timeline.
    if (src->ggpoFrame >= 0) {
        s_nativeResultTimeline.Rewind(src->ggpoFrame);
    }

    diag::ScopedTimer _restoreTimer(diag::OP_LOAD_RESTORE_KEYS);

    // Zero the keys that were injected by the load.
    //
    // If the memento key data from the source state were left in the key,
    // the next save would result in invalidating the memento key data and
    // the `SaveState()` pointing at invalid memory. It's also possible
    // that the keys in the loaded state are not a proper subset of the
    // keys that existed in the state when load was called, so this
    // function can't iterate over the existing tracked keys.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        if (iter->first) {
            memset(iter->first, 0, sizeof(rKey));
        }
    }

    // Finally, restore the original state of all tracked keys.
    for (auto iter = tmpVec.begin(); iter != tmpVec.end(); iter++) {
        *iter->first = iter->second;
    }
    return !sf4e::Game::MementoFailure::restore;
}

bool fSystem::SaveState::Save(SaveState* dst, bool temporary) {
    diag::ScopedTimer _saveTimer(temporary ? -1 : diag::OP_SAVE_TOTAL);
    AssertSaveStateThreadAffinity();
    rSystem* system = rSystem::staticMethods.GetSingleton();

    // Saving into a slot that still holds records would append to them,
    // overwriting the only pointers to the previous payloads and leaking
    // every one. This was assert-only, so in release it corrupted silently.
    // Recover by releasing the slot properly first: the payloads are still
    // owned here, so Free (not Reclaim) is the correct release.
    if (!dst->keys.empty()) {
        spdlog::error(
            "SaveState: saving into a dirty slot (keys={} used={} simFrame={} ggpoFrame={}); releasing first",
            dst->keys.size(),
            dst->used,
            dst->simulationFrame,
            dst->ggpoFrame
        );
        assert(false && "SaveState::Save into a non-empty slot");
        SaveState::Free(dst);
    }

    dst->used = true;
    dst->ownsKeys = true;

    sf4e::Game::MementoFailure::record = false;
    {
        diag::ScopedTimer _t(temporary ? -1 : diag::OP_SAVE_RECORD_MEMENTOS);
        RecordAllToInternalMementos(system, &GGPO_MEMENTO_ID);
    }
    {
        diag::ScopedTimer _t(temporary ? -1 : diag::OP_SAVE_COPY_KEYS);
        for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
            dst->keys.emplace_back(*iter, **iter);

            // If we leave the data in the source key, reinitialization
            // of the source key will end up freeing _our_ data. Make
            // absolutely sure to zero the source key. Ideally, we could
            // just replace the key's state with the state the key had
            // before the call to RecordAll... but the mementos won't
            // be tracked until after that call.
            memset(*iter, 0, sizeof(rKey));
        }
    }
    if (sf4e::Game::MementoFailure::record) {
        // Release the incomplete snapshot now (the default swap release,
        // never the round trip, which would save again). An unused slot is
        // what every caller already treats as "nothing to load".
        FreeBySwap(dst);
        return false;
    }

    {
        diag::ScopedTimer _t(temporary ? -1 : diag::OP_SAVE_SOUND);
        for (
            auto managerIter = Sound::SoundPlayerManager::shadowManagerMap.begin();
            managerIter != Sound::SoundPlayerManager::shadowManagerMap.end();
            managerIter++) {
            rSoundPlayerManager* stubManager = managerIter->first;
            rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
            for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
                // Read-only lookup: an adapter with no current sound is
                // recorded as an empty request rather than inserted into the
                // live map from the save path.
                const auto current = fSoundPlayerManager::adapterToCurrentSound.find(&adapters[i]);
                dst->criPlayerState.emplace_back(
                    &adapters[i],
                    current != fSoundPlayerManager::adapterToCurrentSound.end()
                        ? current->second
                        : Sound::SoundPlayerManager::DeferredSoundRequest()
                );
            }
            auto& managerRecord = dst->managerState.Next();
            managerRecord.first = stubManager;
            Platform::SoundObjectPool<4>::Save(rSoundPlayerManager::GetAdapterPool(stubManager), &managerRecord.second);
        }
    }

    // Telemetry in case the key count keeps rising past the reservation.
    if (dst->keys.size() > kSaveStateKeyReservation) {
        static bool s_warnedKeyGrowth = false;
        if (!s_warnedKeyGrowth) {
            s_warnedKeyGrowth = true;
            spdlog::warn(
                "SaveState: key count {} exceeds the {}-key reservation (capacity {})",
                dst->keys.size(),
                kSaveStateKeyReservation,
                dst->keys.capacity()
            );
        }
    }

    if (!temporary && diag::Enabled()) {
        diag::RollbackDiagnostics& d = diag::G();
        d.trackedKeys.Update((uint32_t)fKey::trackedKeys.size());
        d.saveKeyVectorSize.Update((uint32_t)dst->keys.size());
        d.saveKeyVectorCapacity.Update((uint32_t)dst->keys.capacity());
        d.soundManagers.Update((uint32_t)Sound::SoundPlayerManager::shadowManagerMap.size());
        d.soundAdapters.Update((uint32_t)fSoundPlayerManager::adapterToCurrentSound.size());
        d.criPlayerStateSize.Update((uint32_t)dst->criPlayerState.size());
        d.managerStateSize.Update((uint32_t)dst->managerState.size());
    }

    diag::ScopedTimer _globalsTimer(temporary ? -1 : diag::OP_SAVE_GLOBALS);
    dst->d.CurrentBattleFlow = *rSystem::staticVars.CurrentBattleFlow;
    dst->d.PreviousBattleFlow = *rSystem::staticVars.PreviousBattleFlow;
    dst->d.CurrentBattleFlowSubstate = *rSystem::staticVars.CurrentBattleFlowSubstate;
    dst->d.PreviousBattleFlowSubstate = *rSystem::staticVars.PreviousBattleFlowSubstate;
    dst->d.CurrentBattleFlowFrame = *rSystem::staticVars.CurrentBattleFlowFrame;
    dst->d.CurrentBattleFlowSubstateFrame = *rSystem::staticVars.CurrentBattleFlowSubstateFrame;
    dst->d.PreviousBattleFlowFrame = *rSystem::staticVars.PreviousBattleFlowFrame;
    dst->d.PreviousBattleFlowSubstateFrame = *rSystem::staticVars.PreviousBattleFlowSubstateFrame;
    dst->d.BattleFlowSubstateCallable_aa9258 = *rSystem::staticVars.BattleFlowSubstateCallable_aa9258;
    dst->d.BattleFlowCallback_CallEveryFrame_aa9254 = *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254;

    memcpy_s(&dst->d.gameManager, sizeof(GameManager), (system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager));
    return true;
}
