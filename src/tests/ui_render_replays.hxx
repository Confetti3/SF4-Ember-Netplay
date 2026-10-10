#pragma once
// Saved replay choices, the restored export caption page, and a running
// export's progress and Cancel, in every render locale.
#include "ui_render_support.hxx"
#include "../common/Localization.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/ReplaySummaryText.hxx"
#include <iterator>
#include <vector>
namespace {
template<class Draw,class Page>
void ShootReplays(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page) {
    using namespace sf4e;using namespace ui;
    page("replays");replayinputs::Summary said;said.scored=true;said.score[0]=2;said.score[1]=1;said.rounds=3;said.frames=131*60;
    said.players[0]={1,2,9,-1,1,0};said.players[1]={2,0,0,-1,0,0};
    for(int side=0;side<2;side++){said.stats[side].frames=said.frames;said.stats[side].actions=700-side*40;said.stats[side].jumps=18;said.stats[side].crouched=said.frames*(54-side*23)/100;for(int b=0;b<6;b++)said.stats[side].presses[b]=101-b*14-side*9;}
    platform::replays::ArchivedReplay alice,other,both,older;
    alice.path="a";alice.label="2026-10-05 23:35";alice.names[0]="Alice";alice.names[1]="Bob";alice.watched=alice.video=true;alice.summary=said;alice.fighters[0]=0;alice.fighters[1]=1;
    other.path="b";other.label="2026-10-05 23:36";other.spectated=true;other.fighters[0]=2;other.fighters[1]=3;
    both.path="c";both.label="2026-10-05 23:40";both.names[0]="Carol";both.names[1]="Dave";both.spectated=both.watched=true;both.summary=said;both.fighters[0]=8;both.fighters[1]=30;
    older.path="d";older.label="2026-10-04 20:12";older.fighters[0]=12;older.fighters[1]=-1;
    view.replays.archive=std::make_shared<const std::vector<platform::replays::ArchivedReplay>>(std::vector<platform::replays::ArchivedReplay>{alice,other,both,older});
    view.replays.ready=true;view.replays.notice=loc::T("replays.added");
    std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    // Each replay's row draws its two fighters, player 1 on the left, and no other row draws one.
    std::vector<std::pair<int,float>> faces;
    SetPortraitProbe([&](int fighter,ImVec2 min,ImVec2){faces.emplace_back(fighter,min.x);});
    // The first replay is focused, so a narrow window scrolls the list to it.
    page("replays");shell.Navigation().Focus("replay:a",rows);draw("replays-portraits");
    // One frame's faces: a narrow window may scroll the last rows out of view, which then draw none.
    faces.clear();draw(nullptr,0,1);
    Require(faces.size()>=2&&faces.size()%2==0,"Replay rows did not draw two portraits each");
    Require(faces[0].first==0&&faces[1].first==1&&faces[0].second<faces[1].second,"Replay portraits were not player 1 left, player 2 right");
    Require(faces.size()<8||(faces[6].first==12&&faces[7].first==-1),"A replay with one fighter unknown did not keep its side");
    SetPortraitProbe({});
    Require(rows.size()==6&&rows[2].value=="2-1 · "+std::string(loc::T("replays.seen"))+", "+loc::T("replays.video")&&
        rows[3].value==loc::T("replays.spectated")&&rows[4].value=="2-1 · "+std::string(loc::T("replays.spectated_seen")),"Replay tags changed");
    shell.Navigation().Focus("replay:a",rows);
    draw(nullptr,MenuInput::Select,1);draw("replay-choices");
    for(int i=0;i<3;++i){draw(nullptr,MenuInput::Right,1);draw();}
    draw(nullptr,MenuInput::Select,1);draw("replay-export");
    Require(shell.Navigation().Screen()=="replay-export","Replay Export choice did not reach the caption page");
    const char* ids[]={"cap-names","cap-name1","cap-name2","cap-line","cap-text","cap-set","cap-set1","cap-set2","cap-mark","cap-meter","cap-generate"};
    Require(rows.size()==std::size(ids),"Export caption page lost a control");
    for(std::size_t i=0;i<rows.size();++i)Require(rows[i].id==ids[i],"Export caption control order changed");
    // The Frame meter row, on and focused.
    shell.Navigation().Focus("cap-meter",rows);draw(nullptr,MenuInput::Right,1);draw("replay-export-meter");
    Require(rows[9].value==loc::T("common.on"),"The export's Frame meter row did not turn on");
    draw(nullptr,MenuInput::Left,1);draw();
    SetMenuEntriesProbe({});draw(nullptr,MenuInput::Back,1);draw();
    // A running export leads Replays with its progress and Cancel export,
    // which asks first with Cancel focused.
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    view.replays.ready=false;view.replays.notice.clear();
    view.replays.exportStage=replay::ExportStage::Recording;view.replays.exportFrames=62*60;view.replays.exportTotal=131*60;
    page("replays");draw("replay-export-progress");
    Require(rows.size()>=2&&rows[0].id=="export-progress"&&rows[0].value==ExportProgressText(62*60,131*60)&&rows[1].id=="export-cancel","Export progress or Cancel missing");
    shell.Navigation().Focus("export-cancel",rows);draw();
    draw(nullptr,MenuInput::Select,1);draw("replay-export-cancel");
    Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),"Cancel export did not ask with Cancel focused");
    draw(nullptr,MenuInput::Back,1);draw();
    Require(!shell.Navigation().Confirming(),"Back did not close the cancel question");
    view.replays.exportStage=replay::ExportStage::Cancelling;draw("replay-export-cancelling");
    Require(rows[0].value==loc::T("export.cancelling")&&rows[1].id!="export-cancel","A cancelling export still offered Cancel");
    SetMenuEntriesProbe({});
    view.replays=ReplaysView{};
}
}
