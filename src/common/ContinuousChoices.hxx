#pragma once
#include <cstdint>
#include <type_traits>

namespace sf4e { namespace continuouschoices {
// Private-room rematch votes occupy the upper byte of mappedOn and are stripped
// before native playback. Admission still requires an identical Sidecar hash.
constexpr std::uint32_t WireMask=0xff000000u;
enum Flag : unsigned { Ready=1, Yes=2, No=4, Commit=8 };
enum class Decision { Pending, Rematch, Lobby };
enum class Action { Hold, Restart };
constexpr int TimeoutFrames=1200;
constexpr int ResultWaitFrames=300;
struct State {
    unsigned epoch=1,readyMask=0,yesMask=0,commitMask=0;
    int victoryFrame=-1,offerFrame=-1,decisionFrame=-1,exitFrame=-1,nativeFinishFrame=-1;
    Decision decision=Decision::Pending;
    bool holding=false,timedOut=false,ruleLimit=false,nativeFinish=false;
};
static_assert(std::is_trivially_copyable<State>::value,"Saved with native GGPO state");
inline unsigned WireEpoch(unsigned epoch) { return epoch ? (epoch-1)%15+1 : 0; }
inline std::uint32_t Encode(unsigned epoch,unsigned flags) {
    return epoch&&flags<=15?static_cast<std::uint32_t>((WireEpoch(epoch)<<4)|flags)<<24:0;
}
inline unsigned Decode(std::uint32_t mapped,unsigned epoch) {
    const unsigned byte=mapped>>24;
    return epoch&&(byte>>4)==WireEpoch(epoch)?(byte&15):0;
}
inline void EnterVictory(State& s,int frame,bool ruleLimit=false) {
    if(s.holding)return;
    s.holding=true;s.victoryFrame=frame;s.ruleLimit=ruleLimit;
}
// Frame-driven decisions are rollback state. Confirmation is used only by the
// local adapter to originate Ready/Commit or perform an irreversible exit.
// A commit is transmitted only after BOTH Yes votes belong to the confirmed
// prefix. Prediction can repeat that commit but cannot invent its first edge.
// An opt-out is carried by the original input stream, including before the
// first result. It cannot wait for the room connection whose failure caused it.
inline void FinishNative(State& s,int frame) {
    if(!s.nativeFinish)s.nativeFinishFrame=frame;
    s.nativeFinish=true;s.holding=false;
}
inline void ObserveFallback(State& s,int frame,const std::uint32_t mapped[2]) {
    for(unsigned i=0;i<2;++i)if(Decode(mapped[i],s.epoch)==No)FinishNative(s,frame);
}
inline Action Advance(State& s,int frame,const std::uint32_t mapped[2]) {
    ObserveFallback(s,frame,mapped);
    if(!s.holding)return Action::Hold;
    unsigned flags[2]={Decode(mapped[0],s.epoch),Decode(mapped[1],s.epoch)};
    for(unsigned i=0;i<2;++i)if(flags[i]&Ready)s.readyMask|=1u<<i;
    if(s.offerFrame<0&&s.readyMask==3)s.offerFrame=frame;
    if(s.offerFrame<0) {
        if(frame-s.victoryFrame>=ResultWaitFrames)FinishNative(s,frame);
        return Action::Hold;
    }
    if(s.decision==Decision::Pending) {
        bool no=false;
        for(unsigned i=0;i<2;++i)if(flags[i]&Ready) {
            no=no||bool(flags[i]&No);
            if(flags[i]&Yes)s.yesMask|=1u<<i;
        }
        s.timedOut=frame-s.offerFrame>=TimeoutFrames;
        if(no||s.timedOut||(s.ruleLimit&&s.yesMask==3))s.decision=Decision::Lobby;
        else if(s.yesMask==3)s.decision=Decision::Rematch;
        if(s.decision!=Decision::Pending)s.decisionFrame=frame;
    }
    if(s.decision==Decision::Pending)return Action::Hold;
    for(unsigned i=0;i<2;++i)if((flags[i]&(Ready|Commit))==(Ready|Commit))s.commitMask|=1u<<i;
    if(s.commitMask!=3)return Action::Hold;
    if(s.decision==Decision::Lobby){if(s.exitFrame<0)s.exitFrame=frame;return Action::Hold;}
    const auto epoch=s.epoch+1;
    s=State{};s.epoch=epoch;
    return Action::Restart;
}
} }
