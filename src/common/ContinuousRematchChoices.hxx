#pragma once
#include <cstdint>
#include <type_traits>

namespace sf4e { namespace continuousrematch { namespace choices {
// Private-room rematch votes occupy the upper byte of mappedOn and are stripped
// before native playback. Admission still requires an identical Sidecar hash.
constexpr std::uint32_t WireMask=0xff000000u;
enum Flag : unsigned { Ready=1, Yes=2, No=4, Commit=8 };
enum class Decision { Pending, Rematch, Lobby };
enum class Action { Hold, Restart };
constexpr int TimeoutFrames=1200;
constexpr int ResultWaitFrames=300;
// The native adapter asserts this against GGPO_MAX_PREDICTION_FRAMES. Hold
// past that window after the last Commit edge, so it cannot be speculative.
constexpr int PredictionWindow=8;
struct State {
    unsigned epoch=1,readyMask=0,yesMask=0,commitMask=0;
    int victoryFrame=-1,offerFrame=-1,decisionFrame=-1,commitFrame=-1,exitFrame=-1,nativeFinishFrame=-1;
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
// This is local input production, not simulated state. Once we originate a
// Commit its entire input word stays identical until the next epoch is
// confirmed. A later native/local observation cannot rewrite that consent, and
// the first gameplay input can only correct a frame after the reset.
struct CommittedInput {
    unsigned epoch=0;
    std::uint32_t mapped=0;
    void Set(unsigned game,unsigned flags) { epoch=game;mapped=Encode(game,flags); }
    bool Retain(unsigned confirmedEpoch,std::uint32_t& output) {
        if(!epoch)return false;
        if(confirmedEpoch>epoch){*this={};return false;}
        output=mapped;return true;
    }
};
inline void EnterVictory(State& s,int frame,bool ruleLimit=false) {
    if(s.holding)return;
    s.holding=true;s.victoryFrame=frame;s.ruleLimit=ruleLimit;
}
// The local adapter checks its restart context before originating Commit from
// confirmed consent. That byte then stays constant until the reset itself is
// confirmed. A delayed refusal before Commit can change this saved decision;
// the native reset waits past the prediction window before consuming it.
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
    if(s.commitFrame<0)s.commitFrame=frame;
    if(frame-s.commitFrame<=PredictionWindow)return Action::Hold;
    const auto epoch=s.epoch+1;
    s=State{};s.epoch=epoch;
    return Action::Restart;
}
} } }
