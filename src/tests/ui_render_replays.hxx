#pragma once
// Saved replay choices and the restored export caption page, in every render locale.
#include "ui_render_support.hxx"
#include "../common/Localization.hxx"
#include "../ui/ApplicationShell.hxx"
#include <iterator>
#include <vector>
namespace {
template<class Draw,class Page>
void ShootReplays(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page) {
    using namespace sf4e;using namespace ui;
    page("replays");replayinputs::Summary said;said.scored=true;said.score[0]=2;said.score[1]=1;said.rounds=3;said.frames=131*60;
    said.players[0]={1,2,9,-1,1,0};said.players[1]={2,0,0,-1,0,0};
    for(int side=0;side<2;side++){said.stats[side].frames=said.frames;said.stats[side].actions=700-side*40;said.stats[side].jumps=18;said.stats[side].crouched=said.frames*(54-side*23)/100;for(int b=0;b<6;b++)said.stats[side].presses[b]=101-b*14-side*9;}
    platform::replays::ArchivedReplay alice,other;
    alice.path="a";alice.label="2026-10-05 23:35";alice.names[0]="Alice";alice.names[1]="Bob";alice.watched=alice.video=true;alice.summary=said;alice.fighters[0]=0;alice.fighters[1]=1;
    other.path="b";other.label="2026-10-05 23:36";other.spectated=true;other.fighters[0]=2;other.fighters[1]=3;
    view.replays.archive=std::make_shared<const std::vector<platform::replays::ArchivedReplay>>(std::vector<platform::replays::ArchivedReplay>{alice,other});
    view.replays.ready=true;view.replays.notice=loc::T("replays.added");
    std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    page("replays");shell.Navigation().Focus("replay:a",rows);
    draw(nullptr,MenuInput::Select,1);draw("replay-choices");
    for(int i=0;i<3;++i){draw(nullptr,MenuInput::Right,1);draw();}
    draw(nullptr,MenuInput::Select,1);draw("replay-export");
    Require(shell.Navigation().Screen()=="replay-export","Replay Export choice did not reach the caption page");
    const char* ids[]={"cap-names","cap-name1","cap-name2","cap-line","cap-text","cap-set","cap-set1","cap-set2","cap-mark","cap-generate"};
    Require(rows.size()==std::size(ids),"Export caption page lost a control");
    for(std::size_t i=0;i<rows.size();++i)Require(rows[i].id==ids[i],"Export caption control order changed");
    SetMenuEntriesProbe({});draw(nullptr,MenuInput::Back,1);draw();
    view.replays=ReplaysView{};
}
}
