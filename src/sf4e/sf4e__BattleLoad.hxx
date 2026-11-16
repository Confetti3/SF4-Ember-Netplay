#pragma once

// The log's record of one battle load, and the watch that reports a load whose
// first frame never comes (common/LoadWatch.hxx). The load starts where the
// engine starts its battle jobs, its first frame is the battle's first update
// and the battle's close ends it. The watch is ticked every frame by the
// frame limiter, which keeps running while the load is stuck. The state is
// behind one lock, so the calls may come from any thread.

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../common/BattleLoadFacts.hxx"

namespace sf4e {
namespace battleload {

// The fighters and stage Ember has just put into the battle that is about to
// load. The next load's description takes them once; a battle with none noted
// says that its fighters were not read.
void Note(loadwatch::LoadSource source, const Dimps::GameEvents::VsMode::ConfirmedCharaConditions (&sides)[2], int stage);
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
