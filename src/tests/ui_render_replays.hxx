#pragma once
// Saved replay choices, the restored export caption page, and a running
// export's progress and Cancel, in every render locale.
#include "ui_render_support.hxx"
#include "../common/Localization.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/ReplaySummaryText.hxx"
#include <algorithm>
#include <iterator>
#include <vector>
namespace {
template<class Draw,class Page>
void ShootReplays(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page,std::vector<std::string>& exportsStarted,bool& refuseExports) {
    using namespace sf4e;using namespace ui;
    page("replays");replayinputs::Summary said;said.scored=true;said.score[0]=2;said.score[1]=1;said.rounds=3;said.frames=131*60;
    said.players[0]={1,2,9,-1,1,0};said.players[1]={2,0,0,-1,0,0};
    for(int side=0;side<2;side++){said.stats[side].frames=said.frames;said.stats[side].actions=700-side*40;said.stats[side].jumps=18;said.stats[side].crouched=said.frames*(54-side*23)/100;for(int b=0;b<6;b++)said.stats[side].presses[b]=101-b*14-side*9;}
    platform::replays::ArchivedReplay alice,again,other,both,older;
    again.path="e";again.label="2026-10-05 23:30";again.names[0]="Alice";again.names[1]="Bob";again.summary=said;again.summary->score[0]=1;again.summary->score[1]=2;again.fighters[0]=0;again.fighters[1]=1;
    alice.path="a";alice.label="2026-10-05 23:35";alice.names[0]="Alice";alice.names[1]="Bob";alice.watched=alice.video=true;alice.summary=said;alice.fighters[0]=0;alice.fighters[1]=1;
    other.path="b";other.label="2026-10-05 23:36";other.provenance=sf4e::replayfiles::Provenance::Spectated;other.fighters[0]=2;other.fighters[1]=3;
    both.path="c";both.label="2026-10-05 23:40";both.names[0]="Carol";both.names[1]="Dave";both.provenance=sf4e::replayfiles::Provenance::Spectated;both.watched=true;both.summary=said;both.fighters[0]=8;both.fighters[1]=30;
    older.path="d";older.label="2026-10-04 20:12";older.fighters[0]=12;older.fighters[1]=-1;older.kept=true;
    view.replays.archive=std::make_shared<const std::vector<platform::replays::ArchivedReplay>>(std::vector<platform::replays::ArchivedReplay>{alice,again,other,both,older});
    view.replays.ready=true;view.replays.notice=loc::T("replays.added");
    std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    // The first replay is focused, so a narrow window scrolls the list to it.
    page("replays");shell.Navigation().Focus("replay:a",rows);draw("replays-cards");
    Require(rows.size()==9&&rows[2].id=="replay-filter"&&rows[3].id=="replay-search"&&rows[4].value=="2-1 · "+std::string(loc::T("replays.seen"))+", "+loc::T("replays.video")&&
        rows[6].value==loc::T("replays.spectated")&&rows[7].value=="2-1 · "+std::string(loc::T("replays.spectated_seen"))&&rows[8].value==loc::T("replays.kept"),"Replay tags changed");
    // Sort & filter is its own screen: a card per fighter, then Sort, Show, Side, the count and Reset.
    shell.Navigation().Focus("replay-filter",rows);draw(nullptr,MenuInput::Select,1);draw();draw("replay-filter");
    Require(shell.Navigation().Screen()=="replay-filter"&&rows.size()==selection::FighterCount+5&&rows[selection::FighterCount].id=="rf-sort"&&rows[selection::FighterCount+3].id=="rf-count","Sort and filter did not list its fighters and choices");
    // Show stepped to Spectated lists the two watched live.
    shell.Navigation().Focus("rf-show",rows);draw(nullptr,MenuInput::Right,1);draw();draw(nullptr,MenuInput::Right,1);draw();
    Require(rows[selection::FighterCount+1].value==loc::T("replays.show.spectated")&&rows[selection::FighterCount+3].value=="2 / 5","Show did not narrow the matches to the spectated ones");
    draw(nullptr,MenuInput::Back,1);draw();draw("replays-spectated");
    Require(shell.Navigation().Screen()=="replays"&&rows.size()==6&&rows[4].id=="replay:b"&&rows[5].id=="replay:c"&&rows[2].value.find(loc::T("replays.show.spectated"))!=std::string::npos,"Back did not return to the narrowed list");
    // Reset lists every match again.
    shell.Navigation().Focus("replay-filter",rows);draw(nullptr,MenuInput::Select,1);draw();draw();
    shell.Navigation().Focus("rf-reset",rows);draw(nullptr,MenuInput::Select,1);draw();
    draw(nullptr,MenuInput::Back,1);draw();draw();
    Require(rows.size()==9,"Reset did not list every match again");
    // Alice and Bob's two matches in a row are a set on the Sets tab, 1-1 by who won each; Pick a game lists them oldest first.
    page("replay-sets");draw("replay-sets");
    Require(rows.size()==3&&rows[0].id=="replay-filter"&&rows[1].id=="replay-search"&&rows[2].id=="set:a"&&rows[2].label=="Alice 1-1 Bob","The set of Alice and Bob was not listed");
    // The same filter narrows the Sets tab: Spectated hides a set none of whose games were watched live, and Reset lists it again.
    shell.Navigation().Focus("replay-filter",rows);draw(nullptr,MenuInput::Select,1);draw();draw();
    shell.Navigation().Focus("rf-show",rows);draw(nullptr,MenuInput::Right,1);draw();draw(nullptr,MenuInput::Right,1);draw();
    const auto count=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="rf-count";});
    Require(count!=rows.end()&&count->label==loc::T("replays.sets_shown")&&count->value=="0 / 1","The filter counted matches while the Sets tab was open");
    draw(nullptr,MenuInput::Back,1);draw();draw();
    Require(shell.Navigation().Screen()=="replay-sets"&&rows.size()==3&&rows[2].id=="replay-none-shown","Spectated did not hide the set");
    shell.Navigation().Focus("replay-filter",rows);draw(nullptr,MenuInput::Select,1);draw();draw();
    shell.Navigation().Focus("rf-reset",rows);draw(nullptr,MenuInput::Select,1);draw();
    draw(nullptr,MenuInput::Back,1);draw();draw();
    Require(rows.size()==3&&rows[2].id=="set:a","Reset did not list the set again");
    shell.Navigation().Focus("set:a",rows);draw(nullptr,MenuInput::Select,1);draw();draw(nullptr,MenuInput::Right,1);draw();draw(nullptr,MenuInput::Select,1);draw("replay-set");
    Require(shell.Navigation().Screen()=="replay-set"&&rows.size()==2&&rows[0].id=="replay:e"&&rows[1].id=="replay:a","Pick a game did not list the set's games");
    draw(nullptr,MenuInput::Back,1);draw();
    // Export set videos: the first game's caption is set up and sent by hand; once its video is saved the next game's export follows by itself.
    shell.Navigation().Focus("set:a",rows);draw(nullptr,MenuInput::Select,1);draw();
    for(int i=0;i<2;++i){draw(nullptr,MenuInput::Right,1);draw();}
    draw(nullptr,MenuInput::Select,1);draw();
    Require(shell.Navigation().Screen()=="replay-export","Export set videos did not open the first game's caption");
    // An earlier export saves its video while the caption is open: it is not this set's.
    view.replays.exportEnded=77;view.replays.exportSaved=true;draw();draw();
    shell.Navigation().Focus("cap-generate",rows);draw(nullptr,MenuInput::Select,1);draw();
    Require(exportsStarted==std::vector<std::string>{"e"},"Generate did not send the set's first game");
    draw();draw();
    Require(exportsStarted==std::vector<std::string>{"e"},"An earlier export's end advanced the set before its own export ended");
    // The first game's export (the first sent: number 1) saves its video. The next game's export is refused: it is not counted as sent, and stays for the Next video row.
    refuseExports=true;view.replays.exportEnded=1;view.replays.exportSaved=true;draw();draw();refuseExports=false;
    Require(exportsStarted==std::vector<std::string>{"e"},"A refused export was counted as sent");
    page("replays");draw();
    Require(std::any_of(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="set-next";}),"A refused set export lost its game");
    shell.Navigation().Focus("set-next",rows);draw(nullptr,MenuInput::Select,1);draw();
    Require(shell.Navigation().Screen()=="replay-export","Next video did not offer the refused game again");
    shell.Navigation().Focus("cap-generate",rows);draw(nullptr,MenuInput::Select,1);draw();
    Require(exportsStarted==(std::vector<std::string>{"e","a set 0-1"}),"The set's next video did not follow the one made with the set so far");
    // Its encode fails (export 2), then an unrelated export saves a video: the game stays queued rather than being dropped.
    view.replays.exportEnded=2;view.replays.exportSaved=false;draw();draw();
    view.replays.exportEnded=88;view.replays.exportSaved=true;draw();draw();page("replays");draw();
    Require(exportsStarted.size()==2&&std::any_of(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="set-next";}),"A set export that made no video dropped its game");
    // Sent again (export 3), its video is saved: no game is left, so no export follows.
    shell.Navigation().Focus("set-next",rows);draw(nullptr,MenuInput::Select,1);draw();
    shell.Navigation().Focus("cap-generate",rows);draw(nullptr,MenuInput::Select,1);draw();
    Require(exportsStarted.size()==3,"The failed game's export was not sent again");
    view.replays.exportEnded=3;view.replays.exportSaved=true;draw();draw();
    Require(exportsStarted.size()==3,"A set with no game left sent another export");
    page("replays");draw();
    Require(std::none_of(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="set-next";}),"A finished set offered another video");
    page("replays");
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
    // Ember's mark turned off is saved, and stays off on the next replay's caption; its names come from that replay.
    shell.Navigation().Focus("cap-mark",rows);draw(nullptr,MenuInput::Left,1);draw(nullptr,0,30);
    Require(rows[8].value==loc::T("common.off"),"The export's mark row did not turn off");
    draw(nullptr,MenuInput::Back,1);draw();
    shell.Navigation().Focus("replay:c",rows);draw(nullptr,MenuInput::Select,1);draw();
    for(int i=0;i<3;++i){draw(nullptr,MenuInput::Right,1);draw();}
    draw(nullptr,MenuInput::Select,1);draw("replay-export-again");
    Require(shell.Navigation().Screen()=="replay-export"&&rows[8].value==loc::T("common.off")&&rows[1].value=="Carol","The next export did not keep the mark off or take its own names");
    shell.Navigation().Focus("cap-mark",rows);draw(nullptr,MenuInput::Right,1);draw();
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
