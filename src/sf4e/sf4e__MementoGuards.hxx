#pragma once

#include "sf4e__Game__Battle__System.hxx"

namespace sf4e { namespace memento {

using SaveState = Game::Battle::System::SaveState;
using Key = Dimps::Game::GameMementoKey;

void RegisterPayloads(SaveState* state);
void ForgetPayloads(SaveState* state);
// Whether every saved descriptor would pass release validation. Logs nothing.
bool AllReleasable(const SaveState* state);
void ForgetPayload(SaveState* state, void* payload);
void NoteEngineClear(Key* key, const char* operation);
bool CheckKeyWrite(SaveState* state, Key* address, const Key& saved, const char* operation, bool restoring);
bool CheckRelease(SaveState* state, Key* address, const Key& saved, const char* operation);
// Counts payloads released through a detached key copy, for LogCounters.
void NoteRelease();
void ResetCounters();
void LogCounters(const char* label);
void DrainAbort();
// A guard failure belongs to the session it happened in; starting or
// retiring a session drops it so it cannot end the next match.
void DiscardPendingAbort();
void RequestAbort();

// Restore detours report into Load's existing failure return. Other engine
// operations request an abort after native code and slot mutation unwind.
struct RestoreScope {
    RestoreScope();
    ~RestoreScope();
    RestoreScope(const RestoreScope&) = delete;
    RestoreScope& operator=(const RestoreScope&) = delete;
};

} }
