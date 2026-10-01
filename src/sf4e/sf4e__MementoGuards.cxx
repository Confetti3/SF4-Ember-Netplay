#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__MementoGuards.hxx"
#include "../common/MementoGuards.hxx"

namespace sf4e { namespace memento {
static_assert(sizeof(Key::Metadata) == kMetadataBytes, "the engine metadata layout is 12 bytes");
namespace {
PayloadOwners<SaveState> owners;
unsigned restoreDepth = 0;
bool abortPending = false;
// Set while DrainAbort retires the session, whose free callbacks may trip
// another check; that one belongs to the same abort.
bool draining = false;
struct Counters {
    uint64_t engineClears = 0;
    uint64_t loadWrites = 0;
    uint64_t freeWrites = 0;
    uint64_t badDescriptors = 0;
} counters;

int SlotIndex(const SaveState* state) {
    for (int i = 0; i < NUM_SAVE_STATES; ++i) if (state == &fSystem::saveStates[i]) return i;
    return -1; // training, stress or legacy scratch
}

void Log(const SaveState* state, Key* address, const Key& saved, const char* operation, const char* reason) {
    rSystem* system = rSystem::staticMethods.GetSingleton();
    spdlog::error("MementoGuard: {} at {} key={} object={} payload={} slot={} state={} simFrame={} ggpoFrame={} liveFrame={}",
        reason, operation, (void*)address, saved.mementoableObject, saved.mementos,
        SlotIndex(state), (const void*)state, state->simulationFrame, state->ggpoFrame,
        system ? (int)rSystem::GetNumFramesSimulated_FixedPoint(system)->integral : -1);
}

// Dimps::Game::Memento's vtable (0x0094E328), relocated with the game image.
// Every memento destructor writes it, and the class is never used on its own.
uintptr_t BaseVtable() {
    static const uintptr_t vtable = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x54E328;
    return vtable;
}

void Fail(bool restoring) {
    if (restoring || restoreDepth) {
        Game::MementoFailure::restore = true;
    } else if (fSystem::simGate.phase == sf4e::gate::PHASE_BATTLE_CLOSING) {
        // The match is already over; the skipped write or leaked payload is
        // the whole remedy, and an abort now would only show a false error.
        return;
    } else if (draining) {
        return;
    } else {
        abortPending = true;
        Game::MementoFailure::record = true;
        if (fSystem::ggpo) fSystem::simGate.OnFatal();
    }
}
}

void RegisterPayloads(SaveState* state) {
    for (size_t i = 0; i < state->keys.size(); ++i) {
        const Claimed claimed = owners.Insert(state, i);
        if (claimed == Claimed::Inserted) continue;
        auto& entry = state->keys[i];
        // A payload this snapshot cannot own exclusively is abandoned (leaked)
        // and the capture fails, so it is never loaded or released.
        if (claimed == Claimed::Duplicate) {
            Log(state, entry.first, entry.second, "save.claim", "duplicate payload ownership");
            owners.Revoke(entry.second.mementos);
        } else {
            Log(state, entry.first, entry.second, "save.claim", "ownership table full");
        }
        entry.second = {};
        state->keyFailure = true;
        Game::MementoFailure::record = true;
        Fail(false);
    }
}

bool AllReleasable(const SaveState* state) { return memento::AllReleasable(*state, BaseVtable()); }

void ForgetPayloads(SaveState* state) { owners.Erase(state); }
void ForgetPayload(SaveState* state, void* payload) { owners.Erase(state, payload); }

void NoteEngineClear(Key* key, const char* operation) {
    const auto* claim = owners.Find(key->mementos);
    if (!claim) return;
    ++counters.engineClears;
    Log(claim->owner, key, *key, operation, "engine clearing slot-owned payload");
    // Native ClearKey owns this release. Remove the snapshot's descriptor
    // before native code frees it, so retirement cannot release it twice.
    owners.Revoke(key->mementos);
    Fail(false);
}

bool CheckKeyWrite(SaveState* state, Key* address, const Key& saved, const char* operation, bool restoring) {
    if (fKey::trackedKeys.find(address) != fKey::trackedKeys.end()) return true;
    uint64_t& count = restoring ? counters.loadWrites : counters.freeWrites;
    if (++count == 1) Log(state, address, saved, operation, "write skipped for untracked key; payload leaked");
    Fail(restoring);
    return false;
}

bool CheckRelease(SaveState* state, Key* address, const Key& saved, const char* operation) {
    const char* reason = ValidateDescriptor(saved, BaseVtable());
    if (!reason) return true;
    ++counters.badDescriptors;
    Log(state, address, saved, operation, reason);
    Fail(false);
    return false;
}

RestoreScope::RestoreScope() { ++restoreDepth; }
RestoreScope::~RestoreScope() { --restoreDepth; }
// Battle close ends the match anyway, so a pending abort is dropped with the counters.
void ResetCounters() { counters = {}; abortPending = false; }
void RequestAbort() { Fail(false); }
void LogCounters(const char* label) {
    spdlog::info("SaveSlots [{}]: guards engine_clears={} skipped_load_writes={} skipped_free_writes={} leaked_descriptors={}",
        label, counters.engineClears, counters.loadWrites, counters.freeWrites, counters.badDescriptors);
}
void DiscardPendingAbort() { abortPending = false; }
void DrainAbort() {
    if (!abortPending) return;
    abortPending = false;
    draining = true;
    fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.rollback_unsupported_state"));
    draining = false;
    if (rSystem* system = rSystem::staticMethods.GetSingleton())
        *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
}

} }
