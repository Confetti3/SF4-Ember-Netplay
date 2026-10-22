#pragma once
#include "../common/ContinuousRematch.hxx"
#include <array>
#include "sf4e__Pad.hxx"
#include "../common/RematchPanel.hxx"
#include "../common/ReplayMatchBoundary.hxx"
namespace Dimps { namespace Game { namespace Battle { struct System; } } }
namespace sf4e { namespace continuousrematch {
void Initialize(); // once at startup, before any room Ready
bool Available();
void BeginSession(); // after previous retirement, before first GGPO state save
void InjectLocalInput(Pad::System::Inputs&);
bool SimulateNative();
rematchpanel::View ChoiceView();
bool Choose(rematchpanel::Choice,std::uint64_t offer);
void Poll(); // outer tick: confirmed results, persistence and native finish
using FrameControl=std::array<std::uint32_t,2>;
FrameControl PrepareFrameInputs(Pad::System::Inputs inputs[2]);
bool BeforeFrame(Dimps::Game::Battle::System*,Pad::System::Inputs inputs[2],const FrameControl&);
void AfterFrame(int stateFrame);
State Capture();
int MinimumRestoreFrame(); // Never load native owners from before the last reset.
void Restore(const State&,int stateFrame);
bool DeferResultPublication();
replay::MatchBoundary RecordingMatch(); // confirmed state, outside rollback callbacks
void SessionRetiring();
void CloseBattle();
} }
