#include "../common/ContinuousChoices.hxx"
#include "test_support.hxx"
#include "../common/ButtonLayoutTypes.hxx"
#include <array>
#include <cstdio>
namespace cc=sf4e::continuouschoices;
cc::Action Frame(cc::State& s,int frame,unsigned a,unsigned b) {
    const std::uint32_t wire[2]={cc::Encode(s.epoch,a)|0x123456,cc::Encode(s.epoch,b)|0x654321};
    return cc::Advance(s,frame,wire);
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
    CHECK(Frame(s,106,cc::Ready|cc::Commit,cc::Ready|cc::Commit)==cc::Action::Restart);
    CHECK(!s.holding&&s.epoch==2&&s.resets==1&&s.offerFrame<0);
    s=beforeReset;
    CHECK(Frame(s,106,cc::Ready|cc::Commit,cc::Ready|cc::Commit)==cc::Action::Restart);
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
    const auto a=Frame(s,105,cc::Ready|cc::Commit,cc::Ready|cc::Commit);
    const auto b=Frame(reference,105,cc::Ready|cc::Commit,cc::Ready|cc::Commit);
    CHECK(a==b&&a==cc::Action::Restart&&s.epoch==reference.epoch&&s.resets==reference.resets);
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
        const auto decision=cc::Advance(scored,1000*game+2,commit);
        auto replay=savedScored;
        CHECK(cc::Advance(replay,1000*game+2,commit)==decision);
        CHECK(replay.resets==scored.resets&&replay.epoch==scored.epoch);
        CHECK(decision==(game==3?cc::Action::Hold:cc::Action::Restart));
    }
    CHECK(scored.resets==2&&scored.exitFrame==3002&&scored.ruleLimit);
    // More than ten rematches and several wire-tag wraps, with rollback at
    // every reset. A full epoch remains distinct for local choices/UI offers.
    cc::State longSet;
    for(unsigned game=1;game<=80;++game) {
        CHECK(longSet.epoch==game&&longSet.resets==game-1);
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
        const std::uint32_t commit[2]={cc::Encode(game,cc::Ready|cc::Commit),cc::Encode(game,cc::Ready|cc::Commit)};
        CHECK(cc::Advance(longSet,game*100+2,commit)==cc::Action::Restart);
        CHECK(cc::Advance(rollback,game*100+2,commit)==cc::Action::Restart);
        CHECK(rollback.epoch==longSet.epoch&&rollback.resets==longSet.resets);
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
    std::puts("ContinuousChoices: consent, commit, epoch, rollback, decline, timeout and scored set limit passed");
}
