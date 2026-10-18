#include "sf4e__ContinuousRematch.hxx"
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include <array>
#include <filesystem>
#include <fstream>
#include <map>

namespace sf4e { namespace continuousrematch {
namespace {
using Native=rSystem;
using Json=nlohmann::json;
using Objects=std::array<std::uintptr_t,7>;
using Restart=void(__thiscall*)(Native*,void*,bool);
State state;
bool initialized=false,configured=false,bound=false,ended=false,failed=false,complete=false;
bool sessionEnabled=false;
std::uint64_t sessionSerial=0;
Native* battle=nullptr;
GGPOSession* session=nullptr;
std::uintptr_t base=0;
Objects objects{};
Restart restart=nullptr;
std::wstring path;
Json report;
std::map<int,Json> samples;
std::uint64_t startedMs=0,lastWriteMs=0,resetMs=0,roomEpoch=0,matchGeneration=0;
int tableIndex=-1,lastFrame=-1;
unsigned resetCalls=0,resetReplays=0,loads=0,crossingLoads=0,retirements=0;
namespace cc=continuouschoices;
struct History {int frame=-1;cc::State choices;int winner=-1;};
std::array<History,128> choiceHistory;
quickrematch::Choice localChoice=quickrematch::Choice::None;
unsigned localChoiceEpoch=0;
bool expectedExit=false;
std::uint64_t lastChoiceEvent=0;
unsigned scoredGameReady=0,setGoal=0;
constexpr unsigned MaxResets(){return UINT32_MAX-1;}
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
void Fail(const char* why) {
    if(!failed) {report["failure"]=why;spdlog::error("Continuous rematch: {}",why);}
    failed=true;report["status"]="failed";
}
int ConfirmedInput() {
    int frame=-1;
    return session&&session==fSystem::ggpo&&GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(session,&frame))?frame:-1;
}
History ConfirmedChoices() {
    History result;
    const int confirmed=ConfirmedInput();
    if(confirmed<0)return result;
    for(const auto& sample:choiceHistory)if(sample.frame>result.frame&&sample.frame<=confirmed+1)result=sample;
    return result;
}
std::uint64_t OfferToken(unsigned epoch){return (sessionSerial<<32)|epoch;}
void ReportFailure(const char* reason) {
    spdlog::warn("Continuous rematch: diagnostic report disabled ({})",reason);
    path.clear();
}
void Write() {
    if(path.empty())return;
    const int confirmed=ConfirmedInput();
    report["resetFrame"]=state.resetFrame;report["winnerBeforeReset"]=state.winner;
    report["resetCalls"]=resetCalls;report["resetReplays"]=resetReplays;
    report["loads"]=loads;report["resetCrossingLoads"]=crossingLoads;
    report["fightFrames"]=state.fightFrames;report["activeInputFrames"]=state.inputFrames;
    report["lastStateFrame"]=lastFrame;
    if(confirmed>=0)report["confirmedInputFrame"]=confirmed;
    report["retirements"]=retirements;report["sameSession"]=session&&session==fSystem::ggpo;
    report["localObservationComplete"]=complete;
    {
        report["expectedExit"]=expectedExit;
        report["choiceState"]={{"epoch",state.choices.epoch},{"resets",state.choices.resets},
            {"holding",state.choices.holding},{"offerFrame",state.choices.offerFrame},
            {"decision",static_cast<int>(state.choices.decision)},{"decisionFrame",state.choices.decisionFrame},
            {"winner",state.choiceWinner}};
        report["scoring"]={{"readyGame",scoredGameReady},{"goal",setGoal},
            {"score",{state.score[0],state.score[1]}},{"maxResets",MaxResets()}};
    }
    report["samples"]=Json::array();
    const int retainedConfirmed=report.value("confirmedInputFrame",-1);
    for(const auto& sample:samples)if(sample.first<=retainedConfirmed+1)report["samples"].push_back(sample.second);
    try {
        std::ofstream out(std::filesystem::path(path),std::ios::binary|std::ios::trunc);
        if(!out){ReportFailure("report-write-failed");return;}
        out<<report.dump(2)<<'\n';out.flush();
        if(!out)ReportFailure("report-write-failed");
    } catch(const std::exception&) {ReportFailure("report-write-exception");}
    lastWriteMs=GetTickCount64();
}
void Initialize() {
    initialized=true;
    wchar_t buffer[32768]{};const auto n=GetEnvironmentVariableW(L"SF4E_REMATCH_REPORT",buffer,32768);
    path.clear();
    if(n&&n<32768) {
        path.assign(buffer,n);
        if(sessionSerial) {
            const auto file=std::filesystem::path(path);
            path=(file.parent_path()/(file.stem().wstring()+L"-session-"+std::to_wstring(sessionSerial)+file.extension().wstring())).wstring();
        }
    }
    report={{"kind","continuous-rematch"},{"status","waiting-private-room"},{"failure",""},
        {"onlineRematchValidated",false},{"scoringEnabled",true},{"choicePanelEnabled",true},
        {"sidecarHash",sidecarHash},
        {"observationFrames",ObservationFrames},{"localObservationComplete",false}};
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const unsigned char reset[]={0x55,0x8b,0xec,0x56,0x8b,0xf1,0x83,0xbe,0xf8,0x0d,0x00,0x00,0x00};
    const unsigned char queue[]={0x55,0x8b,0xec,0x83,0xec,0x08,0x53,0x56,0x57};
    const unsigned char call[]={0xe8,0x8f,0xec,0xff,0xff};
    const unsigned char seed[]={0x80,0x7d,0x0c,0x00,0x74,0x24,0x8b,0xbe,0xd4,0x0c,0x00,0x00};
    const unsigned char victoryFinal[]={0x55,0x8b,0xec,0x56,0x8b,0x75,0x08,0x56,0xe8,0x33,0xf3,0xff,0xff};
    const bool layout=Signature(0x1db480,reset)&&Signature(0x1da220,queue)&&Signature(0x1db58c,call)&&
        Signature(0x1db540,seed)&&Signature(0x1dddc0,victoryFinal);
    const bool conflict=EnvFlag("SF4E_START_OFFLINE")||EnvFlag("SF4E_ROLLBACK_STRESS")||EnvFlag("SF4E_LEGACY_SAVESTATE_FREE");
    configured=layout&&!conflict&&sidecarHash.size()==64;
    report["nativeLayoutVerified"]=layout;
    if(configured)restart=reinterpret_cast<Restart>(base+0x1db480);
    else Fail("configuration-or-native-layout-refused");
    Write();
}
bool Bind(Native* system) {
    const auto snapshot=NetplayFacade::GetRuntimeSnapshotShared();
    if(!snapshot||snapshot->session.room!=netplay::RoomState::Joined||
        snapshot->session.gameplay!=netplay::Health::Healthy||!snapshot->room.roomEpoch||
        snapshot->room.practice||snapshot->room.serverOwned||snapshot->room.tournament.Active()||
        !NetplayFacade::IsRuntimeRoomActive()||replayplayback::GetView().playback||
        fSystem::simGate.nativeBattle!=gate::NATIVE_BATTLE_NETPLAY||!ReadObjects(system,objects)||
        (system->*Native::publicMethods.GetGameMode)()!=1||fSystem::nRandomizeLocalInputsEveryXFramesInGGPO)return false;
    int table=-1;
    if(snapshot->room.localMatchGenerationsSent)for(std::size_t i=0;i<snapshot->room.tables.size();++i)
        if(snapshot->room.localMatchGenerations[i]){if(table!=-1)return false;table=static_cast<int>(i);}
    if(table<0)return false;
    const auto& authority=snapshot->room.tables[table];
    if(authority.phase!=room::TablePhase::Playing||authority.matchGeneration!=snapshot->room.localMatchGenerations[table])return false;
    if(!authority.continuous.enabled)return false;
    {
        state.score[0]=authority.score[0];state.score[1]=authority.score[1];
        setGoal=static_cast<unsigned>(authority.rules.format);
        report["initialTable"]=authority;
    }
    if(!Accessible(reinterpret_cast<void*>(objects[1]),requestBytes.size()))return false;
    std::memcpy(requestBytes.data(),reinterpret_cast<void*>(objects[1]),requestBytes.size());
    std::memcpy(menuBytes.data(),reinterpret_cast<char*>(system)+0x6d8,menuBytes.size());
    battle=system;session=fSystem::ggpo;bound=true;startedMs=GetTickCount64();
    roomEpoch=snapshot->room.roomEpoch;matchGeneration=authority.matchGeneration;tableIndex=table;
    report["status"]="waiting-full-victory";report["roomEpoch"]=roomEpoch;report["table"]=table;
    report["matchGeneration"]=matchGeneration;report["localMember"]=snapshot->room.localMember;
    report["role"]=fSystem::localPlayerHandle==GGPO_INVALID_HANDLE?"spectator":"fighter";
    report["sessionPointer"]=reinterpret_cast<std::uintptr_t>(session);
    report["objects"]=objects;report["bindTickMs"]=startedMs;
    spdlog::info("Continuous rematch: bound table={} match={}",table,matchGeneration);
    return true;
}
bool Context(Native* system) {
    Objects current{};
    const auto refuse=[&](const char* why){
        if(!report.contains("contextFailure"))report["contextFailure"]={{"reason",why},{"stateFrame",lastFrame},
            {"flow",*Native::staticVars.CurrentBattleFlow},{"substate",*Native::staticVars.CurrentBattleFlowSubstate},
            {"ready",Accessible(system,0xe44)?*Native::GetReadyState(system):-1}};
        return false;
    };
    if(system!=battle)return refuse("native-system-changed");
    if(session!=fSystem::ggpo)return refuse("ggpo-session-changed");
    if(!ReadObjects(system,current)||current!=objects)return refuse("native-objects-changed");
    if(!Accessible(reinterpret_cast<void*>(objects[1]),requestBytes.size()))return refuse("request-unreadable");
    if(std::memcmp(requestBytes.data(),reinterpret_cast<void*>(objects[1]),requestBytes.size()))return refuse("request-bytes-changed");
    if(std::memcmp(menuBytes.data(),reinterpret_cast<char*>(system)+0x6d8,menuBytes.size()))return refuse("menu-bytes-changed");
    const auto snapshot=NetplayFacade::GetRuntimeSnapshotShared();
    // Roster changes (especially spectator departure) do not invalidate this
    // fighter/table generation. The original transport handles disconnects.
    const bool same=snapshot&&snapshot->room.roomEpoch==roomEpoch&&tableIndex>=0&&
        static_cast<std::size_t>(tableIndex)<snapshot->room.tables.size()&&
        snapshot->room.tables[tableIndex].matchGeneration==matchGeneration;
    return same||refuse("room-table-generation-changed");
}
int NextFrame() {
    return fSystem::localPlayerHandle==GGPO_INVALID_HANDLE?statehash::StateFrameAfterInput(ConfirmedInput()):fSystem::ggpoTimeline.Next();
}
} // namespace

void BeginSession() {
    // New transport generation (including a former spectator taking a seat).
    // No state from the previous set may enter this GGPO save/rollback pool.
    state=State{};bound=ended=failed=complete=false;sessionEnabled=false;
    battle=nullptr;session=nullptr;objects={};samples.clear();choiceHistory={};
    localChoice=quickrematch::Choice::None;localChoiceEpoch=0;expectedExit=false;
    lastChoiceEvent=0;scoredGameReady=setGoal=0;tableIndex=lastFrame=-1;
    startedMs=lastWriteMs=resetMs=roomEpoch=matchGeneration=0;
    resetCalls=resetReplays=loads=crossingLoads=retirements=0;
    requestBytes={};menuBytes={};++sessionSerial;
    room::Table initialTable;
    sessionEnabled=NetplayFacade::GetRuntimeContinuousTable(initialTable);
    if(sessionEnabled) {
        Initialize();
        // GGPO may save frame zero before Bind. Include carried scores in
        // that very first snapshot as well, including a partially played FT.
        state.score[0]=initialTable.score[0];state.score[1]=initialTable.score[1];
        setGoal=static_cast<unsigned>(initialTable.rules.format);
    }
    report["sessionSerial"]=sessionSerial;report["continuousSession"]=sessionEnabled;
    spdlog::info("Continuous rematch: new session={} enabled={}",sessionSerial,sessionEnabled);
}

void Poll() {
    if(!Active())return;
    if(bound&&!ended&&!failed) {
        {
            const auto snapshot=NetplayFacade::GetRuntimeSnapshotShared();
            if(snapshot&&snapshot->room.roomEpoch==roomEpoch&&tableIndex>=0) {
                const auto& table=snapshot->room.tables[tableIndex];
                if(table.matchGeneration==matchGeneration&&table.continuous.enabled&&table.continuous.closed) {
                    expectedExit=table.continuous.finishMask==3;
                    report["status"]=expectedExit?"scored-return-to-lobby":"scored-series-ended-externally";
                    report["finalTable"]=table;
                    Write();fSystem::RetireGgpoSession("continuous-series-finished");return;
                }
                if(table.phase==room::TablePhase::Paused)Fail("continuous-result-disputed-or-timed-out");
            }
        }
        if(!Context(battle))Fail("battle-or-session-context-changed");
        else if(!complete&&Confirmed(state,ConfirmedInput())) {
            complete=true;report["status"]="local-observation-complete-awaiting-peer-comparison";
            report["observationCompleteTickMs"]=GetTickCount64();
            report["retirementsAtCompletion"]=retirements;
            report["sameSessionAtCompletion"]=session==fSystem::ggpo;
            spdlog::info("Continuous rematch: 600 frames confirmed without session retirement");
        }
        if(!failed) {
            const auto confirmed=ConfirmedChoices();const auto& c=confirmed.choices;
            if(confirmed.frame>=0&&c.holding&&confirmed.winner>=0&&confirmed.winner<=1&&
                fSystem::localPlayerHandle!=GGPO_INVALID_HANDLE) {
                const auto outcome=confirmed.winner==0?room::MatchResult::P1Win:room::MatchResult::P2Win;
                if(NetplayFacade::RecordRuntimeContinuousResult(static_cast<std::uint8_t>(tableIndex),matchGeneration,
                    c.resets+1,outcome,c.victoryFrame,ConfirmedInput())&&scoredGameReady!=c.resets+1) {
                    scoredGameReady=c.resets+1;
                    if(report["scoredGames"].size()>=64)report["scoredGames"].erase(report["scoredGames"].begin());
                    report["scoredGames"].push_back({{"game",scoredGameReady},{"winner",confirmed.winner},
                        {"victoryFrame",c.victoryFrame},{"confirmedStateFrame",confirmed.frame}});
                }
            }
            const auto eventKey=(static_cast<std::uint64_t>(c.epoch)<<32)|
                (static_cast<unsigned>(c.decision)<<16)|(c.readyMask<<8)|c.yesMask;
            if(confirmed.frame>=0&&c.holding&&eventKey!=lastChoiceEvent) {
                lastChoiceEvent=eventKey;
                if(report["choiceEvents"].size()<32)report["choiceEvents"].push_back({{"epoch",c.epoch},
                    {"stateFrame",confirmed.frame},{"offerFrame",c.offerFrame},{"decisionFrame",c.decisionFrame},
                    {"decision",static_cast<int>(c.decision)},{"readyMask",c.readyMask},{"yesMask",c.yesMask},
                    {"timedOut",c.timedOut},{"tickMs",GetTickCount64()}});
            }
            if(confirmed.frame>=0&&c.holding&&c.decision==cc::Decision::Lobby&&
                c.exitFrame>=0&&confirmed.frame>=c.exitFrame+16&&!expectedExit) {
                expectedExit=true;report["status"]="confirmed-return-to-lobby";
                report["exitDecisionFrame"]=c.decisionFrame;report["exitEpoch"]=c.epoch;
                report["exitCommitFrame"]=c.exitFrame;report["exitConfirmedStateFrame"]=confirmed.frame;
                report["exitCommitMask"]=c.commitMask;report["sameSessionAtExit"]=session==fSystem::ggpo;
                report["retirementsAtExit"]=retirements;
                report["exitTimedOut"]=c.timedOut;Write();
            }
            if(expectedExit&&confirmed.frame>=0&&c.holding&&c.exitFrame>=0)
                NetplayFacade::FinishRuntimeContinuousMatch(static_cast<std::uint8_t>(tableIndex),matchGeneration,c.resets+1);
        }
    }
    if(GetTickCount64()-lastWriteMs>=1000)Write();
    if(failed&&!ended&&fSystem::ggpo) {
        Write();fSystem::AbortGgpoMatch(loc::T("quick_rematch.failed"));
    }
}
bool BeforeFrame(Native* system,Pad::System::Inputs inputs[2],bool replay) {
    if(!Active())return true;
    if(!initialized||!configured||ended||failed||(!bound&&!Bind(system))||!Context(system)) {
        Fail("online-frame-context-refused");
        fSystem::AbortGgpoMatch(loc::T("quick_rematch.context_invalid"));return false;
    }
    auto* manager=(system->*Native::publicMethods.GetGameManager)();
    if(!Accessible(manager,sizeof(*manager))){Fail("game-manager-unreadable");fSystem::AbortGgpoMatch(loc::T("quick_rematch.interrupted"));return false;}
    const auto callback=reinterpret_cast<std::uintptr_t>(*Native::staticVars.BattleFlowSubstateCallable_aa9258);
    const auto flow=*Native::staticVars.CurrentBattleFlow;
    const auto substate=*Native::staticVars.CurrentBattleFlowSubstate;
    const int winner=(manager->*GameManager::publicMethods.GetNativeResultIndex)();
    const bool canRestart=*Native::GetReadyState(system)==Native::RS_FIGHT&&*Native::GetTransitionType(system)==0&&
        !(*Native::GetSimulationFlags(system)&Native::SSF_PAUSE_OR_HOLD);
    Action action=Action::Continue;
    if(flow==Native::BF__DRAW_RESULT) {
        // The continuous native draw screen has not been validated yet. A
        // draw awards no point; use the existing bounded abort path, keeping
        // earlier committed games, instead of leaving an unhandled panel.
        Fail("draw-native-rematch-not-yet-validated");
        fSystem::AbortGgpoMatch(loc::T("quick_rematch.draw"),NoticeSeverity::Info);return false;
    }
    {
        const std::uint32_t wire[2]={inputs[0].mappedOn,inputs[1].mappedOn};
        inputs[0].mappedOn&=~cc::WireMask;inputs[1].mappedOn&=~cc::WireMask;
        if(!state.choices.holding&&flow==Native::BF__MATCH_RESULT) {
            if(substate>=6||static_cast<std::uint32_t>(callback-base)!=VictoryCallbackRvas[substate]||
                ++state.victoryFrames>1800||winner<0||winner>1)action=Action::Refuse;
            else {
                action=Action::NeutralVictory;
                if(substate==5&&canRestart){
                    if(state.score[winner]==UINT32_MAX){Fail("score-overflow-refused");return false;}
                    ++state.score[winner];
                    cc::EnterVictory(state.choices,NextFrame(),setGoal&&state.score[winner]>=setGoal);
                    state.choiceWinner=winner;
                }
            }
        }
        if(state.choices.holding) {
            if(flow!=Native::BF__MATCH_RESULT||substate!=5||!canRestart||
                static_cast<std::uint32_t>(callback-base)!=VictoryCallbackRvas[5]) {
                Fail("held-victory-state-changed");fSystem::AbortGgpoMatch(loc::T("quick_rematch.victory_changed"));return false;
            }
            const auto decision=cc::Advance(state.choices,NextFrame(),wire,MaxResets());
            action=Action::NeutralVictory;
            if(decision==cc::Action::Restart) {
                action=Action::Restart;state.resetFrame=NextFrame();state.winner=state.choiceWinner;state.victoryFrames=0;
            }
        }
    }
    if(action==Action::Refuse){Fail("native-victory-sequence-refused");fSystem::AbortGgpoMatch(loc::T("quick_rematch.victory_unknown"));return false;}
    if(action==Action::NeutralVictory||action==Action::Restart)inputs[0]=inputs[1]={0,0};
    if(action==Action::Restart) {
        ++resetCalls;if(replay)++resetReplays;
        if(!resetMs)resetMs=GetTickCount64();
        report["firstResetTickMs"]=resetMs;report["status"]="observing-continuous-rematch";
        // Same original request/menu seed on every participant and on replay.
        // Do not recreate GGPO, reset its frame counter, or fence spectators.
        restart(system,nullptr,true);
    }
    return true;
}
void AfterFrame(Native* system,int frame,const Pad::System::Inputs inputs[2]) {
    if(!Active()||!bound||ended||failed)return;
    lastFrame=frame;
    choiceHistory[frame%choiceHistory.size()]={frame,state.choices,state.choiceWinner};
    if(state.resetFrame<0)return;
    const int elapsed=frame-state.resetFrame;
    if(elapsed<0||elapsed>ObservationFrames)return;
    if(*Native::staticVars.CurrentBattleFlow==Native::BF__FIGHT) {
        ++state.fightFrames;
        if(inputs[0].mappedOn||inputs[0].rawOn||inputs[1].mappedOn||inputs[1].rawOn)++state.inputFrames;
    }
    if(!path.empty()&&(elapsed<16||elapsed%30==0)) {
        const auto hashes=fSystem::ComputeSemanticHashes(system);
        while(samples.size()>=144)samples.erase(samples.begin());
        samples[frame]={{"stateFrame",frame},{"relativeFrame",elapsed},
            {"flow",*Native::staticVars.CurrentBattleFlow},{"substate",*Native::staticVars.CurrentBattleFlowSubstate},
            {"engineFrame",Native::GetNumFramesSimulated_FixedPoint(system)->integral},
            // Decimal strings preserve all 64 bits in PS5.1/JSON readers.
            {"hashes",{std::to_string(hashes.overall),std::to_string(hashes.flow),
                std::to_string(hashes.chara[0]),std::to_string(hashes.chara[1])}},
            {"p1",fSystem::CaptureCharaSemantics(system,0).v},{"p2",fSystem::CaptureCharaSemantics(system,1).v},
            {"inputs",{inputs[0].mappedOn,inputs[0].rawOn,inputs[1].mappedOn,inputs[1].rawOn}}};
    }
}
State Capture(){return state;}
void Restore(const State& saved,int frame) {
    if(!Active()||!bound||ended)return;
    ++loads;if(state.resetFrame>=0&&saved.resetFrame<0)++crossingLoads;
    state=saved;lastFrame=frame;
    for(auto& sample:choiceHistory)if(sample.frame>frame)sample=History{};
    samples.erase(samples.upper_bound(frame),samples.end());
}
bool DeferResultPublication() {
    // Each confirmed game is committed through RecordContinuousResult.
    // Suppress the legacy single-result path through native teardown too.
    return Active();
}
void SessionRetiring() {
    if(!Active()||!initialized)return;
    // A refused first frame has no binding yet. Retirement is outside GGPO
    // callbacks: flush that refusal here too, before native updates stop.
    if(!bound){Write();return;}
    if(session!=fSystem::ggpo)return;
    ++retirements;ended=true;report["sessionRetiredTickMs"]=GetTickCount64();
    if(!complete&&!failed&&!expectedExit)report["status"]="ended-before-observation-complete";
    Write();
}
void CloseBattle() {
    if(!Active()||!bound)return;
    report["nativeCloseTickMs"]=GetTickCount64();Write();ended=true;
}
bool SimulateNative(){return !Active()||!bound||ended||!state.choices.holding;}
void InjectLocalInput(Pad::System::Inputs& input) {
    if(!Active()||!bound||ended||failed)return;
    if(input.mappedOn&cc::WireMask){Fail("native-input-overlaps-control-byte");input={0,0};return;}
    const auto confirmed=ConfirmedChoices();const auto& c=confirmed.choices;
    if(confirmed.frame<0||!c.holding)return;
    if(scoredGameReady!=c.resets+1){input={0,0};return;}
    unsigned flags=cc::Ready;
    if(c.decision==cc::Decision::Rematch)flags|=cc::Yes|cc::Commit;
    else if(c.decision==cc::Decision::Lobby)flags|=cc::Commit;
    else if(c.decision==cc::Decision::Pending&&localChoiceEpoch==c.epoch) {
        if(localChoice==quickrematch::Choice::Rematch)flags|=cc::Yes;
        if(localChoice==quickrematch::Choice::Lobby)flags|=cc::No;
    }
    input.mappedOn=cc::Encode(c.epoch,flags);input.rawOn=0;
}
quickrematch::View ChoiceView() {
    quickrematch::View view;
    if(!Active()||!bound||ended||failed||fSystem::localPlayerHandle==GGPO_INVALID_HANDLE)return view;
    const auto confirmed=ConfirmedChoices();const auto& c=confirmed.choices;
    if(confirmed.frame<0||!c.holding||c.offerFrame<0||c.decision!=cc::Decision::Pending)return view;
    view.visible=true;view.offer=OfferToken(c.epoch);view.canRematch=!c.ruleLimit&&c.resets<(MaxResets());
    view.accepted=localChoiceEpoch==c.epoch&&localChoice==quickrematch::Choice::Rematch;
    const int remaining=(std::max)(0,cc::TimeoutFrames-(confirmed.frame-c.offerFrame));
    view.status=loc::Tf("quick_rematch.time_remaining",(remaining+59)/60);
    if(c.ruleLimit)view.status=std::string(loc::T("quick_rematch.set_complete"))+" "+view.status;
    else if(!view.canRematch)view.status=std::string(loc::T("quick_rematch.set_complete"))+" "+view.status;
    else if(view.accepted)view.status=std::string(loc::T("quick_rematch.accepted"))+" "+view.status;
    else if(c.yesMask)view.status=std::string(loc::T("quick_rematch.other_ready"))+" "+view.status;
    return view;
}
bool Choose(quickrematch::Choice choice,std::uint64_t offer) {
    const auto view=ChoiceView();
    if(!view.visible||view.offer!=offer||(choice!=quickrematch::Choice::Rematch&&choice!=quickrematch::Choice::Lobby)||
        (choice==quickrematch::Choice::Rematch&&!view.canRematch))return false;
    const auto confirmed=ConfirmedChoices();localChoiceEpoch=confirmed.choices.epoch;localChoice=choice;
    return true;
}
} }
