#pragma once

// The log's record of one battle load, and the watch that reports a load whose
// first frame never comes (common/LoadWatch.hxx). The load starts where the
// engine starts its battle jobs, its first frame is the battle's first update
// and the battle's close ends it. The watch is ticked every frame by the
// frame limiter, which keeps running while the load is stuck. The state is
// behind one lock. Started runs on the game thread at JobManager::Start and
// reads the foreground Versus or Training event there; the other calls may
// come from any thread.

namespace sf4e {
namespace battleload {

// Ember set up the battle that is about to load (a room's match, the developer
// jump, Training's select or a dummy change); the load's description says so.
void NoteEmberSetUp();
// The engine started loading a battle: logs what is known of it and the process.
void Started();
// The battle's first update ran: the load finished.
void FirstFrame();
// The battle closed.
void Closed();
// Every frame: the reports a stalled load is due.
void Tick();

} // namespace battleload
} // namespace sf4e
