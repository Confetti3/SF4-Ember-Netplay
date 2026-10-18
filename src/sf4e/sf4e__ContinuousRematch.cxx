#include "sf4e__ContinuousRematch.hxx"
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include <array>

namespace sf4e { namespace continuousrematch {
namespace {
using Native=rSystem;
using Objects=std::array<std::uintptr_t,7>;
using Restart=void(__thiscall*)(Native*,void*,bool);
namespace cc=continuouschoices;
State state;
bool initialized=false,configured=false,bound=false,ended=false,failed=false;
bool sessionEnabled=false,localRefused=false;
std::uint64_t sessionSerial=0,roomEpoch=0,matchGeneration=0;
Native* battle=nullptr;
GGPOSession* session=nullptr;
std::uintptr_t base=0;
Objects objects{};
Restart restart=nullptr;
int tableIndex=-1;
struct History {int frame=-1;State state;};
std::array<History,128> history;
quickrematch::Choice localChoice=quickrematch::Choice::None;
unsigned localChoiceEpoch=0,scoredGameReady=0,setGoal=0;
bool Active(){return sessionEnabled;}
static_assert(sizeof(Pad::System::Inputs)==8,"Control protocol uses original 8-byte GGPO input");
std::array<unsigned char,0x648> requestBytes{};
std::array<unsigned char,0x638> menuBytes{};

bool Accessible(const void* p,std::size_t n) {
    auto at=reinterpret_cast<std::uintptr_t>(p);const auto end=at+n;
    if(!at||!n||end<at)return false;
    while(at<end) {
        MEMORY_BASIC_INFORMATION r{};
        if(VirtualQuery(reinterpret_cast<void*>(at),&r,sizeof(r))!=sizeof(r)||
            r.State!=MEM_COMMIT||(r.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto a=r.Protect&0xff;
        if(a!=PAGE_READONLY&&a!=PAGE_READWRITE&&a!=PAGE_WRITECOPY&&a!=PAGE_EXECUTE_READ&&
            a!=PAGE_EXECUTE_READWRITE&&a!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto next=reinterpret_cast<std::uintptr_t>(r.BaseAddress)+r.RegionSize;
        if(next<=at)return false;at=next;
    }
    return true;
}
template<std::size_t N> bool Signature(std::uintptr_t rva,const unsigned char (&bytes)[N]) {
    const void* p=reinterpret_cast<void*>(base+rva);return Accessible(p,N)&&!std::memcmp(p,bytes,N);
}
bool ReadObjects(Native* system,Objects& out) {
    if(!system||!Accessible(system,0xe44)||*reinterpret_cast<std::uintptr_t*>(system)!=base+0x5550ec)return false;
    auto* chara=(system->*Native::publicMethods.GetCharaUnit)();
    if(!Accessible(chara,sizeof(void*)))return false;
    out={{reinterpret_cast<std::uintptr_t>(system),reinterpret_cast<std::uintptr_t>(*Native::GetRequest(system)),
        reinterpret_cast<std::uintptr_t>(chara),
        reinterpret_cast<std::uintptr_t>((system->*Native::publicMethods.GetUnitByIndex)(Native::U_STAGE)),
        reinterpret_cast<std::uintptr_t>((chara->*CharaUnit::publicMethods.GetActorByIndex)(0)),
        reinterpret_cast<std::uintptr_t>((chara->*CharaUnit::publicMethods.GetActorByIndex)(1)),
        reinterpret_cast<std::uintptr_t>((system->*Native::publicMethods.GetUnitByIndex)(Native::U_COM))}};
    for(auto p:out)if(!Accessible(reinterpret_cast<void*>(p),sizeof(void*)))return false;
    return true;
}
int ConfirmedInput() {
    int frame=-1;
    return session&&session==fSystem::ggpo&&GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(session,&frame))?frame:-1;
}
History ConfirmedState() {
    History result;const int confirmed=ConfirmedInput();
    if(confirmed<0)return result;
    for(const auto& sample:history)if(sample.frame>result.frame&&sample.frame<=confirmed+1)result=sample;
    return result;
}
std::uint64_t OfferToken(unsigned epoch){return (sessionSerial<<32)|epoch;}
bool Bind(Native* system) {
    if(!configured||replayplayback::GetView().playback||
        fSystem::simGate.nativeBattle!=gate::NATIVE_BATTLE_NETPLAY||!ReadObjects(system,objects)||
        (system->*Native::publicMethods.GetGameMode)()!=1||fSystem::nRandomizeLocalInputsEveryXFramesInGGPO||
        !Accessible(reinterpret_cast<void*>(objects[1]),requestBytes.size()))return false;
    std::memcpy(requestBytes.data(),reinterpret_cast<void*>(objects[1]),requestBytes.size());
    std::memcpy(menuBytes.data(),reinterpret_cast<char*>(system)+0x6d8,menuBytes.size());
    battle=system;bound=true;
    spdlog::info("Continuous rematch: bound table={} generation={}",tableIndex,matchGeneration);
    return true;
}
bool Context(Native* system) {
    Objects current{};
    return system==battle&&session==fSystem::ggpo&&ReadObjects(system,current)&&current==objects&&
        Accessible(reinterpret_cast<void*>(objects[1]),requestBytes.size())&&
        !std::memcmp(requestBytes.data(),reinterpret_cast<void*>(objects[1]),requestBytes.size())&&
        !std::memcmp(menuBytes.data(),reinterpret_cast<char*>(system)+0x6d8,menuBytes.size());
}
int NextFrame() {
    return fSystem::localPlayerHandle==GGPO_INVALID_HANDLE?statehash::StateFrameAfterInput(ConfirmedInput()):fSystem::ggpoTimeline.Next();
}
void Refuse() {
    if(!localRefused)spdlog::warn("Continuous rematch: adapter refused; finishing the current game normally");
    localRefused=true;
}
} // namespace

void Initialize() {
    if(initialized)return;
    initialized=true;base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const unsigned char reset[]={0x55,0x8b,0xec,0x56,0x8b,0xf1,0x83,0xbe,0xf8,0x0d,0x00,0x00,0x00};
    const unsigned char queue[]={0x55,0x8b,0xec,0x83,0xec,0x08,0x53,0x56,0x57};
    const unsigned char call[]={0xe8,0x8f,0xec,0xff,0xff};
    const unsigned char seed[]={0x80,0x7d,0x0c,0x00,0x74,0x24,0x8b,0xbe,0xd4,0x0c,0x00,0x00};
    const unsigned char victoryFinal[]={0x55,0x8b,0xec,0x56,0x8b,0x75,0x08,0x56,0xe8,0x33,0xf3,0xff,0xff};
    const bool layout=Signature(0x1db480,reset)&&Signature(0x1da220,queue)&&Signature(0x1db58c,call)&&
        Signature(0x1db540,seed)&&Signature(0x1dddc0,victoryFinal);
    configured=layout&&sidecarHash.size()==64&&!EnvFlag("SF4E_START_OFFLINE")&&
        !EnvFlag("SF4E_ROLLBACK_STRESS")&&!EnvFlag("SF4E_LEGACY_SAVESTATE_FREE");
    if(configured)restart=reinterpret_cast<Restart>(base+0x1db480);
    spdlog::info("Continuous rematch: startup capability={} native_layout={}",configured,layout);
}
bool Available(){return configured;}

void BeginSession() {
    state={};history={};bound=ended=failed=localRefused=false;sessionEnabled=false;
    battle=nullptr;session=fSystem::ggpo;objects={};requestBytes={};menuBytes={};
    localChoice=quickrematch::Choice::None;localChoiceEpoch=scoredGameReady=setGoal=0;
    tableIndex=-1;roomEpoch=matchGeneration=0;++sessionSerial;
    room::Table table;
    sessionEnabled=NetplayFacade::GetRuntimeContinuousTable(table);
    if(sessionEnabled) {
        tableIndex=table.id;matchGeneration=table.matchGeneration;
        roomEpoch=NetplayFacade::GetRuntimeSnapshotShared()->room.roomEpoch;
        state.score[0]=table.score[0];state.score[1]=table.score[1];
        setGoal=static_cast<unsigned>(table.rules.format);
        localRefused=!configured;
    }
}

void Poll() {
    if(!Active()||ended||!fSystem::ggpo)return;
    const auto snapshot=NetplayFacade::GetRuntimeSnapshotShared();
    if(snapshot&&snapshot->session.control==netplay::Health::Healthy&&snapshot->room.roomEpoch==roomEpoch&&tableIndex>=0) {
        const auto& table=snapshot->room.tables[tableIndex];
        if(table.matchGeneration==matchGeneration&&table.continuous.closed) {
            fSystem::RetireGgpoSession("continuous-series-finished");return;
        }
        if(table.phase==room::TablePhase::Paused) {
            failed=true;fSystem::AbortGgpoMatch(loc::T("quick_rematch.failed"));return;
        }
    }
    const auto confirmed=ConfirmedState();const auto& s=confirmed.state;const auto& c=s.choices;
    if(confirmed.frame<0||s.resultFrame<0)return;
    const bool fighter=fSystem::localPlayerHandle!=GGPO_INVALID_HANDLE;
    if(fighter&&scoredGameReady!=c.epoch) {
        const auto outcome=s.choiceWinner==0?room::MatchResult::P1Win:
            s.choiceWinner==1?room::MatchResult::P2Win:room::MatchResult::Draw;
        if(NetplayFacade::NotifyRuntimeMatchResult(outcome,s.resultFrame,ConfirmedInput(),c.epoch))
            scoredGameReady=c.epoch;
    }
    const bool exit=(c.nativeFinish&&confirmed.frame>=(std::max)(s.resultFrame,c.nativeFinishFrame)+16)||
        (c.exitFrame>=0&&confirmed.frame>=c.exitFrame+16);
    if(!exit)return;
    if(fighter)NetplayFacade::FinishRuntimeMatch(c.epoch);
    // A missing room quorum must not keep the native victory screen alive.
    // The shared result/finish retries retain this generation while the room
    // recovers. Ordinary choices leave once its receipt lands; a lost receipt
    // also has a bound, independent of the control connection.
    if(c.nativeFinish||(c.exitFrame>=0&&confirmed.frame-c.exitFrame>=cc::ResultWaitFrames))
        fSystem::RetireGgpoSession("continuous-native-finish");
}

bool BeforeFrame(Native* system,Pad::System::Inputs inputs[2]) {
    if(!Active())return true;
    if(!session)session=fSystem::ggpo; // GGPO is created after BeginSession.
    const std::uint32_t wire[2]={inputs[0].mappedOn,inputs[1].mappedOn};
    inputs[0].mappedOn&=~cc::WireMask;inputs[1].mappedOn&=~cc::WireMask;
    cc::ObserveFallback(state.choices,NextFrame(),wire);
    if(ended||failed)return true;
    if(!localRefused&&((!bound&&!Bind(system))||(bound&&!Context(system))))Refuse();
    // A spectator cannot opt the fighters out. If its native layout refuses,
    // only its own view ends, through the existing spectator failure path.
    if(localRefused&&fSystem::localPlayerHandle==GGPO_INVALID_HANDLE) {
        failed=true;fSystem::AbortGgpoMatch(loc::T("quick_rematch.context_invalid"));return false;
    }
    auto* manager=(system->*Native::publicMethods.GetGameManager)();
    if(!Accessible(manager,sizeof(*manager))) {
        failed=true;fSystem::AbortGgpoMatch(loc::T("quick_rematch.interrupted"));return false;
    }
    const auto flow=*Native::staticVars.CurrentBattleFlow;
    const auto substate=*Native::staticVars.CurrentBattleFlowSubstate;
    const auto callback=reinterpret_cast<std::uintptr_t>(*Native::staticVars.BattleFlowSubstateCallable_aa9258);
    const int winner=(manager->*GameManager::publicMethods.GetNativeResultIndex)();
    const bool result=flow==Native::BF__MATCH_RESULT&&(winner==0||winner==1);
    const bool draw=flow==Native::BF__DRAW_RESULT;
    if((result||draw)&&state.resultFrame<0) {
        state.resultFrame=NextFrame();state.choiceWinner=draw?2:winner;
    }
    if(draw)cc::FinishNative(state.choices,NextFrame());
    if(state.choices.nativeFinish) {
        if(state.resultFrame>=0)inputs[0]=inputs[1]={0,0};
        return true;
    }
    const bool canRestart=*Native::GetReadyState(system)==Native::RS_FIGHT&&*Native::GetTransitionType(system)==0&&
        !(*Native::GetSimulationFlags(system)&Native::SSF_PAUSE_OR_HOLD);
    if(!state.choices.holding&&result) {
        if(substate>=6||static_cast<std::uint32_t>(callback-base)!=VictoryCallbackRvas[substate]||
            ++state.victoryFrames>1800) {
            Refuse();return true;
        }
        inputs[0]=inputs[1]={0,0};
        if(substate==5&&canRestart) {
            if(state.score[winner]==UINT32_MAX){Refuse();return true;}
            ++state.score[winner];
            cc::EnterVictory(state.choices,NextFrame(),setGoal&&state.score[winner]>=setGoal);
        }
    }
    if(!state.choices.holding)return true;
    if(!result||substate!=5||!canRestart||static_cast<std::uint32_t>(callback-base)!=VictoryCallbackRvas[5]) {
        Refuse();return true;
    }
    const auto action=cc::Advance(state.choices,NextFrame(),wire);
    inputs[0]=inputs[1]={0,0};
    if(state.choices.nativeFinish)return true;
    if(action==cc::Action::Restart) {
        if(!bound||localRefused){failed=true;fSystem::AbortGgpoMatch(loc::T("quick_rematch.context_invalid"));return false;}
        state.resultFrame=-1;state.choiceWinner=-1;state.victoryFrames=0;
        restart(system,nullptr,true);
    }
    return true;
}
void AfterFrame(int frame) {
    if(Active()&&!ended&&!failed&&frame>=0)history[frame%history.size()]={frame,state};
}
State Capture(){return state;}
void Restore(const State& saved,int frame) {
    if(!Active()||ended)return;
    state=saved;for(auto& sample:history)if(sample.frame>frame)sample=History{};
    if(frame>=0)history[frame%history.size()]={frame,state};
}
bool DeferResultPublication(){return Active();}
void SessionRetiring(){if(Active())ended=true;}
void CloseBattle(){if(Active())ended=true;}
bool SimulateNative(){return !Active()||ended||!state.choices.holding;}
void InjectLocalInput(Pad::System::Inputs& input) {
    if(!Active()||ended||failed)return;
    if(input.mappedOn&cc::WireMask){Refuse();input={0,0};}
    if(localRefused) {input.mappedOn=(input.mappedOn&~cc::WireMask)|cc::Encode(state.choices.epoch,cc::No);return;}
    const auto confirmed=ConfirmedState();const auto& c=confirmed.state.choices;
    if(confirmed.frame<0||!c.holding)return;
    if(scoredGameReady!=c.epoch){input={0,0};return;}
    unsigned flags=cc::Ready;
    if(c.decision==cc::Decision::Rematch)flags|=cc::Yes|cc::Commit;
    else if(c.decision==cc::Decision::Lobby)flags|=cc::Commit;
    else if(localChoiceEpoch==c.epoch) {
        if(localChoice==quickrematch::Choice::Rematch)flags|=cc::Yes;
        if(localChoice==quickrematch::Choice::Lobby)flags|=cc::No;
    }
    input.mappedOn=cc::Encode(c.epoch,flags);input.rawOn=0;
}
quickrematch::View ChoiceView() {
    quickrematch::View view;
    if(!Active()||!bound||ended||failed||localRefused||fSystem::localPlayerHandle==GGPO_INVALID_HANDLE)return view;
    const auto confirmed=ConfirmedState();const auto& c=confirmed.state.choices;
    if(confirmed.frame<0||!c.holding||c.offerFrame<0||c.decision!=cc::Decision::Pending)return view;
    view.visible=true;view.offer=OfferToken(c.epoch);view.canRematch=!c.ruleLimit;
    view.accepted=localChoiceEpoch==c.epoch&&localChoice==quickrematch::Choice::Rematch;
    const int remaining=(std::max)(0,cc::TimeoutFrames-(confirmed.frame-c.offerFrame));
    view.status=loc::Tf("quick_rematch.time_remaining",(remaining+59)/60);
    if(c.ruleLimit)view.status=std::string(loc::T("quick_rematch.set_complete"))+" "+view.status;
    else if(view.accepted)view.status=std::string(loc::T("quick_rematch.accepted"))+" "+view.status;
    else if(c.yesMask)view.status=std::string(loc::T("quick_rematch.other_ready"))+" "+view.status;
    return view;
}
bool Choose(quickrematch::Choice choice,std::uint64_t offer) {
    const auto view=ChoiceView();
    if(!view.visible||view.offer!=offer||(choice!=quickrematch::Choice::Rematch&&choice!=quickrematch::Choice::Lobby)||
        (choice==quickrematch::Choice::Rematch&&!view.canRematch))return false;
    localChoiceEpoch=ConfirmedState().state.choices.epoch;localChoice=choice;return true;
}
} }
