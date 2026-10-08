#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "ReplaySummaryText.hxx"

namespace sf4e { namespace ui {
// The Inputs and stats screen: the replay's own label and length, for each
// player the look and what was pressed, and a row a round whose Select opens
// the round's inputs in the reader. Until the match for that file is handed
// over the screen says it is being read.
void ApplicationShell::BuildInputsRows(const ShellView& v,std::vector<MenuEntry>& rows) {
 namespace in=replayinputs;
 try {
 if(v.replayDetail.revision!=inputsRevision_||v.replayDetail.state==replayinputs::DetailState::Pending){rows.push_back(InfoRow("inputs-reading",loc::T("inputs.reading"),"",loc::T("replays.inputs_detail")));return;}
 if(v.replayDetail.state!=replayinputs::DetailState::Ready||!v.replayDetail.value||v.replayDetail.value->file!=inputsFile_){rows.push_back(InfoRow("inputs-none",loc::T("inputs.unreadable"),"",loc::T("inputs.unreadable_detail")));return;}
 const auto& detail=*v.replayDetail.value;
 const in::Match& match=detail.match;
 const in::Summary& summary=detail.summary;
 const std::string score=ScoreText(summary);
 rows.push_back(InfoRow("inputs-replay",ReplayLabel(detail.label,detail.names,detail.fighters),(score.empty()?"":score+"  ")+MatchText(summary),loc::T("replays.inputs_detail")));
 rows.back().userText=true;
 for(int side=0;side<2;side++){
  rows.push_back(InfoRow("inputs-p"+std::to_string(side+1),loc::Tf("inputs.buttons",ReplayPlayerName(detail.names,side)),PressesText(summary.stats[side]),LookText(summary.players[side])+". "+ActivityText(summary.stats[side])));
  rows.back().userText=!detail.names[side].empty();
 }
 for(std::size_t round=0;round<match.rounds.size();round++){
  const std::size_t number=round+1;
  auto row=Row("inputs-round-"+std::to_string(number),loc::Tf("inputs.round",number),std::string(loc::T("inputs.legend"))+"\n\n"+detail.logs[round]);
  row.value=in::Clock(match.rounds[round].frames);row.reading=true;
  rows.push_back(std::move(row));
 }
 }catch(...){
  // Preserve nightly's no-partial-detail boundary if constructing UI text
  // fails after the worker has successfully prepared the replay.
  rows.clear();rows.push_back(InfoRow("inputs-none",loc::T("inputs.unreadable"),"",loc::T("inputs.unreadable_detail")));
 }
}
} }
