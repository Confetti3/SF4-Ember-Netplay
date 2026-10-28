// sf4e::Game::Battle::System: releasing save states (the detached release, the
// legacy round trip and their policy; docs/design/SAVESTATE_FREE.md). Split
// from sf4e__Game__Battle__System__SaveState.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__MementoGuards.hxx"

// The engine's ClearKey (0x52F3D0) reads only the key's own fields, the
// mementoable object's vtable and the payload, and every memento destructor it
// reaches touches memento-owned memory alone (docs/design/SAVESTATE_FREE.md).
// So a saved payload is released through a copy of its key: the live key and
// the object holding it are never written. The original engine function, not
// the tracking detour, since the copy is not a key the engine knows.
static bool ReleaseDetached(fSystem::SaveState* victim, const std::pair<rKey*, rKey>& entry, const char* operation) {
    if (!entry.first || (victim->keyFailure && !entry.second.mementos)) return false;
    if (!sf4e::memento::CheckRelease(victim, entry.first, entry.second, operation)) return false;
    if (!entry.second.mementos) return true;
    // ClearKey still calls through the mementoable object, which lives with
    // the key; a key the engine no longer tracks means that object is gone.
    if (!sf4e::memento::CheckKeyWrite(victim, entry.first, entry.second, operation, false)) return false;
    sf4e::memento::ForgetPayload(victim, entry.second.mementos);
    rKey detached = entry.second;
    (detached.*rKey::publicMethods.ClearKey)();
    sf4e::memento::NoteRelease();
    return true;
}

static void Clear(fSystem::SaveState* victim) {
    // Only release payloads this state still has a claim on. A state whose
    // ownership was handed back (see SaveState::Free) holds stale copies
    // whose payloads now belong to the live keys; clearing through them
    // would be a double free.
    if (victim->ownsKeys) {
        for (auto iter = victim->keys.begin(); iter != victim->keys.end(); iter++) {
            if (!ReleaseDetached(victim, *iter, "clear.release")) continue;
            // Only the legacy round trip gets here owning keys, with this
            // payload installed at the live key; drop that reference to it.
            // An empty saved key is released without a liveness check, so
            // check before touching the live one.
            if (sf4e::memento::CheckKeyWrite(victim, iter->first, iter->second, "clear.zero", false) &&
                iter->first->mementos == iter->second.mementos)
                memset(iter->first, 0, sizeof(rKey));
        }
    }
    sf4e::memento::ForgetPayloads(victim);
    victim->keys.clear();
    victim->ownsKeys = true;
    victim->keyFailure = false;

    // Restore all non-memento-key state to a sane default. Slot reuse must
    // also reset frame metadata so a stale callback identity can never be
    // attributed to a new frame.
    victim->used = false;
    victim->simulationFrame = -1;
    victim->ggpoFrame = -1;
    victim->d.flow = {};
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
    return FreePolicy().legacyRoundTrip ? "legacy_round_trip" : "detached";
}

void LogSaveStateFreePolicy() {
    spdlog::info(
        "SaveState: free path={} verify={} tracked_keys={}",
        FreePolicy().legacyRoundTrip ? "legacy_round_trip" : "detached",
        FreePolicy().verify,
        fKey::trackedKeys.size()
    );
}

// Everything a release must not change: the semantic gameplay hash, the
// battle-flow globals, the GameManager block and, for the detached path, every
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

// Default release: each saved payload through a copy of its key (see
// ReleaseDetached), then the records are dropped.
void FreeDetached(fSystem::SaveState* victim) {
    diag::ScopedTimer _t(diag::OP_FREE_SWAP);
    if (victim->ownsKeys) {
        for (const auto& entry : victim->keys) ReleaseDetached(victim, entry, "free.release");
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
        FreeDetached(victim);
    }
    if (policy.verify) {
        const uint64_t after = HashLiveStateForFreeCheck(!policy.legacyRoundTrip);
        if (after != before) {
            spdlog::error(
                "SaveState: releasing a state changed live game state (path={} simFrame={} before={:016x} after={:016x})",
                policy.legacyRoundTrip ? "legacy_round_trip" : "detached",
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
    if (sf4e::crash::HeapCheckEnabled()) sf4e::crash::HeapCheckpoint("free", SimulatedFrame());
}

// v0.8.5 release, kept for SF4E_LEGACY_SAVESTATE_FREE A/B comparison.
void fSystem::SaveState::FreeByRoundTrip(SaveState* victim) {
    // The round trip restores the victim before releasing it, so a descriptor
    // that would fail release validation must not be installed at all.
    if (victim->keyFailure || !sf4e::memento::AllReleasable(victim)) {
        FreeDetached(victim);
        return;
    }
    SaveState tmp;
    sf4e::Game::MementoFailure::restore = false;

    {
        diag::ScopedTimer _t(diag::OP_FREE_TMP_SAVE);
        if (!SaveState::Save(&tmp, true)) {
            // Without a complete copy of the live state there is nothing
            // safe to round-trip back into place; release detached instead.
            spdlog::warn("SaveState: round-trip release could not save the live state; releasing detached");
            FreeDetached(victim);
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
    const bool victimRestoreFailed = sf4e::Game::MementoFailure::restore;
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
        sf4e::memento::ForgetPayloads(&tmp);
        tmp.ownsKeys = false;
        tmp.keys.clear();
    }
    if (victimRestoreFailed || sf4e::Game::MementoFailure::restore) {
        // The live battle did not come back whole; it cannot be played on
        // (ledger A-001). Only this legacy A/B release restores live state.
        spdlog::error("SaveState: round-trip release could not restore the live battle; leaving it");
        sf4e::memento::RequestAbort();
        if (rSystem* system = rSystem::staticMethods.GetSingleton())
            *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
    }
}
