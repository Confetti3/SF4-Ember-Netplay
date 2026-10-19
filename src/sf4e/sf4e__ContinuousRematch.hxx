#pragma once
#include "../common/ContinuousRematch.hxx"
#include "sf4e__Pad.hxx"
#include "../common/QuickRematch.hxx"
#include "../common/ReplayMatchBoundary.hxx"
namespace Dimps { namespace Game { namespace Battle { struct System; } } }
namespace sf4e { namespace continuousrematch {
void Initialize(); // once at startup, before any room Ready
bool Available();
void BeginSession(); // after previous retirement, before first GGPO state save
void InjectLocalInput(Pad::System::Inputs&);
bool SimulateNative();
quickrematch::View ChoiceView();
bool Choose(quickrematch::Choice,std::uint64_t offer);
void Poll(); // outer tick: confirmed results, persistence and native finish
bool BeforeFrame(Dimps::Game::Battle::System*,Pad::System::Inputs inputs[2]);
void AfterFrame(int stateFrame);
State Capture();
void Restore(const State&,int stateFrame);
bool DeferResultPublication();
replay::MatchBoundary RecordingMatch(); // confirmed state, outside rollback callbacks
void SessionRetiring();
void CloseBattle();
} }
