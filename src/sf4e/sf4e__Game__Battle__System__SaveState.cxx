// sf4e::Game::Battle::System: saving and loading save states. Hashing is in
// sf4e__Game__Battle__System__StateHash.cxx, releasing in
// sf4e__Game__Battle__System__SaveStateRelease.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__MementoGuards.hxx"

// SaveState operations mutate live engine objects (memento record/restore,
// key clearing) and are only valid on the game main thread — the thread that
// runs BattleUpdate, Steam_PostUpdate, and every GGPO callback (see
// docs/design/GGPO_LIFECYCLE.md). Debug builds assert this; engine-memento work
// must never move to a background thread.
static DWORD s_saveStateThreadId = 0;
void AssertSaveStateThreadAffinity() {
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

// The live simulation frame, for diagnostics; -1 outside a battle.
int SimulatedFrame() {
    rSystem* system = rSystem::staticMethods.GetSingleton();
    return system ? (int)rSystem::GetNumFramesSimulated_FixedPoint(system)->integral : -1;
}

fSystem::SaveState::SaveState() {
    keys.reserve(kSaveStateKeyReservation);
    // Sound records: clear() keeps criPlayerState's capacity and Reset()
    // keeps every manager record with its pool vectors, so after the first
    // save of a battle these never allocate again.
    criPlayerState.reserve(64);
    managerState.Reserve(8);
}

fSystem::SaveState::BattleFlowGlobals fSystem::SaveState::BattleFlowGlobals::Capture() {
    BattleFlowGlobals out;
    out.CurrentBattleFlow = *rSystem::staticVars.CurrentBattleFlow;
    out.PreviousBattleFlow = *rSystem::staticVars.PreviousBattleFlow;
    out.CurrentBattleFlowSubstate = *rSystem::staticVars.CurrentBattleFlowSubstate;
    out.PreviousBattleFlowSubstate = *rSystem::staticVars.PreviousBattleFlowSubstate;
    out.CurrentBattleFlowFrame = *rSystem::staticVars.CurrentBattleFlowFrame;
    out.CurrentBattleFlowSubstateFrame = *rSystem::staticVars.CurrentBattleFlowSubstateFrame;
    out.PreviousBattleFlowFrame = *rSystem::staticVars.PreviousBattleFlowFrame;
    out.PreviousBattleFlowSubstateFrame = *rSystem::staticVars.PreviousBattleFlowSubstateFrame;
    out.BattleFlowSubstateCallable_aa9258 = *rSystem::staticVars.BattleFlowSubstateCallable_aa9258;
    out.BattleFlowCallback_CallEveryFrame_aa9254 = *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254;
    return out;
}

void fSystem::SaveState::BattleFlowGlobals::Restore() const {
    *rSystem::staticVars.CurrentBattleFlow = CurrentBattleFlow;
    *rSystem::staticVars.PreviousBattleFlow = PreviousBattleFlow;
    *rSystem::staticVars.CurrentBattleFlowSubstate = CurrentBattleFlowSubstate;
    *rSystem::staticVars.PreviousBattleFlowSubstate = PreviousBattleFlowSubstate;
    *rSystem::staticVars.CurrentBattleFlowFrame = CurrentBattleFlowFrame;
    *rSystem::staticVars.CurrentBattleFlowSubstateFrame = CurrentBattleFlowSubstateFrame;
    *rSystem::staticVars.PreviousBattleFlowFrame = PreviousBattleFlowFrame;
    *rSystem::staticVars.PreviousBattleFlowSubstateFrame = PreviousBattleFlowSubstateFrame;
    *rSystem::staticVars.BattleFlowSubstateCallable_aa9258 = BattleFlowSubstateCallable_aa9258;
    *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254 = BattleFlowCallback_CallEveryFrame_aa9254;
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
    sf4e::memento::RestoreScope restoreScope;
    if (src->keyFailure) {
        sf4e::Game::MementoFailure::restore = true;
        return;
    }
    rSystem* system = rSystem::staticMethods.GetSingleton();

    src->d.flow.Restore();
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
        if (!sf4e::memento::CheckKeyWrite(src, iter->first, iter->second, "load.install", true)) continue;
        *iter->first = iter->second;
    }

    // Force the system to reload from the replaced mementos.
    if (!sf4e::Game::MementoFailure::restore)
        fSystem::RestoreAllFromInternalMementos(system, &GGPO_MEMENTO_ID);
}

bool fSystem::SaveState::Load(SaveState* src) {
    diag::ScopedTimer _loadTimer(diag::OP_LOAD_TOTAL);
    AssertSaveStateThreadAffinity();
    sf4e::memento::RestoreScope restoreScope;
    sf4e::Game::MementoFailure::restore = src->keyFailure;
    if (src->keyFailure) return false;
    // The replay recorder appends a record per battle update, so it rewinds
    // with the timeline. It goes first: a state from another session is
    // refused before any live state changes.
    if (!sf4e::replay::Restore(src->d.replayRecorder, *Dimps::Game::Battle::ReplaySystem::staticMethods.GetSingleton())) {
        spdlog::error("SaveState: the replay recorder's streams are not the ones this state saved (simFrame={})", src->simulationFrame);
        return false;
    }
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
        if (sf4e::memento::CheckKeyWrite(src, iter->first, iter->second, "load.zero", true)) {
            memset(iter->first, 0, sizeof(rKey));
        }
    }

    // Finally, restore the original state of all tracked keys.
    for (auto iter = tmpVec.begin(); iter != tmpVec.end(); iter++) {
        if (sf4e::memento::CheckKeyWrite(src, iter->first, iter->second, "load.restore", true))
            *iter->first = iter->second;
    }
    if (sf4e::crash::HeapCheckEnabled()) sf4e::crash::HeapCheckpoint("load", src->simulationFrame);
    return !sf4e::Game::MementoFailure::restore;
}

void fSystem::SaveState::ForgetFinishedSounds(SaveState* state) {
    int forgotten = 0;
    for (auto& saved : state->criPlayerState) {
        auto& sound = saved.second;
        if (!sound.bLive) continue;
        // Still playing: a live real sound of the same request whose player
        // has not run out. The real players are asked themselves, by the
        // game's own check, since their records are live until stopped too.
        bool playing = false;
        for (auto managerIter = fSoundPlayerManager::shadowManagerMap.begin();
            !playing && managerIter != fSoundPlayerManager::shadowManagerMap.end(); managerIter++) {
            rSoundPlayerManager* realManager = managerIter->second;
            rSoundPlayerManager::CriPlayerAdapter* realPlayers = *rSoundPlayerManager::GetAdapters(realManager);
            for (int i = 0; !playing && i < *rSoundPlayerManager::GetNumAdapters(realManager); i++) {
                const auto real = fSoundPlayerManager::adapterToCurrentSound.find(&realPlayers[i]);
                playing = real != fSoundPlayerManager::adapterToCurrentSound.end() && real->second.bLive &&
                    fSoundPlayerManager::DeferredSoundRequest::IsEqual(&sound, &real->second) &&
                    (realPlayers[i].*rSoundPlayerManager::CriPlayerAdapter::publicMethods.IsStillPlaying)();
            }
        }
        if (!playing) { sound.bLive = false; ++forgotten; }
    }
    if (forgotten) spdlog::info("SaveState: {} sounds that had ended are not kept as playing in this state", forgotten);
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
    dst->keyFailure = false;
    dst->simulationFrame = SimulatedFrame();

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
    sf4e::memento::RegisterPayloads(dst);
    if (sf4e::Game::MementoFailure::record) {
        // Release the incomplete snapshot now (the default detached release,
        // never the round trip, which would save again). An unused slot is
        // what every caller already treats as "nothing to load".
        FreeDetached(dst);
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
    dst->d.flow = BattleFlowGlobals::Capture();

    memcpy_s(&dst->d.gameManager, sizeof(GameManager), (system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager));
    sf4e::replay::Capture(*Dimps::Game::Battle::ReplaySystem::staticMethods.GetSingleton(), dst->d.replayRecorder);
    if (!temporary && sf4e::crash::HeapCheckEnabled()) sf4e::crash::HeapCheckpoint("save", SimulatedFrame());
    return true;
}
