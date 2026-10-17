#include "../ui/StartupUpdateFlow.hxx"
#include "test_support.hxx"
#include <iostream>
using namespace sf4e;
int main() {
    using Flow=ui::StartupUpdateFlow; using A=Flow::Action;
    platform::ServiceSnapshot state;
    state.launchCheck=platform::LaunchCheck::Running; state.pending=true;
    Flow check(true);
    CHECK(check.Observe(state)==A::None);
    state.pending=false; state.launchCheck=platform::LaunchCheck::Settled;
    CHECK(check.Observe(state)==A::Continue); // offline, no update, disabled or cooldown
    CHECK(check.Observe(state)==A::None);
    state.launchOffer=launcher::InstallOffer{launcher::UpdateChannel::Stable,"v1.4.0",std::string(64,'a')};
    for (auto answer : {ui::NoticeResult::Acknowledged,ui::NoticeResult::Dismissed,ui::NoticeResult::UpdateNow,ui::NoticeResult::SkipUpdate}) {
        Flow flow(true);
        CHECK(flow.Observe(state)==A::Ask && flow.Asking() && flow.Offer()==state.launchOffer);
        CHECK(flow.Observe(state)==A::None); // no double prompt
        auto action=flow.Answer(answer);
        if(answer==ui::NoticeResult::UpdateNow) {
            CHECK(action==A::Install);
            state.lastAction=platform::ServiceAction::InstallUpdate; state.succeeded=false;
            CHECK(flow.Observe(state)==A::None); // failure remains visible; no game launch
        } else if(answer==ui::NoticeResult::SkipUpdate) {
            CHECK(action==A::Skip);
            state.lastAction=platform::ServiceAction::SkipUpdate; state.pending=true;
            CHECK(flow.Observe(state)==A::None);
            state.pending=false; state.succeeded=false;
            CHECK(flow.Observe(state)==A::None); // failed persistence never exits silently
        } else CHECK(action==A::Continue);
    }
    Flow skip(true); CHECK(skip.Observe(state)==A::Ask);
    CHECK(skip.Answer(ui::NoticeResult::SkipUpdate)==A::Skip);
    state.lastAction=platform::ServiceAction::SkipUpdate; state.succeeded=true;
    CHECK(skip.Observe(state)==A::Continue);
    Flow manual(false); CHECK(manual.Observe(state)==A::None);
    Flow cancelled(true); cancelled.Manual(); CHECK(cancelled.Observe(state)==A::None);
    const auto dialog=ui::UpdatePrompt::Dialog(*state.launchOffer,"1.3.0");
    CHECK(dialog.selected==1 && dialog.buttons.size()==3);
    CHECK(dialog.buttons[0].result==ui::NoticeResult::UpdateNow);
    CHECK(dialog.buttons[1].result==ui::NoticeResult::Acknowledged);
    CHECK(dialog.buttons[2].result==ui::NoticeResult::SkipUpdate);
    std::cout<<"Pre-game update decisions passed\n";
}
