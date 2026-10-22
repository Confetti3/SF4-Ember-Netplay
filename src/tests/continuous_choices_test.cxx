#include "../common/ContinuousRematchChoices.hxx"
#include "test_support.hxx"
#include "../common/ButtonLayoutTypes.hxx"
#include "../common/GgpoTimeline.hxx"
#include <array>
#include <cstdio>
namespace cc=sf4e::continuousrematch::choices;
cc::Action Frame(cc::State& s,int frame,unsigned a,unsigned b) {
    const std::uint32_t wire[2]={cc::Encode(s.epoch,a)|0x123456,cc::Encode(s.epoch,b)|0x654321};
    return cc::Advance(s,frame,wire);
}
cc::Action CommitBarrier(cc::State& s,int firstCommit) {
    for(int offset=0;offset<=cc::PredictionWindow;++offset) {
        CHECK(Frame(s,firstCommit+offset,cc::Ready|cc::Commit,cc::Ready|cc::Commit)==cc::Action::Hold);
        CHECK(s.commitFrame==firstCommit&&s.holding);
    }
    return Frame(s,firstCommit+cc::PredictionWindow+1,cc::Ready|cc::Commit,cc::Ready|cc::Commit);
}
void DelayedControlAndRollback() {
    for(int delay=0;delay<=cc::PredictionWindow;++delay) {
        // Only the second Commit starts the barrier, even if the other
        // input has repeated its Commit for the whole prediction window.
        cc::State staggered;cc::EnterVictory(staggered,1);
        Frame(staggered,2,cc::Ready|cc::Yes,cc::Ready|cc::Yes);
        for(int frame=3;frame<3+delay;++frame) {
            CHECK(Frame(staggered,frame,cc::Ready|cc::Commit,cc::Ready|cc::Yes)==cc::Action::Hold);
            CHECK(staggered.commitFrame<0&&staggered.epoch==1);
        }
        const auto savedCommit=staggered;
        CHECK(CommitBarrier(staggered,3+delay)==cc::Action::Restart);
        CHECK(staggered.epoch==2&&!staggered.holding);
        auto replay=savedCommit;
        CHECK(CommitBarrier(replay,3+delay)==cc::Action::Restart&&replay.epoch==staggered.epoch);

        // The peer's real No can correct a predicted Yes, including the
        // latest permitted correction before the barrier expires.
        cc::State predicted;cc::EnterVictory(predicted,1);
        Frame(predicted,2,cc::Ready,cc::Ready);
        const auto beforeVote=predicted;
        Frame(predicted,3,cc::Ready|cc::Yes,cc::Ready|cc::Yes);
        for(int frame=4;frame<4+delay;++frame)
            CHECK(Frame(predicted,frame,cc::Ready|cc::Commit,cc::Ready|cc::Commit)==cc::Action::Hold);
        CHECK(predicted.epoch==1&&predicted.decision==cc::Decision::Rematch);
        predicted=beforeVote;
        Frame(predicted,3,cc::Ready|cc::Yes,cc::Ready|cc::No);
        CHECK(predicted.decision==cc::Decision::Lobby);
        for(int frame=4;frame<=4+cc::PredictionWindow+1;++frame)
            CHECK(Frame(predicted,frame,cc::Ready|cc::Commit,cc::Ready|cc::Commit)==cc::Action::Hold);
        CHECK(predicted.epoch==1&&predicted.exitFrame==4);

        // A peer with no native adapter may replace a predicted Commit by
        // the stream's opt-out. Restore the saved simulation before replay.
        auto refused=savedCommit;
        for(int frame=3+delay;frame<=3+delay+cc::PredictionWindow;++frame)
            CHECK(Frame(refused,frame,cc::Ready|cc::Commit,cc::Ready|cc::Commit)==cc::Action::Hold);
        refused=savedCommit;
        CHECK(Frame(refused,3+delay,cc::Ready|cc::Commit,cc::No)==cc::Action::Hold);
        CHECK(refused.nativeFinish&&!refused.holding&&refused.epoch==1);
        CHECK(refused.nativeFinishFrame==3+delay);
        CHECK(Frame(refused,4+delay+cc::PredictionWindow,cc::Ready|cc::Commit,cc::Ready|cc::Commit)==cc::Action::Hold);
    }
}
void CommittedTransmission() {
    for(unsigned epoch=1;epoch<=80;++epoch) {
        cc::CommittedInput input;
        std::uint32_t output=0x123456;
        CHECK(!input.Retain(epoch,output)&&output==0x123456);
        input.Set(epoch,cc::Ready|cc::Commit);
        const auto committed=cc::Encode(epoch,cc::Ready|cc::Commit);
        // New local buttons or a refusal cannot alter the transmitted word,
        // even after a speculative reset, until the new epoch is confirmed.
        for(int delay=0;delay<=cc::PredictionWindow+1;++delay) {
            output=cc::Encode(epoch,cc::No)|0x654321;
            CHECK(input.Retain(epoch,output)&&output==committed);
            CHECK((output&~cc::WireMask)==0);
        }
        output=0x123456;
        CHECK(!input.Retain(epoch+1,output)&&output==0x123456);
        CHECK(!input.epoch&&!input.mapped);
        // Unlocking compares the full epoch, not its repeated wire tag.
        input.Set(epoch,cc::Ready|cc::Commit);
        CHECK(cc::WireEpoch(epoch)==cc::WireEpoch(epoch+15));
        CHECK(!input.Retain(epoch+15,output));
    }
}
void TerminalDisconnectCannotRewindNativeReset() {
    sf4e::statehash::GgpoTimeline timeline;
    timeline.Saved(109); // The restart has produced native state 109.
    unsigned nativeWrites=0;
    const auto restore=[&] {++nativeWrites;return true;};
    // GGPO's terminal-disconnect path adjusts to the last remote frame before
    // reporting the event, even if connected peers retained Commit perfectly.
    CHECK(!timeline.Load(101,restore,109));
    CHECK(nativeWrites==0&&timeline.Held()==109);
    CHECK(!timeline.Load(108,restore,109)&&nativeWrites==0);
    // Buffers produced by the restart and later ordinary rollback stay usable.
    CHECK(timeline.Load(109,restore,109)&&nativeWrites==1);
    timeline.Saved(117);
    CHECK(timeline.Load(111,restore,109)&&nativeWrites==2&&timeline.Held()==111);
    // A subsequent rematch raises the unsaved boundary; restoring the saved
    // vote state must not reopen a previous native owner's lifetime.
    timeline.Saved(200);
    CHECK(!timeline.Load(199,restore,200)&&nativeWrites==2&&timeline.Held()==200);
    // The default has no reset boundary, preserving ordinary matches.
    CHECK(timeline.Load(4,restore)&&nativeWrites==3&&timeline.Held()==4);
}
int main() {
    static_assert((sf4e::gamepads::FightMask & cc::WireMask)==0,"Controller layouts overlap rematch votes");
    for(unsigned epoch=1;epoch<=15;++epoch)for(unsigned flags=0;flags<16;++flags) {
        const auto wire=cc::Encode(epoch,flags)|0x123456;
        CHECK(cc::Decode(wire,epoch)==flags);CHECK((wire&~cc::WireMask)==0x123456);
        CHECK(cc::Decode(wire,epoch%15+1)==0);
    }
    CHECK(!cc::Encode(0,1)&&!cc::Encode(1,16));
    CHECK(cc::Encode(16,1)==cc::Encode(1,1));
    cc::State s;cc::EnterVictory(s,100);
    CHECK(Frame(s,101,cc::Ready,0)==cc::Action::Hold&&s.offerFrame<0);
    CHECK(Frame(s,102,cc::Ready,cc::Ready)==cc::Action::Hold&&s.offerFrame==102);
    CHECK(Frame(s,103,cc::Ready|cc::Yes,cc::Ready)==cc::Action::Hold);
    CHECK(Frame(s,104,cc::Ready|cc::Yes,cc::Ready|cc::Yes)==cc::Action::Hold);
    CHECK(s.decision==cc::Decision::Rematch&&s.decisionFrame==104);
    const auto beforeCommit=s;
    CHECK(Frame(s,105,cc::Ready|cc::Commit,cc::Ready|cc::Yes)==cc::Action::Hold);
    const auto beforeReset=s;
    CHECK(CommitBarrier(s,106)==cc::Action::Restart);
    CHECK(!s.holding&&s.epoch==2&&s.offerFrame<0);
    s=beforeReset;
    CHECK(CommitBarrier(s,106)==cc::Action::Restart);
    // Old epoch's predicted/late votes cannot answer the next result.
    cc::EnterVictory(s,200,true);
    std::uint32_t stale[2]={cc::Encode(1,15),cc::Encode(1,15)};
    CHECK(cc::Advance(s,201,stale)==cc::Action::Hold&&s.readyMask==0);
    CHECK(Frame(s,202,cc::Ready|cc::Yes,cc::Ready|cc::Yes)==cc::Action::Hold);
    CHECK(s.ruleLimit&&s.decision==cc::Decision::Lobby);
    // No wins simultaneous Yes/No; one decline is sufficient.
    s={};cc::EnterVictory(s,10);
    CHECK(Frame(s,11,cc::Ready|cc::Yes,cc::Ready|cc::No)==cc::Action::Hold);
    CHECK(s.decision==cc::Decision::Lobby);
    CHECK(s.exitFrame<0);
    Frame(s,12,cc::Ready|cc::Commit,cc::Ready);CHECK(s.exitFrame<0);
    Frame(s,13,cc::Ready|cc::Commit,cc::Ready|cc::Commit);CHECK(s.exitFrame==13);
    s={};cc::EnterVictory(s,1);Frame(s,2,cc::Ready,cc::Ready);
    Frame(s,1201,cc::Ready,cc::Ready);CHECK(s.decision==cc::Decision::Pending);
    Frame(s,1202,cc::Ready,cc::Ready);CHECK(s.timedOut&&s.decision==cc::Decision::Lobby);
    // Rollback discards a speculative No and agrees with the real input tape.
    s=beforeCommit;auto reference=s;
    Frame(s,105,cc::Ready|cc::Commit,cc::Ready);CHECK(s.holding);
    s=beforeCommit;
    const auto a=CommitBarrier(s,105);
    const auto b=CommitBarrier(reference,105);
    CHECK(a==b&&a==cc::Action::Restart&&s.epoch==reference.epoch);
    // A corrected vote before agreement can replace a predicted acceptance.
    cc::State vote;cc::EnterVictory(vote,1);Frame(vote,2,cc::Ready,cc::Ready);const auto saved=vote;
    Frame(vote,3,cc::Ready|cc::Yes,cc::Ready|cc::Yes);CHECK(vote.decision==cc::Decision::Rematch);
    vote=saved;Frame(vote,3,cc::Ready|cc::Yes,cc::Ready|cc::No);CHECK(vote.decision==cc::Decision::Lobby);
    // Scored mode can continue through a set, but the deterministic set
    // limit defeats even two stale/forged Yes votes at its deciding game.
    cc::State scored;
    for(unsigned game=1;game<=3;++game) {
        cc::EnterVictory(scored,1000*game,game==3);
        std::uint32_t yes[2]={cc::Encode(scored.epoch,cc::Ready|cc::Yes),cc::Encode(scored.epoch,cc::Ready|cc::Yes)};
        CHECK(cc::Advance(scored,1000*game+1,yes)==cc::Action::Hold);
        CHECK(scored.decision==(game==3?cc::Decision::Lobby:cc::Decision::Rematch));
        auto savedScored=scored;
        std::uint32_t commit[2]={cc::Encode(scored.epoch,cc::Ready|cc::Commit),cc::Encode(scored.epoch,cc::Ready|cc::Commit)};
        const auto decision=game==3?cc::Advance(scored,1000*game+2,commit):CommitBarrier(scored,1000*game+2);
        auto replay=savedScored;
        CHECK((game==3?cc::Advance(replay,1000*game+2,commit):CommitBarrier(replay,1000*game+2))==decision);
        CHECK(replay.epoch==scored.epoch);
        CHECK(decision==(game==3?cc::Action::Hold:cc::Action::Restart));
    }
    CHECK(scored.epoch==3&&scored.exitFrame==3002&&scored.ruleLimit);
    // More than ten rematches and several wire-tag wraps, with rollback at
    // every reset. A full epoch remains distinct for local choices/UI offers.
    cc::State longSet;
    for(unsigned game=1;game<=80;++game) {
        CHECK(longSet.epoch==game);
        cc::EnterVictory(longSet,game*100);
        if(game>1) {
            const std::uint32_t old[2]={cc::Encode(game-1,15),cc::Encode(game-1,15)};
            CHECK(cc::Advance(longSet,game*100,old)==cc::Action::Hold);
            CHECK(longSet.readyMask==0&&longSet.offerFrame<0);
        }
        const std::uint32_t yes[2]={cc::Encode(game,cc::Ready|cc::Yes),cc::Encode(game,cc::Ready|cc::Yes)};
        CHECK(cc::Advance(longSet,game*100+1,yes)==cc::Action::Hold);
        CHECK(longSet.decision==cc::Decision::Rematch&&!longSet.ruleLimit);
        auto rollback=longSet;
        CHECK(CommitBarrier(longSet,game*100+2)==cc::Action::Restart);
        CHECK(CommitBarrier(rollback,game*100+2)==cc::Action::Restart);
        CHECK(rollback.epoch==longSet.epoch);
    }
    // A room loss before either/both Ready bits cannot hold a result forever.
    for(unsigned ready : {0u, unsigned(cc::Ready)}) {
        cc::State waiting; cc::EnterVictory(waiting,100);
        Frame(waiting,399,ready,0); CHECK(waiting.holding && !waiting.nativeFinish);
        const auto rewind=waiting;
        Frame(waiting,400,ready,0);
        CHECK(waiting.nativeFinish && !waiting.holding && waiting.offerFrame<0 && waiting.nativeFinishFrame==400);
        auto replay=rewind; Frame(replay,400,ready,0); CHECK(replay.nativeFinishFrame==waiting.nativeFinishFrame);
    }
    // A delayed confirmation just inside the bound still shows the offer.
    cc::State late; cc::EnterVictory(late,100); Frame(late,399,cc::Ready,cc::Ready);
    Frame(late,401,cc::Ready,cc::Ready); CHECK(late.holding && !late.nativeFinish && late.offerFrame==399);
    // A Bind refusal travels through the input stream, even before a result.
    cc::State refused; const std::uint32_t optOut[2]={cc::Encode(1,cc::No),0};
    const auto beforeRefusal=refused; cc::ObserveFallback(refused,7,optOut); CHECK(refused.nativeFinish);
    refused=beforeRefusal; const std::uint32_t corrected[2]={0,0};
    cc::ObserveFallback(refused,7,corrected); CHECK(!refused.nativeFinish);
    // A draw uses the normal finish, with no vote, reset or score award.
    cc::FinishNative(refused,20); CHECK(refused.nativeFinish && refused.nativeFinishFrame==20 && !refused.holding);
    DelayedControlAndRollback();
    CommittedTransmission();
    TerminalDisconnectCannotRewindNativeReset();
    std::puts("Continuous rematch choices: prediction barrier, immutable input, epoch, rollback, decline, timeout and scored limit passed");
}
