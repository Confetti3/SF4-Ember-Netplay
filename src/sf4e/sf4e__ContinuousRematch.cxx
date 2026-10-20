#include "sf4e__ContinuousRematch.hxx"
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "sf4e__ReplayStore.hxx"
#include "../platform/NativeSignature.hxx"
#include <array>

namespace sf4e { namespace continuousrematch {
namespace {
using Native=rSystem;
using Objects=std::array<std::uintptr_t,8>;
using Restart=void(__thiscall*)(Native*,void*,bool);
namespace cc=choices;
struct Adapter {
    bool initialized=false,available=false;
    std::uintptr_t base=0;
    Restart restart=nullptr;
    std::uint64_t serial=0;
} adapter;
enum class Phase { Inactive, Binding, Running, Refused, Closed, Failed };
struct History {int frame=-1;State state;};
struct Session {
    Phase phase=Phase::Inactive;
    State state;
    std::array<History,128> history;
    Native* battle=nullptr;
    GameManager* manager=nullptr;
    GGPOSession* ggpo=nullptr;
    Objects objects{};
    std::array<unsigned char,0x648> request{};
    std::array<unsigned char,0x638> menu{};
    std::uint64_t roomEpoch=0,generation=0;
    int table=-1;
    rematchpanel::Choice localChoice=rematchpanel::Choice::None;
    unsigned localChoiceEpoch=0,scoredGameReady=0,setGoal=0;
    cc::CommittedInput committed;
    int lastResetStateFrame=-1; // Native ownership boundary, never rolled back.
} current;
bool Active(){return current.phase!=Phase::Inactive;}
bool Live(){return Active()&&current.phase!=Phase::Closed&&current.phase!=Phase::Failed;}
bool Bound(){return current.battle!=nullptr;}
static_assert(sizeof(Pad::System::Inputs)==8,"Control protocol uses original 8-byte GGPO input");
static_assert(cc::PredictionWindow==GGPO_MAX_PREDICTION_FRAMES,"Commit must outlive prediction");

bool ReadObjects(Native* system,Objects& out) {
    if(!system||!native::Accessible(system,0xe44)||
        *reinterpret_cast<std::uintptr_t*>(system)!=adapter.base+0x5550ec)return false;
    auto* chara=(system->*Native::publicMethods.GetCharaUnit)();
    if(!native::Accessible(chara,sizeof(void*)))return false;
    out={{reinterpret_cast<std::uintptr_t>(system),reinterpret_cast<std::uintptr_t>(*Native::GetRequest(system)),
        reinterpret_cast<std::uintptr_t>(chara),
        reinterpret_cast<std::uintptr_t>((system->*Native::publicMethods.GetUnitByIndex)(Native::U_STAGE)),
        reinterpret_cast<std::uintptr_t>((chara->*CharaUnit::publicMethods.GetActorByIndex)(0)),
        reinterpret_cast<std::uintptr_t>((chara->*CharaUnit::publicMethods.GetActorByIndex)(1)),
        reinterpret_cast<std::uintptr_t>((system->*Native::publicMethods.GetUnitByIndex)(Native::U_COM)),
        reinterpret_cast<std::uintptr_t>((system->*Native::publicMethods.GetGameManager)())}};
    for(auto p:out)if(!native::Accessible(reinterpret_cast<void*>(p),sizeof(void*)))return false;
    return native::Accessible(reinterpret_cast<void*>(out[7]),sizeof(GameManager));
}
int ConfirmedInput() {
    int frame=-1;
    return current.ggpo&&current.ggpo==fSystem::ggpo&&
        GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(current.ggpo,&frame))?frame:-1;
}
History ConfirmedState() {
    History result;const int confirmed=ConfirmedInput();
    if(confirmed<0)return result;
    for(const auto& sample:current.history)if(sample.frame>result.frame&&sample.frame<=confirmed+1)result=sample;
    return result;
}
std::uint64_t OfferToken(unsigned epoch){return (adapter.serial<<32)|epoch;}
bool Bind(Native* system) {
    if(!adapter.available||replayplayback::GetView().playback||
        fSystem::simGate.nativeBattle!=gate::NATIVE_BATTLE_NETPLAY||!ReadObjects(system,current.objects)||
        (system->*Native::publicMethods.GetGameMode)()!=1||fSystem::nRandomizeLocalInputsEveryXFramesInGGPO||
        !native::Accessible(reinterpret_cast<void*>(current.objects[1]),current.request.size()))return false;
    std::memcpy(current.request.data(),reinterpret_cast<void*>(current.objects[1]),current.request.size());
    std::memcpy(current.menu.data(),reinterpret_cast<char*>(system)+0x6d8,current.menu.size());
    current.battle=system;current.manager=reinterpret_cast<GameManager*>(current.objects[7]);current.phase=Phase::Running;
    spdlog::info("Continuous rematch: bound table={} generation={}",current.table,current.generation);
    return true;
}
// This expensive native preflight runs at bind and before originating Commit,
// never in the ordinary frame loop. The captured menu includes +0xcd4, the
// fixed seed used by Restart(..., true), not the evolving +0x54 RNG.
bool Context() {
    Objects objects{};
    return current.ggpo==fSystem::ggpo&&ReadObjects(current.battle,objects)&&objects==current.objects&&
        native::Accessible(reinterpret_cast<void*>(objects[1]),current.request.size())&&
        !std::memcmp(current.request.data(),reinterpret_cast<void*>(objects[1]),current.request.size())&&
        !std::memcmp(current.menu.data(),reinterpret_cast<char*>(current.battle)+0x6d8,current.menu.size());
}
void Refuse() {
    if(current.phase!=Phase::Refused)spdlog::warn("Continuous rematch: adapter refused; finishing the current game normally");
    current.phase=Phase::Refused;
}
bool Abort(const char* message) {
    current.phase=Phase::Failed;fSystem::AbortGgpoMatch(message);return false;
}
} // namespace

void Initialize() {
    if(adapter.initialized)return;
    adapter.initialized=true;adapter.base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const unsigned char reset[]={0x55,0x8b,0xec,0x56,0x8b,0xf1,0x83,0xbe,0xf8,0x0d,0x00,0x00,0x00};
    const unsigned char queue[]={0x55,0x8b,0xec,0x83,0xec,0x08,0x53,0x56,0x57};
    const unsigned char call[]={0xe8,0x8f,0xec,0xff,0xff};
    const unsigned char seed[]={0x80,0x7d,0x0c,0x00,0x74,0x24,0x8b,0xbe,0xd4,0x0c,0x00,0x00};
    const unsigned char victoryFinal[]={0x55,0x8b,0xec,0x56,0x8b,0x75,0x08,0x56,0xe8,0x33,0xf3,0xff,0xff};
    const bool layout=native::Signature(adapter.base,0x1db480,reset)&&native::Signature(adapter.base,0x1da220,queue)&&
        native::Signature(adapter.base,0x1db58c,call)&&native::Signature(adapter.base,0x1db540,seed)&&
        native::Signature(adapter.base,0x1dddc0,victoryFinal);
    // Play Offline is a launch/UI choice, not a native capability. Debug modes
    // that change save-state behavior are negotiated before the battle starts.
    adapter.available=layout&&replaystore::ContinuousRecordingAvailable()&&sidecarHash.size()==64&&
        !EnvFlag("SF4E_ROLLBACK_STRESS")&&!EnvFlag("SF4E_LEGACY_SAVESTATE_FREE");
    if(adapter.available)adapter.restart=reinterpret_cast<Restart>(adapter.base+0x1db480);
    spdlog::info("Continuous rematch: startup capability={} native_layout={}",adapter.available,layout);
}
bool Available(){return adapter.available;}

void BeginSession() {
    current=Session{};current.ggpo=fSystem::ggpo;++adapter.serial;
    room::Table table;
    if(!NetplayFacade::GetRuntimeContinuousTable(table))return;
    current.phase=adapter.available?Phase::Binding:Phase::Refused;
    current.table=table.id;current.generation=table.matchGeneration;
    current.roomEpoch=NetplayFacade::GetRuntimeSnapshotShared()->room.roomEpoch;
    current.state.score[0]=table.score[0];current.state.score[1]=table.score[1];
    current.setGoal=static_cast<unsigned>(table.rules.format);
}

void Poll() {
    if(!Live()||!fSystem::ggpo)return;
    const auto snapshot=NetplayFacade::GetRuntimeSnapshotShared();
    if(snapshot&&snapshot->session.control==netplay::Health::Healthy&&
        snapshot->room.roomEpoch==current.roomEpoch&&current.table>=0&&current.table<room::TableCount) {
        const auto& table=snapshot->room.tables[current.table];
        if(table.matchGeneration==current.generation&&table.continuous.closed) {
            fSystem::RetireGgpoSession("continuous-series-finished");return;
        }
        if(table.phase==room::TablePhase::Paused) {Abort(loc::T("quick_rematch.failed"));return;}
    }
    const auto confirmed=ConfirmedState();const auto& s=confirmed.state;const auto& c=s.choices;
    if(confirmed.frame<0||s.resultFrame<0)return;
    const bool fighter=fSystem::localPlayerHandle!=GGPO_INVALID_HANDLE;
    if(fighter&&current.scoredGameReady!=c.epoch) {
        const auto outcome=s.choiceWinner==0?room::MatchResult::P1Win:
            s.choiceWinner==1?room::MatchResult::P2Win:room::MatchResult::Draw;
        if(NetplayFacade::NotifyRuntimeMatchResult(outcome,s.resultFrame,ConfirmedInput(),c.epoch))
            current.scoredGameReady=c.epoch;
    }
    const bool exit=(c.nativeFinish&&confirmed.frame>=(std::max)(s.resultFrame,c.nativeFinishFrame)+16)||
        (c.exitFrame>=0&&confirmed.frame>=c.exitFrame+16);
    if(!exit)return;
    if(fighter)NetplayFacade::FinishRuntimeMatch(c.epoch);
    // Retained room retries own a missing receipt; they cannot hold the native
    // victory screen forever if the control connection is unavailable.
    if(c.nativeFinish||(c.exitFrame>=0&&confirmed.frame-c.exitFrame>=cc::ResultWaitFrames))
        fSystem::RetireGgpoSession("continuous-native-finish");
}

FrameControl PrepareFrameInputs(Pad::System::Inputs inputs[2]) {
    FrameControl wire{{inputs[0].mappedOn,inputs[1].mappedOn}};
    for(unsigned i=0;i<2;++i) {
        inputs[i].mappedOn&=~cc::WireMask;
        inputs[i].rawOn=training::ClearReservedInputBits(inputs[i].rawOn);
    }
    return wire;
}
bool BeforeFrame(Native* system,Pad::System::Inputs inputs[2],const FrameControl& wire) {
    if(!Live())return true;
    if(!current.ggpo)current.ggpo=fSystem::ggpo; // GGPO is created after BeginSession.
    auto& state=current.state;
    const int frame=fSystem::SimulatedStateFrame();
    cc::ObserveFallback(state.choices,frame,wire.data());
    if(current.phase==Phase::Binding&&!Bind(system))Refuse();
    // Frozen-roster negotiation handles unsupported spectators before launch.
    // An unexpected runtime object failure still uses the existing abort path.
    if(current.phase==Phase::Refused&&!Bound()) {
        if(fSystem::localPlayerHandle==GGPO_INVALID_HANDLE||
            !native::Accessible(system,0xe44)||*reinterpret_cast<std::uintptr_t*>(system)!=adapter.base+0x5550ec)
            return Abort(loc::T("quick_rematch.context_invalid"));
        // A refused restart can still finish this match normally. Retain only
        // the validated result owner, so the streamed No does not lose the
        // ordinary result/replay receipt or disable the fighter's controls.
        auto* manager=(system->*Native::publicMethods.GetGameManager)();
        if(!native::Accessible(manager,sizeof(*manager)))return Abort(loc::T("quick_rematch.context_invalid"));
        current.battle=system;current.manager=manager;
    }
    if(system!=current.battle||current.ggpo!=fSystem::ggpo)return Abort(loc::T("quick_rematch.interrupted"));
    auto* manager=current.manager;
    const auto flow=*Native::staticVars.CurrentBattleFlow;
    const auto substate=*Native::staticVars.CurrentBattleFlowSubstate;
    const auto callback=reinterpret_cast<std::uintptr_t>(*Native::staticVars.BattleFlowSubstateCallable_aa9258);
    const int winner=(manager->*GameManager::publicMethods.GetNativeResultIndex)();
    const bool result=flow==Native::BF__MATCH_RESULT&&(winner==0||winner==1);
    const bool draw=flow==Native::BF__DRAW_RESULT;
    if((result||draw)&&state.resultFrame<0) {
        state.resultFrame=frame;state.choiceWinner=draw?2:winner;
    }
    if(draw)cc::FinishNative(state.choices,frame);
    if(state.choices.nativeFinish) {
        if(state.resultFrame>=0)inputs[0]=inputs[1]={0,0};
        return true;
    }
    const bool canRestart=*Native::GetReadyState(system)==Native::RS_FIGHT&&*Native::GetTransitionType(system)==0&&
        !(*Native::GetSimulationFlags(system)&Native::SSF_PAUSE_OR_HOLD);
    if(!state.choices.holding&&result) {
        if(substate>=6||static_cast<std::uint32_t>(callback-adapter.base)!=VictoryCallbackRvas[substate]||
            ++state.victoryFrames>1800) {Refuse();return true;}
        inputs[0]=inputs[1]={0,0};
        if(substate==5&&canRestart) {
            if(state.score[winner]==UINT32_MAX){Refuse();return true;}
            ++state.score[winner];
            cc::EnterVictory(state.choices,frame,current.setGoal&&state.score[winner]>=current.setGoal);
        }
    }
    if(!state.choices.holding)return true;
    if(!result||substate!=5||!canRestart||static_cast<std::uint32_t>(callback-adapter.base)!=VictoryCallbackRvas[5])
        return Abort(loc::T("quick_rematch.context_invalid"));
    const auto action=cc::Advance(state.choices,frame,wire.data());
    inputs[0]=inputs[1]={0,0};
    if(action==cc::Action::Restart) {
        // A spectator never originates Commit. Its one preflight per restart
        // may close its own view, but cannot change the fighters' decision.
        if(fSystem::localPlayerHandle==GGPO_INVALID_HANDLE&&!Context())return Abort(loc::T("quick_rematch.context_invalid"));
        // Fighters preflight before Commit. Its fixed input is retained until
        // the new epoch is confirmed, and the decision outlives prediction.
        // No process-local check can change the accepted decision here.
        state.resultFrame=-1;state.choiceWinner=-1;state.victoryFrames=0;
        current.lastResetStateFrame=(std::max)(current.lastResetStateFrame,frame);
        adapter.restart(system,nullptr,true);
    }
    return true;
}
void AfterFrame(int frame) {
    if(Live()&&frame>=0)current.history[frame%current.history.size()]={frame,current.state};
}
State Capture(){return current.state;}
int MinimumRestoreFrame(){return current.lastResetStateFrame;}
void Restore(const State& saved,int frame) {
    if(!Live())return;
    current.state=saved;
    for(auto& sample:current.history)if(sample.frame>frame)sample=History{};
    if(frame>=0)current.history[frame%current.history.size()]={frame,current.state};
}
bool DeferResultPublication(){return Active();}
replay::MatchBoundary RecordingMatch() {
    replay::MatchBoundary view;
    view.continuous=Live()&&fSystem::ggpo;
    if(!view.continuous)return view;
    const auto confirmed=ConfirmedState();
    if(confirmed.frame<0)return view;
    view.token=OfferToken(confirmed.state.choices.epoch);
    view.firstGame=confirmed.state.choices.epoch==1;
    view.decided=confirmed.state.resultFrame>=0;
    return view;
}
void SessionRetiring(){if(Active())current.phase=Phase::Closed;}
void CloseBattle(){SessionRetiring();}
bool SimulateNative(){return !Live()||!current.state.choices.holding;}
void InjectLocalInput(Pad::System::Inputs& input) {
    if(!Live())return;
    const auto confirmed=ConfirmedState();const auto& c=confirmed.state.choices;
    if(current.committed.Retain(confirmed.frame<0?0:c.epoch,input.mappedOn)) {input.rawOn=0;return;}
    if(input.mappedOn&cc::WireMask){Refuse();input={0,0};}
    if(current.phase==Phase::Refused) {
        input.mappedOn=(input.mappedOn&~cc::WireMask)|cc::Encode(current.state.choices.epoch,cc::No);return;
    }
    if(confirmed.frame<0||!c.holding)return;
    input={0,0};
    if(current.scoredGameReady!=c.epoch)return;
    unsigned flags=cc::Ready;
    if(c.decision==cc::Decision::Rematch) {
        if(!Context()){Refuse();input.mappedOn=cc::Encode(c.epoch,cc::No);return;}
        flags|=cc::Yes|cc::Commit;
    } else if(c.decision==cc::Decision::Lobby)flags|=cc::Commit;
    else if(current.localChoiceEpoch==c.epoch) {
        if(current.localChoice==rematchpanel::Choice::Rematch)flags|=cc::Yes;
        if(current.localChoice==rematchpanel::Choice::Lobby)flags|=cc::No;
    }
    input.mappedOn=cc::Encode(c.epoch,flags);
    if(flags&cc::Commit)current.committed.Set(c.epoch,flags);
}
rematchpanel::View ChoiceView() {
    rematchpanel::View view;
    if(!Live()||!Bound()||current.phase==Phase::Refused||fSystem::localPlayerHandle==GGPO_INVALID_HANDLE)return view;
    const auto confirmed=ConfirmedState();const auto& c=confirmed.state.choices;
    if(confirmed.frame<0||!c.holding||c.offerFrame<0||c.decision!=cc::Decision::Pending)return view;
    view.visible=true;view.offer=OfferToken(c.epoch);view.canRematch=!c.ruleLimit;
    view.accepted=current.localChoiceEpoch==c.epoch&&current.localChoice==rematchpanel::Choice::Rematch;
    const int remaining=(std::max)(0,cc::TimeoutFrames-(confirmed.frame-c.offerFrame));
    view.status=loc::Tf("quick_rematch.time_remaining",(remaining+59)/60);
    if(c.ruleLimit)view.status=std::string(loc::T("quick_rematch.set_complete"))+" "+view.status;
    else if(view.accepted)view.status=std::string(loc::T("quick_rematch.accepted"))+" "+view.status;
    else if(c.yesMask)view.status=std::string(loc::T("quick_rematch.other_ready"))+" "+view.status;
    return view;
}
bool Choose(rematchpanel::Choice choice,std::uint64_t offer) {
    const auto view=ChoiceView();
    if(!view.visible||view.offer!=offer||(choice!=rematchpanel::Choice::Rematch&&choice!=rematchpanel::Choice::Lobby)||
        (choice==rematchpanel::Choice::Rematch&&!view.canRematch))return false;
    current.localChoiceEpoch=ConfirmedState().state.choices.epoch;current.localChoice=choice;return true;
}
} }
