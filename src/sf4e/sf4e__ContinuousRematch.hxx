#pragma once
#include "../common/ContinuousRematch.hxx"
#include "sf4e__Pad.hxx"
#include "../common/QuickRematch.hxx"
namespace Dimps { namespace Game { namespace Battle { struct System; } } }
namespace sf4e { namespace continuousrematch {
void BeginSession(); // after previous retirement, before first GGPO state save
void InjectLocalInput(Pad::System::Inputs&);
bool SimulateNative();
quickrematch::View ChoiceView();
bool Choose(quickrematch::Choice,std::uint64_t offer);
void Poll(); // outer native update only; report I/O and timeout cleanup
bool BeforeFrame(Dimps::Game::Battle::System*,Pad::System::Inputs inputs[2],bool replay);
void AfterFrame(Dimps::Game::Battle::System*,int stateFrame,const Pad::System::Inputs inputs[2]);
State Capture();
void Restore(const State&,int stateFrame);
bool DeferResultPublication();
void SessionRetiring();
void CloseBattle();
} }
